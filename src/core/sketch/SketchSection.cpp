// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/sketch/SketchSection.h"

#include "cad/CurveSample.h"
#include "cad/NonDestructiveBoolean.h"
#include "common/Logger.h"
#include "core/Feature.h"
#include "core/mesh/Slice.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_HSequenceOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>

#include <algorithm>
#include <cmath>
#include <limits>

namespace sketch {

const char* ToString(SectionFit fit) {
  switch (fit) {
  case SectionFit::Polyline:
    return "polyline";
  case SectionFit::Arcs:
    return "arcs";
  case SectionFit::Spline:
    return "spline";
  }
  return "polyline";
}

std::optional<SectionFit> SectionFitFromString(const std::string& name) {
  if (name == "polyline")
    return SectionFit::Polyline;
  if (name == "arcs")
    return SectionFit::Arcs;
  if (name == "spline")
    return SectionFit::Spline;
  return std::nullopt;
}

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

double SegmentDistance(const glm::dvec2& p, const glm::dvec2& a,
                       const glm::dvec2& b) {
  const glm::dvec2 ab = b - a;
  const double len2 = glm::dot(ab, ab);
  if (len2 <= 0.0)
    return glm::length(p - a);
  const double t = std::clamp(glm::dot(p - a, ab) / len2, 0.0, 1.0);
  return glm::length(p - (a + t * ab));
}

// Points a closed chain is fitted through: its first point again at the end.
std::vector<glm::dvec2> Walk(const CutChain& c) {
  std::vector<glm::dvec2> pts = c.points;
  if (c.closed && pts.size() > 2)
    pts.push_back(pts.front());
  return pts;
}

// ---- polyline --------------------------------------------------------------

// Douglas-Peucker on [first, last], the kept indices into `keep`.
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

void FitPolyline(const std::vector<glm::dvec2>& pts, double tol,
                 FittedSection& out) {
  const size_t n = pts.size();
  std::vector<char> keep(n, 0);
  keep[0] = keep[n - 1] = 1;
  if (glm::length(pts.front() - pts.back()) <= 0.0 && n > 3) {
    // A closed walk: split at the point farthest from the start, so DP has
    // a chord to measure from.
    size_t apex = 1;
    for (size_t k = 1; k + 1 < n; k++)
      if (glm::length(pts[k] - pts[0]) > glm::length(pts[apex] - pts[0]))
        apex = k;
    keep[apex] = 1;
    KeepDp(pts, 0, apex, tol, keep);
    KeepDp(pts, apex, n - 1, tol, keep);
  } else {
    KeepDp(pts, 0, n - 1, tol, keep);
  }
  size_t prev = 0;
  for (size_t k = 1; k < n; k++) {
    if (!keep[k])
      continue;
    for (size_t q = prev + 1; q < k; q++)
      out.deviation =
          std::max(out.deviation, SegmentDistance(pts[q], pts[prev], pts[k]));
    if (glm::length(pts[k] - pts[prev]) > 0.0)
      out.entities.push_back(Line{pts[prev], pts[k]});
    prev = k;
  }
}

// ---- lines and arcs --------------------------------------------------------

double LineDeviation(const std::vector<glm::dvec2>& pts, size_t i, size_t j) {
  double worst = 0.0;
  for (size_t k = i + 1; k < j; k++)
    worst = std::max(worst, SegmentDistance(pts[k], pts[i], pts[j]));
  return worst;
}

double WrapAngle(double a) {
  a = std::fmod(a, 2.0 * M_PI);
  return a < 0.0 ? a + 2.0 * M_PI : a;
}

// The arc through pts[i], the middle point and pts[j], and the largest
// distance from the points it spans to it; infinite when there is no such
// arc (three points in a line, or a sweep past a full turn).
struct ArcTry {
  double deviation = kInf;
  Arc arc{};
};
ArcTry ArcThrough(const std::vector<glm::dvec2>& pts, size_t i, size_t j,
                  double tol) {
  ArcTry out;
  if (j < i + 2)
    return out;
  const size_t m = (i + j) / 2;
  const glm::dvec2 a = pts[i], b = pts[m], c = pts[j];
  const double d =
      2.0 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
  const double chord = glm::length(c - a) + glm::length(b - a);
  if (std::abs(d) <= 1e-12 * chord * chord)
    return out;
  const double a2 = glm::dot(a, a), b2 = glm::dot(b, b), c2 = glm::dot(c, c);
  const glm::dvec2 centre(
      (a2 * (b.y - c.y) + b2 * (c.y - a.y) + c2 * (a.y - b.y)) / d,
      (a2 * (c.x - b.x) + b2 * (a.x - c.x) + c2 * (b.x - a.x)) / d);
  const double r = glm::length(a - centre);
  // So flat a line does as well: a radius past a thousand times the span
  // reads as a numerical accident, not a curve.
  if (!(r < 1e3 * std::max(chord, tol)))
    return out;
  const auto ang = [&](const glm::dvec2& p) {
    return std::atan2(p.y - centre.y, p.x - centre.x);
  };
  const bool ccw = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x) > 0.0;
  const double sweep =
      ccw ? WrapAngle(ang(c) - ang(a)) : WrapAngle(ang(a) - ang(c));
  if (sweep <= 0.0 || sweep > 2.0 * M_PI - 1e-6)
    return out;
  double worst = 0.0, along = 0.0;
  for (size_t k = i + 1; k < j; k++) {
    const double t =
        ccw ? WrapAngle(ang(pts[k]) - ang(a)) : WrapAngle(ang(a) - ang(pts[k]));
    double dist;
    if (t <= sweep + 1e-9) {
      dist = std::abs(glm::length(pts[k] - centre) - r);
      // The points must go round the way the arc does: one doubling back is
      // not on it, however close to the circle.
      if (t + 1e-9 < along)
        dist = std::max(dist, (along - t) * r);
      along = std::max(along, t);
    } else {
      dist = std::min(glm::length(pts[k] - a), glm::length(pts[k] - c));
    }
    worst = std::max(worst, dist);
    if (worst > tol)
      break;
  }
  // Between the points too: the cut is the segments joining them, and a
  // segment strays from a circle most at its ends or where it passes
  // closest to the centre. Two far-apart points of a B-Rep line edge lie on
  // many arcs that the line between them does not.
  for (size_t k = i; k < j && worst <= tol; k++) {
    const glm::dvec2 p0 = pts[k], p1 = pts[k + 1];
    const glm::dvec2 e = p1 - p0;
    const double len2 = glm::dot(e, e);
    if (len2 <= 0.0)
      continue;
    const double t = std::clamp(glm::dot(centre - p0, e) / len2, 0.0, 1.0);
    const glm::dvec2 q = p0 + t * e;
    worst = std::max(worst, std::abs(glm::length(q - centre) - r));
  }
  out.deviation = worst;
  const double start = ccw ? ang(a) : ang(c);
  out.arc = Arc{centre, r, start, start + sweep};
  return out;
}

// The farthest j from i (up to `last`) for which `ok(i, j)` holds, assuming
// it holds for every shorter span once it holds: doubling, then bisecting.
template <class Ok>
size_t Farthest(size_t i, size_t first, size_t last, const Ok& ok) {
  if (first > last || !ok(first))
    return 0;
  size_t good = first, step = 1;
  size_t bad = last + 1;
  while (true) {
    const size_t next = std::min(last, good + step);
    if (next == good)
      break;
    if (ok(next)) {
      good = next;
      if (good == last)
        return good;
      step *= 2;
    } else {
      bad = next;
      break;
    }
  }
  while (bad > good + 1) {
    const size_t mid = good + (bad - good) / 2;
    if (ok(mid))
      good = mid;
    else
      bad = mid;
  }
  (void)i;
  return good;
}

void FitArcs(const std::vector<glm::dvec2>& pts, double tol,
             FittedSection& out) {
  const size_t last = pts.size() - 1;
  size_t i = 0;
  while (i < last) {
    const size_t jl = Farthest(i, i + 1, last, [&](size_t j) {
      return LineDeviation(pts, i, j) <= tol;
    });
    const size_t ja =
        i + 2 <= last
            ? Farthest(i, i + 2, last,
                       [&](size_t j) {
                         return ArcThrough(pts, i, j, tol).deviation <= tol;
                       })
            : 0;
    if (ja > jl) {
      const ArcTry t = ArcThrough(pts, i, ja, kInf);
      out.deviation = std::max(out.deviation, t.deviation);
      out.entities.push_back(t.arc);
      i = ja;
    } else {
      const size_t j = std::max(jl, i + 1);
      out.deviation = std::max(out.deviation, LineDeviation(pts, i, j));
      if (glm::length(pts[j] - pts[i]) > 0.0)
        out.entities.push_back(Line{pts[i], pts[j]});
      i = j;
    }
  }
}

// ---- splines ---------------------------------------------------------------

// The p+1 basis functions non-zero at u on the clamped uniform knots of n
// poles (Piegl & Tiller A2.2), and the span they start at.
size_t Basis(const std::vector<double>& U, int n, int p, double u, double* N) {
  const double umax = U[n];
  u = std::clamp(u, 0.0, umax);
  int k = p;
  while (k < n - 1 && U[k + 1] <= u)
    k++;
  double left[8], right[8];
  N[0] = 1.0;
  for (int j = 1; j <= p; j++) {
    left[j] = u - U[k + 1 - j];
    right[j] = U[k + j] - u;
    double saved = 0.0;
    for (int r = 0; r < j; r++) {
      const double den = right[r + 1] + left[j - r];
      const double t = den != 0.0 ? N[r] / den : 0.0;
      N[r] = saved + right[r + 1] * t;
      saved = left[j - r] * t;
    }
    N[j] = saved;
  }
  return static_cast<size_t>(k - p);
}

// Solves the symmetric positive definite `A x = b` (dense, row-major n×n) in
// place by Cholesky; false when it is not positive definite.
bool Cholesky(std::vector<double>& A, std::vector<glm::dvec2>& b, size_t n) {
  for (size_t j = 0; j < n; j++) {
    double d = A[j * n + j];
    for (size_t k = 0; k < j; k++)
      d -= A[j * n + k] * A[j * n + k];
    if (!(d > 0.0))
      return false;
    d = std::sqrt(d);
    A[j * n + j] = d;
    for (size_t i = j + 1; i < n; i++) {
      double s = A[i * n + j];
      for (size_t k = 0; k < j; k++)
        s -= A[i * n + k] * A[j * n + k];
      A[i * n + j] = s / d;
    }
  }
  for (size_t i = 0; i < n; i++) {
    glm::dvec2 s = b[i];
    for (size_t k = 0; k < i; k++)
      s -= A[i * n + k] * b[k];
    b[i] = s / A[i * n + i];
  }
  for (size_t i = n; i-- > 0;) {
    glm::dvec2 s = b[i];
    for (size_t k = i + 1; k < n; k++)
      s -= A[k * n + i] * b[k];
    b[i] = s / A[i * n + i];
  }
  return true;
}

// The largest distance from pts to the spline, each point measured against
// the spline's polyline (32 segments a span) in the spans next to its
// parameter.
double SplineDeviation(const BSpline& s, const std::vector<glm::dvec2>& pts,
                       const std::vector<double>& params) {
  const int n = static_cast<int>(s.poles.size());
  const int p = BSplineDegree(s);
  const int spans = n - p;
  constexpr int kPer = 32;
  std::vector<glm::dvec2> poly;
  poly.reserve(static_cast<size_t>(spans * kPer + 1));
  for (int q = 0; q <= spans * kPer; q++)
    poly.push_back(BSplineEvalUV(s, static_cast<double>(q) / kPer));
  double worst = 0.0;
  for (size_t k = 0; k < pts.size(); k++) {
    const int at = static_cast<int>(params[k] * kPer);
    const int lo = std::max(0, at - 2 * kPer);
    const int hi = std::min(spans * kPer, at + 2 * kPer);
    double best = kInf;
    for (int q = lo; q < hi; q++)
      best = std::min(best, SegmentDistance(pts[k], poly[q], poly[q + 1]));
    if (hi == lo)
      best = glm::length(pts[k] - poly[lo]);
    worst = std::max(worst, best);
  }
  // And the other way: the spline's samples to the points' polyline, so a
  // loop or a bulge no point lies near is seen too.
  for (int q = 0; q <= spans * kPer; q++) {
    const double u = static_cast<double>(q) / kPer;
    const size_t at = static_cast<size_t>(
        std::lower_bound(params.begin(), params.end(), u) - params.begin());
    const size_t reach = std::max<size_t>(8, pts.size() / std::max(spans, 1));
    const size_t lo = at > 2 * reach ? at - 2 * reach : 0;
    const size_t hi = std::min(pts.size() - 1, at + 2 * reach);
    double best = kInf;
    for (size_t k = lo; k < hi; k++)
      best = std::min(best, SegmentDistance(poly[q], pts[k], pts[k + 1]));
    if (hi == lo)
      best = glm::length(poly[q] - pts[lo]);
    worst = std::max(worst, best);
  }
  return worst;
}

// Least squares with both ends pinned, n poles, a small second-difference
// penalty keeping spans without points well posed.
std::optional<BSpline> FitOne(const std::vector<glm::dvec2>& pts,
                              const std::vector<double>& t, int n) {
  BSpline s;
  const size_t m = pts.size();
  if (n <= 2 || m <= 2) {
    s.poles = {pts.front(), pts.back()};
    return s;
  }
  const int p = std::min(3, n - 1);
  s.degree = 3;
  const std::vector<double> U = BSplineKnots(n, p);
  const size_t unknowns = static_cast<size_t>(n - 2);
  std::vector<double> A(unknowns * unknowns, 0.0);
  std::vector<glm::dvec2> b(unknowns, glm::dvec2(0.0));
  double N[8];
  for (size_t k = 1; k + 1 < m; k++) {
    const size_t first = Basis(U, n, p, t[k] * (n - p), N);
    glm::dvec2 r = pts[k];
    for (int a = 0; a <= p; a++) {
      const size_t ia = first + static_cast<size_t>(a);
      if (ia == 0)
        r -= N[a] * pts.front();
      else if (ia == static_cast<size_t>(n - 1))
        r -= N[a] * pts.back();
    }
    for (int a = 0; a <= p; a++) {
      const size_t ia = first + static_cast<size_t>(a);
      if (ia == 0 || ia >= static_cast<size_t>(n - 1))
        continue;
      b[ia - 1] += N[a] * r;
      for (int c = 0; c <= p; c++) {
        const size_t ic = first + static_cast<size_t>(c);
        if (ic == 0 || ic >= static_cast<size_t>(n - 1))
          continue;
        A[(ia - 1) * unknowns + (ic - 1)] += N[a] * N[c];
      }
    }
  }
  double trace = 0.0;
  for (size_t i = 0; i < unknowns; i++)
    trace += A[i * unknowns + i];
  const double lambda = 1e-6 * std::max(trace / unknowns, 1e-12);
  // Second differences over the full pole row, the pinned ends moved to the
  // right-hand side.
  for (int i = 1; i + 1 < n; i++) {
    const int idx[3] = {i - 1, i, i + 1};
    const double w[3] = {1.0, -2.0, 1.0};
    for (int a = 0; a < 3; a++) {
      if (idx[a] == 0 || idx[a] == n - 1)
        continue;
      const size_t ia = static_cast<size_t>(idx[a] - 1);
      for (int c = 0; c < 3; c++) {
        if (idx[c] == 0)
          b[ia] -= lambda * w[a] * w[c] * pts.front();
        else if (idx[c] == n - 1)
          b[ia] -= lambda * w[a] * w[c] * pts.back();
        else
          A[ia * unknowns + static_cast<size_t>(idx[c] - 1)] +=
              lambda * w[a] * w[c];
      }
    }
  }
  if (!Cholesky(A, b, unknowns))
    return std::nullopt;
  s.poles.reserve(static_cast<size_t>(n));
  s.poles.push_back(pts.front());
  s.poles.insert(s.poles.end(), b.begin(), b.end());
  s.poles.push_back(pts.back());
  return s;
}

constexpr int kMaxPoles = 255; // PointRef::which is a uint8_t

// The chain with no segment longer than `spacing`: a spline is fitted to
// points and measured at points, and a long straight span given as its two
// ends says nothing of what lies between.
std::vector<glm::dvec2> Densified(const std::vector<glm::dvec2>& pts,
                                  double spacing) {
  std::vector<glm::dvec2> out;
  if (pts.empty() || !(spacing > 0.0))
    return pts;
  out.push_back(pts.front());
  for (size_t k = 1; k < pts.size(); k++) {
    const double len = glm::length(pts[k] - pts[k - 1]);
    const int parts =
        static_cast<int>(std::min(1.0e4, std::ceil(len / spacing)));
    for (int q = 1; q < parts; q++)
      out.push_back(pts[k - 1] + (pts[k] - pts[k - 1]) * (double(q) / parts));
    out.push_back(pts[k]);
  }
  return out;
}

// A spline chain's spacing: a few tolerances, but no more than about 2000
// points along it.
double SplineSpacing(const std::vector<glm::dvec2>& pts, double tol) {
  double length = 0.0;
  for (size_t k = 1; k < pts.size(); k++)
    length += glm::length(pts[k] - pts[k - 1]);
  return std::max(4.0 * tol, length / 2000.0);
}

void FitSplinePiece(const std::vector<glm::dvec2>& pts, double tol,
                    std::vector<BSpline>& out, double& deviation) {
  const size_t m = pts.size();
  if (m < 2)
    return;
  // Chord length, normalised.
  std::vector<double> t(m, 0.0);
  for (size_t k = 1; k < m; k++)
    t[k] = t[k - 1] + glm::length(pts[k] - pts[k - 1]);
  const double total = t.back();
  if (!(total > 0.0))
    return;
  for (double& v : t)
    v /= total;
  const int most = static_cast<int>(std::min<size_t>(kMaxPoles, m));
  std::optional<BSpline> best;
  double bestDev = kInf;
  for (int n = std::min(4, most);;
       n = std::min(most, std::max(n + 1, n * 3 / 2))) {
    if (auto s = FitOne(pts, t, n)) {
      std::vector<double> params(m);
      const int p = BSplineDegree(*s);
      for (size_t k = 0; k < m; k++)
        params[k] = t[k] * (static_cast<int>(s->poles.size()) - p);
      const double dev = SplineDeviation(*s, pts, params);
      if (dev < bestDev) {
        bestDev = dev;
        best = std::move(s);
      }
      if (dev <= tol)
        break;
    }
    if (n >= most)
      break;
  }
  if ((!best || bestDev > tol) && m > 8) {
    // Past what one spline of a sketch's size holds: two that meet.
    const size_t mid = m / 2;
    const std::vector<glm::dvec2> a(pts.begin(), pts.begin() + mid + 1);
    const std::vector<glm::dvec2> b(pts.begin() + mid, pts.end());
    FitSplinePiece(a, tol, out, deviation);
    FitSplinePiece(b, tol, out, deviation);
    return;
  }
  if (!best)
    return;
  deviation = std::max(deviation, bestDev);
  out.push_back(std::move(*best));
}

// A walk fitted as splines: a closed one as two halves that meet at both
// ends, never as one spline closed on itself -- an extrusion of a lone
// closed spline weighs wrong in OpenCASCADE (docs/known-bugs.md).
void FitSplineWalk(const std::vector<glm::dvec2>& walk, bool closed, double tol,
                   std::vector<BSpline>& out, double& deviation) {
  const std::vector<glm::dvec2> pts = Densified(walk, SplineSpacing(walk, tol));
  if (pts.size() < 2)
    return;
  if (closed && pts.size() >= 5) {
    const size_t mid = pts.size() / 2;
    FitSplinePiece({pts.begin(), pts.begin() + mid + 1}, tol, out, deviation);
    FitSplinePiece({pts.begin() + mid, pts.end()}, tol, out, deviation);
    return;
  }
  FitSplinePiece(pts, tol, out, deviation);
}

} // namespace

std::vector<BSpline> FitSplines(const std::vector<glm::dvec2>& points,
                                bool closed, double tolerance,
                                double* deviation) {
  std::vector<BSpline> out;
  double dev = 0.0;
  CutChain c{points, closed};
  FitSplineWalk(Walk(c), closed && points.size() > 2, tolerance, out, dev);
  if (deviation != nullptr)
    *deviation = dev;
  return out;
}

FittedSection FitSection(const std::vector<CutChain>& chains, SectionFit fit,
                         double tolerance) {
  FittedSection out;
  for (const CutChain& c : chains) {
    const std::vector<glm::dvec2> pts = Walk(c);
    if (pts.size() < 2)
      continue;
    switch (fit) {
    case SectionFit::Polyline:
      FitPolyline(pts, tolerance, out);
      break;
    case SectionFit::Arcs:
      FitArcs(pts, tolerance, out);
      break;
    case SectionFit::Spline: {
      std::vector<BSpline> pieces;
      FitSplineWalk(pts, c.closed && c.points.size() > 2, tolerance, pieces,
                    out.deviation);
      for (BSpline& s : pieces)
        out.entities.push_back(std::move(s));
      break;
    }
    }
  }
  return out;
}

namespace {

// The B-Rep bodies' section, chained: each wire's edges in order, every edge
// discretised to `deflection`.
void CutBrep(const Body& body, const SketchPlane& plane, double deflection,
             double span, SectionCut& out) {
  const glm::dvec3 o = plane.GetOrigin(), u = plane.GetUAxis(),
                   v = plane.GetVAxis();
  glm::dvec3 lo, hi;
  if (!BodyBounds(body, lo, hi))
    return;
  // A face of the plane covering the body: centred on the body's box
  // projected onto the plane, as the section cutter is.
  const glm::dvec3 c = 0.5 * (lo + hi);
  const glm::dvec3 n = plane.GetNormal();
  const glm::dvec3 centre = c - n * glm::dot(c - o, n);
  const double s = span + 1.0;
  const auto pt = [&](double a, double b) {
    const glm::dvec3 w = centre + u * a + v * b;
    return gp_Pnt(w.x, w.y, w.z);
  };
  BRepBuilderAPI_MakePolygon poly(pt(-s, -s), pt(s, -s), pt(s, s), pt(-s, s),
                                  Standard_True);
  const TopoDS_Face cutter = BRepBuilderAPI_MakeFace(poly.Wire()).Face();
  BRepAlgoAPI_Section section;
  if (!cad::BuildNonDestructive(section, body.shape.GetShape(), cutter)) {
    out.error = "the section of '" + body.name + "' failed in OpenCASCADE";
    return;
  }
  Handle(TopTools_HSequenceOfShape) edges = new TopTools_HSequenceOfShape;
  for (TopExp_Explorer ex(section.Shape(), TopAbs_EDGE); ex.More(); ex.Next())
    edges->Append(ex.Current());
  if (edges->IsEmpty())
    return;
  Handle(TopTools_HSequenceOfShape) wires;
  ShapeAnalysis_FreeBounds::ConnectEdgesToWires(
      edges, std::max(1e-6, deflection * 0.1), Standard_False, wires);
  const auto toUV = [&](const gp_Pnt& p) {
    return plane.WorldToPlane(glm::dvec3(p.X(), p.Y(), p.Z()));
  };
  for (int w = 1; w <= wires->Length(); w++) {
    const TopoDS_Wire wire = TopoDS::Wire(wires->Value(w));
    CutChain chain;
    for (BRepTools_WireExplorer we(wire); we.More(); we.Next()) {
      const TopoDS_Edge& edge = we.Current();
      BRepAdaptor_Curve curve(edge);
      const std::vector<cad::CurveSample> samples =
          cad::SampleCurve(curve, deflection);
      std::vector<glm::dvec2> pts;
      if (samples.size() >= 2) {
        for (const cad::CurveSample& q : samples)
          pts.push_back(toUV(q.p));
      } else {
        pts.push_back(toUV(curve.Value(curve.FirstParameter())));
        pts.push_back(toUV(curve.Value(curve.LastParameter())));
      }
      if (we.Orientation() == TopAbs_REVERSED)
        std::reverse(pts.begin(), pts.end());
      for (const glm::dvec2& q : pts)
        if (chain.points.empty() || glm::length(q - chain.points.back()) > 1e-9)
          chain.points.push_back(q);
    }
    chain.closed = BRep_Tool::IsClosed(wire) ||
                   (chain.points.size() > 2 &&
                    glm::length(chain.points.front() - chain.points.back()) <=
                        std::max(1e-6, deflection * 0.1));
    if (chain.closed && chain.points.size() > 2 &&
        glm::length(chain.points.front() - chain.points.back()) <=
            std::max(1e-6, deflection * 0.1))
      chain.points.pop_back();
    if (chain.points.size() >= 2)
      out.chains.push_back(std::move(chain));
  }
}

} // namespace

SectionCut CutBodies(const std::vector<const Body*>& bodies,
                     const SketchPlane& plane, double deflection) {
  SectionCut out;
  glm::dvec3 lo(kInf), hi(-kInf);
  for (const Body* b : bodies) {
    glm::dvec3 a, z;
    if (b != nullptr && !b->reference && BodyBounds(*b, a, z)) {
      lo = glm::min(lo, a);
      hi = glm::max(hi, z);
    }
  }
  if (!(lo.x <= hi.x))
    return out;
  out.diagonal = glm::length(hi - lo);
  for (const Body* b : bodies) {
    if (b == nullptr || b->reference)
      continue;
    if (b->IsMesh()) {
      const mesh::SliceResult s =
          mesh::Slice(*b->mesh, plane.GetOrigin(), plane.GetNormal());
      for (const mesh::SlicePolyline& l : s.polylines) {
        CutChain chain;
        chain.closed = l.closed;
        chain.points.reserve(l.points.size());
        for (const glm::dvec3& p : l.points)
          chain.points.push_back(plane.WorldToPlane(p));
        if (chain.points.size() >= 2)
          out.chains.push_back(std::move(chain));
      }
      continue;
    }
    if (!b->shape.IsValid())
      continue;
    try {
      CutBrep(*b, plane, deflection, out.diagonal, out);
    } catch (const Standard_Failure& e) {
      out.error = "the section of '" + b->name +
                  "' failed in OpenCASCADE: " + e.GetMessageString();
    }
    if (!out.error.empty())
      return out;
  }
  return out;
}

} // namespace sketch
