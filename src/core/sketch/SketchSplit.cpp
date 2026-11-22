// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/sketch/SketchSplit.h"

#include "core/Angles.h"
#include "core/Units.h"
#include "core/sketch/SketchSpline.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

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
    if (bs->poles.size() < 2 || BSplineDegree(*bs) == 0)
      return std::nullopt;
    const BSpline g = *bs;
    const auto [lo, hi] = SplineDomain(g);
    return Param{lo, hi, 64 * SplineSpans(g),
                 [g](double u) { return BSplineEvalUV(g, u); }};
  }
  return std::nullopt; // a point or a label: no curve
}

// The parameter of the point of `c` nearest `p`: the nearest of four
// samples per step, then a ternary search between its neighbours.
double NearestParam(const Param& c, glm::dvec2 p) {
  const int n = 4 * std::max(c.samples, 1);
  const double step = (c.t1 - c.t0) / n;
  double best = c.t0, bestD = std::numeric_limits<double>::infinity();
  for (int i = 0; i <= n; i++) {
    const double t = c.t0 + step * i;
    const double d = glm::length(c.at(t) - p);
    if (d < bestD) {
      bestD = d;
      best = t;
    }
  }
  double lo = std::max(c.t0, best - step), hi = std::min(c.t1, best + step);
  for (int k = 0; k < 200 && hi - lo > 0.0; k++) {
    const double m1 = lo + (hi - lo) / 3.0, m2 = hi - (hi - lo) / 3.0;
    if (m1 <= lo || m2 >= hi)
      break;
    if (glm::length(c.at(m1) - p) <= glm::length(c.at(m2) - p))
      hi = m2;
    else
      lo = m1;
  }
  const double mid = 0.5 * (lo + hi);
  return glm::length(c.at(mid) - p) <= bestD ? mid : best;
}

// Is the curve a line, an arc or a circle (whose crossings have a side)?
bool Simple(const Entity& e) {
  return std::holds_alternative<Line>(e) || std::holds_alternative<Arc>(e) ||
         std::holds_alternative<Circle>(e);
}

std::vector<glm::dvec2> CrossingsSimple(const Entity& e, const Entity& other);

// Two curves neither of which is a line, an arc or a circle: where their
// polylines cross, refined by Newton on both parameters.
std::vector<glm::dvec2> CrossingsGeneral(const Param& a, const Param& b) {
  std::vector<glm::dvec2> out;
  const auto sample = [](const Param& c, std::vector<double>& ts,
                         std::vector<glm::dvec2>& ps) {
    const int n = std::max(c.samples, 8);
    for (int i = 0; i <= n; i++) {
      ts.push_back(c.t0 + (c.t1 - c.t0) * i / n);
      ps.push_back(c.at(ts.back()));
    }
  };
  std::vector<double> ta, tb;
  std::vector<glm::dvec2> pa, pb;
  sample(a, ta, pa);
  sample(b, tb, pb);
  // Chunks of 16 segments with their boxes, so most pairs are skipped.
  struct Box {
    size_t i0, i1;
    glm::dvec2 lo, hi;
  };
  const auto boxes = [](const std::vector<glm::dvec2>& ps) {
    std::vector<Box> out;
    for (size_t i = 0; i + 1 < ps.size(); i += 16) {
      Box bx{i, std::min(i + 16, ps.size() - 1), ps[i], ps[i]};
      for (size_t k = bx.i0; k <= bx.i1; k++) {
        bx.lo = glm::min(bx.lo, ps[k]);
        bx.hi = glm::max(bx.hi, ps[k]);
      }
      out.push_back(bx);
    }
    return out;
  };
  const std::vector<Box> ba = boxes(pa), bb = boxes(pb);
  const auto cross = [](glm::dvec2 u, glm::dvec2 v) {
    return u.x * v.y - u.y * v.x;
  };
  const auto keep = [&](glm::dvec2 p) {
    for (const glm::dvec2& q : out)
      if (glm::length(q - p) < 1e-9)
        return;
    out.push_back(p);
  };
  for (const Box& x : ba)
    for (const Box& y : bb) {
      if (x.hi.x < y.lo.x || y.hi.x < x.lo.x || x.hi.y < y.lo.y ||
          y.hi.y < x.lo.y)
        continue;
      for (size_t i = x.i0; i < x.i1; i++)
        for (size_t j = y.i0; j < y.i1; j++) {
          const glm::dvec2 r = pa[i + 1] - pa[i], q = pb[j + 1] - pb[j];
          const double den = cross(r, q);
          if (std::abs(den) < 1e-300)
            continue;
          const double s = cross(pb[j] - pa[i], q) / den;
          const double t = cross(pb[j] - pa[i], r) / den;
          if (s < 0.0 || s > 1.0 || t < 0.0 || t > 1.0)
            continue;
          // Newton on F(u, w) = a(u) - b(w), from the polylines' crossing.
          double u = ta[i] + s * (ta[i + 1] - ta[i]);
          double w = tb[j] + t * (tb[j + 1] - tb[j]);
          const double hu = 1e-7 * (a.t1 - a.t0), hw = 1e-7 * (b.t1 - b.t0);
          bool ok = false;
          for (int it = 0; it < 60; it++) {
            const glm::dvec2 F = a.at(u) - b.at(w);
            if (glm::length(F) < 1e-13 * (1.0 + glm::length(a.at(u)))) {
              ok = true;
              break;
            }
            const glm::dvec2 du = (a.at(u + hu) - a.at(u - hu)) / (2.0 * hu);
            const glm::dvec2 dw = (b.at(w + hw) - b.at(w - hw)) / (2.0 * hw);
            // J = [du, -dw]; solve J [x, y] = -F.
            const double det = cross(du, -dw);
            if (std::abs(det) < 1e-300)
              break;
            const double x = cross(-F, -dw) / det;
            const double y = cross(du, -F) / det;
            u += x;
            w += y;
            if (std::abs(x) < 1e-16 * (a.t1 - a.t0) &&
                std::abs(y) < 1e-16 * (b.t1 - b.t0)) {
              ok = glm::length(a.at(u) - b.at(w)) < 1e-9;
              break;
            }
          }
          const double su = 1e-12 * (a.t1 - a.t0), sw = 1e-12 * (b.t1 - b.t0);
          if (ok && u >= a.t0 - su && u <= a.t1 + su && w >= b.t0 - sw &&
              w <= b.t1 + sw)
            keep(a.at(std::clamp(u, a.t0, a.t1)));
        }
    }
  return out;
}

} // namespace

bool Splittable(const Entity& e) {
  if (const auto* bs = std::get_if<BSpline>(&e))
    return bs->poles.size() >= 2 && BSplineDegree(*bs) >= 1;
  return Simple(e) || std::holds_alternative<Ellipse>(e) ||
         std::holds_alternative<EllipseArc>(e);
}

size_t CutsFor(const Entity& e) {
  return std::holds_alternative<Circle>(e) || std::holds_alternative<Ellipse>(e)
             ? 2
             : 1;
}

std::optional<double> CurveParam(const Entity& e, glm::dvec2 p) {
  if (const auto* el = std::get_if<Ellipse>(&e))
    return Into(EllipseParamOfPoint(el->center, el->rMajor, el->rMinor,
                                    el->rotation, p),
                0.0);
  if (const auto* ea = std::get_if<EllipseArc>(&e)) {
    double en = ea->endParam;
    while (en <= ea->startParam)
      en += kTwoPi;
    const double t = Into(EllipseParamOfPoint(ea->center, ea->rMajor,
                                              ea->rMinor, ea->rotation, p),
                          ea->startParam);
    // Past the sweep: the nearer end.
    if (t > en)
      return t - en < ea->startParam + kTwoPi - t ? en : ea->startParam;
    return t;
  }
  if (const auto* bs = std::get_if<BSpline>(&e))
    return SplineParamNearest(*bs, p);
  return std::nullopt;
}

double CurveLength(const Entity& e) {
  if (const auto* l = std::get_if<Line>(&e))
    return glm::length(l->b - l->a);
  if (const auto* c = std::get_if<Circle>(&e))
    return kTwoPi * c->radius;
  if (const auto* a = std::get_if<Arc>(&e))
    return a->radius * (SweepEnd(*a) - a->startAngle);
  const auto c = ParamOf(e);
  if (!c)
    return 0.0;
  const int n = 8 * std::max(c->samples, 8);
  double len = 0.0;
  glm::dvec2 prev = c->at(c->t0);
  for (int i = 1; i <= n; i++) {
    const glm::dvec2 q = c->at(c->t0 + (c->t1 - c->t0) * i / n);
    len += glm::length(q - prev);
    prev = q;
  }
  return len;
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
  } else if (const auto* bs = std::get_if<BSpline>(&e)) {
    if (!Splittable(e))
      return std::nullopt;
    q = BSplineEvalUV(*bs, SplineParamNearest(*bs, p));
  } else if (const auto c = ParamOf(e)) {
    q = c->at(NearestParam(*c, p));
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
  if (const auto* ea = std::get_if<EllipseArc>(&e))
    return glm::length(p - EllipseArcPointUV(*ea, 1)) < tol ||
           glm::length(p - EllipseArcPointUV(*ea, 2)) < tol;
  if (const auto* bs = std::get_if<BSpline>(&e))
    return !bs->poles.empty() && (glm::length(p - bs->poles.front()) < tol ||
                                  glm::length(p - bs->poles.back()) < tol);
  return false;
}

std::vector<glm::dvec2> Crossings(const Entity& e, const Entity& other) {
  if (Simple(e))
    return CrossingsSimple(e, other);
  if (!Splittable(e))
    return {};
  if (Simple(other))
    return CrossingsSimple(other, e);
  const auto a = ParamOf(e), b = ParamOf(other);
  if (!a || !b)
    return {};
  return CrossingsGeneral(*a, *b);
}

namespace {
std::vector<glm::dvec2> CrossingsSimple(const Entity& e, const Entity& other) {
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
} // namespace

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
  } else if (const auto* ea = std::get_if<EllipseArc>(&e)) {
    if (cuts.size() != 1)
      return out;
    double en = ea->endParam;
    while (en <= ea->startParam)
      en += kTwoPi;
    const double t = *CurveParam(e, cuts[0]);
    EllipseArc a1 = *ea, a2 = *ea;
    a1.startParam = ea->startParam;
    a1.endParam = t;
    a2.startParam = t;
    a2.endParam = en;
    piece(a1, true);
    piece(a2, false);
  } else if (const auto* el = std::get_if<Ellipse>(&e)) {
    if (cuts.size() != 2)
      return out;
    const double t1 = *CurveParam(e, cuts[0]);
    double t2 = Into(*CurveParam(e, cuts[1]), t1);
    if (t2 <= t1)
      t2 += kTwoPi;
    const auto arc = [&](double s0, double s1) {
      return EllipseArc{el->center,   el->rMajor, el->rMinor,
                        el->rotation, s0,         s1};
    };
    piece(arc(t1, t2), true);
    piece(arc(t2, t1 + kTwoPi), false);
  } else if (const auto* bs = std::get_if<BSpline>(&e)) {
    if (cuts.size() != 1)
      return out;
    const auto cut = CutSpline(*bs, SplineParamNearest(*bs, cuts[0]));
    if (!cut)
      return out;
    std::vector<BSpline> splines;
    for (const SplinePiece& p : cut->pieces)
      splines.push_back(p.spline);
    const size_t keeper = KeeperOf(splines);
    for (size_t i = 0; i < splines.size(); i++)
      piece(splines[i], i == keeper);
  }
  return out;
}

size_t KeeperOf(const std::vector<BSpline>& pieces) {
  size_t best = 0;
  double bestLen = -1.0;
  for (size_t i = 0; i < pieces.size(); i++) {
    const double len = CurveLength(pieces[i]);
    if (len > bestLen + 1e-12) {
      bestLen = len;
      best = i;
    }
  }
  return best;
}

glm::dvec2 TangentAt(const Entity& e, glm::dvec2 p) {
  glm::dvec2 d{1.0, 0.0};
  if (const auto* l = std::get_if<Line>(&e)) {
    d = l->b - l->a;
  } else if (const auto* bs = std::get_if<BSpline>(&e)) {
    if (Splittable(e))
      d = BSplineDerivUV(*bs, SplineParamNearest(*bs, p));
  } else if (const auto c = ParamOf(e)) {
    const double t = NearestParam(*c, p);
    const double h = 1e-6 * (c->t1 - c->t0);
    d = c->at(std::min(t + h, c->t1)) - c->at(std::max(t - h, c->t0));
  }
  const double len = glm::length(d);
  return len > 0.0 ? d / len : glm::dvec2(1.0, 0.0);
}

const char* ConstraintWord(ConstraintKind k) {
  using K = ConstraintKind;
  switch (k) {
  case K::Coincident:
    return "coincident";
  case K::Horizontal:
    return "horizontal";
  case K::Vertical:
    return "vertical";
  case K::Parallel:
    return "parallel";
  case K::Perpendicular:
    return "perpendicular";
  case K::Equal:
    return "equal";
  case K::Distance:
    return "distance";
  case K::Radius:
    return "radius";
  case K::Angle:
    return "angle";
  case K::Tangent:
    return "tangent";
  case K::PointOnLine:
    return "point_on_line";
  case K::Symmetric:
    return "symmetric";
  case K::Midpoint:
    return "midpoint";
  case K::Fixed:
    return "fixed";
  case K::Concentric:
    return "concentric";
  case K::Collinear:
    return "collinear";
  case K::DistanceX:
    return "distance_x";
  case K::DistanceY:
    return "distance_y";
  case K::Diameter:
    return "diameter";
  case K::DistancePL:
    return "distance_pl";
  case K::Curvature:
    return "curvature";
  case K::ArcLength:
    return "arc_length";
  case K::SplineJoint:
    return "smooth";
  }
  return "constraint";
}

const char* ConstraintPhrase(ConstraintKind k) {
  using K = ConstraintKind;
  switch (k) {
  case K::PointOnLine:
    return "point on curve";
  case K::DistanceX:
    return "horizontal distance";
  case K::DistanceY:
    return "vertical distance";
  case K::DistancePL:
    return "distance";
  case K::ArcLength:
    return "arc length";
  default:
    return ConstraintWord(k);
  }
}

namespace {

// 1st, 2nd, 3rd, 4th, ..., 11th, 12th, 13th, 21st.
std::string Ordinal(size_t n) {
  const size_t tens = n % 100;
  const char* suffix = "th";
  if (tens < 11 || tens > 13) {
    if (n % 10 == 1)
      suffix = "st";
    else if (n % 10 == 2)
      suffix = "nd";
    else if (n % 10 == 3)
      suffix = "rd";
  }
  return std::to_string(n) + suffix;
}

} // namespace

std::string DescribeCutDrop(const SketchFeature::CutDrop& d,
                            const units::System& sys) {
  using Where = SketchFeature::CutDrop::Where;
  // The poles a person counts from one: "the spline's 3rd pole", "the
  // spline's 2nd and 3rd poles".
  std::string poles;
  for (size_t i = 0; i < d.poles.size(); i++)
    poles += (i == 0                    ? ""
              : i + 1 == d.poles.size() ? " and "
                                        : ", ") +
             Ordinal(d.poles[i] + 1);
  poles = "the spline's " + poles + (d.poles.size() == 1 ? " pole" : " poles");
  if (d.constraint < 0) {
    const auto pf = PoleField(d.field);
    if (!pf)
      return "the expression on the spline's " + d.field;
    return "the expression on " + poles + "'s " + (pf->second ? "x" : "y");
  }
  std::string what = ConstraintPhrase(d.kind);
  if (IsDimension(d.kind)) {
    const std::string value =
        d.kind != ConstraintKind::Angle ? std::string()
        : sys.angle == units::Angle::Radian
            ? units::FormatNumber(d.value, 3) + " rad"
            : units::FormatNumber(angles::ToDegrees(d.value), 3) + "\xc2\xb0";
    const std::string shown =
        !value.empty()
            ? value
            : units::FormatNumber(d.value / units::MmPerLength(sys.length), 3) +
                  " " + units::ToString(sys.length);
    what += " " + (d.driven ? "(" + shown + ")" : shown);
  }
  switch (d.where) {
  case Where::Poles:
    return what + " on " + poles;
  case Where::RemovedEnd:
    return what + " at the end of the spline the cut removes";
  case Where::RemovedPart:
    return what + " on the part of the spline the cut removes";
  }
  return what;
}

} // namespace sketch
