// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/mesh/Fit.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace mesh {

const char* KindName(Kind k) {
  switch (k) {
  case Kind::Plane:
    return "plane";
  case Kind::Cylinder:
    return "cylinder";
  case Kind::Cone:
    return "cone";
  case Kind::Sphere:
    return "sphere";
  }
  return "plane";
}

double FitResult::Distance(const glm::dvec3& p) const {
  switch (kind) {
  case Kind::Plane:
    return glm::dot(p - origin, axis);
  case Kind::Cylinder: {
    const glm::dvec3 w = p - origin;
    return glm::length(w - glm::dot(w, axis) * axis) - radius;
  }
  case Kind::Cone: {
    const glm::dvec3 w = p - origin;
    const double h = glm::dot(w, axis);
    const double rho = glm::length(w - h * axis);
    return rho * std::cos(halfAngle) - h * std::sin(halfAngle);
  }
  case Kind::Sphere:
    return glm::length(p - origin) - radius;
  }
  return 0.0;
}

glm::dvec3 FitResult::Project(const glm::dvec3& p) const {
  switch (kind) {
  case Kind::Plane:
    return p - glm::dot(p - origin, axis) * axis;
  case Kind::Cylinder: {
    const glm::dvec3 w = p - origin;
    const double h = glm::dot(w, axis);
    const glm::dvec3 r = w - h * axis;
    const double len = glm::length(r);
    return len > 0.0 ? origin + h * axis + r * (radius / len) : p;
  }
  case Kind::Cone: {
    const glm::dvec3 w = p - origin;
    const double h = glm::dot(w, axis);
    const glm::dvec3 r = w - h * axis;
    const double len = glm::length(r);
    if (len == 0.0)
      return p; // on the axis: every generator is as near
    const glm::dvec3 g =
        std::cos(halfAngle) * axis + std::sin(halfAngle) * (r / len);
    return origin + std::max(0.0, glm::dot(w, g)) * g;
  }
  case Kind::Sphere: {
    const glm::dvec3 w = p - origin;
    const double len = glm::length(w);
    return len > 0.0 ? origin + w * (radius / len) : p;
  }
  }
  return p;
}

glm::dvec3 FitResult::Normal(const glm::dvec3& p) const {
  switch (kind) {
  case Kind::Plane:
    return axis;
  case Kind::Sphere: {
    const glm::dvec3 w = p - origin;
    const double l = glm::length(w);
    return l > 0.0 ? w / l : glm::dvec3(0.0);
  }
  case Kind::Cylinder:
  case Kind::Cone: {
    const glm::dvec3 w = p - origin;
    const glm::dvec3 r = w - glm::dot(w, axis) * axis;
    const double l = glm::length(r);
    if (!(l > 0.0))
      return glm::dvec3(0.0);
    if (kind == Kind::Cylinder)
      return r / l;
    return std::cos(halfAngle) * (r / l) - std::sin(halfAngle) * axis;
  }
  }
  return glm::dvec3(0.0);
}

namespace {

// Two unit vectors square to `a` and to each other.
void Basis(const glm::dvec3& a, glm::dvec3& e1, glm::dvec3& e2) {
  const glm::dvec3 helper = std::abs(a.x) < 0.6   ? glm::dvec3(1, 0, 0)
                            : std::abs(a.y) < 0.6 ? glm::dvec3(0, 1, 0)
                                                  : glm::dvec3(0, 0, 1);
  e1 = glm::normalize(glm::cross(a, helper));
  e2 = glm::cross(a, e1);
}

// Solves the n x n system `a x = b` in place (row-major a), by elimination
// with partial pivoting. False when it is singular.
bool Solve(std::vector<double>& a, std::vector<double>& b, size_t n) {
  for (size_t col = 0; col < n; col++) {
    size_t pivot = col;
    for (size_t r = col + 1; r < n; r++)
      if (std::abs(a[r * n + col]) > std::abs(a[pivot * n + col]))
        pivot = r;
    if (std::abs(a[pivot * n + col]) < 1e-300)
      return false;
    if (pivot != col) {
      for (size_t c = 0; c < n; c++)
        std::swap(a[col * n + c], a[pivot * n + c]);
      std::swap(b[col], b[pivot]);
    }
    for (size_t r = col + 1; r < n; r++) {
      const double f = a[r * n + col] / a[col * n + col];
      if (f == 0.0)
        continue;
      for (size_t c = col; c < n; c++)
        a[r * n + c] -= f * a[col * n + c];
      b[r] -= f * b[col];
    }
  }
  for (size_t i = n; i-- > 0;) {
    double s = b[i];
    for (size_t c = i + 1; c < n; c++)
      s -= a[i * n + c] * b[c];
    b[i] = s / a[i * n + i];
  }
  return true;
}

// Levenberg-Marquardt over `n` parameters that perturb a model from where it
// stands: `residuals(p, r)` evaluates the model moved by `p`, and
// `commit(p)` moves it there, after which the parameters are zero again.
// `steps[j]` is the finite-difference step of parameter j.
template <typename Residuals, typename Commit>
void Refine(size_t n, const std::vector<double>& steps, Residuals residuals,
            Commit commit, int iterations = 40) {
  std::vector<double> zero(n, 0.0), p(n), r0, r1;
  residuals(zero.data(), r0);
  double cost = 0.0;
  for (double v : r0)
    cost += v * v;
  double lambda = 1e-3;
  std::vector<std::vector<double>> jac(n);
  for (int it = 0; it < iterations; it++) {
    for (size_t j = 0; j < n; j++) {
      std::fill(p.begin(), p.end(), 0.0);
      p[j] = steps[j];
      residuals(p.data(), r1);
      jac[j].resize(r0.size());
      for (size_t i = 0; i < r0.size(); i++)
        jac[j][i] = (r1[i] - r0[i]) / steps[j];
    }
    std::vector<double> jtj(n * n, 0.0), jtr(n, 0.0);
    for (size_t a = 0; a < n; a++) {
      for (size_t b = a; b < n; b++) {
        double s = 0.0;
        for (size_t i = 0; i < r0.size(); i++)
          s += jac[a][i] * jac[b][i];
        jtj[a * n + b] = jtj[b * n + a] = s;
      }
      double s = 0.0;
      for (size_t i = 0; i < r0.size(); i++)
        s += jac[a][i] * r0[i];
      jtr[a] = -s;
    }
    bool improved = false;
    for (int tries = 0; tries < 8 && !improved; tries++) {
      std::vector<double> m = jtj, d = jtr;
      for (size_t a = 0; a < n; a++)
        m[a * n + a] += lambda * std::max(jtj[a * n + a], 1e-30);
      if (!Solve(m, d, n)) {
        lambda *= 10.0;
        continue;
      }
      residuals(d.data(), r1);
      double c1 = 0.0;
      for (double v : r1)
        c1 += v * v;
      if (std::isfinite(c1) && c1 < cost) {
        commit(d.data());
        const double gain = cost - c1;
        r0.swap(r1);
        cost = c1;
        lambda = std::max(lambda * 0.1, 1e-12);
        improved = true;
        if (gain <= 1e-14 * cost)
          return;
      } else {
        lambda *= 10.0;
      }
    }
    if (!improved)
      return;
  }
}

glm::dvec3 Centroid(const std::vector<glm::dvec3>& pts) {
  glm::dvec3 c(0.0);
  for (const glm::dvec3& p : pts)
    c += p;
  return pts.empty() ? c : c / static_cast<double>(pts.size());
}

double Scale(const std::vector<glm::dvec3>& pts, const glm::dvec3& c) {
  double s = 0.0;
  for (const glm::dvec3& p : pts)
    s += glm::dot(p - c, p - c);
  return pts.empty() ? 1.0 : std::max(1e-12, std::sqrt(s / double(pts.size())));
}

void Residuals(FitResult& f, const std::vector<glm::dvec3>& pts) {
  double sum = 0.0, worst = 0.0;
  for (const glm::dvec3& p : pts) {
    const double d = std::abs(f.Distance(p));
    sum += d * d;
    worst = std::max(worst, d);
  }
  f.rms = pts.empty() ? 0.0 : std::sqrt(sum / double(pts.size()));
  f.max = worst;
  f.inliers = pts.size();
  f.ok = std::isfinite(f.rms) && std::isfinite(f.max);
}

// The algebraic (Kasa) circle through 2-D points: centre and radius.
bool Circle2(const std::vector<glm::dvec2>& pts, glm::dvec2& centre,
             double& radius) {
  // x^2 + y^2 + D x + E y + F = 0, least squares.
  std::vector<double> a(9, 0.0), b(3, 0.0);
  for (const glm::dvec2& p : pts) {
    const double row[3] = {p.x, p.y, 1.0};
    const double rhs = -(p.x * p.x + p.y * p.y);
    for (int i = 0; i < 3; i++) {
      for (int j = 0; j < 3; j++)
        a[i * 3 + j] += row[i] * row[j];
      b[i] += row[i] * rhs;
    }
  }
  if (!Solve(a, b, 3))
    return false;
  centre = {-b[0] / 2.0, -b[1] / 2.0};
  const double r2 = glm::dot(centre, centre) - b[2];
  if (!(r2 > 0.0))
    return false;
  radius = std::sqrt(r2);
  return true;
}

} // namespace

void SymmetricEigen3(const glm::dmat3& m, glm::dvec3& values,
                     glm::dmat3& vectors) {
  double a[3][3];
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      a[i][j] = m[j][i];
  double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (int sweep = 0; sweep < 50; sweep++) {
    const double off =
        a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
    if (off < 1e-300)
      break;
    for (int p = 0; p < 2; p++)
      for (int q = p + 1; q < 3; q++) {
        if (a[p][q] == 0.0)
          continue;
        const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
        const double t = (theta >= 0 ? 1.0 : -1.0) /
                         (std::abs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
        for (int k = 0; k < 3; k++) {
          const double akp = a[k][p], akq = a[k][q];
          a[k][p] = c * akp - s * akq;
          a[k][q] = s * akp + c * akq;
        }
        for (int k = 0; k < 3; k++) {
          const double apk = a[p][k], aqk = a[q][k];
          a[p][k] = c * apk - s * aqk;
          a[q][k] = s * apk + c * aqk;
        }
        for (int k = 0; k < 3; k++) {
          const double vkp = v[k][p], vkq = v[k][q];
          v[k][p] = c * vkp - s * vkq;
          v[k][q] = s * vkp + c * vkq;
        }
      }
  }
  int order[3] = {0, 1, 2};
  std::sort(order, order + 3, [&](int x, int y) { return a[x][x] < a[y][y]; });
  for (int i = 0; i < 3; i++) {
    values[i] = a[order[i]][order[i]];
    vectors[i] = glm::dvec3(v[0][order[i]], v[1][order[i]], v[2][order[i]]);
  }
}

glm::dvec3 AxisFromNormals(const std::vector<glm::dvec3>& normals,
                           const std::vector<double>& weights,
                           bool throughCentre, double& offset, double& spread) {
  double total = 0.0;
  glm::dvec3 mean(0.0);
  for (size_t i = 0; i < normals.size(); i++) {
    total += weights[i];
    mean += weights[i] * normals[i];
  }
  if (total <= 0.0) {
    offset = spread = 0.0;
    return {0, 0, 1};
  }
  mean /= total;
  if (throughCentre)
    mean = glm::dvec3(0.0);
  glm::dmat3 cov(0.0);
  for (size_t i = 0; i < normals.size(); i++) {
    const glm::dvec3 d = normals[i] - mean;
    cov += weights[i] * glm::outerProduct(d, d);
  }
  glm::dvec3 values;
  glm::dmat3 vectors;
  SymmetricEigen3(cov, values, vectors);
  const glm::dvec3 axis = vectors[0];
  offset = glm::dot(mean, axis);
  spread = std::sqrt(std::max(0.0, values[0]) / total);
  return axis;
}

FitResult FitPlane(const std::vector<glm::dvec3>& points) {
  FitResult f;
  f.kind = Kind::Plane;
  if (points.size() < 3)
    return f;
  const glm::dvec3 c = Centroid(points);
  glm::dmat3 cov(0.0);
  for (const glm::dvec3& p : points)
    cov += glm::outerProduct(p - c, p - c);
  glm::dvec3 values;
  glm::dmat3 vectors;
  SymmetricEigen3(cov, values, vectors);
  if (!(values[1] > 0.0))
    return f; // collinear
  f.origin = c;
  f.axis = vectors[0];
  Residuals(f, points);
  return f;
}

FitResult FitCylinder(const std::vector<glm::dvec3>& points,
                      const glm::dvec3& axisGuess, double giveUpAbove) {
  FitResult f;
  f.kind = Kind::Cylinder;
  if (points.size() < 5 || glm::length(axisGuess) == 0.0)
    return f;
  const glm::dvec3 c0 = Centroid(points);
  const double scale = Scale(points, c0);
  std::vector<glm::dvec3> pts(points.size());
  for (size_t i = 0; i < points.size(); i++)
    pts[i] = (points[i] - c0) / scale;
  glm::dvec3 a = glm::normalize(axisGuess), e1, e2;
  Basis(a, e1, e2);
  std::vector<glm::dvec2> flat(pts.size());
  for (size_t i = 0; i < pts.size(); i++)
    flat[i] = {glm::dot(pts[i], e1), glm::dot(pts[i], e2)};
  glm::dvec2 centre;
  double radius = 0.0;
  if (!Circle2(flat, centre, radius))
    return f;
  glm::dvec3 c = centre.x * e1 + centre.y * e2;
  if (giveUpAbove > 0.0) {
    for (const glm::dvec2& q : flat)
      if (std::abs(glm::length(q - centre) - radius) * scale > giveUpAbove)
        return f;
  }
  // Parameters: the axis tipped by (u, v), its point moved by (s, t) square
  // to it, the radius grown by dr.
  const auto model = [&](const double* p, glm::dvec3& ap, glm::dvec3& cp,
                         double& rp) {
    ap = glm::normalize(a + p[0] * e1 + p[1] * e2);
    cp = c + p[2] * e1 + p[3] * e2;
    rp = radius + p[4];
  };
  const auto residuals = [&](const double* p, std::vector<double>& r) {
    glm::dvec3 ap, cp;
    double rp;
    model(p, ap, cp, rp);
    r.resize(pts.size());
    for (size_t i = 0; i < pts.size(); i++) {
      const glm::dvec3 w = pts[i] - cp;
      r[i] = glm::length(w - glm::dot(w, ap) * ap) - rp;
    }
  };
  const auto commit = [&](const double* p) {
    glm::dvec3 ap, cp;
    double rp;
    model(p, ap, cp, rp);
    a = ap;
    c = cp - glm::dot(cp, ap) * ap; // keep the point near the centroid
    radius = rp;
    Basis(a, e1, e2);
  };
  Refine(5, {1e-7, 1e-7, 1e-7, 1e-7, 1e-7}, residuals, commit);
  f.axis = a;
  f.origin = c0 + c * scale;
  f.radius = radius * scale;
  Residuals(f, points);
  f.ok = f.ok && f.radius > 0.0;
  return f;
}

FitResult FitCone(const std::vector<glm::dvec3>& points,
                  const std::vector<glm::dvec3>& normals,
                  const std::vector<glm::dvec3>& onPlane,
                  const glm::dvec3& axisGuess, double giveUpAbove) {
  FitResult f;
  f.kind = Kind::Cone;
  if (points.size() < 6 || normals.size() < 3 || glm::length(axisGuess) == 0.0)
    return f;
  const glm::dvec3 c0 = Centroid(points);
  const double scale = Scale(points, c0);
  std::vector<glm::dvec3> pts(points.size());
  for (size_t i = 0; i < points.size(); i++)
    pts[i] = (points[i] - c0) / scale;
  // The apex is on every facet plane: least squares over them.
  std::vector<double> m(9, 0.0), b(3, 0.0);
  for (size_t i = 0; i < normals.size(); i++) {
    const glm::dvec3 n = normals[i];
    const double d = glm::dot(n, (onPlane[i] - c0) / scale);
    for (int r = 0; r < 3; r++) {
      for (int k = 0; k < 3; k++)
        m[r * 3 + k] += n[r] * n[k];
      b[r] += n[r] * d;
    }
  }
  if (!Solve(m, b, 3))
    return f;
  glm::dvec3 v(b[0], b[1], b[2]);
  glm::dvec3 a = glm::normalize(axisGuess);
  double along = 0.0;
  for (const glm::dvec3& p : pts)
    along += glm::dot(p - v, a);
  if (along < 0.0)
    a = -a;
  double beta = 0.0;
  for (const glm::dvec3& p : pts) {
    const glm::dvec3 w = p - v;
    const double h = glm::dot(w, a);
    beta += std::atan2(glm::length(w - h * a), h);
  }
  beta /= double(pts.size());
  if (giveUpAbove > 0.0) {
    const double cb = std::cos(beta), sb = std::sin(beta);
    for (const glm::dvec3& p : pts) {
      const glm::dvec3 w = p - v;
      const double h = glm::dot(w, a);
      if (std::abs(glm::length(w - h * a) * cb - h * sb) * scale > giveUpAbove)
        return f;
    }
  }
  glm::dvec3 e1, e2;
  Basis(a, e1, e2);
  const auto model = [&](const double* p, glm::dvec3& ap, glm::dvec3& vp,
                         double& bp) {
    ap = glm::normalize(a + p[0] * e1 + p[1] * e2);
    vp = v + glm::dvec3(p[2], p[3], p[4]);
    bp = beta + p[5];
  };
  const auto residuals = [&](const double* p, std::vector<double>& r) {
    glm::dvec3 ap, vp;
    double bp;
    model(p, ap, vp, bp);
    r.resize(pts.size());
    const double cb = std::cos(bp), sb = std::sin(bp);
    for (size_t i = 0; i < pts.size(); i++) {
      const glm::dvec3 w = pts[i] - vp;
      const double h = glm::dot(w, ap);
      r[i] = glm::length(w - h * ap) * cb - h * sb;
    }
  };
  const auto commit = [&](const double* p) {
    model(p, a, v, beta);
    Basis(a, e1, e2);
  };
  Refine(6, {1e-7, 1e-7, 1e-7, 1e-7, 1e-7, 1e-7}, residuals, commit);
  f.axis = a;
  f.origin = c0 + v * scale;
  f.halfAngle = beta;
  Residuals(f, points);
  f.ok = f.ok && beta > 0.0 && beta < M_PI / 2.0;
  return f;
}

FitResult FitSphere(const std::vector<glm::dvec3>& points, double giveUpAbove) {
  FitResult f;
  f.kind = Kind::Sphere;
  if (points.size() < 5)
    return f;
  const glm::dvec3 c0 = Centroid(points);
  const double scale = Scale(points, c0);
  std::vector<glm::dvec3> pts(points.size());
  for (size_t i = 0; i < points.size(); i++)
    pts[i] = (points[i] - c0) / scale;
  // x^2+y^2+z^2 = 2 c.p + k, least squares.
  std::vector<double> m(16, 0.0), b(4, 0.0);
  for (const glm::dvec3& p : pts) {
    const double row[4] = {2 * p.x, 2 * p.y, 2 * p.z, 1.0};
    const double rhs = glm::dot(p, p);
    for (int r = 0; r < 4; r++) {
      for (int k = 0; k < 4; k++)
        m[r * 4 + k] += row[r] * row[k];
      b[r] += row[r] * rhs;
    }
  }
  if (!Solve(m, b, 4))
    return f;
  glm::dvec3 c(b[0], b[1], b[2]);
  const double r2 = b[3] + glm::dot(c, c);
  if (!(r2 > 0.0))
    return f;
  double radius = std::sqrt(r2);
  if (giveUpAbove > 0.0) {
    for (const glm::dvec3& p : pts)
      if (std::abs(glm::length(p - c) - radius) * scale > giveUpAbove)
        return f;
  }
  const auto residuals = [&](const double* p, std::vector<double>& r) {
    const glm::dvec3 cp = c + glm::dvec3(p[0], p[1], p[2]);
    r.resize(pts.size());
    for (size_t i = 0; i < pts.size(); i++)
      r[i] = glm::length(pts[i] - cp) - (radius + p[3]);
  };
  const auto commit = [&](const double* p) {
    c += glm::dvec3(p[0], p[1], p[2]);
    radius += p[3];
  };
  Refine(4, {1e-7, 1e-7, 1e-7, 1e-7}, residuals, commit);
  f.origin = c0 + c * scale;
  f.radius = radius * scale;
  Residuals(f, points);
  f.ok = f.ok && f.radius > 0.0;
  return f;
}

namespace {

// splitmix64: a generator whose sequence is the same on every platform,
// unlike std::uniform_int_distribution's.
struct Rng {
  uint64_t state;
  uint64_t Next() {
    uint64_t x = (state += 0x9e3779b97f4a7c15ull);
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
  }
  size_t Below(size_t n) { return static_cast<size_t>(Next() % n); }
};

// FNV-1a over the points' bits: the seed, so the input decides the samples.
uint64_t SeedOf(Kind kind, const std::vector<glm::dvec3>& points) {
  uint64_t h = 0xcbf29ce484222325ull ^ static_cast<uint64_t>(kind);
  for (const glm::dvec3& p : points)
    for (int i = 0; i < 3; i++) {
      uint64_t bits = 0;
      std::memcpy(&bits, &p[i], sizeof bits);
      h = (h ^ bits) * 0x100000001b3ull;
    }
  return h;
}

// How many points a hypothesis is drawn from.
int SampleSize(Kind k) {
  switch (k) {
  case Kind::Plane:
  case Kind::Cone:
    return 3;
  case Kind::Cylinder:
  case Kind::Sphere:
    return 2;
  }
  return 3;
}

// The surface through the oriented points `p[i]`, `n[i]` (unit), or a
// failed result when they do not define one.
FitResult Hypothesis(Kind kind, const glm::dvec3* p, const glm::dvec3* n) {
  FitResult f;
  f.kind = kind;
  switch (kind) {
  case Kind::Plane: {
    const glm::dvec3 c = glm::cross(p[1] - p[0], p[2] - p[0]);
    const double len = glm::length(c);
    const double span =
        std::max(glm::length(p[1] - p[0]), glm::length(p[2] - p[0]));
    if (!(len > 1e-9 * span * span))
      return f;
    f.origin = (p[0] + p[1] + p[2]) / 3.0;
    f.axis = c / len;
    f.ok = true;
    return f;
  }
  case Kind::Sphere: {
    // The centre is where the two normal lines come closest.
    const glm::dvec3 w = p[0] - p[1];
    const double b = glm::dot(n[0], n[1]), d = glm::dot(n[0], w),
                 e = glm::dot(n[1], w), den = 1.0 - b * b;
    if (!(den > 1e-9))
      return f;
    const double s = (b * e - d) / den, t = (e - b * d) / den;
    const glm::dvec3 c = 0.5 * ((p[0] + s * n[0]) + (p[1] + t * n[1]));
    f.origin = c;
    f.radius = 0.5 * (glm::length(p[0] - c) + glm::length(p[1] - c));
    f.ok = f.radius > 0.0;
    return f;
  }
  case Kind::Cylinder: {
    // Both normals are square to the axis; in the plane square to it the
    // two normal lines meet on the axis.
    glm::dvec3 a = glm::cross(n[0], n[1]);
    const double len = glm::length(a);
    if (!(len > 1e-6))
      return f;
    a /= len;
    const auto flat = [&](const glm::dvec3& v) {
      return v - glm::dot(v, a) * a;
    };
    const glm::dvec3 q0 = flat(p[0]), q1 = flat(p[1]);
    const glm::dvec3 m0 = glm::normalize(flat(n[0])),
                     m1 = glm::normalize(flat(n[1]));
    const glm::dvec3 w = q0 - q1;
    const double b = glm::dot(m0, m1), d = glm::dot(m0, w), e = glm::dot(m1, w),
                 den = 1.0 - b * b;
    if (!(den > 1e-9))
      return f;
    const double s = (b * e - d) / den, t = (e - b * d) / den;
    const glm::dvec3 c = 0.5 * ((q0 + s * m0) + (q1 + t * m1));
    f.origin = c;
    f.axis = a;
    f.radius = 0.5 * (glm::length(q0 - c) + glm::length(q1 - c));
    f.ok = f.radius > 0.0;
    return f;
  }
  case Kind::Cone: {
    // The apex is on all three tangent planes; the unit directions from it
    // to the points lie on a circle about the axis.
    std::vector<double> m(9), b(3);
    for (int i = 0; i < 3; i++) {
      for (int k = 0; k < 3; k++)
        m[i * 3 + k] = n[i][k];
      b[i] = glm::dot(n[i], p[i]);
    }
    if (!Solve(m, b, 3))
      return f;
    const glm::dvec3 apex(b[0], b[1], b[2]);
    glm::dvec3 u[3];
    for (int i = 0; i < 3; i++) {
      const glm::dvec3 d = p[i] - apex;
      const double l = glm::length(d);
      if (!(l > 0.0))
        return f;
      u[i] = d / l;
    }
    glm::dvec3 a = glm::cross(u[1] - u[0], u[2] - u[0]);
    const double len = glm::length(a);
    if (!(len > 1e-9))
      return f;
    a /= len;
    if (glm::dot(a, u[0] + u[1] + u[2]) < 0.0)
      a = -a;
    double beta = 0.0;
    for (const glm::dvec3& v : u)
      beta += std::acos(std::clamp(glm::dot(a, v), -1.0, 1.0));
    beta /= 3.0;
    if (!(beta > 0.0 && beta < M_PI / 2.0))
      return f;
    f.origin = apex;
    f.axis = a;
    f.halfAngle = beta;
    f.ok =
        std::isfinite(apex.x) && std::isfinite(apex.y) && std::isfinite(apex.z);
    return f;
  }
  }
  return f;
}

// The least-squares fit of `kind` over `pts`, started from `start`.
FitResult Refit(Kind kind, const FitResult& start,
                const std::vector<glm::dvec3>& pts,
                const std::vector<glm::dvec3>& at,
                const std::vector<glm::dvec3>& nrm) {
  switch (kind) {
  case Kind::Plane: {
    FitResult f = FitPlane(pts);
    if (f.ok && glm::dot(f.axis, start.axis) < 0.0)
      f.axis = -f.axis;
    return f;
  }
  case Kind::Cylinder:
    return FitCylinder(pts, start.axis);
  case Kind::Cone:
    return nrm.size() < 3 ? FitResult{} : FitCone(pts, nrm, at, start.axis);
  case Kind::Sphere:
    return FitSphere(pts);
  }
  return {};
}

} // namespace

FitResult FitRobust(Kind kind, const std::vector<glm::dvec3>& points,
                    const std::vector<glm::dvec3>& facetCentres,
                    const std::vector<glm::dvec3>& facetNormals,
                    double toleranceMm, const glm::dvec3* through) {
  FitResult none;
  none.kind = kind;
  const size_t n = points.size(), nf = facetCentres.size();
  const int s = SampleSize(kind);
  const bool oriented = !facetNormals.empty();
  // A plane draws its hypotheses from the points (three on one face of a
  // coarse mesh exist where three triangle centres may not); the curved
  // kinds from the oriented samples.
  const bool fromPoints = kind == Kind::Plane;
  if ((oriented ? facetNormals.size() != nf : kind != Kind::Plane) ||
      n < static_cast<size_t>(std::max(s, 6)) || !(toleranceMm > 0.0) ||
      (oriented && nf < static_cast<size_t>(s)))
    return none;
  Rng rng{SeedOf(kind, points) * 0x9e3779b97f4a7c15ull +
          SeedOf(kind, facetCentres)};
  // Hypotheses are scored on a fixed sample of the points: past a few
  // thousand the inlier share is known as well as it needs to be.
  constexpr size_t kScored = 4096;
  const auto strided = [&](size_t count) {
    std::vector<size_t> out;
    const size_t take = std::min(count, kScored);
    out.reserve(take);
    for (size_t i = 0; i < take; i++)
      out.push_back(take == count ? i : i * count / take);
    return out;
  };
  const std::vector<size_t> scored = strided(n);
  const std::vector<size_t> scoredFacets =
      oriented ? strided(nf) : std::vector<size_t>{};
  const auto passes = [&](const FitResult& f) {
    return through == nullptr || std::abs(f.Distance(*through)) <= toleranceMm;
  };
  // MSAC: each point costs its squared distance, and an outlier the
  // tolerance's square, so of two hypotheses keeping as many points the
  // tighter wins -- one tilted towards a lump keeps a few more of its foot
  // and fits the rest worse. And how well the oriented samples on the
  // surface agree with its normal: what tells two of equal cost apart -- a
  // box's face from the diagonal plane through the same corners.
  const double tol2 = toleranceMm * toleranceMm;
  const auto costOf = [&](const FitResult& f, const std::vector<size_t>* which,
                          size_t& in) {
    double c = 0.0;
    in = 0;
    const size_t count = which ? which->size() : n;
    for (size_t k = 0; k < count; k++) {
      const double d = f.Distance(points[which ? (*which)[k] : k]);
      if (std::abs(d) <= toleranceMm) {
        in++;
        c += d * d;
      } else {
        c += tol2;
      }
    }
    return c;
  };
  const auto score = [&](const FitResult& f, double& agree, size_t& in) {
    const double cost = costOf(f, &scored, in);
    agree = 0.0;
    for (size_t i : scoredFacets)
      if (std::abs(f.Distance(facetCentres[i])) <= toleranceMm)
        agree += std::abs(glm::dot(facetNormals[i], f.Normal(facetCentres[i])));
    return cost;
  };
  // Enough draws that a sample of inliers only is drawn with 99.9%
  // certainty at the best inlier share seen so far.
  constexpr int kLeast = 64, kMost = 4000;
  FitResult best;
  size_t bestIn = 0;
  double bestCost = std::numeric_limits<double>::max(), bestAgree = 0.0;
  int needed = kMost;
  const size_t pool = fromPoints ? n : nf;
  for (int it = 0; it < needed && it < kMost; it++) {
    size_t pick[3] = {0, 0, 0};
    for (int k = 0; k < s; k++) {
      pick[k] = rng.Below(pool);
      for (int j = 0; j < k; j++)
        if (pick[j] == pick[k])
          pick[k] = rng.Below(pool);
    }
    glm::dvec3 p[3], q[3];
    bool usable = true;
    for (int k = 0; k < s; k++) {
      if (fromPoints) {
        p[k] = points[pick[k]];
        continue;
      }
      p[k] = facetCentres[pick[k]];
      const double l = glm::length(facetNormals[pick[k]]);
      usable = usable && l > 0.0;
      q[k] = usable ? facetNormals[pick[k]] / l : glm::dvec3(0.0);
    }
    if (!usable)
      continue;
    const FitResult h = Hypothesis(kind, p, q);
    if (!h.ok || !passes(h))
      continue;
    double agree = 0.0;
    size_t in = 0;
    const double cost = score(h, agree, in);
    // Equal to a millionth of the tolerance: a float file's rounding must
    // not decide between a face and the diagonal through its corners.
    const double tie = 1e-6 * tol2 * double(scored.size());
    if (cost < bestCost - tie ||
        (cost <= bestCost + tie && agree > bestAgree)) {
      bestIn = in;
      bestCost = cost;
      bestAgree = agree;
      best = h;
      const double w = double(in) / double(scored.size());
      const double miss = 1.0 - std::pow(w, s);
      needed = miss <= 0.0
                   ? kLeast
                   : std::max(kLeast, static_cast<int>(std::ceil(
                                          std::log(1e-3) / std::log(miss))));
    }
  }
  if (!best.ok || bestIn < static_cast<size_t>(s))
    return none;
  if (kind == Kind::Plane && oriented) {
    glm::dvec3 facing(0.0);
    for (const glm::dvec3& v : facetNormals)
      facing += v;
    if (glm::dot(facing, best.axis) < 0.0)
      best.axis = -best.axis;
  }
  // A cone's least squares starts from the tangent planes of the samples on
  // it.
  const auto inliersOf = [&](const FitResult& f, std::vector<glm::dvec3>& pts) {
    pts.clear();
    for (size_t i = 0; i < n; i++)
      if (std::abs(f.Distance(points[i])) <= toleranceMm)
        pts.push_back(points[i]);
  };
  const auto planesOf = [&](const FitResult& f, std::vector<glm::dvec3>& at,
                            std::vector<glm::dvec3>& nrm) {
    at.clear();
    nrm.clear();
    for (size_t i = 0; i < nf && oriented; i++)
      if (std::abs(f.Distance(facetCentres[i])) <= toleranceMm) {
        at.push_back(facetCentres[i]);
        nrm.push_back(glm::normalize(facetNormals[i]));
      }
  };
  // Least squares over the inliers, and again over the inliers of that,
  // while the whole cost (MSAC's, over every point) falls.
  std::vector<glm::dvec3> pts, at, nrm;
  inliersOf(best, pts);
  FitResult kept = best;
  size_t keptIn = 0;
  double keptCost = costOf(kept, nullptr, keptIn);
  Residuals(kept, pts);
  for (int round = 0; round < 8; round++) {
    if (kind == Kind::Cone)
      planesOf(kept, at, nrm);
    FitResult f = Refit(kind, kept, pts, at, nrm);
    if (!f.ok || !passes(f))
      break;
    size_t in = 0;
    const double cost = costOf(f, nullptr, in);
    if (!(cost < keptCost))
      break;
    std::vector<glm::dvec3> next;
    inliersOf(f, next);
    Residuals(f, next);
    if (!f.ok)
      break;
    const bool settled = cost > keptCost * (1.0 - 1e-9);
    kept = f;
    keptCost = cost;
    pts = std::move(next);
    if (settled)
      break;
  }
  kept.kind = kind;
  return kept;
}

FitResult FitRobust(Kind kind, const std::vector<glm::dvec3>& points,
                    const std::vector<glm::dvec3>& normals, double toleranceMm,
                    const glm::dvec3* through) {
  if (!normals.empty() && normals.size() != points.size())
    return FitResult{};
  return FitRobust(kind, points,
                   normals.empty() ? std::vector<glm::dvec3>{} : points,
                   normals, toleranceMm, through);
}

} // namespace mesh
