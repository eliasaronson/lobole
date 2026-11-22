// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/sketch/SketchFillet.h"

#include "core/sketch/SketchSpline.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <tuple>

namespace sketch {
namespace {

constexpr double kTwoPi = 2.0 * M_PI;

double Wrap(double a) {
  a = std::fmod(a, kTwoPi);
  if (a < 0.0)
    a += kTwoPi;
  return a >= kTwoPi ? 0.0 : a;
}

double Cross(glm::dvec2 a, glm::dvec2 b) { return a.x * b.y - a.y * b.x; }

std::string Name(const Entity& e) {
  const char* kind = std::visit(
      [](const auto& g) -> const char* {
        using T = std::decay_t<decltype(g)>;
        if constexpr (std::is_same_v<T, Point>)
          return "point";
        else if constexpr (std::is_same_v<T, Line>)
          return "line";
        else if constexpr (std::is_same_v<T, Circle>)
          return "circle";
        else if constexpr (std::is_same_v<T, Arc>)
          return "arc";
        else if constexpr (std::is_same_v<T, Ellipse>)
          return "ellipse";
        else if constexpr (std::is_same_v<T, EllipseArc>)
          return "elliptical arc";
        else if constexpr (std::is_same_v<T, BSpline>)
          return "spline";
        else
          return "label";
      },
      e);
  return std::string(kind) + " " + std::to_string(EntityId(e));
}

// The direction the curve leaves its end `which` in, unit; nullopt when the
// entity has no such end (or it is degenerate there).
std::optional<glm::dvec2> Leaving(const Entity& e, uint8_t which) {
  glm::dvec2 d{0.0};
  if (const auto* l = std::get_if<Line>(&e)) {
    if (which > 1)
      return std::nullopt;
    d = which == 0 ? l->b - l->a : l->a - l->b;
  } else if (const auto* a = std::get_if<Arc>(&e)) {
    if (which == 1)
      d = {-std::sin(a->startAngle), std::cos(a->startAngle)};
    else if (which == 2)
      d = {std::sin(a->endAngle), -std::cos(a->endAngle)};
    else
      return std::nullopt;
  } else if (const auto* ea = std::get_if<EllipseArc>(&e)) {
    if (which != 1 && which != 2)
      return std::nullopt;
    const double t = which == 1 ? ea->startParam : ea->endParam;
    const glm::dvec2 local(-ea->rMajor * std::sin(t), ea->rMinor * std::cos(t));
    const double c = std::cos(ea->rotation), s = std::sin(ea->rotation);
    d = glm::dvec2(c * local.x - s * local.y, s * local.x + c * local.y);
    if (which == 2)
      d = -d;
  } else if (const auto* bs = std::get_if<BSpline>(&e)) {
    const size_t n = bs->poles.size();
    if (n < 2 || glm::length(bs->poles.front() - bs->poles.back()) < 1e-9)
      return std::nullopt; // closed: its seam is not an end
    const bool first = which == 0;
    if (!first && which != n - 1)
      return std::nullopt;
    const glm::dvec2 end = first ? bs->poles.front() : bs->poles.back();
    // The first pole that is not on the end: the curve leaves toward it.
    for (size_t k = 1; k < n; k++) {
      const glm::dvec2 q = first ? bs->poles[k] : bs->poles[n - 1 - k];
      if (glm::length(q - end) > 1e-12) {
        d = q - end;
        break;
      }
    }
  } else {
    return std::nullopt;
  }
  const double len = glm::length(d);
  if (len < 1e-12)
    return std::nullopt;
  return d / len;
}

} // namespace

std::vector<uint8_t> EndsOf(const Entity& e) {
  if (std::holds_alternative<Line>(e))
    return {0, 1};
  if (std::holds_alternative<Arc>(e) || std::holds_alternative<EllipseArc>(e))
    return {1, 2};
  if (const auto* bs = std::get_if<BSpline>(&e)) {
    const size_t n = bs->poles.size();
    if (n < 2 || n > 256 ||
        glm::length(bs->poles.front() - bs->poles.back()) < 1e-9)
      return {};
    return {0, static_cast<uint8_t>(n - 1)};
  }
  return {};
}

namespace {

// A line or an arc seen from the corner end of it.
struct Local {
  bool isLine = true;
  glm::dvec2 corner{0.0};
  glm::dvec2 leaving{0.0}; // unit
  double length = 0.0;     // line
  glm::dvec2 center{0.0};  // arc
  double radius = 0.0, sweep = 0.0;
  bool atStart = true;
};

std::optional<Local> LocalOf(const Entity& e, uint8_t which) {
  const auto d = Leaving(e, which);
  if (!d)
    return std::nullopt;
  Local l;
  l.leaving = *d;
  l.corner = EntityPointUV(e, which);
  if (const auto* ln = std::get_if<Line>(&e)) {
    l.length = glm::length(ln->b - ln->a);
    return l;
  }
  if (const auto* a = std::get_if<Arc>(&e)) {
    l.isLine = false;
    l.center = a->center;
    l.radius = a->radius;
    l.sweep = a->endAngle - a->startAngle;
    if (l.sweep <= 0.0)
      l.sweep = Wrap(l.sweep);
    l.atStart = which == 1;
    return l;
  }
  return std::nullopt;
}

// An offset of a line or an arc: a line (point + unit direction) or a circle.
struct Offset {
  bool isLine = true;
  glm::dvec2 p{0.0}, d{0.0}; // line
  glm::dvec2 o{0.0};
  double rho = 0.0; // circle
};

std::vector<glm::dvec2> Intersect(const Offset& a, const Offset& b) {
  std::vector<glm::dvec2> out;
  if (a.isLine && b.isLine) {
    const double den = Cross(a.d, b.d);
    if (std::abs(den) < 1e-15)
      return out;
    const double s = Cross(b.p - a.p, b.d) / den;
    out.push_back(a.p + s * a.d);
    return out;
  }
  if (a.isLine != b.isLine) {
    const Offset& l = a.isLine ? a : b;
    const Offset& c = a.isLine ? b : a;
    const glm::dvec2 q = l.p - c.o;
    const double bq = glm::dot(q, l.d);
    const double disc = bq * bq - (glm::dot(q, q) - c.rho * c.rho);
    if (disc < 0.0)
      return out;
    const double sq = std::sqrt(disc);
    out.push_back(l.p + (-bq - sq) * l.d);
    out.push_back(l.p + (-bq + sq) * l.d);
    return out;
  }
  const glm::dvec2 dv = b.o - a.o;
  const double dist = glm::length(dv);
  if (dist < 1e-15)
    return out;
  const double x = (dist * dist + a.rho * a.rho - b.rho * b.rho) / (2.0 * dist);
  const double h2 = a.rho * a.rho - x * x;
  if (h2 < 0.0)
    return out;
  const glm::dvec2 u = dv / dist, n(-u.y, u.x);
  const double h = std::sqrt(h2);
  out.push_back(a.o + x * u + h * n);
  out.push_back(a.o + x * u - h * n);
  return out;
}

} // namespace

std::vector<CurveEnd> CurveEndsAt(const std::vector<Entity>& entities,
                                  glm::dvec2 vertex, double tol) {
  std::vector<CurveEnd> ends;
  for (const Entity& e : entities)
    for (uint8_t w : EndsOf(e)) {
      if (glm::length(EntityPointUV(e, w) - vertex) > tol)
        continue;
      const auto d = Leaving(e, w);
      if (!d)
        continue;
      ends.push_back({EntityId(e), w, *d, Wrap(std::atan2(d->y, d->x))});
    }
  std::sort(ends.begin(), ends.end(), [](const CurveEnd& a, const CurveEnd& b) {
    return a.angle < b.angle;
  });
  return ends;
}

std::vector<SharedEnd> SharedEnds(const Entity& a, const Entity& b,
                                  double tol) {
  std::vector<SharedEnd> out;
  for (uint8_t wa : EndsOf(a))
    for (uint8_t wb : EndsOf(b)) {
      const glm::dvec2 pa = EntityPointUV(a, wa);
      if (glm::length(pa - EntityPointUV(b, wb)) < tol)
        out.push_back({wa, wb, pa});
    }
  return out;
}

std::expected<Corner, std::string> CornerAt(const std::vector<Entity>& entities,
                                            glm::dvec2 vertex,
                                            glm::dvec2 cursor, double onVertex,
                                            double tol) {
  const std::vector<CurveEnd> ends = CurveEndsAt(entities, vertex, tol);
  const auto name = [&](const CurveEnd& c) {
    for (const Entity& e : entities)
      if (EntityId(e) == c.entity)
        return Name(e);
    return std::string("entity ") + std::to_string(c.entity);
  };
  if (ends.empty())
    return std::unexpected("no curve ends here");
  if (ends.size() == 1)
    return std::unexpected("only " + name(ends[0]) +
                           " ends here; a corner is two curves meeting");
  const size_t n = ends.size();
  Corner c;
  c.vertex = vertex;
  double width = 0.0;
  const glm::dvec2 d = cursor - vertex;
  if (glm::length(d) <= onVertex) {
    if (n > 2)
      return std::unexpected(std::to_string(n) +
                             " curves end at this point; move into the "
                             "corner to round");
    c.a = ends[0];
    c.b = ends[1];
    width = c.b.angle - c.a.angle;
    if (width > M_PI) {
      std::swap(c.a, c.b);
      width = kTwoPi - width;
    }
  } else {
    const double phi = Wrap(std::atan2(d.y, d.x));
    size_t i = n - 1; // below every end: the sector wrapping through 0
    for (size_t k = 0; k < n; k++)
      if (ends[k].angle <= phi)
        i = k;
    c.a = ends[i];
    c.b = ends[(i + 1) % n];
    width = c.b.angle - c.a.angle + (i == n - 1 ? kTwoPi : 0.0);
  }
  if (std::abs(width - M_PI) < 1e-9)
    return std::unexpected(name(c.a) + " and " + name(c.b) +
                           " continue each other here; there is no corner "
                           "to round");
  if (width > M_PI)
    return std::unexpected("this side of the corner is wider than half a "
                           "turn; click inside the corner");
  if (width < 1e-9)
    return std::unexpected(name(c.a) + " and " + name(c.b) +
                           " leave this point in one direction; there is no "
                           "corner between them");
  return c;
}

namespace {

// A curve read from its corner end: sigma >= 0 runs away from the corner
// along the curve's own parameter, up to `sMax` at the far end.
struct Walk {
  std::function<void(double, glm::dvec2&, glm::dvec2&, glm::dvec2&)> eval;
  double sMax = 0.0;
};

std::optional<Walk> WalkOf(const Entity& e, uint8_t which) {
  Walk w;
  if (const auto* l = std::get_if<Line>(&e)) {
    if (which > 1)
      return std::nullopt;
    const glm::dvec2 c = which == 0 ? l->a : l->b;
    const glm::dvec2 d = which == 0 ? l->b - l->a : l->a - l->b;
    w.sMax = glm::length(d);
    if (!(w.sMax > 0.0))
      return std::nullopt;
    const glm::dvec2 u = d / w.sMax;
    w.eval = [c, u](double s, glm::dvec2& p, glm::dvec2& d1, glm::dvec2& d2) {
      p = c + s * u;
      d1 = u;
      d2 = glm::dvec2(0.0);
    };
    return w;
  }
  if (const auto* a = std::get_if<Arc>(&e)) {
    if (which != 1 && which != 2)
      return std::nullopt;
    double en = a->endAngle;
    while (en <= a->startAngle)
      en += kTwoPi;
    w.sMax = en - a->startAngle;
    const double t0 = which == 1 ? a->startAngle : en;
    const double dir = which == 1 ? 1.0 : -1.0;
    const glm::dvec2 o = a->center;
    const double r = a->radius;
    w.eval = [=](double s, glm::dvec2& p, glm::dvec2& d1, glm::dvec2& d2) {
      const double t = t0 + dir * s;
      const glm::dvec2 rad(std::cos(t), std::sin(t));
      p = o + r * rad;
      d1 = dir * r * glm::dvec2(-rad.y, rad.x);
      d2 = -r * rad;
    };
    return w;
  }
  if (const auto* ea = std::get_if<EllipseArc>(&e)) {
    if (which != 1 && which != 2)
      return std::nullopt;
    double en = ea->endParam;
    while (en <= ea->startParam)
      en += kTwoPi;
    w.sMax = en - ea->startParam;
    const double t0 = which == 1 ? ea->startParam : en;
    const double dir = which == 1 ? 1.0 : -1.0;
    const EllipseArc g = *ea;
    const double c = std::cos(g.rotation), sn = std::sin(g.rotation);
    const auto rot = [c, sn](glm::dvec2 v) {
      return glm::dvec2(c * v.x - sn * v.y, sn * v.x + c * v.y);
    };
    w.eval = [=](double s, glm::dvec2& p, glm::dvec2& d1, glm::dvec2& d2) {
      const double t = t0 + dir * s;
      p = EllipseArcEvalUV(g, t);
      d1 = dir *
           rot(glm::dvec2(-g.rMajor * std::sin(t), g.rMinor * std::cos(t)));
      d2 = rot(glm::dvec2(-g.rMajor * std::cos(t), -g.rMinor * std::sin(t)));
    };
    return w;
  }
  if (const auto* bs = std::get_if<BSpline>(&e)) {
    const auto ends = EndsOf(e);
    if (std::find(ends.begin(), ends.end(), which) == ends.end())
      return std::nullopt;
    const auto [lo, hi] = SplineDomain(*bs);
    w.sMax = hi - lo;
    const bool first = which == 0;
    const BSpline g = *bs;
    w.eval = [=](double s, glm::dvec2& p, glm::dvec2& d1, glm::dvec2& d2) {
      const double u = first ? lo + s : hi - s;
      p = BSplineEvalUV(g, u);
      d1 = (first ? 1.0 : -1.0) * BSplineDerivUV(g, u);
      d2 = BSplineDeriv2UV(g, u);
    };
    return w;
  }
  return std::nullopt;
}

// The arc of `g` from one tangent point to the other that faces the
// corner: the one whose span holds the corner's direction from the centre.
std::optional<FilletRefusal> ArcFacingCorner(FilletGeometry& g, double radius) {
  const glm::dvec2 P = g.center, C = g.corner;
  const double a1 = std::atan2(g.t1.y - P.y, g.t1.x - P.x);
  const double a2 = std::atan2(g.t2.y - P.y, g.t2.x - P.x);
  const double toCorner = std::atan2(C.y - P.y, C.x - P.x);
  const double sweep12 = Wrap(a2 - a1);
  if (sweep12 < 1e-12 || sweep12 > kTwoPi - 1e-12)
    return FilletRefusal{"the fillet has no length at this corner"};
  g.arcStartsAtT1 = Wrap(toCorner - a1) <= sweep12;
  g.arc = g.arcStartsAtT1 ? Arc{P, radius, a1, a1 + sweep12}
                          : Arc{P, radius, a2, a2 + Wrap(a1 - a2)};
  return std::nullopt;
}

// The fillet with an elliptical arc or a spline in the pair: Newton on the
// two curves' parameters for the centre both offsets reach.
std::expected<FilletGeometry, FilletRefusal>
FilletByNewton(const Entity& c1, uint8_t w1, const Entity& c2, uint8_t w2,
               double radius, glm::dvec2 t1, glm::dvec2 t2) {
  const auto refuse = [](std::string why, bool tooLarge = false) {
    return std::unexpected(FilletRefusal{std::move(why), tooLarge});
  };
  const auto k1 = WalkOf(c1, w1), k2 = WalkOf(c2, w2);
  if (!k1 || !k2)
    return refuse(Name(!k1 ? c1 : c2) +
                  " has no end there a fillet can round (a fillet takes "
                  "lines, arcs, elliptical arcs and open splines)");
  // Each curve's normal toward the inside of the corner keeps the side it
  // has at the corner: +-90 degrees from its tangent.
  const auto perp = [](glm::dvec2 v) { return glm::dvec2(-v.y, v.x); };
  const double s1 = glm::dot(perp(t1), t2) > 0.0 ? 1.0 : -1.0;
  const double s2 = glm::dot(perp(t2), t1) > 0.0 ? 1.0 : -1.0;
  struct At {
    glm::dvec2 p, n, dOffset; // point, inward normal, d(p + r n)/d sigma
    double speed = 0.0, kappaIn = 0.0;
  };
  const auto at = [&](const Walk& k, double sgn, double s) {
    glm::dvec2 p, d1, d2;
    k.eval(s, p, d1, d2);
    At a;
    a.p = p;
    a.speed = glm::length(d1);
    const glm::dvec2 T = a.speed > 0.0 ? d1 / a.speed : glm::dvec2(1.0, 0.0);
    a.n = sgn * perp(T);
    const glm::dvec2 dT =
        a.speed > 0.0 ? (d2 - glm::dot(d2, T) * T) / a.speed : glm::dvec2(0.0);
    a.dOffset = d1 + radius * sgn * perp(dT);
    a.kappaIn = a.speed > 0.0
                    ? sgn * Cross(d1, d2) / (a.speed * a.speed * a.speed)
                    : 0.0;
    return a;
  };
  const auto F = [&](double x, double y) {
    const At a = at(*k1, s1, x), b = at(*k2, s2, y);
    return (a.p + radius * a.n) - (b.p + radius * b.n);
  };
  // Seed: the fillet of the two end tangents, its tangent length mapped
  // onto each parameter by the speed at the corner.
  const double half = 0.5 * std::acos(std::clamp(glm::dot(t1, t2), -1.0, 1.0));
  const double reach = radius / std::tan(half);
  double x = reach / std::max(at(*k1, s1, 0.0).speed, 1e-300);
  double y = reach / std::max(at(*k2, s2, 0.0).speed, 1e-300);
  const double scale = 1.0 + radius + glm::length(at(*k1, s1, 0.0).p);
  bool converged = false;
  for (int it = 0; it < 100; it++) {
    const glm::dvec2 f = F(x, y);
    if (glm::length(f) < 1e-13 * scale) {
      converged = true;
      break;
    }
    const At a = at(*k1, s1, x), b = at(*k2, s2, y);
    // J [dx, dy] = -f, J = [dOffset1, -dOffset2].
    const double det = Cross(a.dOffset, -b.dOffset);
    if (!(std::abs(det) > 0.0))
      break;
    double dx = Cross(-f, -b.dOffset) / det;
    double dy = Cross(a.dOffset, -f) / det;
    // Halve the step until it lowers the residual, staying past the corner.
    double step = 1.0;
    for (int h = 0; h < 40; h++) {
      const double nx = std::max(0.0, x + step * dx);
      const double ny = std::max(0.0, y + step * dy);
      if (glm::length(F(nx, ny)) < glm::length(f)) {
        x = nx;
        y = ny;
        break;
      }
      step *= 0.5;
      if (h == 39) {
        dx = dy = 0.0;
      }
    }
    if (dx == 0.0 && dy == 0.0)
      break;
  }
  if (!converged)
    return refuse("no fillet of this radius touches both curves", true);

  constexpr double kEps = 1e-7; // mm
  const At a = at(*k1, s1, x), b = at(*k2, s2, y);
  for (const auto& [k, sgn, sig, e] :
       {std::tuple{&*k1, s1, x, &c1}, std::tuple{&*k2, s2, y, &c2}}) {
    const double speed = at(*k, sgn, sig).speed;
    if (!(sig * speed > kEps) ||
        !(sig < k->sMax - kEps / std::max(speed, 1e-300)))
      return refuse("the fillet would take all of " + Name(*e), true);
    // No stretch between the corner and the tangent point may bend toward
    // the inside tighter than the fillet.
    constexpr int kSamples = 64;
    for (int i = 0; i <= kSamples; i++)
      if (at(*k, sgn, sig * i / kSamples).kappaIn * radius >= 1.0 - 1e-12)
        return refuse(Name(*e) + " bends tighter than the fillet", true);
  }
  FilletGeometry g;
  g.corner = EntityPointUV(c1, w1);
  g.t1 = a.p;
  g.t2 = b.p;
  g.center = 0.5 * ((a.p + radius * a.n) + (b.p + radius * b.n));
  g.trimmed1 = c1;
  g.trimmed2 = c2;
  // Each curve cut back to its tangent point (a spline is cut by the
  // caller at param1/2).
  const auto trim = [&](const Entity& e, uint8_t w, double sig, glm::dvec2 T,
                        Entity& out, double& param) {
    out = e;
    if (auto* ln = std::get_if<Line>(&out)) {
      (w == 0 ? ln->a : ln->b) = T;
      param = sig;
    } else if (auto* ar = std::get_if<Arc>(&out)) {
      if (w == 1)
        ar->startAngle += sig;
      else
        ar->endAngle -= sig;
      param = sig;
    } else if (auto* ea = std::get_if<EllipseArc>(&out)) {
      while (ea->endParam <= ea->startParam)
        ea->endParam += kTwoPi;
      if (w == 1)
        ea->startParam += sig;
      else
        ea->endParam -= sig;
      param = w == 1 ? ea->startParam : ea->endParam;
    } else if (const auto* bs = std::get_if<BSpline>(&out)) {
      const auto [lo, hi] = SplineDomain(*bs);
      param = w == 0 ? lo + sig : hi - sig;
    }
  };
  trim(c1, w1, x, g.t1, g.trimmed1, g.param1);
  trim(c2, w2, y, g.t2, g.trimmed2, g.param2);
  if (auto r = ArcFacingCorner(g, radius))
    return std::unexpected(*r);
  return g;
}

} // namespace

std::expected<FilletGeometry, FilletRefusal>
FilletOfCurves(const Entity& c1, uint8_t w1, const Entity& c2, uint8_t w2,
               double radius) {
  const auto refuse = [](std::string why, bool tooLarge = false) {
    return std::unexpected(FilletRefusal{std::move(why), tooLarge});
  };
  if (!(radius > 0.0) || !std::isfinite(radius))
    return refuse("the fillet radius must be positive");
  const auto d1 = Leaving(c1, w1), d2 = Leaving(c2, w2);
  if (!d1 || !d2)
    return refuse(Name(!d1 ? c1 : c2) +
                  " has no end there a fillet can round (a fillet takes "
                  "lines, arcs, elliptical arcs and open splines)");
  const auto l1 = LocalOf(c1, w1), l2 = LocalOf(c2, w2);
  const glm::dvec2 t1 = *d1, t2 = *d2;
  const double cr = Cross(t1, t2);
  if (std::abs(cr) < 1e-12)
    return refuse(Name(c1) + " and " + Name(c2) +
                  (glm::dot(t1, t2) < 0.0
                       ? " continue each other at their shared end; there is "
                         "no corner to round"
                       : " leave their shared end in one direction; there is "
                         "no corner between them"));
  if (!l1 || !l2) // an elliptical arc or a spline: no closed form
    return FilletByNewton(c1, w1, c2, w2, radius, t1, t2);
  const glm::dvec2 C = l1->corner;

  // The normal of each curve's tangent that points into the corner.
  const auto inward = [](glm::dvec2 t, glm::dvec2 other) {
    glm::dvec2 n(-t.y, t.x);
    return glm::dot(n, other) < 0.0 ? -n : n;
  };
  const glm::dvec2 n1 = inward(t1, t2), n2 = inward(t2, t1);
  const auto offset =
      [&](const Local& l, glm::dvec2 n,
          const Entity& e) -> std::expected<Offset, FilletRefusal> {
    Offset o;
    if (l.isLine) {
      o.p = l.corner + radius * n;
      o.d = l.leaving;
      return o;
    }
    o.isLine = false;
    o.o = l.center;
    // The arc bends into the corner when its centre is on the inside: the
    // fillet then runs inside its circle, and must be the smaller.
    if (glm::dot(l.center - l.corner, n) > 0.0) {
      if (radius >= l.radius * (1.0 - 1e-12))
        return refuse(Name(e) + " bends tighter than the fillet", true);
      o.rho = l.radius - radius;
    } else {
      o.rho = l.radius + radius;
    }
    return o;
  };
  const auto o1 = offset(*l1, n1, c1);
  if (!o1)
    return std::unexpected(o1.error());
  const auto o2 = offset(*l2, n2, c2);
  if (!o2)
    return std::unexpected(o2.error());
  const std::vector<glm::dvec2> meets = Intersect(*o1, *o2);
  if (meets.empty())
    return refuse("no fillet of this radius touches both curves", true);
  glm::dvec2 P = meets[0];
  for (const glm::dvec2& q : meets)
    if (glm::length(q - C) < glm::length(P - C))
      P = q;

  // Each tangent point, and how far along its curve from the corner it is.
  constexpr double kEps = 1e-7; // mm
  FilletGeometry g;
  g.corner = C;
  g.center = P;
  const auto foot = [&](const Local& l, const Entity& e, glm::dvec2& T,
                        Entity& trimmed,
                        uint8_t w) -> std::optional<FilletRefusal> {
    trimmed = e;
    if (l.isLine) {
      const double s = glm::dot(P - l.corner, l.leaving);
      if (!(s > kEps) || !(s < l.length - kEps))
        return FilletRefusal{"the fillet would take all of " + Name(e), true};
      T = l.corner + s * l.leaving;
      auto& ln = std::get<Line>(trimmed);
      (w == 0 ? ln.a : ln.b) = T;
      return std::nullopt;
    }
    const glm::dvec2 rel = P - l.center;
    const double len = glm::length(rel);
    if (len < 1e-15)
      return FilletRefusal{Name(e) + " bends tighter than the fillet", true};
    const glm::dvec2 u = rel / len;
    T = l.center + l.radius * u;
    auto& arc = std::get<Arc>(trimmed);
    const double theta = std::atan2(u.y, u.x);
    const double delta =
        l.atStart ? Wrap(theta - arc.startAngle) : Wrap(arc.endAngle - theta);
    if (!(delta * l.radius > kEps) || !(delta < l.sweep - kEps / l.radius))
      return FilletRefusal{"the fillet would take all of " + Name(e), true};
    if (l.atStart)
      arc.startAngle += delta;
    else
      arc.endAngle -= delta;
    return std::nullopt;
  };
  if (auto r = foot(*l1, c1, g.t1, g.trimmed1, w1))
    return std::unexpected(*r);
  if (auto r = foot(*l2, c2, g.t2, g.trimmed2, w2))
    return std::unexpected(*r);

  if (auto r = ArcFacingCorner(g, radius))
    return std::unexpected(*r);
  return g;
}

double LargestFillet(const Entity& c1, uint8_t w1, const Entity& c2, uint8_t w2,
                     double upTo) {
  const auto fits = [&](double r) {
    return FilletOfCurves(c1, w1, c2, w2, r).has_value();
  };
  if (!(upTo > 0.0))
    return 0.0;
  if (fits(upTo))
    return upTo;
  // A radius that fits, from a thousandth of the one asked down.
  double hi = upTo, lo = upTo * 1e-3;
  for (int k = 0; k < 6 && !fits(lo); k++) {
    hi = lo;
    lo *= 0.1;
  }
  if (!fits(lo))
    return 0.0;
  for (int i = 0; i < 100 && hi - lo > 1e-12 * upTo; i++) {
    const double mid = 0.5 * (lo + hi);
    (fits(mid) ? lo : hi) = mid;
  }
  // Down to three significant figures, so the number shown fits.
  const double step = std::pow(10.0, std::floor(std::log10(lo)) - 2.0);
  const double shown = std::floor(lo / step + 1e-9) * step;
  return shown > 0.0 && fits(shown) ? shown : lo;
}

} // namespace sketch
