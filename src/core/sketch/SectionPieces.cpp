// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/sketch/SectionPieces.h"

#include "core/Angles.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace sketch {

namespace {

using T = PieceThresholds;
constexpr double kInf = std::numeric_limits<double>::infinity();

double Cross(const glm::dvec2& a, const glm::dvec2& b) {
  return a.x * b.y - a.y * b.x;
}

// ---- statistics ------------------------------------------------------------

// The regularized incomplete beta function I_x(a, b), by its continued
// fraction (Lentz), for Student's t distribution below.
double IncompleteBeta(double a, double b, double x) {
  if (x <= 0.0)
    return 0.0;
  if (x >= 1.0)
    return 1.0;
  const double lbeta = std::lgamma(a + b) - std::lgamma(a) - std::lgamma(b);
  const double front = std::exp(lbeta + a * std::log(x) + b * std::log1p(-x));
  if (x > (a + 1.0) / (a + b + 2.0))
    return 1.0 - IncompleteBeta(b, a, 1.0 - x);
  const double tiny = 1e-300;
  double f = 1.0, c = 1.0, d = 0.0;
  for (int i = 0; i <= 400; i++) {
    const int m = i / 2;
    double num;
    if (i == 0)
      num = 1.0;
    else if (i % 2 == 0)
      num = (m * (b - m) * x) / ((a + 2.0 * m - 1.0) * (a + 2.0 * m));
    else
      num =
          -((a + m) * (a + b + m) * x) / ((a + 2.0 * m) * (a + 2.0 * m + 1.0));
    d = 1.0 + num * d;
    d = std::abs(d) < tiny ? tiny : d;
    d = 1.0 / d;
    c = 1.0 + num / c;
    c = std::abs(c) < tiny ? tiny : c;
    const double cd = c * d;
    f *= cd;
    if (std::abs(1.0 - cd) < 1e-12)
      break;
  }
  return front * (f - 1.0) / a;
}

// F(1, dof)'s critical value at level `alpha`: the square of Student's t
// at two-sided `alpha`, found by bisection on its tail
// P(|T| > t) = I_{v / (v + t^2)}(v / 2, 1 / 2).
double FCritical(double alpha, double dof) {
  const double v = std::max(dof, 1.0);
  double lo = 0.0, hi = 1e4;
  for (int it = 0; it < 200; it++) {
    const double t = 0.5 * (lo + hi);
    const double tail = IncompleteBeta(0.5 * v, 0.5, v / (v + t * t));
    (tail > alpha ? lo : hi) = t;
  }
  const double t = 0.5 * (lo + hi);
  return t * t;
}

// The lag-one correlation of a residual sequence, taken into [0, the clamp].
double LagOne(const std::vector<double>& r) {
  double num = 0.0, den = 0.0;
  for (size_t k = 0; k < r.size(); k++) {
    den += r[k] * r[k];
    if (k + 1 < r.size())
      num += r[k] * r[k + 1];
  }
  if (!(den > 0.0))
    return 0.0;
  return std::clamp(num / den, 0.0, T::kMaxCorrelation);
}

// How much a mean of `w` neighbouring residuals with lag-one correlation
// `rho` (an AR(1) sequence) varies, against w independent ones.
double WindowVarianceFactor(int w, double rho) {
  double f = 1.0, rl = 1.0;
  for (int l = 1; l < w; l++) {
    rl *= rho;
    f += 2.0 * (1.0 - double(l) / w) * rl;
  }
  return f;
}

// ---- models ----------------------------------------------------------------

struct Model {
  bool arc = false;
  bool ok = false;
  glm::dvec2 p0{0.0}, dir{1.0, 0.0}; // a line through p0 along unit dir
  glm::dvec2 centre{0.0};
  double radius = 0.0;

  double Residual(const glm::dvec2& p) const {
    return arc ? glm::length(p - centre) - radius : Cross(dir, p - p0);
  }
  // Unit tangent at the point of the model nearest p (either sense).
  glm::dvec2 Tangent(const glm::dvec2& p) const {
    if (!arc)
      return dir;
    const glm::dvec2 q = p - centre;
    const double l = glm::length(q);
    return l > 0.0 ? glm::dvec2(-q.y, q.x) / l : glm::dvec2(1.0, 0.0);
  }
  glm::dvec2 Project(const glm::dvec2& p) const {
    if (!arc)
      return p0 + dir * glm::dot(p - p0, dir);
    const glm::dvec2 q = p - centre;
    const double l = glm::length(q);
    return l > 0.0 ? centre + q * (radius / l)
                   : centre + glm::dvec2(radius, 0.0);
  }
};

glm::dvec2 Centroid(const std::vector<glm::dvec2>& pts) {
  glm::dvec2 m(0.0);
  for (const glm::dvec2& p : pts)
    m += p;
  return m / double(std::max<size_t>(pts.size(), 1));
}

// Total least squares: the line through the centroid along the points'
// principal direction.
Model FitLine(const std::vector<glm::dvec2>& pts) {
  Model m;
  if (pts.size() < 2)
    return m;
  const glm::dvec2 c = Centroid(pts);
  double sxx = 0.0, sxy = 0.0, syy = 0.0;
  for (const glm::dvec2& p : pts) {
    const glm::dvec2 q = p - c;
    sxx += q.x * q.x;
    sxy += q.x * q.y;
    syy += q.y * q.y;
  }
  if (!(sxx + syy > 0.0))
    return m;
  const double a = 0.5 * std::atan2(2.0 * sxy, sxx - syy);
  m.p0 = c;
  m.dir = {std::cos(a), std::sin(a)};
  // Along the chain's own direction.
  if (glm::dot(pts.back() - pts.front(), m.dir) < 0.0)
    m.dir = -m.dir;
  m.ok = true;
  return m;
}

// Solves the 3x3 system A x = b by Cramer's rule; false when singular
// against the matrix's own scale.
bool Solve3(const double A[3][3], const double b[3], double x[3]) {
  const auto det3 = [](const double M[3][3]) {
    return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) -
           M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
           M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
  };
  const double det = det3(A);
  double scale = 0.0;
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      scale = std::max(scale, std::abs(A[i][j]));
  if (!(std::abs(det) > 1e-14 * scale * scale * scale))
    return false;
  for (int k = 0; k < 3; k++) {
    double M[3][3];
    for (int i = 0; i < 3; i++)
      for (int j = 0; j < 3; j++)
        M[i][j] = j == k ? b[i] : A[i][j];
    x[k] = det3(M) / det;
  }
  return true;
}

bool Invert3(const double A[3][3], double inv[3][3]) {
  for (int k = 0; k < 3; k++) {
    const double e[3] = {k == 0 ? 1.0 : 0.0, k == 1 ? 1.0 : 0.0,
                         k == 2 ? 1.0 : 0.0};
    double x[3];
    if (!Solve3(A, e, x))
      return false;
    for (int i = 0; i < 3; i++)
      inv[i][k] = x[i];
  }
  return true;
}

double Span(const std::vector<glm::dvec2>& pts) {
  glm::dvec2 lo(kInf), hi(-kInf);
  for (const glm::dvec2& p : pts) {
    lo = glm::min(lo, p);
    hi = glm::max(hi, p);
  }
  return pts.empty() ? 0.0 : glm::length(hi - lo);
}

// The circle nearest the points geometrically: Kåsa's algebraic fit, then
// Gauss-Newton on the true distances.
Model FitArc(const std::vector<glm::dvec2>& pts) {
  Model m;
  m.arc = true;
  if (pts.size() < 3)
    return m;
  const glm::dvec2 c0 = Centroid(pts);
  double suu = 0.0, suv = 0.0, svv = 0.0, su3 = 0.0, sv3 = 0.0, s2 = 0.0;
  for (const glm::dvec2& p : pts) {
    const glm::dvec2 q = p - c0;
    const double r2 = glm::dot(q, q);
    suu += q.x * q.x;
    suv += q.x * q.y;
    svv += q.y * q.y;
    su3 += q.x * r2;
    sv3 += q.y * r2;
    s2 += r2;
  }
  const double det = suu * svv - suv * suv;
  if (!(std::abs(det) > 1e-18 * (suu + svv) * (suu + svv)))
    return m;
  const double a = 0.5 * (su3 * svv - sv3 * suv) / det;
  const double b = 0.5 * (sv3 * suu - su3 * suv) / det;
  glm::dvec2 centre = c0 + glm::dvec2(a, b);
  double r = std::sqrt(a * a + b * b + s2 / double(pts.size()));
  const double span = std::max(Span(pts), 1e-12);
  for (int it = 0; it < 20; it++) {
    double A[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}, g[3] = {0, 0, 0};
    for (const glm::dvec2& p : pts) {
      const glm::dvec2 q = p - centre;
      const double d = glm::length(q);
      if (!(d > 0.0))
        continue;
      const double J[3] = {-q.x / d, -q.y / d, -1.0};
      const double res = d - r;
      for (int i = 0; i < 3; i++) {
        g[i] -= J[i] * res;
        for (int j = 0; j < 3; j++)
          A[i][j] += J[i] * J[j];
      }
    }
    double step[3];
    if (!Solve3(A, g, step))
      break;
    centre += glm::dvec2(step[0], step[1]);
    r += step[2];
    if (std::abs(step[0]) + std::abs(step[1]) + std::abs(step[2]) <
        1e-13 * (span + r))
      break;
  }
  if (!(r > 0.0) || !(r < T::kMaxRadiusSpan * span) ||
      !std::isfinite(centre.x) || !std::isfinite(centre.y))
    return m;
  m.centre = centre;
  m.radius = r;
  m.ok = true;
  return m;
}

// ---- the chain -------------------------------------------------------------

// Points by index on an open chain or round a closed one; a piece is a span
// [s, e] of indices, both included, that on a closed chain may run over the
// seam (e < s), or be the whole loop (e == s).
struct ChainView {
  const std::vector<glm::dvec2>& P;
  bool closed;
  size_t n;

  size_t Count(size_t s, size_t e) const {
    if (!closed)
      return e - s + 1;
    const size_t l = (e + n - s) % n;
    return (l == 0 ? n : l) + 1;
  }
  size_t At(size_t s, size_t k) const { return closed ? (s + k) % n : s + k; }
  void Gather(size_t s, size_t e, std::vector<glm::dvec2>& out) const {
    const size_t c = Count(s, e);
    out.resize(c);
    for (size_t k = 0; k < c; k++)
      out[k] = P[At(s, k)];
  }
};

struct Context {
  double sigma = 0.0;
  double rho = 0.0;
  double s = 1.0; // the detail's scale
  double floor = 0.0;
};

struct Verdict {
  bool explains = false;
  double cost = kInf;
  double rms = 0.0;
  double stretch = 0.0;
  size_t stretchAt = 0; // the window's middle index
};

double SegmentDistance(const glm::dvec2& p, const glm::dvec2& a,
                       const glm::dvec2& b) {
  const glm::dvec2 ab = b - a;
  const double len2 = glm::dot(ab, ab);
  if (len2 <= 0.0)
    return glm::length(p - a);
  const double t = std::clamp(glm::dot(p - a, ab) / len2, 0.0, 1.0);
  return glm::length(p - (a + t * ab));
}

// The worst mean of kStretchPoints consecutive values, and where.
double WorstStretch(const std::vector<double>& r, int w, size_t* at) {
  const size_t n = r.size();
  if (n == 0)
    return 0.0;
  const size_t ww = std::min<size_t>(static_cast<size_t>(w), n);
  double sum = 0.0;
  for (size_t k = 0; k < ww; k++)
    sum += r[k];
  double worst = std::abs(sum) / ww;
  size_t where = ww / 2;
  for (size_t k = ww; k < n; k++) {
    sum += r[k] - r[k - ww];
    const double v = std::abs(sum) / ww;
    if (v > worst) {
      worst = v;
      where = k - ww + 1 + ww / 2;
    }
  }
  if (at != nullptr)
    *at = where;
  return worst;
}

// The angle (radians) between the line through `pts`' first or last `m`
// points and the model there, and that line's own angle sigma.
void EndHeading(const Model& model, const std::vector<glm::dvec2>& pts,
                size_t first, size_t m, const Context& cx, double& angle,
                double& sigma) {
  std::vector<glm::dvec2> end(pts.begin() + first, pts.begin() + first + m);
  const Model local = FitLine(end);
  angle = 0.0;
  sigma = kInf;
  if (!local.ok)
    return;
  const glm::dvec2 mid = Centroid(end);
  const glm::dvec2 t = model.Tangent(mid);
  angle = std::asin(std::min(1.0, std::abs(Cross(local.dir, t))));
  double stt = 0.0;
  for (const glm::dvec2& p : end) {
    const double a = glm::dot(p - mid, local.dir);
    stt += a * a;
  }
  const double infl = (1.0 + cx.rho) / (1.0 - cx.rho);
  sigma = stt > 0.0 ? cx.sigma * std::sqrt(infl / stt) : kInf;
}

// The three tests: does `model` explain `pts`?
Verdict Judge(const Model& model, const std::vector<glm::dvec2>& pts,
              const Context& cx) {
  Verdict v;
  if (!model.ok)
    return v;
  const size_t n = pts.size();
  // Three points lie on a circle whatever they are: an arc is told from a
  // corner by more.
  if (model.arc && n < static_cast<size_t>(T::kMinPiecePoints))
    return v;
  // A wall wanders over its length: the allowance reaches kLineWander
  // over kWanderPoints, and a short line has an arc's.
  const double allow =
      model.arc ? 1.0
                : 1.0 + (T::kLineWander - 1.0) *
                            std::min(1.0, double(n) / T::kWanderPoints);
  const int params = model.arc ? 3 : 2;
  std::vector<double> r(n);
  double ssr = 0.0;
  for (size_t k = 0; k < n; k++) {
    r[k] = model.Residual(pts[k]);
    ssr += r[k] * r[k];
  }
  v.rms = std::sqrt(ssr / double(std::max<int>(1, int(n) - params)));
  const double rmsLimit = cx.sigma * (1.0 + T::kSigmaSlack * cx.s) * allow;
  v.cost = v.rms / rmsLimit;
  v.stretch = WorstStretch(r, T::kStretchPoints, &v.stretchAt);
  const int w = static_cast<int>(
      std::min<size_t>(static_cast<size_t>(T::kStretchPoints), n));
  const double windows = double(n - static_cast<size_t>(w) + 1);
  const double windowSigma =
      cx.sigma * std::sqrt(WindowVarianceFactor(w, cx.rho) / w);
  const double stretchLimit =
      allow * cx.s * windowSigma *
      (T::kStretchSlack + std::sqrt(2.0 * std::log(std::max(windows, 1.0))));
  if (v.rms > rmsLimit || v.stretch > stretchLimit)
    return v;
  const size_t m = std::min<size_t>(T::kHeadingPoints, n / 2);
  if (m >= 4) {
    for (const size_t first : {size_t(0), n - m}) {
      double angle = 0.0, sigma = 0.0;
      EndHeading(model, pts, first, m, cx, angle, sigma);
      if (angle > cx.s * T::kHeadingSigmas * sigma)
        return v;
    }
  }
  v.explains = true;
  return v;
}

// ---- noise -----------------------------------------------------------------

// Each point's scaled miss from the quadratic least squares through it and
// its neighbours, in the frame of the window's chord.
std::vector<double> LocalMisses(const ChainView& cv) {
  const int K = T::kNoiseHalfWindow;
  std::vector<double> out;
  if (cv.n < size_t(2 * K + 1))
    return out;
  const size_t stride = std::max<size_t>(1, cv.n / 6000);
  for (size_t k = cv.closed ? 0 : K; cv.closed ? k < cv.n : k + K < cv.n;
       k += stride) {
    const glm::dvec2 o = cv.P[k];
    const glm::dvec2 a = cv.P[cv.closed ? (k + cv.n - K) % cv.n : k - K];
    const glm::dvec2 b = cv.P[cv.closed ? (k + K) % cv.n : k + K];
    glm::dvec2 u = b - a;
    const double len = glm::length(u);
    if (!(len > 0.0))
      continue;
    u /= len;
    const glm::dvec2 vdir(-u.y, u.x);
    double A[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}, g[3] = {0, 0, 0};
    for (int j = -K; j <= K; j++) {
      const size_t idx = cv.closed ? (k + cv.n + j) % cv.n : k + j;
      const glm::dvec2 q = cv.P[idx] - o;
      const double x = glm::dot(q, u), y = glm::dot(q, vdir);
      const double row[3] = {1.0, x, x * x};
      for (int r = 0; r < 3; r++) {
        g[r] += row[r] * y;
        for (int c = 0; c < 3; c++)
          A[r][c] += row[r] * row[c];
      }
    }
    double coef[3], inv[3][3];
    if (!Solve3(A, g, coef) || !Invert3(A, inv))
      continue;
    // The centre point is at x = 0, y = 0: its residual is -a0, its
    // leverage inv[0][0].
    const double h = std::clamp(inv[0][0], 0.0, 0.9);
    out.push_back(-coef[0] / std::sqrt(1.0 - h));
  }
  return out;
}

double Median(std::vector<double> v) {
  if (v.empty())
    return 0.0;
  const size_t mid = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + mid, v.end());
  return v[mid];
}

// Douglas-Peucker on pts[first..last], the kept indices marked.
void KeepDp(const std::vector<glm::dvec2>& pts, size_t first, size_t last,
            double tol, std::vector<char>& keep) {
  std::vector<std::pair<size_t, size_t>> stack{{first, last}};
  while (!stack.empty()) {
    const auto [a, b] = stack.back();
    stack.pop_back();
    if (b <= a + 1)
      continue;
    double worst = -1.0;
    size_t at = a;
    for (size_t k = a + 1; k < b; k++) {
      const double d = SegmentDistance(pts[k], pts[a], pts[b]);
      if (d > worst) {
        worst = d;
        at = k;
      }
    }
    if (worst > tol) {
      keep[at] = 1;
      stack.push_back({a, at});
      stack.push_back({at, b});
    }
  }
}

// The chain's corners: Douglas-Peucker breaks (indices into the chain).
std::vector<size_t> Corners(const ChainView& cv, double tol) {
  std::vector<glm::dvec2> walk = cv.P;
  if (cv.closed)
    walk.push_back(cv.P.front());
  const size_t last = walk.size() - 1;
  std::vector<char> keep(walk.size(), 0);
  keep[0] = keep[last] = 1;
  if (cv.closed) {
    size_t apex = 1;
    for (size_t k = 1; k < last; k++)
      if (glm::length(walk[k] - walk[0]) > glm::length(walk[apex] - walk[0]))
        apex = k;
    keep[apex] = 1;
    KeepDp(walk, 0, apex, tol, keep);
    KeepDp(walk, apex, last, tol, keep);
  } else {
    KeepDp(walk, 0, last, tol, keep);
  }
  std::vector<size_t> br;
  for (size_t k = 0; k < walk.size(); k++)
    if (keep[k] && !(cv.closed && k == last))
      br.push_back(k);
  return br;
}

// ---- pieces ----------------------------------------------------------------

struct Piece {
  bool arc = false;
  Model model;
  Verdict verdict;
};

// The better explaining model of a span: a line if one explains it, else an
// arc if one does, else whichever misses less (explains false).
Piece JudgeSpan(const std::vector<glm::dvec2>& pts, const Context& cx) {
  Piece p;
  p.model = FitLine(pts);
  p.verdict = Judge(p.model, pts, cx);
  if (p.verdict.explains)
    return p;
  Piece a;
  a.arc = true;
  a.model = FitArc(pts);
  a.verdict = Judge(a.model, pts, cx);
  if (a.verdict.explains || a.verdict.cost < p.verdict.cost)
    return a;
  return p;
}

Model FitKind(const std::vector<glm::dvec2>& pts, bool arc) {
  return arc ? FitArc(pts) : FitLine(pts);
}

// Where two neighbouring pieces meet: where they cross (a tangent meeting's
// two crossings by the point between them), else where they come closest;
// `hint` the current break, `scale` the shorter piece's length.
glm::dvec2 Meeting(const Model& a, const Model& b, const glm::dvec2& hint,
                   double scale) {
  std::vector<glm::dvec2> cross;
  glm::dvec2 closest = hint;
  if (!a.arc && !b.arc) {
    const double d = Cross(a.dir, b.dir);
    if (std::abs(d) > 1e-9) {
      const double t = Cross(b.p0 - a.p0, b.dir) / d;
      cross.push_back(a.p0 + a.dir * t);
    } else {
      closest = 0.5 * (a.Project(hint) + b.Project(hint));
    }
  } else if (a.arc != b.arc) {
    const Model& l = a.arc ? b : a;
    const Model& c = a.arc ? a : b;
    const glm::dvec2 foot = l.p0 + l.dir * glm::dot(c.centre - l.p0, l.dir);
    const double h = glm::length(c.centre - foot);
    if (h < c.radius) {
      const double half = std::sqrt(c.radius * c.radius - h * h);
      cross.push_back(foot - l.dir * half);
      cross.push_back(foot + l.dir * half);
    } else {
      const glm::dvec2 out = h > 0.0 ? (foot - c.centre) / h : glm::dvec2(0);
      closest = 0.5 * (foot + c.centre + out * c.radius);
    }
  } else {
    const glm::dvec2 d = b.centre - a.centre;
    const double l = glm::length(d);
    if (l > 0.0 && l < a.radius + b.radius &&
        l > std::abs(a.radius - b.radius)) {
      const double x =
          (l * l + a.radius * a.radius - b.radius * b.radius) / (2.0 * l);
      const double y = std::sqrt(std::max(0.0, a.radius * a.radius - x * x));
      const glm::dvec2 u = d / l, v(-u.y, u.x);
      cross.push_back(a.centre + u * x + v * y);
      cross.push_back(a.centre + u * x - v * y);
    } else if (l > 0.0) {
      // Apart or one inside the other: the closest points on the line of
      // centres, on the side of the break.
      const glm::dvec2 u = d / l;
      const glm::dvec2 pa = a.Project(hint), pb = b.Project(hint);
      const glm::dvec2 qa1 = a.centre + u * a.radius,
                       qa2 = a.centre - u * a.radius;
      const glm::dvec2 qb1 = b.centre + u * b.radius,
                       qb2 = b.centre - u * b.radius;
      const glm::dvec2 qa =
          glm::length(qa1 - pa) < glm::length(qa2 - pa) ? qa1 : qa2;
      const glm::dvec2 qb =
          glm::length(qb1 - pb) < glm::length(qb2 - pb) ? qb1 : qb2;
      closest = 0.5 * (qa + qb);
      if (glm::length(closest - hint) > scale)
        closest = 0.5 * (pa + pb);
    }
  }
  if (cross.empty())
    return closest;
  if (cross.size() == 2 &&
      glm::length(cross[0] - cross[1]) < T::kTangentShare * scale)
    return 0.5 * (cross[0] + cross[1]);
  std::sort(cross.begin(), cross.end(),
            [&](const glm::dvec2& p, const glm::dvec2& q) {
              return glm::length(p - hint) < glm::length(q - hint);
            });
  return cross.front();
}

double PathLength(const std::vector<glm::dvec2>& pts) {
  double l = 0.0;
  for (size_t k = 1; k < pts.size(); k++)
    l += glm::length(pts[k] - pts[k - 1]);
  return l;
}

// How far an arc over `pts` bows from its chord: r (1 - cos(sweep / 2)).
double Sagitta(const Model& m, const std::vector<glm::dvec2>& pts) {
  if (!m.arc || pts.size() < 2)
    return 0.0;
  double sweep = 0.0;
  for (size_t k = 1; k < pts.size(); k++) {
    const glm::dvec2 q0 = pts[k - 1] - m.centre, q1 = pts[k] - m.centre;
    sweep += std::atan2(Cross(q0, q1), glm::dot(q0, q1));
  }
  const double half = std::min(std::abs(sweep), angles::kTwoPi) / 2.0;
  return m.radius * (1.0 - std::cos(std::min(half, angles::kPi / 2.0))) +
         (half > angles::kPi / 2.0 ? m.radius : 0.0);
}

// The most a line of n points may wander: its rms allowance (Judge's).
double LineAllowance(size_t n, const Context& cx) {
  const double allow = 1.0 + (T::kLineWander - 1.0) *
                                 std::min(1.0, double(n) / T::kWanderPoints);
  return cx.sigma * (1.0 + T::kSigmaSlack * cx.s) * allow;
}

class PiecesFitter {
public:
  PiecesFitter(const CutChain& c, double floor, double detail)
      : cv_{c.points, c.closed, c.points.size()} {
    cx_.floor = floor;
    cx_.s = std::pow(T::kDetailRange, 1.0 - 2.0 * std::clamp(detail, 0.0, 1.0));
  }

  ChainNoise Noise() {
    std::vector<double> misses = LocalMisses(cv_);
    for (double& m : misses)
      m = std::abs(m);
    const double sigma = misses.empty() ? 0.0 : Median(misses) / 0.6745;
    cx_.sigma = std::max(sigma, cx_.floor);
    return {cx_.sigma, 0.0};
  }

  // The correlation of the noise, pooled over the corner runs' residuals
  // (each run's better of line and arc): long enough runs that the fit
  // takes little of it.
  double Sigma() const { return cx_.sigma; }

  double Correlation(const std::vector<size_t>& br) {
    double num = 0.0, den = 0.0;
    std::vector<glm::dvec2> pts;
    std::vector<double> runRms;
    const size_t pieces = cv_.closed ? br.size() : br.size() - 1;
    for (size_t i = 0; i < pieces; i++) {
      cv_.Gather(br[i], br[(i + 1) % br.size()], pts);
      if (pts.size() < 20)
        continue;
      const Model l = FitLine(pts), a = FitArc(pts);
      std::vector<double> rl(pts.size()), ra(pts.size());
      double sl = 0.0, sa = kInf;
      for (size_t k = 0; k < pts.size(); k++) {
        rl[k] = l.ok ? l.Residual(pts[k]) : 0.0;
        sl += rl[k] * rl[k];
      }
      if (a.ok) {
        sa = 0.0;
        for (size_t k = 0; k < pts.size(); k++) {
          ra[k] = a.Residual(pts[k]);
          sa += ra[k] * ra[k];
        }
      }
      // Robustly: a sigma from the median miss, and the misses clipped at
      // three of it, so a round the run swallowed does not read as noise.
      std::vector<double> r = sa < sl ? ra : rl;
      std::vector<double> mag(r.size());
      for (size_t k = 0; k < r.size(); k++)
        mag[k] = std::abs(r[k]);
      const double rs = Median(mag) / 0.6745;
      runRms.push_back(rs);
      for (double& v : r)
        v = std::clamp(v, -3.0 * rs, 3.0 * rs);
      for (size_t k = 0; k + 1 < r.size(); k++) {
        num += r[k] * r[k + 1];
        den += r[k] * r[k];
      }
    }
    // An exact cut: what scatter there is lies under the floor.
    if (den > 0.0 && den > 1e-6 * cx_.floor * cx_.floor)
      cx_.rho = std::clamp(num / den, 0.0, T::kMaxCorrelation);
    // Noise shared between neighbours hides from the local quadratics
    // (they follow it); the runs' own scatter does not.
    if (!runRms.empty())
      cx_.sigma = std::max(cx_.sigma, Median(runRms));
    return cx_.rho;
  }

  PiecesFit Run(int chainIndex) {
    PiecesFit out;
    out.noise = Noise();
    if (cv_.n < 2)
      return out;
    // The noise about the corner runs first; then, the pieces found, about
    // the pieces -- long runs of their own kind, which follow less of it
    // -- and the pieces found again if it moved.
    Correlation(Corners(cv_, T::kSplitSigmas * cx_.sigma));
    Pass();
    const double sigma = cx_.sigma, rho = cx_.rho;
    Correlation(br_);
    if (std::abs(cx_.sigma - sigma) > 0.05 * sigma ||
        std::abs(cx_.rho - rho) > 0.05)
      Pass();
    out.noise = {cx_.sigma, cx_.rho};
    Emit(chainIndex, out);
    return out;
  }

private:
  void Pass() {
    br_ = Corners(cv_, T::kSplitSigmas * cx_.sigma);
    if (!cv_.closed && br_.size() < 2)
      br_ = {0, cv_.n - 1};
    if (cv_.closed && br_.empty())
      br_ = {0};
    pieces_.clear();
    Merge();
    GiveAwayLeftovers();
    Settle();
    for (int pass = 0; pass < T::kSlidePasses; pass++)
      if (!Slide())
        break;
    // The breaks slid, neighbours a noise spike or a swallowed end kept
    // apart may be one piece now: merged again, from the pieces as they are.
    if (PieceCount() > 1) {
      Merge();
      GiveAwayLeftovers();
      Settle();
      for (int pass = 0; pass < T::kSlidePasses; pass++)
        if (!Slide())
          break;
    }
    if (ChooseKinds()) {
      for (int pass = 0; pass < T::kSlidePasses; pass++)
        if (!Slide())
          break;
      // A piece the kinds turned may be its neighbour's own kind now: a
      // round's tangent stretch, read as a line, and the round.
      if (PieceCount() > 1) {
        Merge();
        GiveAwayLeftovers();
        Settle();
        for (int pass = 0; pass < T::kSlidePasses; pass++)
          if (!Slide())
            break;
        ChooseKinds();
      }
    }
  }

  ChainView cv_;
  Context cx_;
  std::vector<size_t> br_;
  std::vector<Piece> pieces_; // after Settle, one per piece
  std::vector<glm::dvec2> buf_, slideA_, slideB_;

  size_t PieceCount() const { return cv_.closed ? br_.size() : br_.size() - 1; }
  size_t Start(size_t i) const { return br_[i]; }
  size_t End(size_t i) const { return br_[(i + 1) % br_.size()]; }

  // Merging piece i with the next: the merged span's best piece.
  Piece JudgeMerge(size_t i) {
    const size_t b = br_.size();
    cv_.Gather(br_[i], br_[(i + 2) % b], buf_);
    return JudgeSpan(buf_, cx_);
  }

  void Merge() {
    // pairCost[i]: piece i merged with piece i+1 (cyclic on a closed chain).
    const auto pairs = [&] {
      return cv_.closed ? (br_.size() >= 2 ? br_.size() : 0)
                        : (br_.size() >= 3 ? br_.size() - 2 : 0);
    };
    std::vector<double> cost(pairs());
    const auto eval = [&](size_t i) {
      const Piece p = JudgeMerge(i);
      return p.verdict.explains ? p.verdict.cost : kInf;
    };
    for (size_t i = 0; i < cost.size(); i++)
      cost[i] = eval(i);
    while (!cost.empty()) {
      const auto it = std::min_element(cost.begin(), cost.end());
      if (!std::isfinite(*it))
        break;
      const size_t i = static_cast<size_t>(it - cost.begin());
      // Remove the break between piece i and i+1: br_[i+1].
      const size_t b = br_.size();
      const size_t gone = (i + 1) % b;
      br_.erase(br_.begin() + gone);
      // The pair after the merged one goes (the merged pair's slot is
      // judged again below); on an open chain's last pair, that one.
      cost.erase(cost.begin() + (cv_.closed
                                     ? gone % cost.size()
                                     : std::min(i + 1, cost.size() - 1)));
      if (cv_.closed && br_.size() == 1) {
        cost.clear();
        break;
      }
      cost.resize(pairs());
      if (cost.empty())
        break;
      // The merged piece's index after the erase.
      const size_t mi = gone == 0 ? br_.size() - 1 : gone - 1;
      const size_t np = cost.size();
      if (cv_.closed) {
        const size_t bb = br_.size();
        cost[mi % np] = eval(mi % bb);
        cost[(mi + bb - 1) % bb % np] = eval((mi + bb - 1) % bb);
      } else {
        if (mi < np)
          cost[mi] = eval(mi);
        if (mi >= 1 && mi - 1 < np)
          cost[mi - 1] = eval(mi - 1);
      }
    }
  }

  void GiveAwayLeftovers() {
    // A leftover is short in points AND in length against the noise: an
    // exact cut's straight edge is two points far apart, and a piece.
    const double shortLength = T::kLeftoverSigmas * cx_.sigma;
    while (PieceCount() > 1) {
      size_t shortest = SIZE_MAX, fewest = SIZE_MAX;
      for (size_t i = 0; i < PieceCount(); i++) {
        const size_t c = cv_.Count(Start(i), End(i));
        cv_.Gather(Start(i), End(i), buf_);
        if (c < fewest && PathLength(buf_) <= shortLength) {
          fewest = c;
          shortest = i;
        }
      }
      if (fewest >= static_cast<size_t>(T::kMinPiecePoints))
        break;
      const size_t np = PieceCount();
      // Merge with the neighbour whose merged span misses less.
      double before = kInf, after = kInf;
      const bool hasPrev = cv_.closed || shortest > 0;
      const bool hasNext = cv_.closed || shortest + 1 < np;
      if (hasPrev)
        before = JudgeMerge((shortest + np - 1) % np).verdict.cost;
      if (hasNext)
        after = JudgeMerge(shortest).verdict.cost;
      const size_t gone =
          before <= after ? shortest : (shortest + 1) % br_.size();
      br_.erase(br_.begin() + gone);
    }
  }

  // Each piece's kind and model, from its current span.
  void Settle() {
    pieces_.clear();
    for (size_t i = 0; i < PieceCount(); i++) {
      cv_.Gather(Start(i), End(i), buf_);
      pieces_.push_back(JudgeSpan(buf_, cx_));
    }
  }

  void Refit(size_t i) {
    cv_.Gather(Start(i), End(i), buf_);
    Piece& p = pieces_[i];
    p.model = FitKind(buf_, p.arc);
    if (!p.model.ok) {
      p.arc = !p.arc;
      p.model = FitKind(buf_, p.arc);
    }
    p.verdict = Judge(p.model, buf_, cx_);
  }

  // Moves each break to the chain point nearest where its two pieces meet.
  // True when any moved.
  // Moves break k to the chain point nearest where its two pieces meet,
  // refitting both. True when it moved.
  bool SlideBreak(size_t k) {
    const size_t np = PieceCount();
    const size_t ia = (k + np - 1) % np, ib = k % np;
    const Piece& a = pieces_[ia];
    const Piece& b = pieces_[ib];
    if (!a.model.ok || !b.model.ok)
      return false;
    cv_.Gather(Start(ia), End(ia), slideA_);
    cv_.Gather(Start(ib), End(ib), slideB_);
    const double scale = std::min(PathLength(slideA_), PathLength(slideB_));
    const glm::dvec2 m = Meeting(a.model, b.model, cv_.P[br_[k]], scale);
    // Search the points of both pieces, keeping a few at the far ends.
    const size_t keep = static_cast<size_t>(T::kMinPiecePoints) - 1;
    if (slideA_.size() + slideB_.size() < 2 * keep + 3)
      return false;
    const size_t lo = keep; // offsets from Start(ia)
    const size_t hi = slideA_.size() - 1 + slideB_.size() - 1 - keep;
    size_t best = slideA_.size() - 1;
    double bestD = kInf;
    for (size_t off = lo; off <= hi; off++) {
      const double d = glm::length(cv_.P[cv_.At(Start(ia), off)] - m);
      if (d < bestD) {
        bestD = d;
        best = off;
      }
    }
    const size_t nb = cv_.At(Start(ia), best);
    if (nb == br_[k])
      return false;
    br_[k] = nb;
    Refit(ia);
    Refit(ib);
    return true;
  }

  // Every inner break slid once. True when any moved. A closed chain's
  // breaks keep their cyclic order (each slides only between its
  // neighbours), though one may slide over the seam.
  bool Slide() {
    if (PieceCount() < 2)
      return false;
    bool moved = false;
    const size_t first = cv_.closed ? 0 : 1;
    const size_t last = cv_.closed ? br_.size() : br_.size() - 1;
    for (size_t k = first; k < last; k++)
      moved = SlideBreak(k) || moved;
    return moved;
  }

  // Moves break k to where the two pieces' squared misses, each refitted,
  // sum least, within `window` points of where it is. True when it moved.
  bool OptimiseBreak(size_t k, size_t window) {
    const size_t np = PieceCount();
    const size_t ia = (k + np - 1) % np, ib = k % np;
    const size_t na = cv_.Count(Start(ia), End(ia));
    const size_t nb = cv_.Count(Start(ib), End(ib));
    const size_t keep = static_cast<size_t>(T::kMinPiecePoints) - 1;
    if (na + nb < 2 * keep + 3)
      return false;
    const size_t at = na - 1; // the break's offset from Start(ia)
    const size_t lo = std::max(keep, at > window ? at - window : 0);
    const size_t hi = std::min(na - 1 + nb - 1 - keep, at + window);
    const size_t s0 = Start(ia), was = br_[k];
    const bool arcA = pieces_[ia].arc, arcB = pieces_[ib].arc;
    double best = kInf;
    size_t bestOff = at;
    for (size_t off = lo; off <= hi; off++) {
      br_[k] = cv_.At(s0, off);
      double ssr = 0.0;
      for (const auto& [piece, arc] : {std::pair<size_t, bool>{ia, arcA},
                                       std::pair<size_t, bool>{ib, arcB}}) {
        cv_.Gather(Start(piece), End(piece), buf_);
        const Model m = FitKind(buf_, arc);
        if (!m.ok) {
          ssr = kInf;
          break;
        }
        for (const glm::dvec2& q : buf_) {
          const double r = m.Residual(q);
          ssr += r * r;
        }
      }
      if (ssr < best) {
        best = ssr;
        bestOff = off;
      }
    }
    br_[k] = cv_.At(s0, bestOff);
    Refit(ia);
    Refit(ib);
    return br_[k] != was;
  }

  // Piece i's two breaks at their least squares, in turn, twice.
  void OptimiseAround(size_t i) {
    const size_t n = cv_.Count(Start(i), End(i));
    const size_t window = std::clamp<size_t>(n, T::kMinPiecePoints, 40);
    for (int round = 0; round < 2; round++) {
      bool moved = false;
      if (Inner(i))
        moved = OptimiseBreak(i, window) || moved;
      if (Inner((i + 1) % br_.size()))
        moved = OptimiseBreak((i + 1) % br_.size(), window) || moved;
      if (!moved)
        break;
    }
  }

  // Whether break k may slide (an open chain's ends may not).
  bool Inner(size_t k) const {
    return cv_.closed || (k > 0 && k + 1 < br_.size());
  }

  // The squared misses of pieces [i-1, i+1] (as many as there are), over
  // their union, and its point count.
  double LocalSsr(size_t i, size_t& points, std::vector<double>* res) {
    const size_t np = PieceCount();
    std::vector<size_t> list;
    if (cv_.closed ? np >= 2 : i > 0)
      list.push_back((i + np - 1) % np);
    list.push_back(i);
    if ((cv_.closed ? np >= 3 : i + 1 < np))
      list.push_back((i + 1) % np);
    double ssr = 0.0;
    points = 0;
    if (res != nullptr)
      res->clear();
    for (size_t q = 0; q < list.size(); q++) {
      const size_t j = list[q];
      cv_.Gather(Start(j), End(j), buf_);
      // A break's point belongs to both its pieces: counted once.
      for (size_t k = q > 0 ? 1 : 0; k < buf_.size(); k++) {
        const double r = pieces_[j].model.Residual(buf_[k]);
        ssr += r * r;
        points++;
        if (res != nullptr)
          res->push_back(r);
      }
    }
    return ssr;
  }

  // The F test: each piece the simpler kind unless the other is better
  // beyond chance, each kind judged with the piece's own two breaks slid to
  // where it meets its neighbours -- a round's tangent ends are taken by
  // the walls until the round is an arc. True when any changed.
  bool ChooseKinds() {
    bool changed = false;
    for (size_t i = 0; i < PieceCount(); i++) {
      cv_.Gather(Start(i), End(i), buf_);
      if (buf_.size() < 6)
        continue;
      // A short piece's breaks at their least squares first, in its kind
      // and then in the other: where a round meets a wall is a matter of a
      // few points that decide it.
      const bool shortPiece = buf_.size() < 4 * T::kHeadingPoints;
      if (shortPiece)
        OptimiseAround(i);
      const std::vector<size_t> keptBr = br_;
      const std::vector<Piece> keptPieces = pieces_;
      size_t nBefore = 0, nAfter = 0;
      std::vector<double> rBefore, rAfter;
      const double before = LocalSsr(i, nBefore, &rBefore);
      const bool wasArc = pieces_[i].arc;
      pieces_[i].arc = !wasArc;
      Refit(i);
      if (pieces_[i].arc == wasArc || !pieces_[i].model.ok) {
        br_ = keptBr;
        pieces_ = keptPieces;
        continue;
      }
      const size_t np = PieceCount();
      for (int pass = 0; pass < T::kSlidePasses; pass++) {
        bool moved = false;
        if (Inner(i))
          moved = SlideBreak(i) || moved;
        if (Inner((i + 1) % br_.size()))
          moved = SlideBreak((i + 1) % br_.size()) || moved;
        if (!moved)
          break;
      }
      if (shortPiece)
        OptimiseAround(i);
      const double after = LocalSsr(i, nAfter, &rAfter);
      const double sa = wasArc ? before : after; // the arc's
      const double sl = wasArc ? after : before;
      const std::vector<double>& ra = wasArc ? rBefore : rAfter;
      const double dof = double(std::max(nBefore, nAfter)) -
                         3.0 * double(std::min<size_t>(np, 3)) - 1.0;
      const double rho = LagOne(ra);
      const double infl = (1.0 + rho) / (1.0 - rho);
      const double resid =
          std::max(sa / std::max(dof, 1.0), cx_.floor * cx_.floor);
      const double F = (sl - sa) / resid / infl;
      const bool arcBetter = dof >= 1.0 && F > FCritical(T::kArcAlpha, dof);
      cv_.Gather(Start(i), End(i), buf_);
      bool explains = Judge(pieces_[i].model, buf_, cx_).explains;
      // Walls wander, rounds do not: an arc that bows less than a line of
      // its length may wander is a line.
      const Model& arcModel = wasArc ? keptPieces[i].model : pieces_[i].model;
      std::vector<glm::dvec2> arcPts;
      if (wasArc)
        cv_.Gather(keptBr[i], keptBr[(i + 1) % keptBr.size()], arcPts);
      else
        arcPts = buf_;
      const bool bows =
          Sagitta(arcModel, arcPts) > LineAllowance(arcPts.size(), cx_);
      // Turned into an arc: only when better and it explains; turned into
      // a line: only when the arc was not better and the line explains.
      const bool keep = wasArc ? ((!arcBetter || !bows) && explains)
                               : (arcBetter && bows && explains);
      if (keep) {
        changed = true;
      } else {
        br_ = keptBr;
        pieces_ = keptPieces;
      }
    }
    return changed;
  }

  // The uncertainty of the piece's parameters from the fit's normal
  // equations, sigma^2 the residuals' variance, widened by their
  // correlation.
  void Uncertainty(const Model& m, const std::vector<glm::dvec2>& pts,
                   PieceFit& f) {
    const size_t n = pts.size();
    std::vector<double> r(n);
    double ssr = 0.0;
    for (size_t k = 0; k < n; k++) {
      r[k] = m.Residual(pts[k]);
      ssr += r[k] * r[k];
    }
    const int params = m.arc ? 3 : 2;
    const double dof = double(std::max<int>(1, int(n) - params));
    const double s2 = std::max(ssr / dof, cx_.floor * cx_.floor * 1e-4);
    f.sigmaMm = std::sqrt(ssr / double(std::max<size_t>(n, 1)));
    const double rho = LagOne(r);
    f.inflation = (1.0 + rho) / (1.0 - rho);
    size_t at = 0;
    f.stretchMm = WorstStretch(r, T::kStretchPoints, &at);
    if (!m.arc) {
      const glm::dvec2 c = Centroid(pts);
      double stt = 0.0;
      for (const glm::dvec2& p : pts) {
        const double t = glm::dot(p - c, m.dir);
        stt += t * t;
      }
      f.offsetSigmaMm = std::sqrt(s2 * f.inflation / double(n));
      f.angleSigma = stt > 0.0 ? std::sqrt(s2 * f.inflation / stt) : kInf;
      return;
    }
    double A[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    for (const glm::dvec2& p : pts) {
      const glm::dvec2 q = p - m.centre;
      const double d = glm::length(q);
      if (!(d > 0.0))
        continue;
      const double J[3] = {-q.x / d, -q.y / d, -1.0};
      for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
          A[i][j] += J[i] * J[j];
    }
    double inv[3][3];
    if (!Invert3(A, inv)) {
      f.centreSigmaMm = glm::dvec2(kInf);
      f.radiusSigmaMm = kInf;
      return;
    }
    const double k = s2 * f.inflation;
    f.centreSigmaMm = {std::sqrt(std::max(0.0, k * inv[0][0])),
                       std::sqrt(std::max(0.0, k * inv[1][1]))};
    f.radiusSigmaMm = std::sqrt(std::max(0.0, k * inv[2][2]));
  }

  void Emit(int chainIndex, PiecesFit& out) {
    const size_t np = PieceCount();
    if (np == 0)
      return;
    // Where each piece starts: its meeting with the one before, or the
    // chain's own end.
    std::vector<glm::dvec2> meet(br_.size());
    std::vector<glm::dvec2> pa, pb;
    for (size_t k = 0; k < br_.size(); k++) {
      const bool end = !cv_.closed && (k == 0 || k + 1 == br_.size());
      if (end || np < 2) {
        meet[k] = cv_.P[br_[k]];
        continue;
      }
      const size_t ia = (k + np - 1) % np, ib = k % np;
      cv_.Gather(Start(ia), End(ia), pa);
      cv_.Gather(Start(ib), End(ib), pb);
      meet[k] = Meeting(pieces_[ia].model, pieces_[ib].model, cv_.P[br_[k]],
                        std::min(PathLength(pa), PathLength(pb)));
    }
    for (size_t i = 0; i < np; i++) {
      cv_.Gather(Start(i), End(i), buf_);
      Piece& p = pieces_[i];
      if (!p.model.ok) {
        p.arc = false;
        p.model = FitLine(buf_);
      }
      const Model& m = p.model;
      PieceFit f;
      f.chain = chainIndex;
      f.from = static_cast<uint32_t>(Start(i));
      f.to = static_cast<uint32_t>(End(i));
      f.points = static_cast<uint32_t>(buf_.size());
      f.arc = m.arc;
      Uncertainty(m, buf_, f);
      const glm::dvec2 s = m.ok ? m.Project(meet[i]) : buf_.front();
      const glm::dvec2 e =
          m.ok ? m.Project(meet[(i + 1) % br_.size()]) : buf_.back();
      for (const glm::dvec2& q : buf_)
        out.deviation = std::max(out.deviation, std::abs(m.Residual(q)));
      if (!m.arc || !m.ok) {
        out.entities.push_back(Line{s, e});
        out.ends.push_back({0, 1});
      } else if (cv_.closed && np == 1) {
        out.entities.push_back(Circle{m.centre, m.radius});
        out.ends.push_back({0, 0});
      } else {
        // The way round the chain goes, from its points' turning.
        double sweep = 0.0;
        for (size_t k = 1; k < buf_.size(); k++) {
          const glm::dvec2 q0 = buf_[k - 1] - m.centre, q1 = buf_[k] - m.centre;
          sweep += std::atan2(Cross(q0, q1), glm::dot(q0, q1));
        }
        const auto ang = [&](const glm::dvec2& q) {
          return std::atan2(q.y - m.centre.y, q.x - m.centre.x);
        };
        const auto wrap = [](double a) {
          a = std::fmod(a, angles::kTwoPi);
          return a <= 0.0 ? a + angles::kTwoPi : a;
        };
        const double as = ang(s), ae = ang(e);
        if (sweep > 0.0) {
          out.entities.push_back(
              Arc{m.centre, m.radius, as, as + wrap(ae - as)});
          out.ends.push_back({1, 2});
        } else {
          out.entities.push_back(
              Arc{m.centre, m.radius, ae, ae + wrap(as - ae)});
          out.ends.push_back({2, 1});
        }
      }
      out.pieces.push_back(f);
    }
  }
};

} // namespace

ChainNoise MeasureNoise(const CutChain& chain, double floorSigma) {
  // Measured about the chain's own pieces, as the fit measures it.
  PiecesFitter f(chain, floorSigma, 0.5);
  return f.Run(0).noise;
}

namespace detail {

// The chain with no step many times longer than its median step: an exact cut's
// straight edge is its two ends, and a fit weighs points -- the edge
// would count for two against a round's dozens. `original` maps each point
// back to the chain's own index (a filled-in point to its step's start).
CutChain Evened(const CutChain& c, std::vector<uint32_t>& original) {
  CutChain out;
  out.closed = c.closed;
  const size_t n = c.points.size();
  const size_t steps = c.closed ? n : n - 1;
  std::vector<double> len(steps);
  for (size_t k = 0; k < steps; k++)
    len[k] = glm::length(c.points[(k + 1) % n] - c.points[k]);
  const double spacing = steps ? Median(len) : 0.0;
  // At most ten times the points: a chain sampled unevenly on purpose is
  // left as it is beyond that.
  const size_t cap = 10 * n;
  for (size_t k = 0; k < n; k++) {
    out.points.push_back(c.points[k]);
    original.push_back(static_cast<uint32_t>(k));
    // Only a step many times the median: a noisy scan's zigzag is its own.
    if (k >= steps || !(spacing > 0.0) || len[k] < T::kEvenSteps * spacing)
      continue;
    const int parts = static_cast<int>(
        std::min<double>(double(cap), std::floor(len[k] / spacing)));
    for (int q = 1; q < parts && out.points.size() < cap; q++) {
      out.points.push_back(c.points[k] + (c.points[(k + 1) % n] - c.points[k]) *
                                             (double(q) / parts));
      original.push_back(static_cast<uint32_t>(k));
    }
  }
  return out;
}

} // namespace detail

CutChain EvenedChain(const CutChain& chain) {
  std::vector<uint32_t> original;
  return chain.points.size() >= 2 ? detail::Evened(chain, original) : chain;
}

PiecesFit FitPieces(const CutChain& chain, int chainIndex, double tolerance,
                    double detail) {
  std::vector<uint32_t> original;
  const CutChain even =
      chain.points.size() >= 2 ? detail::Evened(chain, original) : chain;
  PiecesFitter f(even, std::max(tolerance, 0.0) * T::kNoiseFloorShare, detail);
  PiecesFit out = f.Run(chainIndex);
  if (!original.empty())
    for (PieceFit& p : out.pieces) {
      p.from = original[p.from];
      // A filled-in end counts to the step's far end.
      const uint32_t to = p.to;
      p.to = original[to];
      if (to + 1 < original.size() ? original[to + 1] == original[to] &&
                                         even.points[to] != chain.points[p.to]
                                   : false)
        p.to = static_cast<uint32_t>((p.to + 1) % chain.points.size());
    }
  return out;
}

SpanMiss MeasureMiss(const Entity& entity, const std::vector<glm::dvec2>& pts,
                     const ChainNoise& noise, double detail) {
  SpanMiss out;
  Model m;
  if (const auto* l = std::get_if<Line>(&entity)) {
    const glm::dvec2 d = l->b - l->a;
    if (!(glm::length(d) > 0.0))
      return out;
    m.dir = d / glm::length(d);
    m.p0 = l->a;
    m.ok = true;
  } else if (const auto* a = std::get_if<Arc>(&entity)) {
    m.arc = true;
    m.centre = a->center;
    m.radius = a->radius;
    m.ok = true;
  } else if (const auto* c = std::get_if<Circle>(&entity)) {
    m.arc = true;
    m.centre = c->center;
    m.radius = c->radius;
    m.ok = true;
  } else {
    return out;
  }
  out.points = pts.size();
  if (pts.empty())
    return out;
  std::vector<double> r(pts.size());
  double ss = 0.0, sum = 0.0;
  for (size_t k = 0; k < pts.size(); k++) {
    r[k] = m.Residual(pts[k]);
    ss += r[k] * r[k];
    sum += r[k];
  }
  out.sigmaMm = std::sqrt(ss / double(pts.size()));
  out.meanMm = sum / double(pts.size());
  size_t at = 0;
  out.stretchMm = WorstStretch(r, T::kStretchPoints, &at);
  out.stretchAt = pts[std::min(at, pts.size() - 1)];
  Context cx;
  cx.sigma = noise.sigma;
  cx.rho = noise.correlation;
  cx.s = std::pow(T::kDetailRange, 1.0 - 2.0 * std::clamp(detail, 0.0, 1.0));
  out.explained = Judge(m, pts, cx).explains;
  return out;
}

} // namespace sketch
