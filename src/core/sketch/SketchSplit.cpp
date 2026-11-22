// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/sketch/SketchSplit.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace sketch {

namespace {

constexpr double kTwoPi = 2.0 * M_PI;

// `ang` moved into [s, s + 2π).
double Into(double ang, double s) {
  while (ang < s)
    ang += kTwoPi;
  while (ang >= s + kTwoPi)
    ang -= kTwoPi;
  return ang;
}

// An arc's sweep end, past its start (CCW-always).
double SweepEnd(const Arc& a) {
  double e = a.endAngle;
  while (e <= a.startAngle)
    e += kTwoPi;
  return e;
}

// Does the point at angle `ang` lie on arc `a`'s sweep (within `slack` rad)?
bool OnSweep(const Arc& a, double ang, double slack) {
  const double x = Into(ang, a.startAngle - slack);
  return x <= SweepEnd(a) + slack;
}

// A curve as its parameter range and a point at each parameter.
struct Param {
  double t0 = 0.0, t1 = 0.0;
  int samples = 0;
  std::function<glm::dvec2(double)> at;
};

std::optional<Param> ParamOf(const Entity& e) {
  if (const auto* l = std::get_if<Line>(&e)) {
    const glm::dvec2 a = l->a, b = l->b;
    return Param{0.0, 1.0, 1, [a, b](double t) { return a + t * (b - a); }};
  }
  if (const auto* c = std::get_if<Circle>(&e)) {
    const glm::dvec2 o = c->center;
    const double r = c->radius;
    return Param{0.0, kTwoPi, 360, [o, r](double t) {
                   return o + r * glm::dvec2(std::cos(t), std::sin(t));
                 }};
  }
  if (const auto* a = std::get_if<Arc>(&e)) {
    const glm::dvec2 o = a->center;
    const double r = a->radius, s = a->startAngle, en = SweepEnd(*a);
    const int n =
        std::max(8, static_cast<int>(std::ceil(360.0 * (en - s) / kTwoPi)));
    return Param{s, en, n, [o, r](double t) {
                   return o + r * glm::dvec2(std::cos(t), std::sin(t));
                 }};
  }
  if (const auto* el = std::get_if<Ellipse>(&e)) {
    const Ellipse g = *el;
    return Param{0.0, kTwoPi, 360,
                 [g](double t) { return EllipsePointUV(g, t); }};
  }
  if (const auto* ea = std::get_if<EllipseArc>(&e)) {
    const EllipseArc g = *ea;
    double en = g.endParam;
    while (en <= g.startParam)
      en += kTwoPi;
    const int n = std::max(
        8, static_cast<int>(std::ceil(360.0 * (en - g.startParam) / kTwoPi)));
    return Param{g.startParam, en, n,
                 [g](double t) { return EllipseArcEvalUV(g, t); }};
  }
  if (const auto* bs = std::get_if<BSpline>(&e)) {
    const int n = static_cast<int>(bs->poles.size());
    const int p = BSplineDegree(*bs);
    if (n < 2 || p == 0)
      return std::nullopt;
    const BSpline g = *bs;
    return Param{0.0, static_cast<double>(n - p), 64 * (n - p),
                 [g](double u) { return BSplineEvalUV(g, u); }};
  }
  return std::nullopt; // a point or a label: no curve
}

} // namespace

bool Splittable(const Entity& e) {
  return std::holds_alternative<Line>(e) || std::holds_alternative<Arc>(e) ||
         std::holds_alternative<Circle>(e);
}

double CurveLength(const Entity& e) {
  if (const auto* l = std::get_if<Line>(&e))
    return glm::length(l->b - l->a);
  if (const auto* c = std::get_if<Circle>(&e))
    return kTwoPi * c->radius;
  if (const auto* a = std::get_if<Arc>(&e))
    return a->radius * (SweepEnd(*a) - a->startAngle);
  return 0.0;
}

std::optional<NearestPoint> NearestOnCurve(const Entity& e, glm::dvec2 p) {
  glm::dvec2 q{0.0};
  if (const auto* l = std::get_if<Line>(&e)) {
    const glm::dvec2 d = l->b - l->a;
    const double len2 = glm::dot(d, d);
    const double t =
        len2 > 0.0 ? std::clamp(glm::dot(p - l->a, d) / len2, 0.0, 1.0) : 0.0;
    q = l->a + t * d;
  } else if (const auto* c = std::get_if<Circle>(&e)) {
    const glm::dvec2 r = p - c->center;
    const double len = glm::length(r);
    q = c->center + c->radius * (len > 0.0 ? r / len : glm::dvec2(1.0, 0.0));
  } else if (const auto* a = std::get_if<Arc>(&e)) {
    const double ang = std::atan2(p.y - a->center.y, p.x - a->center.x);
    if (OnSweep(*a, ang, 0.0)) {
      q = a->center + a->radius * glm::dvec2(std::cos(ang), std::sin(ang));
    } else {
      const glm::dvec2 s = ArcPointUV(*a, 1), en = ArcPointUV(*a, 2);
      q = glm::length(p - s) <= glm::length(p - en) ? s : en;
    }
  } else {
    return std::nullopt;
  }
  return NearestPoint{q, glm::length(p - q)};
}

bool AtCurveEnd(const Entity& e, glm::dvec2 p, double tol) {
  if (const auto* l = std::get_if<Line>(&e))
    return glm::length(p - l->a) < tol || glm::length(p - l->b) < tol;
  if (const auto* a = std::get_if<Arc>(&e))
    return glm::length(p - ArcPointUV(*a, 1)) < tol ||
           glm::length(p - ArcPointUV(*a, 2)) < tol;
  return false;
}

std::vector<glm::dvec2> Crossings(const Entity& e, const Entity& other) {
  std::vector<glm::dvec2> out;
  // The signed distance from `e`'s curve: across a line, out of a circle.
  std::function<double(glm::dvec2)> side;
  std::function<bool(glm::dvec2)> inside; // within e's extent
  if (const auto* l = std::get_if<Line>(&e)) {
    const glm::dvec2 a = l->a, d = l->b - l->a;
    const double len = glm::length(d);
    if (len <= 0.0)
      return out;
    side = [a, d, len](glm::dvec2 p) {
      const glm::dvec2 r = p - a;
      return (d.x * r.y - d.y * r.x) / len;
    };
    inside = [a, d, len](glm::dvec2 p) {
      const double t = glm::dot(p - a, d) / (len * len);
      const double slack = 1e-9 / len;
      return t >= -slack && t <= 1.0 + slack;
    };
  } else if (const auto* c = std::get_if<Circle>(&e)) {
    const glm::dvec2 o = c->center;
    const double r = c->radius;
    side = [o, r](glm::dvec2 p) { return glm::length(p - o) - r; };
    inside = [](glm::dvec2) { return true; };
  } else if (const auto* a = std::get_if<Arc>(&e)) {
    const Arc g = *a;
    side = [g](glm::dvec2 p) { return glm::length(p - g.center) - g.radius; };
    inside = [g](glm::dvec2 p) {
      return OnSweep(g, std::atan2(p.y - g.center.y, p.x - g.center.x),
                     1e-9 / std::max(g.radius, 1e-12));
    };
  } else {
    return out;
  }
  const auto keep = [&](glm::dvec2 p) {
    if (!inside(p))
      return;
    for (const glm::dvec2& q : out)
      if (glm::length(q - p) < 1e-9)
        return;
    out.push_back(p);
  };
  // A line and a circle, or two circles, in closed form: sampling a line
  // that enters and leaves a circle sees no sign change between its ends.
  const auto circleOf =
      [](const Entity& c) -> std::optional<std::pair<glm::dvec2, double>> {
    if (const auto* k = std::get_if<Circle>(&c))
      return std::pair{k->center, k->radius};
    if (const auto* a = std::get_if<Arc>(&c))
      return std::pair{a->center, a->radius};
    return std::nullopt;
  };
  const auto onOther = [&](glm::dvec2 p) {
    if (const auto* a = std::get_if<Arc>(&other))
      return OnSweep(*a, std::atan2(p.y - a->center.y, p.x - a->center.x),
                     1e-9 / std::max(a->radius, 1e-12));
    return true;
  };
  const auto lineCircle = [&](glm::dvec2 a, glm::dvec2 b, glm::dvec2 o,
                              double r, bool clampToSegment) {
    const glm::dvec2 d = b - a, f = a - o;
    const double A = glm::dot(d, d);
    if (A <= 0.0)
      return;
    const double B = 2.0 * glm::dot(f, d), C = glm::dot(f, f) - r * r;
    const double disc = B * B - 4.0 * A * C;
    if (disc <= 0.0) // a touch is no crossing
      return;
    const double sq = std::sqrt(disc);
    for (const double t : {(-B - sq) / (2.0 * A), (-B + sq) / (2.0 * A)}) {
      const double slack = 1e-9 / std::sqrt(A);
      if (clampToSegment && (t < -slack || t > 1.0 + slack))
        continue;
      const glm::dvec2 p = a + t * d;
      if (onOther(p))
        keep(p);
    }
  };
  const auto selfCircle = circleOf(e);
  if (const auto* ol = std::get_if<Line>(&other); ol && selfCircle) {
    lineCircle(ol->a, ol->b, selfCircle->first, selfCircle->second, true);
    return out;
  }
  if (const auto oc = circleOf(other)) {
    if (const auto* l = std::get_if<Line>(&e)) {
      lineCircle(l->a, l->b, oc->first, oc->second, false); // `inside` clamps
      return out;
    }
    const glm::dvec2 c1 = selfCircle->first, c2 = oc->first;
    const double r1 = selfCircle->second, r2 = oc->second;
    const double dist = glm::length(c2 - c1);
    if (dist <= 0.0 || dist >= r1 + r2 || dist <= std::abs(r1 - r2))
      return out;
    const double x = (r1 * r1 - r2 * r2 + dist * dist) / (2.0 * dist);
    const double h = std::sqrt(std::max(0.0, r1 * r1 - x * x));
    const glm::dvec2 u = (c2 - c1) / dist, n{-u.y, u.x};
    for (const double sgn : {1.0, -1.0}) {
      const glm::dvec2 p = c1 + x * u + sgn * h * n;
      if (onOther(p))
        keep(p);
    }
    return out;
  }
  const auto param = ParamOf(other);
  if (!param)
    return out;
  const Param& c = *param;
  double tPrev = c.t0;
  double gPrev = side(c.at(tPrev));
  if (gPrev == 0.0)
    keep(c.at(tPrev));
  for (int i = 1; i <= c.samples; i++) {
    const double t = c.t0 + (c.t1 - c.t0) * i / c.samples;
    const double g = side(c.at(t));
    if (g == 0.0) {
      keep(c.at(t));
    } else if (gPrev != 0.0 && (g < 0.0) != (gPrev < 0.0)) {
      // A sign change in (tPrev, t): bisect it down to the last bit.
      double lo = tPrev, hi = t, gLo = gPrev;
      for (int k = 0; k < 200 && hi - lo > 0.0; k++) {
        const double mid = 0.5 * (lo + hi);
        if (mid <= lo || mid >= hi)
          break;
        const double gm = side(c.at(mid));
        if (gm == 0.0) {
          lo = hi = mid;
          break;
        }
        if ((gm < 0.0) == (gLo < 0.0)) {
          lo = mid;
          gLo = gm;
        } else {
          hi = mid;
        }
      }
      const glm::dvec2 pl = c.at(lo), ph = c.at(hi);
      keep(std::abs(side(pl)) <= std::abs(side(ph)) ? pl : ph);
    }
    tPrev = t;
    gPrev = g;
  }
  return out;
}

std::vector<Entity> SplitPieces(const Entity& e,
                                const std::vector<glm::dvec2>& cuts) {
  std::vector<Entity> out;
  const uint32_t id = EntityId(e);
  const bool construction = IsConstruction(e);
  const auto piece = [&](Entity p, bool first) {
    SetEntityId(p, first ? id : 0);
    SetConstruction(p, construction);
    out.push_back(std::move(p));
  };
  if (const auto* l = std::get_if<Line>(&e)) {
    if (cuts.size() != 1)
      return out;
    piece(Line{l->a, cuts[0]}, true);
    piece(Line{cuts[0], l->b}, false);
  } else if (const auto* a = std::get_if<Arc>(&e)) {
    if (cuts.size() != 1)
      return out;
    const double s = a->startAngle, en = SweepEnd(*a);
    const double t =
        Into(std::atan2(cuts[0].y - a->center.y, cuts[0].x - a->center.x), s);
    piece(Arc{a->center, a->radius, s, t}, true);
    piece(Arc{a->center, a->radius, t, en}, false);
  } else if (const auto* c = std::get_if<Circle>(&e)) {
    if (cuts.size() != 2)
      return out;
    const auto angle = [&](glm::dvec2 p) {
      return std::atan2(p.y - c->center.y, p.x - c->center.x);
    };
    const double t1 = angle(cuts[0]);
    double t2 = Into(angle(cuts[1]), t1);
    if (t2 <= t1)
      t2 += kTwoPi;
    piece(Arc{c->center, c->radius, t1, t2}, true);
    piece(Arc{c->center, c->radius, t2, t1 + kTwoPi}, false);
  }
  return out;
}

} // namespace sketch
