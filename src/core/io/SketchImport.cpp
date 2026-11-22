// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/SketchImport.h"

#include "core/sketch/SketchSpline.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace sketchimport {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

// (-pi, pi]
double Wrap(double a) {
  a = std::fmod(a, kTwoPi);
  if (a <= -std::numbers::pi)
    a += kTwoPi;
  else if (a > std::numbers::pi)
    a -= kTwoPi;
  return a;
}

glm::dmat2 Rot(double a) {
  const double c = std::cos(a), s = std::sin(a);
  return glm::dmat2(c, s, -s, c); // columns
}

glm::dvec2 EllipsePoint(const glm::dvec2& c, double rMaj, double rMin,
                        double rot, double t) {
  const glm::dvec2 u(std::cos(rot), std::sin(rot));
  const glm::dvec2 v(-u.y, u.x);
  return c + rMaj * std::cos(t) * u + rMin * std::sin(t) * v;
}

struct End {
  glm::dvec2* p; // null for an end the entity's geometry fixes
  glm::dvec2 at;
};

std::vector<End> EndsOf(sketch::Entity& e) {
  std::vector<End> out;
  if (auto* l = std::get_if<sketch::Line>(&e)) {
    out.push_back({&l->a, l->a});
    out.push_back({&l->b, l->b});
  } else if (auto* a = std::get_if<sketch::Arc>(&e)) {
    out.push_back(
        {nullptr, a->center + a->radius * glm::dvec2(std::cos(a->startAngle),
                                                     std::sin(a->startAngle))});
    out.push_back(
        {nullptr, a->center + a->radius * glm::dvec2(std::cos(a->endAngle),
                                                     std::sin(a->endAngle))});
  } else if (auto* ea = std::get_if<sketch::EllipseArc>(&e)) {
    out.push_back({nullptr, EllipsePoint(ea->center, ea->rMajor, ea->rMinor,
                                         ea->rotation, ea->startParam)});
    out.push_back({nullptr, EllipsePoint(ea->center, ea->rMajor, ea->rMinor,
                                         ea->rotation, ea->endParam)});
  } else if (auto* s = std::get_if<sketch::BSpline>(&e)) {
    if (s->poles.size() >= 2 && s->poles.front() != s->poles.back()) {
      out.push_back({&s->poles.front(), s->poles.front()});
      out.push_back({&s->poles.back(), s->poles.back()});
    }
  }
  return out;
}

// Points bucketed by a grid of `tol` cells, so joining a plate's thousands
// of ends is not quadratic.
struct Grid {
  double cell;
  std::unordered_map<long long, std::vector<size_t>> buckets;
  static long long Key(long long x, long long y) {
    return (x * 73856093LL) ^ (y * 19349663LL);
  }
  long long Cx(double v) const {
    return static_cast<long long>(std::floor(v / cell));
  }
  void Add(glm::dvec2 p, size_t i) {
    buckets[Key(Cx(p.x), Cx(p.y))].push_back(i);
  }
  template <class F> void Near(glm::dvec2 p, F&& f) const {
    const long long x = Cx(p.x), y = Cx(p.y);
    for (long long dx = -1; dx <= 1; dx++)
      for (long long dy = -1; dy <= 1; dy++) {
        const auto it = buckets.find(Key(x + dx, y + dy));
        if (it == buckets.end())
          continue;
        for (size_t i : it->second)
          f(i);
      }
  }
};

} // namespace

sketch::Entity ConicEntity(glm::dvec2 c, const glm::dmat2& L, double t0,
                           double t1, bool full) {
  const glm::dvec2 col0 = L[0], col1 = L[1];
  // MᵀM = [[p q] [q r]]: its eigenvectors are the parameter directions of
  // the axes, its eigenvalues their squared lengths.
  const double p = glm::dot(col0, col0), q = glm::dot(col0, col1),
               r = glm::dot(col1, col1);
  const double mean = 0.5 * (p + r);
  const double dev = std::sqrt(0.25 * (p - r) * (p - r) + q * q);
  const double rMaj = std::sqrt(mean + dev);
  const double rMin = std::sqrt(std::max(mean - dev, 0.0));
  const double det = col0.x * col1.y - col0.y * col1.x;
  const double s = det < 0.0 ? -1.0 : 1.0;
  const double sweep = full ? kTwoPi : t1 - t0;
  const auto at = [&](double t) {
    return c + L * glm::dvec2(std::cos(t), std::sin(t));
  };
  if (rMin <= 1e-12 * rMaj) // squashed flat: what is left is a segment
    return sketch::Line{at(t0), at(full ? t0 + std::numbers::pi : t1)};
  if (rMaj - rMin <= 1e-9 * rMaj) {
    // A similarity, r·Rot(alpha)·diag(1, s): the point at t sits at angle
    // alpha + s·t, so a mirror runs the arc the other way.
    const double radius = 0.5 * (rMaj + rMin);
    const double alpha = std::atan2(col0.y, col0.x);
    if (full)
      return sketch::Circle{c, radius};
    const double start = Wrap(s > 0 ? alpha + t0 : alpha - t1);
    return sketch::Arc{c, radius, start, start + sweep};
  }
  // The major axis's direction in parameter space, and in the plane.
  const double phi = 0.5 * std::atan2(2.0 * q, p - r);
  const glm::dvec2 v1(std::cos(phi), std::sin(phi));
  const glm::dvec2 u = L * v1 / rMaj;
  const double rotation = std::atan2(u.y, u.x);
  if (full)
    return sketch::Ellipse{c, rMaj, rMin, rotation};
  // Parameter e of the ellipse at t is s·(t − phi).
  const double start = Wrap(s > 0 ? t0 - phi : -(t1 - phi));
  return sketch::EllipseArc{c, rMaj, rMin, rotation, start, start + sweep};
}

sketch::Entity TransformEntity(const sketch::Entity& e, const Affine2& a) {
  sketch::Entity out = std::visit(
      [&](const auto& x) -> sketch::Entity {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, sketch::Point>) {
          return sketch::Point{a.Apply(x.p)};
        } else if constexpr (std::is_same_v<T, sketch::Line>) {
          return sketch::Line{a.Apply(x.a), a.Apply(x.b)};
        } else if constexpr (std::is_same_v<T, sketch::Circle>) {
          return ConicEntity(a.Apply(x.center), a.lin * x.radius, 0.0, kTwoPi,
                             true);
        } else if constexpr (std::is_same_v<T, sketch::Arc>) {
          double end = x.endAngle;
          while (end <= x.startAngle)
            end += kTwoPi;
          return ConicEntity(a.Apply(x.center), a.lin * x.radius, x.startAngle,
                             end, false);
        } else if constexpr (std::is_same_v<T, sketch::Ellipse>) {
          const glm::dmat2 L =
              Rot(x.rotation) * glm::dmat2(x.rMajor, 0.0, 0.0, x.rMinor);
          return ConicEntity(a.Apply(x.center), a.lin * L, 0.0, kTwoPi, true);
        } else if constexpr (std::is_same_v<T, sketch::EllipseArc>) {
          const glm::dmat2 L =
              Rot(x.rotation) * glm::dmat2(x.rMajor, 0.0, 0.0, x.rMinor);
          double end = x.endParam;
          while (end <= x.startParam)
            end += kTwoPi;
          return ConicEntity(a.Apply(x.center), a.lin * L, x.startParam, end,
                             false);
        } else if constexpr (std::is_same_v<T, sketch::BSpline>) {
          sketch::BSpline s = x; // affine-invariant: the poles carry it
          for (glm::dvec2& pole : s.poles)
            pole = a.Apply(pole);
          s.id = 0;
          return s;
        } else {
          T t = x;
          t.anchor = a.Apply(x.anchor);
          return t;
        }
      },
      e);
  sketch::SetConstruction(out, sketch::IsConstruction(e));
  return out;
}

glm::dvec2 EvalSpline(const std::vector<double>& U,
                      const std::vector<glm::dvec2>& P, int p, double u) {
  const int n = static_cast<int>(P.size());
  if (n == 0)
    return glm::dvec2(0.0);
  const double lo = U[p], hi = U[n];
  u = std::clamp(u, lo, hi);
  int k = p;
  while (k < n - 1 && U[k + 1] <= u)
    k++;
  std::vector<glm::dvec2> d(p + 1);
  for (int j = 0; j <= p; j++)
    d[j] = P[j + k - p];
  for (int rr = 1; rr <= p; rr++)
    for (int j = p; j >= rr; j--) {
      const double denom = U[j + 1 + k - rr] - U[j + k - p];
      const double alpha = denom > 0.0 ? (u - U[j + k - p]) / denom : 0.0;
      d[j] = (1.0 - alpha) * d[j - 1] + alpha * d[j];
    }
  return d[p];
}

std::vector<sketch::BSpline>
SplineToSketch(const std::vector<double>& knotsIn,
               const std::vector<glm::dvec2>& polesIn, int p) {
  const int n = static_cast<int>(polesIn.size());
  if (p < 1 || n < p + 1 || knotsIn.size() != static_cast<size_t>(n + p + 1))
    throw std::runtime_error("a spline of degree " + std::to_string(p) +
                             " with " + std::to_string(n) + " poles needs " +
                             std::to_string(n + p + 1) + " knots, not " +
                             std::to_string(knotsIn.size()));
  for (size_t i = 1; i < knotsIn.size(); i++)
    if (knotsIn[i] < knotsIn[i - 1])
      throw std::runtime_error("a spline's knots must not decrease");
  const double a = knotsIn[p], b = knotsIn[n];
  if (!(b > a))
    throw std::runtime_error("a spline's knots span nothing");
  for (size_t i = 0; i < knotsIn.size();) {
    size_t j = i;
    while (j < knotsIn.size() && knotsIn[j] == knotsIn[i])
      j++;
    if (j - i > static_cast<size_t>(p + 1))
      throw std::runtime_error(
          "a spline's knot " + std::format("{}", knotsIn[i]) + " is repeated " +
          std::to_string(j - i) + " times, more than its degree + 1 (" +
          std::to_string(p + 1) + ")");
    i = j;
  }
  // Clamped over [a, b]: each end raised to multiplicity p by Boehm's rule
  // (the curve does not change), the poles and knots outside dropped.
  std::vector<double> U = knotsIn;
  std::vector<glm::dvec2> P = polesIn;
  const auto multiplicity = [&](double u) {
    return static_cast<int>(std::count(U.begin(), U.end(), u));
  };
  for (const double end : {a, b})
    while (multiplicity(end) < p)
      sketch::InsertKnot(U, P, p, end);
  // One spline, cut only where the sketch cannot store it whole: at an
  // interior knot of multiplicity p or more (a corner, or a break), and
  // where a piece would carry more than 255 poles.
  std::vector<double> breaks{a};
  for (int i = p + 1; i < static_cast<int>(U.size()) - p - 1; i++)
    if (U[i] > a && U[i] < b && U[i] != breaks.back() &&
        multiplicity(U[i]) >= p)
      breaks.push_back(U[i]);
  breaks.push_back(b);
  std::vector<sketch::BSpline> whole;
  for (size_t i = 0; i + 1 < breaks.size(); i++)
    whole.push_back(sketch::SliceSpline(U, P, p, breaks[i], breaks[i + 1]));
  std::vector<sketch::BSpline> out;
  for (sketch::BSpline piece : whole) {
    while (piece.poles.size() > 255) {
      // The last interior knot that leaves at most 255 poles before it.
      std::vector<double> K = piece.knots;
      std::vector<glm::dvec2> Q = piece.poles;
      std::optional<double> found;
      for (int i = p + 1; i < static_cast<int>(Q.size()); i++)
        if (i <= 255 && K[i] != K[i - 1])
          found = K[i];
      if (!found)
        throw std::runtime_error("a spline of degree " + std::to_string(p) +
                                 " has more than the 255 poles a sketch "
                                 "spline holds and no knot to cut it at");
      const double at = *found;
      while (std::count(K.begin(), K.end(), at) < p)
        sketch::InsertKnot(K, Q, p, at);
      out.push_back(sketch::SliceSpline(K, Q, p, K.front(), at));
      piece = sketch::SliceSpline(K, Q, p, at, K.back());
    }
    out.push_back(std::move(piece));
  }
  // Evenly spaced knots are the sketch's default: stored as a plain spline.
  for (sketch::BSpline& piece : out)
    if (sketch::UniformUpToScale(piece.knots, p))
      piece.knots.clear();
  return out;
}

void SnapEndpoints(std::vector<sketch::Entity>& entities, double tol) {
  struct Rep {
    glm::dvec2 at;
  };
  std::vector<Rep> reps;
  Grid grid{tol, {}};
  const auto findRep = [&](glm::dvec2 p) -> long long {
    long long best = -1;
    double bestD = tol;
    grid.Near(p, [&](size_t i) {
      const double d = glm::length(reps[i].at - p);
      if (d < bestD) {
        bestD = d;
        best = static_cast<long long>(i);
      }
    });
    return best;
  };
  // The ends the geometry fixes first: those are where the joints are.
  std::vector<std::vector<End>> ends;
  ends.reserve(entities.size());
  for (sketch::Entity& e : entities)
    ends.push_back(EndsOf(e));
  for (const auto& list : ends)
    for (const End& end : list)
      if (end.p == nullptr && findRep(end.at) < 0) {
        grid.Add(end.at, reps.size());
        reps.push_back({end.at});
      }
  for (auto& list : ends)
    for (End& end : list) {
      if (end.p == nullptr)
        continue;
      const long long r = findRep(end.at);
      if (r >= 0) {
        *end.p = reps[static_cast<size_t>(r)].at;
      } else {
        grid.Add(end.at, reps.size());
        reps.push_back({end.at});
      }
    }
}

std::vector<glm::dvec2> OpenEnds(const std::vector<sketch::Entity>& entities,
                                 double tol) {
  std::vector<glm::dvec2> pts;
  for (const sketch::Entity& e : entities) {
    if (sketch::IsConstruction(e))
      continue;
    sketch::Entity copy = e;
    for (const End& end : EndsOf(copy))
      pts.push_back(end.at);
  }
  Grid grid{tol, {}};
  for (size_t i = 0; i < pts.size(); i++)
    grid.Add(pts[i], i);
  std::vector<glm::dvec2> open;
  for (size_t i = 0; i < pts.size(); i++) {
    bool joined = false;
    grid.Near(pts[i], [&](size_t j) {
      if (j != i && glm::length(pts[j] - pts[i]) < tol)
        joined = true;
    });
    if (!joined)
      open.push_back(pts[i]);
  }
  return open;
}

bool Bounds(const std::vector<sketch::Entity>& entities, glm::dvec2& lo,
            glm::dvec2& hi) {
  bool any = false;
  const auto add = [&](glm::dvec2 p) {
    lo = any ? glm::min(lo, p) : p;
    hi = any ? glm::max(hi, p) : p;
    any = true;
  };
  for (const sketch::Entity& e : entities) {
    std::visit(
        [&](const auto& x) {
          using T = std::decay_t<decltype(x)>;
          if constexpr (std::is_same_v<T, sketch::Point>) {
            add(x.p);
          } else if constexpr (std::is_same_v<T, sketch::Line>) {
            add(x.a);
            add(x.b);
          } else if constexpr (std::is_same_v<T, sketch::Circle>) {
            add(x.center - glm::dvec2(x.radius));
            add(x.center + glm::dvec2(x.radius));
          } else if constexpr (std::is_same_v<T, sketch::Arc>) {
            double end = x.endAngle;
            while (end <= x.startAngle)
              end += kTwoPi;
            for (int i = 0; i <= 64; i++) {
              const double t = x.startAngle + (end - x.startAngle) * i / 64.0;
              add(x.center + x.radius * glm::dvec2(std::cos(t), std::sin(t)));
            }
          } else if constexpr (std::is_same_v<T, sketch::Ellipse>) {
            for (int i = 0; i < 64; i++)
              add(EllipsePoint(x.center, x.rMajor, x.rMinor, x.rotation,
                               kTwoPi * i / 64.0));
          } else if constexpr (std::is_same_v<T, sketch::EllipseArc>) {
            double end = x.endParam;
            while (end <= x.startParam)
              end += kTwoPi;
            for (int i = 0; i <= 64; i++)
              add(EllipsePoint(x.center, x.rMajor, x.rMinor, x.rotation,
                               x.startParam + (end - x.startParam) * i / 64.0));
          } else if constexpr (std::is_same_v<T, sketch::BSpline>) {
            for (const glm::dvec2& q : sketch::BSplineSampleUV(x))
              add(q);
          } else {
            add(x.anchor);
          }
        },
        e);
  }
  return any;
}

} // namespace sketchimport
