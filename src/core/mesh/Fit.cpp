// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/mesh/Fit.h"

#include <algorithm>
#include <array>
#include <cmath>
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

} // namespace mesh
