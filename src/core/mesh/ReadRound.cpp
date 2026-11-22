// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/mesh/ReadRound.h"

#include "core/Angles.h"
#include "core/mesh/Slice.h"
#include "core/sketch/SectionPieces.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace mesh {

const char* ToString(EdgeReading r) {
  switch (r) {
  case EdgeReading::Round:
    return "round";
  case EdgeReading::Chamfer:
    return "chamfer";
  case EdgeReading::Sharp:
    return "sharp";
  }
  return "?";
}

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

double Cross2(const glm::dvec2& a, const glm::dvec2& b) {
  return a.x * b.y - a.y * b.x;
}

glm::dvec3 ClosestOnTriangle(const glm::dvec3& p, const glm::dvec3& a,
                             const glm::dvec3& b, const glm::dvec3& c) {
  // Ericson, Real-Time Collision Detection 5.1.5.
  const glm::dvec3 ab = b - a, ac = c - a, ap = p - a;
  const double d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
  if (d1 <= 0.0 && d2 <= 0.0)
    return a;
  const glm::dvec3 bp = p - b;
  const double d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
  if (d3 >= 0.0 && d4 <= d3)
    return b;
  const double vc = d1 * d4 - d3 * d2;
  if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
    return a + ab * (d1 / (d1 - d3));
  const glm::dvec3 cp = p - c;
  const double d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
  if (d6 >= 0.0 && d5 <= d6)
    return c;
  const double vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
    return a + ac * (d2 / (d2 - d6));
  const double va = d3 * d6 - d5 * d4;
  if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
    return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
  const double den = 1.0 / (va + vb + vc);
  return a + ab * (vb * den) + ac * (vc * den);
}

double Median(std::vector<double> v) {
  if (v.empty())
    return 0.0;
  const size_t m = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + m, v.end());
  return v[m];
}

double RayDistance(const glm::dvec2& q, const glm::dvec2& from,
                   const glm::dvec2& dir) {
  const double t = std::max(0.0, glm::dot(q - from, dir));
  return glm::length(q - (from + dir * t));
}

double SegmentDistance(const glm::dvec2& q, const glm::dvec2& a,
                       const glm::dvec2& b) {
  const glm::dvec2 ab = b - a;
  const double l2 = glm::dot(ab, ab);
  const double t =
      l2 > 0.0 ? std::clamp(glm::dot(q - a, ab) / l2, 0.0, 1.0) : 0.0;
  return glm::length(q - (a + ab * t));
}

// The corner in the section's plane: face one comes in along d1 to C, face
// two leaves along d2; the readings' distances from a point.
struct Corner {
  glm::dvec2 C{0.0}, d1{1.0, 0.0}, d2{0.0, 1.0};
  double half = 0.0; // half the inside angle
  glm::dvec2 bis{0.0};

  double Sharp(const glm::dvec2& q) const {
    return std::min(RayDistance(q, C, -d1), RayDistance(q, C, d2));
  }
  double Tangent(double r) const { return r / std::tan(half); }
  glm::dvec2 Centre(double r) const { return C + bis * (r / std::sin(half)); }
  double Round(const glm::dvec2& q, double r) const {
    const double t = Tangent(r);
    const glm::dvec2 t1 = C - d1 * t, t2 = C + d2 * t, o = Centre(r);
    double d = std::min(RayDistance(q, t1, -d1), RayDistance(q, t2, d2));
    // On the arc between its tangent points: the side of each radius.
    const glm::dvec2 a = t1 - o, b = t2 - o, w = q - o;
    const double s = Cross2(a, b) >= 0.0 ? 1.0 : -1.0;
    if (s * Cross2(a, w) >= 0.0 && s * Cross2(w, b) >= 0.0)
      d = std::min(d, std::abs(glm::length(w) - r));
    return d;
  }
  double Chamfer(const glm::dvec2& q, double a, double b) const {
    const glm::dvec2 A = C - d1 * a, B = C + d2 * b;
    return std::min({RayDistance(q, A, -d1), RayDistance(q, B, d2),
                     SegmentDistance(q, A, B)});
  }
};

template <class F> double Ssr(const std::vector<glm::dvec2>& pts, F f) {
  double s = 0.0;
  for (const glm::dvec2& q : pts) {
    const double d = f(q);
    s += d * d;
  }
  return s;
}

// The minimum of f on [lo, hi]: a grid, then golden sections about the
// best of it.
template <class F> double Minimise1(F f, double lo, double hi, double& at) {
  const int n = 48;
  double best = kInf;
  at = lo;
  for (int i = 0; i <= n; i++) {
    const double x = lo + (hi - lo) * i / n;
    const double v = f(x);
    if (v < best) {
      best = v;
      at = x;
    }
  }
  double a = std::max(lo, at - (hi - lo) / n),
         b = std::min(hi, at + (hi - lo) / n);
  const double g = 0.5 * (std::sqrt(5.0) - 1.0);
  double x1 = b - g * (b - a), x2 = a + g * (b - a);
  double f1 = f(x1), f2 = f(x2);
  for (int it = 0; it < 60; it++) {
    if (f1 < f2) {
      b = x2;
      x2 = x1;
      f2 = f1;
      x1 = b - g * (b - a);
      f1 = f(x1);
    } else {
      a = x1;
      x1 = x2;
      f1 = f2;
      x2 = a + g * (b - a);
      f2 = f(x2);
    }
  }
  if (std::min(f1, f2) < best) {
    best = std::min(f1, f2);
    at = f1 < f2 ? x1 : x2;
  }
  return best;
}

} // namespace

double NearestOnMeshes(const std::vector<const TriMesh*>& meshes,
                       const glm::dvec3& point, const TriMesh** which) {
  double nearest = kInf;
  *which = nullptr;
  for (const TriMesh* mm : meshes) {
    if (mm == nullptr)
      continue;
    for (uint32_t t = 0; t < mm->triangles.size(); t++) {
      const double d =
          glm::length(ClosestOnTriangle(point, mm->Corner(t, 0),
                                        mm->Corner(t, 1), mm->Corner(t, 2)) -
                      point);
      if (d < nearest) {
        nearest = d;
        *which = mm;
      }
    }
  }
  return nearest;
}

RoundReading ReadRound(const std::vector<const TriMesh*>& meshes,
                       const glm::dvec3& point) {
  RoundReading out;
  // The scan at the point.
  const TriMesh* m = nullptr;
  const double nearest = NearestOnMeshes(meshes, point, &m);
  if (m == nullptr) {
    out.noScan = true;
    out.why = "no scan: the body has no mesh";
    return out;
  }
  const double spacing = std::max(MedianEdgeLength(*m), 1e-9);
  glm::dvec3 lo, hi;
  m->Bounds(lo, hi);
  const double diagonal = glm::length(hi - lo);
  // The point need only be near the edge: a model's sharp edge stands off
  // the scan's round by up to the round's size.
  if (nearest > 40.0 * spacing) {
    out.noScan = true;
    out.why = "no scan at the point: the nearest of the mesh is " +
              std::to_string(nearest) + " mm away";
    return out;
  }
  double window = 40.0 * spacing;
  glm::dvec3 firstEdge(0.0, 0.0, 1.0);
  const double cap = std::max(window, 0.25 * diagonal);
  for (int round = 0; round < 6;
       round++, window = std::min(cap, 2.0 * window)) {
    out.windowMm = window;
    // The two faces: the triangles about the point, their normals in two.
    std::vector<glm::dvec3> normals;
    std::vector<double> weights;
    for (uint32_t t = 0; t < m->triangles.size(); t++) {
      const glm::dvec3 c =
          (m->Corner(t, 0) + m->Corner(t, 1) + m->Corner(t, 2)) / 3.0;
      if (glm::length(c - point) > window)
        continue;
      const glm::dvec3 an = m->AreaNormal(t);
      const double a = glm::length(an);
      if (!(a > 0.0))
        continue;
      normals.push_back(an / a);
      weights.push_back(a);
    }
    if (normals.size() < 8) {
      out.noScan = true;
      out.why = "no scan at the point: too few triangles about it";
      return out;
    }
    glm::dvec3 mean(0.0);
    for (size_t i = 0; i < normals.size(); i++)
      mean += normals[i] * weights[i];
    glm::dvec3 na = normals[0], nb = normals[0];
    double farthest = 2.0;
    for (const glm::dvec3& n : normals)
      if (glm::dot(n, mean) / std::max(glm::length(mean), 1e-30) < farthest) {
        farthest = glm::dot(n, mean) / std::max(glm::length(mean), 1e-30);
        na = n;
      }
    farthest = 2.0;
    for (const glm::dvec3& n : normals)
      if (glm::dot(n, na) < farthest) {
        farthest = glm::dot(n, na);
        nb = n;
      }
    for (int it = 0; it < 12; it++) {
      glm::dvec3 sa(0.0), sb(0.0);
      for (size_t i = 0; i < normals.size(); i++)
        (glm::dot(normals[i], na) >= glm::dot(normals[i], nb) ? sa : sb) +=
            normals[i] * weights[i];
      if (!(glm::length(sa) > 0.0) || !(glm::length(sb) > 0.0))
        break;
      na = glm::normalize(sa);
      nb = glm::normalize(sb);
    }
    if (round == 0 && glm::dot(na, nb) > std::cos(angles::ToRadians(10.0))) {
      out.why = "no edge at the point: the scan there is one face";
      return out;
    }
    // The edge is read where the window first opened: a wider one may
    // take in other faces of the part (its ends, another edge), and the
    // edge runs the same way.
    if (round == 0)
      firstEdge = glm::normalize(glm::cross(na, nb));
    const glm::dvec3 e = firstEdge;
    out.edgeDir = e;
    // The section square to the edge, and its run through the point.
    const SliceResult slice = Slice(*m, point, e);
    const SlicePolyline* line = nullptr;
    size_t at = 0;
    double best = kInf;
    for (const SlicePolyline& pl : slice.polylines)
      for (size_t k = 0; k < pl.points.size(); k++)
        if (const double d = glm::length(pl.points[k] - point); d < best) {
          best = d;
          line = &pl;
          at = k;
        }
    if (line == nullptr) {
      out.noScan = true;
      out.why = "no scan at the point: the section square to the edge cuts "
                "nothing";
      return out;
    }
    const size_t n = line->points.size();
    const auto step = [&](size_t k, int dir) -> std::optional<size_t> {
      if (dir > 0) {
        if (k + 1 < n)
          return k + 1;
        return line->closed ? std::optional<size_t>(0) : std::nullopt;
      }
      if (k > 0)
        return k - 1;
      return line->closed ? std::optional<size_t>(n - 1) : std::nullopt;
    };
    size_t first = at, last = at, count = 1;
    while (count < n) {
      const auto k = step(first, -1);
      if (!k || glm::length(line->points[*k] - point) > window)
        break;
      first = *k;
      count++;
    }
    while (count < n) {
      const auto k = step(last, 1);
      if (!k || glm::length(line->points[*k] - point) > window)
        break;
      last = *k;
      count++;
    }
    glm::dvec3 u, v;
    PlaneAxes(e, u, v);
    std::vector<glm::dvec2> pts;
    for (size_t k = first, c = 0; c < count; c++, k = (k + 1) % n)
      pts.push_back({glm::dot(line->points[k] - point, u),
                     glm::dot(line->points[k] - point, v)});
    out.points = pts.size();
    if (pts.size() < 30)
      continue; // a wider window
    // The faces: lines through each end's outer third, then grown in while
    // the points stay on them.
    const size_t third = pts.size() * 3 / 10;
    std::vector<glm::dvec2> a(pts.begin(), pts.begin() + third),
        b(pts.end() - third, pts.end());
    sketch::SpanFit fa = sketch::FitSpan(a, false, 1e-6);
    sketch::SpanFit fb = sketch::FitSpan(b, false, 1e-6);
    if (!fa.ok || !fb.ok)
      continue;
    std::vector<double> res;
    for (const glm::dvec2& q : a)
      res.push_back(std::abs(Cross2(fa.dir, q - fa.p0)));
    for (const glm::dvec2& q : b)
      res.push_back(std::abs(Cross2(fb.dir, q - fb.p0)));
    const double sigma = std::max(Median(res) / 0.6745, 1e-6);
    const auto onA = [&](size_t k) {
      return std::abs(Cross2(fa.dir, pts[k] - fa.p0)) < 3.0 * sigma;
    };
    const auto onB = [&](size_t k) {
      return std::abs(Cross2(fb.dir, pts[k] - fb.p0)) < 3.0 * sigma;
    };
    // Face one runs from the start until five points in a row leave it.
    size_t endA = 0;
    for (size_t k = 0; k < pts.size(); k++) {
      bool off = true;
      for (size_t j = k; j < std::min(pts.size(), k + 5); j++)
        off = off && !onA(j);
      if (off)
        break;
      endA = k;
    }
    size_t startB = pts.size() - 1;
    for (size_t k = pts.size(); k-- > 0;) {
      bool off = true;
      for (size_t j = k >= 4 ? k - 4 : 0; j <= k; j++)
        off = off && !onB(j);
      if (off)
        break;
      startB = k;
    }
    if (endA + 1 >= startB) {
      // The faces run into each other: no corner between them.
      endA = std::min(endA, pts.size() / 2);
      startB = std::max(startB, endA + 1);
    }
    std::vector<glm::dvec2> faceA(pts.begin(), pts.begin() + endA + 1),
        faceB(pts.begin() + startB, pts.end());
    std::vector<glm::dvec2> zone(pts.begin() + endA + 1, pts.begin() + startB);
    double zoneLength = 0.0;
    for (size_t k = endA; k + 1 <= startB && k + 1 < pts.size(); k++)
      zoneLength += glm::length(pts[k + 1] - pts[k]);
    if (zoneLength > window / 3.0 && window < cap)
      continue; // the corner is large in the window: a wider one
    if (faceA.size() < 10 || faceB.size() < 10) {
      out.why = "the faces beside the edge are too short to read in the "
                "scan";
      return out;
    }
    fa = sketch::FitSpan(faceA, false, 1e-6);
    fb = sketch::FitSpan(faceB, false, 1e-6);
    Corner k;
    k.d1 = fa.dir;
    if (glm::dot(k.d1, faceA.back() - faceA.front()) < 0.0)
      k.d1 = -k.d1;
    k.d2 = fb.dir;
    if (glm::dot(k.d2, faceB.back() - faceB.front()) < 0.0)
      k.d2 = -k.d2;
    const double den = Cross2(k.d1, k.d2);
    if (std::abs(den) < std::sin(angles::ToRadians(5.0))) {
      out.why = "no edge at the point: the faces in the section are "
                "parallel";
      return out;
    }
    const double tA = Cross2(fb.p0 - fa.p0, k.d2) / den;
    k.C = fa.p0 + k.d1 * tA;
    const double inside =
        std::acos(std::clamp(glm::dot(-k.d1, k.d2), -1.0, 1.0));
    k.half = 0.5 * inside;
    k.bis = glm::normalize(-k.d1 + k.d2);
    out.angleDeg = angles::ToDegrees(inside);
    out.corner = point + u * k.C.x + v * k.C.y;
    // The noise, from the faces' own residuals, widened by their
    // correlation.
    std::vector<double> signedRes;
    for (const glm::dvec2& q : faceA)
      signedRes.push_back(Cross2(fa.dir, q - fa.p0));
    for (const glm::dvec2& q : faceB)
      signedRes.push_back(Cross2(fb.dir, q - fb.p0));
    std::vector<double> mag;
    for (double r : signedRes)
      mag.push_back(std::abs(r));
    const double s = std::max(Median(mag) / 0.6745, 1e-6);
    double num = 0.0, dd = 0.0;
    for (size_t i = 0; i + 1 < signedRes.size(); i++) {
      num += signedRes[i] * signedRes[i + 1];
      dd += signedRes[i] * signedRes[i];
    }
    const double rho = dd > 0.0 ? std::clamp(num / dd, 0.0, 0.95) : 0.0;
    const double s2 = s * s * (1.0 + rho) / (1.0 - rho);
    out.noiseMm = s;
    // The three readings, the faces held where they are.
    const double extent = std::min(glm::length(faceA.front() - k.C),
                                   glm::length(faceB.back() - k.C));
    const double ssrSharp =
        Ssr(pts, [&](const glm::dvec2& q) { return k.Sharp(q); });
    double r = 0.0;
    const double ssrRound = Minimise1(
        [&](double rr) {
          return Ssr(pts, [&](const glm::dvec2& q) { return k.Round(q, rr); });
        },
        0.0, 0.8 * extent * std::tan(k.half), r);
    // The chamfer: a grid of its two distances, then refined about the
    // best one each in turn.
    double ca = 0.0, cb = 0.0, ssrChamfer = ssrSharp;
    const int g = 24;
    for (int i = 0; i <= g; i++)
      for (int j = 0; j <= g; j++) {
        const double x = 0.8 * extent * i / g, y = 0.8 * extent * j / g;
        const double v2 =
            Ssr(pts, [&](const glm::dvec2& q) { return k.Chamfer(q, x, y); });
        if (v2 < ssrChamfer) {
          ssrChamfer = v2;
          ca = x;
          cb = y;
        }
      }
    for (int it = 0; it < 6; it++) {
      const double span = 0.8 * extent / g * std::pow(0.5, it);
      double x = ca, y = cb;
      ssrChamfer =
          std::min(ssrChamfer, Minimise1(
                                   [&](double t) {
                                     return Ssr(pts, [&](const glm::dvec2& q) {
                                       return k.Chamfer(q, t, cb);
                                     });
                                   },
                                   std::max(0.0, ca - span), ca + span, x));
      ca = x;
      ssrChamfer =
          std::min(ssrChamfer, Minimise1(
                                   [&](double t) {
                                     return Ssr(pts, [&](const glm::dvec2& q) {
                                       return k.Chamfer(q, ca, t);
                                     });
                                   },
                                   std::max(0.0, cb - span), cb + span, y));
      cb = y;
    }
    const double aic[3] = {ssrRound / s2 + 2.0, ssrChamfer / s2 + 4.0,
                           ssrSharp / s2};
    int verdict = 0;
    for (int i = 1; i < 3; i++)
      if (aic[i] < aic[verdict])
        verdict = i;
    out.verdict = static_cast<EdgeReading>(verdict);
    for (int i = 0; i < 3; i++) {
      out.worse[i] = aic[i] - aic[verdict];
      if (i != verdict && out.worse[i] < kAlsoFits)
        out.alsoFits.push_back(static_cast<EdgeReading>(i));
    }
    out.chamferMm[0] = ca;
    out.chamferMm[1] = cb;
    // The round's radius and its sigma: held tangent to both faces, the
    // faces' own uncertainty carried (sketch::FitTangentRound).
    out.radiusMm = r;
    const double t = k.Tangent(r);
    std::vector<glm::dvec2> before, arc, after;
    for (const glm::dvec2& q : pts) {
      const double along1 = glm::dot(k.C - q, k.d1); // to the corner on face 1
      const double along2 = glm::dot(q - k.C, k.d2);
      if (along1 > t + 2.0 * spacing && along2 < 0.0)
        before.push_back(q);
      else if (along2 > t + 2.0 * spacing && along1 < 0.0)
        after.push_back(q);
      else
        arc.push_back(q);
    }
    sketch::RoundSide sb, sa;
    sb.tangent = before.size() >= 2;
    sb.pts = before;
    sb.p0 = fa.p0;
    sb.dir = fa.dir;
    sa.tangent = after.size() >= 2;
    sa.pts = after;
    sa.p0 = fb.p0;
    sa.dir = fb.dir;
    const sketch::RoundFit held =
        r > 0.0 ? sketch::FitTangentRound(arc, k.Centre(r), r, sb, sa, 1e-6)
                : sketch::RoundFit{};
    if (held.ok && held.tangentEnds == 2) {
      out.radiusMm = held.radius;
      out.sigmaMm = held.radiusSigmaMm;
    } else {
      // The SSR's curvature in r: sigma^2 = 2 s^2 / f''.
      const double h = std::max(1e-3 * std::max(r, spacing), 1e-6);
      const auto f = [&](double rr) {
        return Ssr(pts, [&](const glm::dvec2& q) { return k.Round(q, rr); });
      };
      const double curv =
          (f(r + h) - 2.0 * ssrRound + f(std::max(0.0, r - h))) / (h * h);
      out.sigmaMm = curv > 0.0 ? std::sqrt(2.0 * s2 / curv) : kInf;
    }
    out.ok = true;
    return out;
  }
  out.why = "could not read the edge: the section about the point never "
            "showed two faces (the window grew to " +
            std::to_string(window) + " mm)";
  return out;
}

} // namespace mesh
