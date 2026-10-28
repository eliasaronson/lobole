// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/ContactFollower.h"

#include "cad/CADKernel.h"
#include "cad/NonDestructiveBoolean.h"
#include "core/Angles.h"
#include "core/AssemblyLoopSolve.h"
#include "core/Conventions.h"
#include "core/Document.h"
#include "core/MassProps.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepLProp_CLProps.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <numeric>

namespace contact {

namespace {

constexpr double kPi = angles::kPi;
constexpr double kTwoPi = angles::kTwoPi;

glm::dvec2 J(glm::dvec2 v) { return {-v.y, v.x}; }
glm::dvec2 Rot(double a, glm::dvec2 v) {
  const double c = std::cos(a), s = std::sin(a);
  return {c * v.x - s * v.y, s * v.x + c * v.y};
}
double Wrap2Pi(double a) {
  a = std::fmod(a, kTwoPi);
  return a < 0.0 ? a + kTwoPi : a;
}

std::string Num(double v) {
  char b[48];
  std::snprintf(b, sizeof b, "%.4g", v);
  return b;
}

// Where segment p0-p1 meets the circle |x - c| = r: up to two points.
int CircleSegment(glm::dvec2 c, double r, glm::dvec2 p0, glm::dvec2 p1,
                  glm::dvec2 out[2]) {
  const glm::dvec2 d = p1 - p0, f = p0 - c;
  const double A = glm::dot(d, d);
  if (A < 1e-30)
    return 0;
  const double B = 2.0 * glm::dot(f, d);
  const double C = glm::dot(f, f) - r * r;
  const double disc = B * B - 4.0 * A * C;
  if (disc < 0.0)
    return 0;
  const double sq = std::sqrt(disc);
  int n = 0;
  for (double l : {(-B - sq) / (2.0 * A), (-B + sq) / (2.0 * A)}) {
    if (l < 0.0 || l > 1.0)
      continue;
    if (n == 1 && sq == 0.0)
      break;
    out[n++] = p0 + l * d;
  }
  return n;
}

// The distance range from c to segment p0-p1.
std::pair<double, double> SegmentRange(glm::dvec2 c, glm::dvec2 p0,
                                       glm::dvec2 p1) {
  const glm::dvec2 d = p1 - p0;
  const double l2 = glm::dot(d, d);
  double t = l2 > 0.0 ? glm::dot(c - p0, d) / l2 : 0.0;
  t = std::clamp(t, 0.0, 1.0);
  const double lo = glm::length(p0 + t * d - c);
  const double hi = std::max(glm::length(p0 - c), glm::length(p1 - c));
  return {lo, hi};
}

struct Candidate {
  double q = 0.0;
  glm::dvec2 point{0.0};
  bool camEdge = false; // a follower vertex on a cam segment (else the reverse)
  int camA = 0, camB = 0; // the cam segment, or the cam vertex (camA)
  int folA = 0, folB = 0; // the follower vertex (folA), or its segment
  double lambda = 0.0;    // along whichever is the segment
  double lever = 1.0;     // q to distance at the point
};

// Signed radius at a contact: + convex toward the other body.
double SignedRadius(double radius, glm::dvec2 centre, glm::dvec2 p,
                    glm::dvec2 nTowardOther) {
  if (radius <= 0.0)
    return 0.0;
  // Convex when the centre lies on the body's own side, away from the other.
  return glm::dot(centre - p, nTowardOther) < 0.0 ? radius : -radius;
}

} // namespace

Outline SectionOutline(const TopoDS_Shape& shape, double z, double deflection,
                       std::string* why) {
  Outline o;
  const double half = 1e5;
  const TopoDS_Face plane =
      BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0.0, 0.0, z), gp_Dir(0, 0, 1)),
                              -half, half, -half, half)
          .Face();
  BRepAlgoAPI_Section sec;
  sec.Approximation(Standard_True);
  sec.ComputePCurveOn1(Standard_False);
  sec.ComputePCurveOn2(Standard_False);
  if (!cad::BuildNonDestructive(sec, shape, plane) || sec.Shape().IsNull()) {
    if (why)
      *why = "sectioning it across the cam's axis failed";
    return o;
  }
  o.lo = glm::dvec2(std::numeric_limits<double>::infinity());
  o.hi = -o.lo;
  for (TopExp_Explorer ex(sec.Shape(), TopAbs_EDGE); ex.More(); ex.Next()) {
    const TopoDS_Edge e = TopoDS::Edge(ex.Current());
    BRepAdaptor_Curve c(e);
    GCPnts_QuasiUniformDeflection sampler(c, deflection);
    if (!sampler.IsDone() || sampler.NbPoints() < 2)
      continue;
    const int first = static_cast<int>(o.pts.size());
    for (int i = 1; i <= sampler.NbPoints(); i++) {
      const double u = sampler.Parameter(i);
      BRepLProp_CLProps props(c, u, 2, 1e-9);
      const gp_Pnt p = props.Value();
      o.pts.push_back({p.X(), p.Y()});
      double radius = 0.0;
      glm::dvec2 centre{p.X(), p.Y()};
      if (props.IsTangentDefined()) {
        const double k = props.Curvature();
        if (k > 1e-12) {
          gp_Pnt cc;
          props.CentreOfCurvature(cc);
          radius = 1.0 / k;
          centre = {cc.X(), cc.Y()};
        }
      }
      o.radius.push_back(radius);
      o.centre.push_back(centre);
      glm::dvec2 tan{0.0};
      if (props.IsTangentDefined()) {
        gp_Dir td;
        props.Tangent(td);
        tan = glm::normalize(glm::dvec2(td.X(), td.Y()));
      }
      o.tangent.push_back(tan);
      o.lo = glm::min(o.lo, o.pts.back());
      o.hi = glm::max(o.hi, o.pts.back());
      if (i > 1)
        o.segs.push_back({static_cast<int>(o.pts.size()) - 2,
                          static_cast<int>(o.pts.size()) - 1});
    }
    (void)first;
  }
  if (o.segs.empty()) {
    if (why)
      *why = "the plane across the cam's axis misses it";
    o.pts.clear();
  }
  return o;
}

Touch FirstTouch(const Outline& cam, double camAngle, const Outline& fol,
                 const Path& path, int toward, double travelRef, double tol) {
  Touch out;
  const double c = std::cos(camAngle), s = std::sin(camAngle);
  std::vector<glm::dvec2> cp(cam.pts.size());
  for (size_t i = 0; i < cp.size(); i++)
    cp[i] = {c * cam.pts[i].x - s * cam.pts[i].y,
             s * cam.pts[i].x + c * cam.pts[i].y};

  // Each candidate is a q at which a vertex of one outline lands on a
  // segment of the other as the follower is carried along its path.
  std::vector<Candidate> cands;
  // The coordinate the path preserves (the perpendicular offset for a slide,
  // the radius about the pivot for a turn), per vertex.
  const auto keep = [&](glm::dvec2 p) {
    return path.turn ? glm::length(p - path.pivot) : glm::dot(p, J(path.dir));
  };
  std::vector<std::pair<double, int>> fk(fol.pts.size());
  for (size_t i = 0; i < fol.pts.size(); i++)
    fk[i] = {keep(fol.pts[i]), static_cast<int>(i)};
  std::sort(fk.begin(), fk.end());
  if (fk.empty() || cp.empty())
    return out;
  const double fLo = fk.front().first, fHi = fk.back().first;
  std::vector<std::pair<double, int>> ck;
  for (size_t i = 0; i < cp.size(); i++) {
    const double k = keep(cp[i]);
    if (k >= fLo - tol && k <= fHi + tol)
      ck.push_back({k, static_cast<int>(i)});
  }
  std::sort(ck.begin(), ck.end());

  const auto range = [&](glm::dvec2 p0, glm::dvec2 p1) {
    if (path.turn)
      return SegmentRange(path.pivot, p0, p1);
    const double a = glm::dot(p0, J(path.dir)), b = glm::dot(p1, J(path.dir));
    return std::pair{std::min(a, b), std::max(a, b)};
  };
  const auto inRange = [](const std::vector<std::pair<double, int>>& v,
                          double lo, double hi, auto&& fn) {
    auto it = std::lower_bound(v.begin(), v.end(), std::pair{lo, -1});
    for (; it != v.end() && it->first <= hi; ++it)
      fn(it->second);
  };
  const double sense = path.sense;
  const double perpTol = 1e-12;

  // A: follower vertices onto cam segments.
  for (const auto& [ia, ib] : cam.segs) {
    const glm::dvec2 p0 = cp[ia], p1 = cp[ib];
    const auto [lo, hi] = range(p0, p1);
    if (hi < fLo || lo > fHi)
      continue;
    inRange(fk, lo, hi, [&](int k) {
      const glm::dvec2 f = fol.pts[k];
      if (!path.turn) {
        const glm::dvec2 perp = J(path.dir);
        const double w0 = glm::dot(p0, perp), w1 = glm::dot(p1, perp);
        if (std::abs(w1 - w0) < perpTol)
          return;
        const double l = (glm::dot(f, perp) - w0) / (w1 - w0);
        if (l < 0.0 || l > 1.0)
          return;
        const glm::dvec2 x = p0 + l * (p1 - p0);
        cands.push_back(
            {glm::dot(x - f, path.dir), x, true, ia, ib, k, 0, l, 1.0});
      } else {
        const double rho = glm::length(f - path.pivot);
        glm::dvec2 xs[2];
        const int n = CircleSegment(path.pivot, rho, p0, p1, xs);
        const double af = std::atan2(f.y - path.pivot.y, f.x - path.pivot.x);
        for (int m = 0; m < n; m++) {
          const glm::dvec2 x = xs[m];
          const double ax = std::atan2(x.y - path.pivot.y, x.x - path.pivot.x);
          const double l = glm::length(x - p0) / glm::length(p1 - p0);
          cands.push_back({sense * (ax - af), x, true, ia, ib, k, 0, l, rho});
        }
      }
    });
  }
  // B: cam vertices onto follower segments (at q = 0, carried back).
  for (const auto& [ua, ub] : fol.segs) {
    const glm::dvec2 u = fol.pts[ua], v = fol.pts[ub];
    const auto [lo, hi] = range(u, v);
    inRange(ck, lo, hi, [&](int k) {
      const glm::dvec2 cv = cp[k];
      if (!path.turn) {
        const glm::dvec2 perp = J(path.dir);
        const double w0 = glm::dot(u, perp), w1 = glm::dot(v, perp);
        if (std::abs(w1 - w0) < perpTol)
          return;
        const double l = (glm::dot(cv, perp) - w0) / (w1 - w0);
        if (l < 0.0 || l > 1.0)
          return;
        const glm::dvec2 y = u + l * (v - u);
        cands.push_back(
            {glm::dot(cv - y, path.dir), cv, false, k, 0, ua, ub, l, 1.0});
      } else {
        const double rho = glm::length(cv - path.pivot);
        glm::dvec2 ys[2];
        const int n = CircleSegment(path.pivot, rho, u, v, ys);
        const double ac = std::atan2(cv.y - path.pivot.y, cv.x - path.pivot.x);
        for (int m = 0; m < n; m++) {
          const glm::dvec2 y = ys[m];
          const double ay = std::atan2(y.y - path.pivot.y, y.x - path.pivot.x);
          const double l = glm::length(y - u) / glm::length(v - u);
          cands.push_back({sense * (ac - ay), cv, false, k, 0, ua, ub, l, rho});
        }
      }
    });
  }
  if (cands.empty())
    return out;

  // The first reached, moving q in the `toward` sense from clear. A turn's
  // q is periodic: each candidate is taken in the turn that starts half a
  // turn behind the reference.
  if (path.turn) {
    const double start = travelRef - toward * kPi;
    for (Candidate& cd : cands)
      cd.q = start + toward * Wrap2Pi(toward * (cd.q - start));
  }
  size_t best = 0;
  for (size_t i = 1; i < cands.size(); i++)
    if (toward > 0 ? cands[i].q < cands[best].q : cands[i].q > cands[best].q)
      best = i;
  const Candidate& b = cands[best];
  out.found = true;
  out.travel = b.q;
  out.point = b.point;

  // The follower carried to the touch, for its segment and its centres.
  const auto carry = [&](glm::dvec2 p) {
    if (!path.turn)
      return p + b.q * path.dir;
    return path.pivot + Rot(sense * b.q, p - path.pivot);
  };
  out.motion =
      path.turn ? sense * glm::normalize(J(b.point - path.pivot)) : path.dir;
  // The normal from the curves' own tangents at the segment's ends, blended
  // along it: a chord's direction is off by up to half the angle it spans.
  glm::dvec2 seg = b.camEdge ? cp[b.camB] - cp[b.camA]
                             : carry(fol.pts[b.folB]) - carry(fol.pts[b.folA]);
  {
    const auto& tv = b.camEdge ? cam.tangent : fol.tangent;
    const int i0 = b.camEdge ? b.camA : b.folA,
              i1 = b.camEdge ? b.camB : b.folB;
    glm::dvec2 t = (1.0 - b.lambda) * tv[i0] + b.lambda * tv[i1];
    if (glm::length(t) > 0.5 &&
        glm::dot(t, b.camEdge
                        ? Rot(-camAngle, seg)
                        : (path.turn ? Rot(-sense * b.q, seg) : seg)) > 0.0) {
      t = glm::normalize(t);
      seg =
          b.camEdge ? Rot(camAngle, t) : (path.turn ? Rot(sense * b.q, t) : t);
    }
  }
  glm::dvec2 n = glm::normalize(J(seg));
  if (glm::dot(n, out.motion * double(toward)) > 0.0)
    n = -n;
  out.normal = n;
  const int camV = b.camEdge ? (b.lambda < 0.5 ? b.camA : b.camB) : b.camA;
  const int folV = b.camEdge ? b.folA : (b.lambda < 0.5 ? b.folA : b.folB);
  out.camRadius = SignedRadius(cam.radius[camV],
                               Rot(camAngle, cam.centre[camV]), b.point, n);
  out.followerRadius =
      SignedRadius(fol.radius[folV], carry(fol.centre[folV]), b.point, -n);

  // Refine: the polyline's answer is within its chords' deflection, and its
  // normal within a chord's angle. Locally each curve is its osculating
  // circle (or its line), and two of those touch where a closed form says,
  // so the answer is solved again on them, from the polyline's q. Exact for
  // circles and lines; second order on a spline.
  {
    const auto carryAt = [&](glm::dvec2 p, double q) {
      if (!path.turn)
        return p + q * path.dir;
      return path.pivot + Rot(sense * q, p - path.pivot);
    };
    const auto turnAt = [&](glm::dvec2 v, double q) {
      return path.turn ? Rot(sense * q, v) : v;
    };
    const bool camCircle = cam.radius[camV] > 0.0;
    const bool folCircle = fol.radius[folV] > 0.0;
    const double rc = std::abs(out.camRadius),
                 rf = std::abs(out.followerRadius);
    const double sc = out.camRadius >= 0.0 ? 1.0 : -1.0;
    const double sf = out.followerRadius >= 0.0 ? 1.0 : -1.0;
    const glm::dvec2 Cc = Rot(camAngle, cam.centre[camV]);
    const glm::dvec2 Pc = cp[camV];
    const glm::dvec2 Cf0 = fol.centre[folV], Pf0 = fol.pts[folV];
    // The follower line's normal toward the cam, at q = 0.
    glm::dvec2 mf0{0.0};
    if (!folCircle && glm::length(fol.tangent[folV]) > 0.5) {
      mf0 = J(fol.tangent[folV]);
      if (glm::dot(turnAt(mf0, b.q), n) > 0.0)
        mf0 = -mf0;
    }
    std::function<double(double)> gap;
    if (camCircle && folCircle)
      gap = [&](double q) {
        return glm::length(carryAt(Cf0, q) - Cc) - std::abs(sc * rc + sf * rf);
      };
    else if (!camCircle && folCircle && sf > 0.0)
      gap = [&](double q) { return glm::dot(n, carryAt(Cf0, q) - Pc) - rf; };
    else if (camCircle && !folCircle && sc > 0.0 && glm::length(mf0) > 0.5)
      gap = [&](double q) {
        return glm::dot(turnAt(mf0, q), Cc - carryAt(Pf0, q)) - rc;
      };
    if (gap) {
      double q = b.q;
      bool ok = false;
      for (int it = 0; it < 12; it++) {
        const double h = 1e-7 * std::max(1.0, std::abs(q));
        const double g = gap(q);
        const double dg = (gap(q + h) - gap(q - h)) / (2.0 * h);
        if (!(std::abs(dg) > 1e-12))
          break;
        const double step = g / dg;
        q -= step;
        if (std::abs(step) < 1e-13 * std::max(1.0, std::abs(q))) {
          ok = true;
          break;
        }
      }
      if (ok && std::abs(q - b.q) * b.lever <= 2.0 * tol) {
        out.travel = q;
        glm::dvec2 nn;
        glm::dvec2 pt;
        if (camCircle && folCircle) {
          nn = glm::normalize(carryAt(Cf0, q) - Cc);
          if (glm::dot(nn, n) < 0.0)
            nn = -nn;
          pt = Cc + sc * rc * nn;
        } else if (!camCircle) {
          nn = n;
          pt = carryAt(Cf0, q) - rf * nn;
        } else {
          nn = -turnAt(mf0, q);
          pt = Cc + rc * nn;
        }
        out.normal = n = nn;
        out.point = pt;
        out.motion =
            path.turn ? sense * glm::normalize(J(pt - path.pivot)) : path.dir;
      }
    }
  }

  // A second point touching within the tolerance, well apart from the
  // first: the follower bridges a hollow.
  const double apart = 0.02 * std::min(cam.Size(), fol.Size());
  for (const Candidate& cd : cands) {
    if (std::abs(cd.q - b.q) * cd.lever > tol)
      continue;
    if (glm::length(cd.point - b.point) > apart) {
      out.bridge = true;
      out.point2 = cd.point;
      break;
    }
  }
  return out;
}

namespace {

// Past this pressure angle a follower falling toward its cam is sliding
// down a step face, not driven: the interval is part of a drop.
constexpr double kDropPressureDeg = 75.0;

struct TNode {
  double a = 0.0, q = 0.0;
  bool bridge = false;
  // The interval to the next node:
  bool hasMid = false, ok = false, drop = false;
  double midQ = 0.0;
};

template <class N> double NodeTangent(const std::vector<N>& n, size_t k) {
  const size_t m = n.size();
  const size_t kp = (k + m - 1) % m, kn = (k + 1) % m;
  const double h0 =
      Wrap2Pi(n[k].a - n[kp].a) == 0.0 ? kTwoPi : Wrap2Pi(n[k].a - n[kp].a);
  const double h1 =
      Wrap2Pi(n[kn].a - n[k].a) == 0.0 ? kTwoPi : Wrap2Pi(n[kn].a - n[k].a);
  const double d0 = (n[k].q - n[kp].q) / h0;
  const double d1 = (n[kn].q - n[k].q) / h1;
  const bool dropBefore = n[kp].drop, dropAfter = n[k].drop;
  if (dropBefore && dropAfter)
    return 0.0;
  if (dropBefore)
    return d1;
  if (dropAfter)
    return d0;
  return (h1 * d0 + h0 * d1) / (h0 + h1);
}

template <class N>
double Interp(const std::vector<N>& n, double a, double* slope) {
  a = Wrap2Pi(a);
  const size_t m = n.size();
  // The interval holding a: the last node at or before it, wrapping.
  auto it = std::upper_bound(n.begin(), n.end(), a,
                             [](double v, const N& t) { return v < t.a; });
  const size_t i = it == n.begin() ? m - 1 : size_t(it - n.begin()) - 1;
  const size_t j = (i + 1) % m;
  double h = n[j].a - n[i].a;
  double u = a - n[i].a;
  if (h <= 0.0)
    h += kTwoPi;
  if (u < 0.0)
    u += kTwoPi;
  const double t = u / h;
  if (n[i].drop) {
    if (slope)
      *slope = (n[j].q - n[i].q) / h;
    return n[i].q + t * (n[j].q - n[i].q);
  }
  const double p0 = n[i].q, p1 = n[j].q;
  const double m0 = NodeTangent(n, i) * h, m1 = NodeTangent(n, j) * h;
  const double t2 = t * t, t3 = t2 * t;
  if (slope)
    *slope = ((6 * t2 - 6 * t) * p0 + (3 * t2 - 4 * t + 1) * m0 +
              (-6 * t2 + 6 * t) * p1 + (3 * t2 - 2 * t) * m1) /
             h;
  return (2 * t3 - 3 * t2 + 1) * p0 + (t3 - 2 * t2 + t) * m0 +
         (-2 * t3 + 3 * t2) * p1 + (t3 - t2) * m1;
}

} // namespace

Table BuildTable(const Outline& cam, double camSense, const Outline& fol,
                 const Path& path, int toward, double tol) {
  Table out;
  // A turn's q is radians; its tolerance is the contact tolerance over the
  // arm's reach.
  double qTol = tol;
  if (path.turn) {
    double reach = 0.0;
    for (glm::dvec2 p : fol.pts)
      reach = std::max(reach, glm::length(p - path.pivot));
    qTol = tol / std::max(reach, 1e-9);
  }
  out.tolerance = qTol;
  double ref = 0.0;
  std::string failAt;
  const auto solve = [&](double a, bool* bridge) -> double {
    const Touch t = FirstTouch(cam, camSense * a, fol, path, toward, ref, tol);
    if (!t.found) {
      if (failAt.empty())
        failAt = Num(angles::ToDegrees(a));
      return 0.0;
    }
    if (bridge)
      *bridge = t.bridge;
    return t.travel;
  };
  std::vector<TNode> n;
  const int n0 = 72;
  for (int k = 0; k < n0; k++) {
    TNode t;
    t.a = kTwoPi * k / n0;
    t.q = solve(t.a, &t.bridge);
    n.push_back(t);
  }
  const size_t kMaxNodes = 40000;
  bool changed = true;
  while (changed && failAt.empty()) {
    changed = false;
    for (size_t i = 0; i < n.size() && failAt.empty(); i++) {
      TNode& cur = n[i];
      if (cur.ok || cur.drop)
        continue;
      const size_t j = (i + 1) % n.size();
      double w = n[j].a - cur.a;
      if (w <= 0.0)
        w += kTwoPi;
      const double mid = Wrap2Pi(cur.a + 0.5 * w);
      if (!cur.hasMid) {
        cur.midQ = solve(mid, nullptr);
        cur.hasMid = true;
      }
      const double err = std::abs(Interp(n, mid, nullptr) - cur.midQ);
      if (err <= 0.25 * qTol) {
        cur.ok = true;
        continue;
      }
      if (w < 1e-6 || n.size() >= kMaxNodes) {
        if (std::abs(n[j].q - cur.q) > 10.0 * qTol && w < 1e-6) {
          cur.drop = true;
        } else {
          cur.ok = true;
          out.bound = std::max(out.bound, err);
        }
        continue;
      }
      TNode ins;
      ins.a = mid;
      ins.q = cur.midQ;
      solve(mid, &ins.bridge);
      cur.hasMid = false;
      // The neighbours' tangents change with the new node: look again.
      n[(i + n.size() - 1) % n.size()].ok = false;
      n[j].ok = false;
      // Insert keeping the order (mid may wrap past 2pi to the front).
      const auto at =
          std::upper_bound(n.begin(), n.end(), ins.a,
                           [](double v, const TNode& t) { return v < t.a; });
      n.insert(at, ins);
      changed = true;
      i++; // skip the node just added this pass
    }
  }
  if (!failAt.empty()) {
    out.error = "the follower never touches the cam at a driver angle of " +
                failAt +
                " degrees: carried toward it, it passes clear (it would "
                "travel without end), so there is no pose to give it";
    return out;
  }
  for (size_t i = 0; i < n.size(); i++) {
    const TNode& t = n[i];
    if (t.ok && t.hasMid)
      out.bound = std::max(
          out.bound,
          std::abs(Interp(n,
                          Wrap2Pi(t.a +
                                  0.5 * Wrap2Pi(n[(i + 1) % n.size()].a - t.a)),
                          nullptr) -
                   t.midQ));
  }
  // Steps less than a degree apart are one drop: the intervals between
  // them are the follower riding down the step face, read linearly.
  const double joinGap = angles::ToRadians(1.0);
  for (size_t i = 0; i < n.size(); i++) {
    if (!n[i].drop)
      continue;
    size_t j = i + 1;
    while (j < n.size() && !n[j].drop && n[j].a - n[i + 1].a < joinGap)
      j++;
    if (j < n.size() && n[j].drop)
      for (size_t k = i + 1; k < j; k++)
        n[k].drop = true;
  }
  out.nodes.reserve(n.size());
  for (size_t i = 0; i < n.size(); i++)
    out.nodes.push_back({n[i].a, n[i].q, n[i].drop});
  // A drop, for what follows the follower, is more than a step: a rounded
  // tip slides down a snail's step face before it falls clear, the contact
  // normal nearly square to its motion. A cam cannot drive a follower there,
  // so an interval falling toward the cam with the pressure angle over
  // kDropPressureDeg at both ends belongs to the drop it leads to.
  std::vector<double> pressure(n.size(), 0.0);
  for (size_t i = 0; i < n.size(); i++) {
    const Touch t =
        FirstTouch(cam, camSense * n[i].a, fol, path, toward, ref, tol);
    if (t.found)
      pressure[i] = angles::ToDegrees(std::acos(
          std::clamp(std::abs(glm::dot(t.normal, t.motion)), 0.0, 1.0)));
  }
  std::vector<bool> falling(n.size(), false);
  for (size_t i = 0; i < n.size(); i++) {
    const size_t j = (i + 1) % n.size();
    falling[i] = n[i].drop || (pressure[i] > kDropPressureDeg &&
                               pressure[j] > kDropPressureDeg &&
                               toward * (n[j].q - n[i].q) > 0.0);
  }
  for (size_t i = 0; i < n.size(); i++) {
    if (!falling[i])
      continue;
    size_t j = i + 1;
    while (j < n.size() && !falling[j] && n[j].a - n[i + 1].a < joinGap)
      j++;
    if (j < n.size() && falling[j])
      for (size_t k = i + 1; k < j; k++)
        falling[k] = true;
  }
  for (size_t i = 0; i < n.size(); i++) {
    if (!falling[i] || (i > 0 && falling[i - 1]))
      continue;
    size_t k = i;
    while (k + 1 < n.size() && falling[k + 1])
      k++;
    // ...and the whole descent it is part of: from the top it rolls off
    // (a rounded tip leaves a snail's corner before the face is steep) to
    // where it comes to rest.
    const size_t N = n.size();
    size_t first = i, back = 0;
    while (back++ < N && toward * (n[first].q - n[(first + N - 1) % N].q) > 0.0)
      first = (first + N - 1) % N;
    size_t last = (k + 1) % N, fwd = 0;
    while (fwd++ < N && toward * (n[(last + 1) % N].q - n[last].q) > qTol)
      last = (last + 1) % N;
    const TNode& top = n[first];
    const TNode& end = n[last];
    // A steep stretch that never steps is a fall only if it falls far: a
    // tolerance-sized wobble at a high pressure angle is not a drop.
    if (std::abs(end.q - top.q) > 10.0 * qTol)
      out.drops.push_back({top.a, top.q, end.q, end.a});
  }
  // Bridged ranges, from the nodes that bridge.
  for (size_t i = 0; i < n.size(); i++) {
    if (!n[i].bridge)
      continue;
    const size_t prev = (i + n.size() - 1) % n.size();
    if (n[prev].bridge && i != 0)
      continue;
    size_t k = i;
    while (n[(k + 1) % n.size()].bridge && (k + 1) % n.size() != i)
      k = (k + 1) % n.size();
    out.bridges.push_back({n[i].a, n[k].a});
  }
  return out;
}

double TableValue(const Table& t, double a, double* slope) {
  if (t.nodes.empty()) {
    if (slope)
      *slope = 0.0;
    return 0.0;
  }
  return Interp(t.nodes, a, slope);
}

// ---- the assembly side ------------------------------------------------------

namespace {

glm::dmat4 OccMatrix(const Occurrence& o) {
  return glm::translate(glm::dmat4(1.0), o.position) *
         glm::mat4_cast(o.orientation);
}

struct UnionFind {
  std::map<uint64_t, uint64_t> parent;
  uint64_t Find(uint64_t x) {
    auto it = parent.find(x);
    if (it == parent.end() || it->second == x)
      return x;
    const uint64_t r = Find(it->second);
    parent[x] = r;
    return r;
  }
  void Join(uint64_t a, uint64_t b) {
    a = Find(a);
    b = Find(b);
    if (a != b)
      parent[a] = b;
  }
};

UnionFind RigidGroups(const AssemblyDocument& asmDoc) {
  UnionFind uf;
  uint64_t firstFixed = 0;
  for (const Occurrence& o : asmDoc.GetOccurrences()) {
    if (o.attachedTo != 0)
      uf.Join(o.id, o.attachedTo);
    if (o.fixed) {
      if (firstFixed == 0)
        firstFixed = o.id;
      else
        uf.Join(o.id, firstFixed);
    }
  }
  for (const Mate& m : asmDoc.GetMates())
    if (m.type == MateType::Fastened && m.a.innerPath.empty() &&
        m.b.innerPath.empty())
      uf.Join(m.a.occurrenceId, m.b.occurrenceId);
  return uf;
}

std::string OccName(const AssemblyDocument& asmDoc, uint64_t id) {
  const Occurrence* o = asmDoc.FindById(id);
  return o ? "'" + o->name + "' (" + std::to_string(id) + ")"
           : std::to_string(id);
}

// One mate's pose of its moving side in its base side's connector frame.
glm::dmat4 MovingInBase(const Mate& m, MateDof dof, double value, bool baseIsA,
                        const glm::dmat4& fa, const glm::dmat4& fb) {
  Mate c = m;
  assembly::SetMateDof(c, dof, value);
  const glm::dmat4 adj = assembly::MateAdjustFor(
      c.flipped, c.rotation, c.offset, c.dx, c.dy, c.ballRotation);
  return baseIsA ? adj * glm::inverse(fb)
                 : glm::inverse(adj) * glm::inverse(fa);
}

struct Setup {
  const Mate* ma = nullptr;
  const Mate* mb = nullptr;
  bool aBaseIsA = true, bBaseIsA = true;
  glm::dmat4 faA{1.0}, fbA{1.0}, faB{1.0}, fbB{1.0};
  uint64_t baseA = 0, baseB = 0, movA = 0, movB = 0;
  uint64_t cam = 0, follower = 0;
  const Body* camBody = nullptr;
  const Body* folBody = nullptr;
  glm::dmat4 camExtra{1.0}, folExtra{1.0}; // moving occurrence -> cam/follower
  glm::dmat4 planeToB{1.0}; // b's base connector frame in the plane frame
  bool turn = false;
};

glm::dmat4 CamInPlane(const Setup& s, double a) {
  return MovingInBase(*s.ma, MateDof::Rotation, a, s.aBaseIsA, s.faA, s.fbA) *
         s.camExtra;
}
glm::dmat4 FollowerInPlane(const Setup& s, double q) {
  return s.planeToB *
         MovingInBase(*s.mb, s.turn ? MateDof::Rotation : MateDof::Offset, q,
                      s.bBaseIsA, s.faB, s.fbB) *
         s.folExtra;
}

const Body* PickBody(const Document* part, uint64_t featureId,
                     const std::string& role, const std::string& key,
                     std::string* why) {
  if (part == nullptr) {
    *why = "the " + role + "'s part is not loaded";
    return nullptr;
  }
  const auto& bodies = part->GetBodies();
  std::vector<const Body*> solids;
  for (const Body& b : bodies)
    if (b.shape.IsValid())
      solids.push_back(&b);
  if (featureId != 0) {
    for (const Body* b : solids)
      if (b->createdBy == featureId)
        return b;
    *why = "the " + role + "'s part has no body made by feature " +
           std::to_string(featureId) + " (`" + key + "`)";
    return nullptr;
  }
  if (solids.size() == 1)
    return solids.front();
  if (solids.empty()) {
    *why = "the " + role + "'s part has no body";
    return nullptr;
  }
  std::string names;
  for (const Body* b : solids) {
    if (!names.empty())
      names += ", ";
    names += "'" + b->name + "' (feature " + std::to_string(b->createdBy) + ")";
  }
  *why = "the " + role + "'s part has several bodies; name the one that " +
         "touches with `" + key + "` (the feature id that made it): " + names;
  return nullptr;
}

std::optional<Setup> MakeSetup(const AssemblyDocument& asmDoc,
                               const Relation& r,
                               const assembly::PartResolver& partById,
                               std::string* why) {
  Setup s;
  const auto fail = [&](std::string w) -> std::optional<Setup> {
    *why = std::move(w);
    return std::nullopt;
  };
  if (!r.mateAPath.empty() || !r.mateBPath.empty())
    return fail("a contact relation couples two mates of this assembly, not "
                "mates inside a sub-assembly");
  s.ma = asmDoc.FindMateById(r.mateA);
  s.mb = asmDoc.FindMateById(r.mateB);
  if (s.ma == nullptr || s.mb == nullptr)
    return fail("a mate this relation couples is gone");
  if (r.mateA == r.mateB)
    return fail("a contact relation couples two different mates: the cam's "
                "turn (a) and the follower's (b)");
  if (s.ma->type != MateType::Revolute && s.ma->type != MateType::Cylindrical)
    return fail("a contact relation's driver (a) is the cam's turn: a "
                "revolute or cylindrical mate");
  if (r.dofA != MateDof::Rotation)
    return fail("a contact relation's driver (a) is a rotation");
  if (s.mb->type == MateType::Slider) {
    if (r.dofB != MateDof::Offset)
      return fail("a slider follower's DOF is its offset");
    s.turn = false;
  } else if (s.mb->type == MateType::Revolute ||
             s.mb->type == MateType::Cylindrical) {
    if (r.dofB != MateDof::Rotation)
      return fail("a contact relation turns a revolute or cylindrical "
                  "follower (dof_b rotation) or slides a slider");
    s.turn = true;
  } else {
    return fail("a contact relation's follower (b) slides on a slider or "
                "turns on a revolute or cylindrical mate");
  }
  std::string w;
  const auto frame = [&](const Connector& c) -> std::optional<glm::dmat4> {
    auto f = assembly::ConnectorLocalFrame(c, asmDoc, partById, &w);
    return f;
  };
  const auto fa = frame(s.ma->a), fb = frame(s.ma->b);
  const auto ga = frame(s.mb->a), gb = frame(s.mb->b);
  if (!fa || !fb || !ga || !gb)
    return fail("a mate's connector does not resolve: " + w);
  s.faA = *fa;
  s.fbA = *fb;
  s.faB = *ga;
  s.fbB = *gb;

  // The bases: the side of each mate rigid with a side of the other.
  UnionFind uf = RigidGroups(asmDoc);
  const uint64_t a1 = s.ma->a.occurrenceId, a2 = s.ma->b.occurrenceId;
  const uint64_t b1 = s.mb->a.occurrenceId, b2 = s.mb->b.occurrenceId;
  int found = 0;
  for (int i = 0; i < 2; i++)
    for (int j = 0; j < 2; j++) {
      const uint64_t ab = i == 0 ? a1 : a2, bb = j == 0 ? b1 : b2;
      if (uf.Find(ab) != uf.Find(bb))
        continue;
      found++;
      s.aBaseIsA = i == 0;
      s.bBaseIsA = j == 0;
    }
  if (found == 0)
    return fail("the cam's axle and the follower's guide must not move "
                "relative to each other: no side of mate a is fixed, "
                "fastened or attached to a side of mate b");
  if (found > 1)
    return fail("the cam and the follower are rigid with each other or with "
                "both mates' bases: nothing would move");
  s.baseA = s.aBaseIsA ? a1 : a2;
  s.movA = s.aBaseIsA ? a2 : a1;
  s.baseB = s.bBaseIsA ? b1 : b2;
  s.movB = s.bBaseIsA ? b2 : b1;

  // The cam and the follower: the moving sides, or an occurrence rigid
  // with one, named.
  const auto pick = [&](uint64_t named, uint64_t mov, const char* role,
                        const char* key, uint64_t* out) -> bool {
    const std::vector<uint64_t> group = RigidGroup(asmDoc, mov);
    if (named != 0) {
      if (std::find(group.begin(), group.end(), named) == group.end()) {
        w = std::string("`") + key + "` " + OccName(asmDoc, named) +
            " is not carried by its mate: it is not " + OccName(asmDoc, mov) +
            " or fastened to it";
        return false;
      }
      *out = named;
      return true;
    }
    if (group.size() > 1) {
      std::string names;
      for (uint64_t g : group)
        names += (names.empty() ? "" : ", ") + OccName(asmDoc, g);
      w = std::string("the mate moves several occurrences that could be the ") +
          role + "; name it with `" + key + "`: " + names;
      return false;
    }
    *out = mov;
    return true;
  };
  if (!pick(r.camOccurrence, s.movA, "cam", "cam", &s.cam) ||
      !pick(r.followerOccurrence, s.movB, "follower", "follower", &s.follower))
    return fail(w);
  const Occurrence* camOcc = asmDoc.FindById(s.cam);
  const Occurrence* folOcc = asmDoc.FindById(s.follower);
  if (!camOcc || !folOcc)
    return fail("the cam's or the follower's occurrence is gone");
  if (camOcc->refKind != OccurrenceRef::Part ||
      folOcc->refKind != OccurrenceRef::Part)
    return fail("the cam and the follower are parts, not sub-assemblies");
  s.camBody = PickBody(
      partById ? partById(camOcc->partId, camOcc->configurationId) : nullptr,
      r.camBody, "cam", "cam_body", &w);
  if (!s.camBody)
    return fail(w);
  s.folBody = PickBody(
      partById ? partById(folOcc->partId, folOcc->configurationId) : nullptr,
      r.followerBody, "follower", "follower_body", &w);
  if (!s.folBody)
    return fail(w);
  const auto extra = [&](uint64_t mov, uint64_t occ) {
    if (mov == occ)
      return glm::dmat4(1.0);
    return glm::inverse(OccMatrix(*asmDoc.FindById(mov))) *
           OccMatrix(*asmDoc.FindById(occ));
  };
  s.camExtra = extra(s.movA, s.cam);
  s.folExtra = extra(s.movB, s.follower);
  const glm::dmat4 planeWorld =
      OccMatrix(*asmDoc.FindById(s.baseA)) * (s.aBaseIsA ? s.faA : s.fbA);
  const glm::dmat4 bBaseWorld =
      OccMatrix(*asmDoc.FindById(s.baseB)) * (s.bBaseIsA ? s.faB : s.fbB);
  s.planeToB = glm::inverse(planeWorld) * bBaseWorld;
  return s;
}

// The plane checks and the path, from the mates' transforms.
bool PathOf(const Setup& s, double widthTol, Path* path, double* camSense,
            std::string* why) {
  // The cam turns about the plane's Z by construction; which way, read off
  // the transform.
  const glm::dmat4 c0 = CamInPlane(s, 0.0), c1 = CamInPlane(s, 0.1);
  const glm::dmat4 rc = c1 * glm::inverse(c0);
  *camSense = std::atan2(rc[0][1], rc[0][0]) > 0.0 ? 1.0 : -1.0;
  const glm::dmat4 f0 = FollowerInPlane(s, 0.0);
  if (!s.turn) {
    const glm::dvec3 d =
        glm::dvec3(FollowerInPlane(s, 1.0)[3]) - glm::dvec3(f0[3]);
    if (std::abs(d.z) > widthTol)
      return (*why = "the follower slides " +
                     Num(angles::ToDegrees(
                         std::asin(std::clamp(d.z, -1.0, 1.0)))) +
                     " degrees out of the cam's plane: only a follower that "
                     "slides square to the cam's axis is built (a disc cam); "
                     "a barrel or face cam is not"),
             false;
    path->turn = false;
    path->dir = glm::normalize(glm::dvec2(d.x, d.y));
    return true;
  }
  const glm::dmat4 f1 = FollowerInPlane(s, 0.1);
  const glm::dmat4 rf = f1 * glm::inverse(f0);
  const glm::dvec3 z = glm::dvec3(rf[2]);
  // The rotation's axis must be the plane's Z: an in-plane rotation keeps Z.
  // Its axis vector: from the skew part of the rotation.
  const glm::dvec3 axis{rf[1][2] - rf[2][1], rf[2][0] - rf[0][2],
                        rf[0][1] - rf[1][0]};
  const double tilt = std::atan2(std::hypot(axis.x, axis.y), std::abs(axis.z));
  (void)z;
  if (tilt > widthTol)
    return (*why = "the follower turns about an axis " +
                   Num(angles::ToDegrees(tilt)) +
                   " degrees off the cam's: only a follower whose pivot is "
                   "parallel to the cam's axis is built (a disc cam); a "
                   "conical or face cam is not"),
           false;
  const double ang = std::atan2(rf[0][1], rf[0][0]);
  path->turn = true;
  path->sense = ang > 0.0 ? 1.0 : -1.0;
  // The pivot: the fixed point of the in-plane rotation.
  const double c = rf[0][0], sn = rf[0][1];
  const glm::dvec2 t{rf[3][0], rf[3][1]};
  // (I - R) p = t
  const double a11 = 1 - c, a12 = sn, a21 = -sn, a22 = 1 - c;
  const double det = a11 * a22 - a12 * a21;
  path->pivot = {(t.x * a22 - a12 * t.y) / det, (a11 * t.y - a21 * t.x) / det};
  return true;
}

std::string Key(const Setup& s, int toward) {
  std::string k;
  char b[64];
  const auto add = [&](const glm::dmat4& m) {
    for (int c = 0; c < 4; c++)
      for (int r = 0; r < 3; r++) {
        std::snprintf(b, sizeof b, "%.10g,", m[c][r]);
        k += b;
      }
  };
  std::snprintf(b, sizeof b, "%p|%p|%d|%d|", s.camBody->shape.GeometryId(),
                s.folBody->shape.GeometryId(), toward, int(s.turn));
  k += b;
  add(CamInPlane(s, 0.0));
  add(CamInPlane(s, 0.1));
  add(FollowerInPlane(s, 0.0));
  add(FollowerInPlane(s, 0.1));
  return k;
}

struct Entry {
  CADShape cam, follower; // held so the ids cannot be recycled
  std::shared_ptr<const Model> model;
};

std::mutex g_mu;
std::list<std::pair<std::string, Entry>> g_lru;
CacheStats g_stats;
constexpr size_t kCapacity = 64;

} // namespace

std::vector<uint64_t> RigidGroup(const AssemblyDocument& asmDoc, uint64_t occ) {
  UnionFind uf = RigidGroups(asmDoc);
  std::vector<uint64_t> out;
  const uint64_t root = uf.Find(occ);
  for (const Occurrence& o : asmDoc.GetOccurrences())
    if (uf.Find(o.id) == root)
      out.push_back(o.id);
  if (std::find(out.begin(), out.end(), occ) == out.end())
    out.push_back(occ);
  return out;
}

namespace {

// The setup, the path and the shared width: everything up to the 2D solve.
struct Prepared {
  Setup s;
  Path path;
  double camSense = 1.0;
  CADShape camPlaced, folPlaced;
  double z = 0.0, tolerance = 0.0, width = 0.0;
};

std::optional<Prepared> Prepare(const AssemblyDocument& asmDoc,
                                const Relation& r,
                                const assembly::PartResolver& partById,
                                std::string* why) {
  auto s = MakeSetup(asmDoc, r, partById, why);
  if (!s)
    return std::nullopt;
  Prepared p;
  p.s = *s;
  p.camPlaced = s->camBody->shape.Transformed(CamInPlane(*s, 0.0));
  p.folPlaced = s->folBody->shape.Transformed(FollowerInPlane(*s, 0.0));
  glm::dvec3 clo, chi, flo, fhi;
  p.camPlaced.GetBoundingBox(clo, chi, true);
  p.folPlaced.GetBoundingBox(flo, fhi, true);
  const double zlo = std::max(clo.z, flo.z), zhi = std::min(chi.z, fhi.z);
  if (!(zhi > zlo)) {
    *why = "the follower does not overlap the cam along its axis (the cam "
           "spans " +
           Num(clo.z) + " to " + Num(chi.z) + " mm, the follower " +
           Num(flo.z) + " to " + Num(fhi.z) + " mm): the two never meet";
    return std::nullopt;
  }
  p.z = 0.5 * (zlo + zhi);
  p.width = zhi - zlo;
  const double size = std::min(glm::length(glm::dvec2(chi - clo)),
                               glm::length(glm::dvec2(fhi - flo)));
  p.tolerance = 1e-5 * size;
  // A tilt that moves the contact by less than the tolerance across the
  // shared width is the planar case.
  const double widthTol = std::max(p.tolerance / (zhi - zlo), 1e-9);
  if (!PathOf(*s, widthTol, &p.path, &p.camSense, why))
    return std::nullopt;
  return p;
}

} // namespace

std::string ContactProblem(const AssemblyDocument& asmDoc, const Relation& r,
                           const assembly::PartResolver& partById) {
  std::string why;
  if (!Prepare(asmDoc, r, partById, &why))
    return why;
  if (r.toward != 1 && r.toward != -1)
    return "a contact relation's `toward` is positive or negative";
  return {};
}

std::shared_ptr<const Model> ModelFor(const AssemblyDocument& asmDoc,
                                      const Relation& r,
                                      const assembly::PartResolver& partById,
                                      std::string* why) {
  auto p = Prepare(asmDoc, r, partById, why);
  if (!p)
    return nullptr;
  if (r.toward != 1 && r.toward != -1) {
    *why = "a contact relation's `toward` is positive or negative";
    return nullptr;
  }
  const std::string key = Key(p->s, r.toward);
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto it = g_lru.begin(); it != g_lru.end(); ++it)
      if (it->first == key &&
          it->second.cam.SameGeometryAs(p->s.camBody->shape) &&
          it->second.follower.SameGeometryAs(p->s.folBody->shape)) {
        g_lru.splice(g_lru.begin(), g_lru, it);
        g_stats.hits++;
        auto m = std::make_shared<Model>(*g_lru.front().second.model);
        // The occurrences may differ for the same geometry; the rest is
        // the geometry's.
        m->cam = p->s.cam;
        m->follower = p->s.follower;
        m->base = p->s.baseA;
        m->planeInBase = p->s.aBaseIsA ? p->s.faA : p->s.fbA;
        m->camInPlane0 = CamInPlane(p->s, 0.0);
        m->camPlaced = p->camPlaced;
        m->followerPlaced = p->folPlaced;
        m->camBody = p->s.camBody->createdBy;
        m->followerBody = p->s.folBody->createdBy;
        return m;
      }
  }
  auto m = std::make_shared<Model>();
  m->cam = p->s.cam;
  m->follower = p->s.follower;
  m->turn = p->s.turn;
  m->base = p->s.baseA;
  m->planeInBase = p->s.aBaseIsA ? p->s.faA : p->s.fbA;
  m->camSense = p->camSense;
  m->path = p->path;
  m->z = p->z;
  m->width = p->width;
  m->camInPlane0 = CamInPlane(p->s, 0.0);
  m->camPlaced = p->camPlaced;
  m->followerPlaced = p->folPlaced;
  m->camBody = p->s.camBody->createdBy;
  m->followerBody = p->s.folBody->createdBy;
  m->tolerance = p->tolerance;
  m->toward = r.toward;
  const double deflection = 0.125 * p->tolerance;
  std::string w;
  m->camOutline = SectionOutline(p->camPlaced.GetShape(), p->z, deflection, &w);
  if (m->camOutline.pts.empty()) {
    *why = "the cam: " + w;
    return nullptr;
  }
  m->followerOutline =
      SectionOutline(p->folPlaced.GetShape(), p->z, deflection, &w);
  if (m->followerOutline.pts.empty()) {
    *why = "the follower: " + w;
    return nullptr;
  }
  m->table = BuildTable(m->camOutline, m->camSense, m->followerOutline, m->path,
                        m->toward, m->tolerance);
  if (!m->table.error.empty()) {
    *why = m->table.error;
    return nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_stats.builds++;
    g_lru.push_front({key, Entry{p->s.camBody->shape, p->s.folBody->shape, m}});
    if (g_lru.size() > kCapacity)
      g_lru.pop_back();
  }
  return m;
}

int TowardFromGravity(const AssemblyDocument& asmDoc, const Relation& r,
                      const assembly::PartResolver& partById,
                      std::string* why) {
  Relation probe = r;
  probe.toward = 1;
  auto p = Prepare(asmDoc, probe, partById, why);
  if (!p)
    return 0;
  const Occurrence* base = asmDoc.FindById(p->s.baseA);
  const glm::dmat4 planeWorld =
      OccMatrix(*base) * (p->s.aBaseIsA ? p->s.faA : p->s.fbA);
  const glm::dvec3 g = conv::kGravity;
  double q = 0.0, scale = 0.0;
  for (uint64_t id : RigidGroup(asmDoc, p->s.movB)) {
    const Occurrence* o = asmDoc.FindById(id);
    if (!o || o->refKind != OccurrenceRef::Part || !partById)
      continue;
    const Document* part = partById(o->partId, o->configurationId);
    if (!part)
      continue;
    std::string err;
    const auto mass = Measure::MassOfBodies(part->GetBodies(), &err);
    if (!mass)
      continue;
    // The weight: grams where a material says, else the volume (the sign
    // is all that is wanted, and one density cancels).
    const double w = mass->massKnown ? mass->massG : mass->volume;
    const glm::dvec3 com =
        glm::dvec3(OccMatrix(*o) * glm::dvec4(mass->centerOfMass, 1.0));
    glm::dvec3 dx;
    if (!p->path.turn) {
      dx = glm::dvec3(planeWorld * glm::dvec4(p->path.dir, 0.0, 0.0));
    } else {
      const glm::dvec3 axis =
          glm::dvec3(planeWorld * glm::dvec4(0.0, 0.0, p->path.sense, 0.0));
      const glm::dvec3 pivot =
          glm::dvec3(planeWorld * glm::dvec4(p->path.pivot, 0.0, 1.0));
      dx = glm::cross(axis, com - pivot);
    }
    q += w * glm::dot(g, dx);
    scale += w * glm::length(g) * glm::length(dx);
  }
  if (!(scale > 0.0) || std::abs(q) < 0.02 * scale) {
    *why = "gravity barely acts along the follower's motion (a horizontal "
           "slide, or a pivot under its own centre of mass), so it cannot "
           "say which way the follower is held: give `toward` (positive or "
           "negative, the way mate b's value moves to reach the cam)";
    return 0;
  }
  return q > 0.0 ? 1 : -1;
}

CacheStats Stats() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_stats;
}

} // namespace contact
