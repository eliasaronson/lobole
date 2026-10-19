// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/MeasureSections.h"

#include "cad/NonDestructiveBoolean.h"
#include "core/Parallel.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepGProp.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <GProp_GProps.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>

namespace Measure::sections {

namespace {

using Clipper2Lib::Path64;
using Clipper2Lib::Paths64;
using Clipper2Lib::Point64;

// Two events nearer than this along the axis are one, mm.
constexpr double kEventTol = 1.0e-6;
// A band thinner than this is a face meeting a face, not matter, mm.
constexpr double kThinBand = 1.0e-5;
// The triangles a slicing mesh aims at, and the most it may come to: a
// bound on the memory one solid's slices take (a mesh is ~100 B a
// triangle while it is built).
constexpr double kMeshBudget = 1.5e6;
constexpr size_t kMeshCeiling = 6000000;
// The most bands one solid is cut into.
constexpr size_t kBandCeiling = 20000;

double BoxRadialMin(const Bnd_Box& b) {
  double x0, y0, z0, x1, y1, z1;
  b.Get(x0, y0, z0, x1, y1, z1);
  const double dx = x0 > 0.0 ? x0 : (x1 < 0.0 ? -x1 : 0.0);
  const double dy = y0 > 0.0 ? y0 : (y1 < 0.0 ? -y1 : 0.0);
  return std::hypot(dx, dy);
}

// Is the face along the axis -- its normal square to Z everywhere -- so
// that it sweeps one curve along Z and the section does not change over it?
// Horizontal faces are not along it, but sit at one Z and change nothing
// between events. Anything else varies.
enum class FaceKind { Along, Flat, Varies };

FaceKind Classify(const TopoDS_Face& f, double zSpan) {
  if (zSpan <= kEventTol)
    return FaceKind::Flat;
  BRepAdaptor_Surface s(f);
  constexpr double kSquare = 1.0e-9;
  switch (s.GetType()) {
  case GeomAbs_Plane:
    return std::abs(s.Plane().Axis().Direction().Z()) < kSquare
               ? FaceKind::Along
               : FaceKind::Varies;
  case GeomAbs_Cylinder:
    return std::abs(s.Cylinder().Axis().Direction().Z()) > 1.0 - kSquare
               ? FaceKind::Along
               : FaceKind::Varies;
  case GeomAbs_SurfaceOfExtrusion:
    return std::abs(s.Direction().Z()) > 1.0 - kSquare ? FaceKind::Along
                                                       : FaceKind::Varies;
  case GeomAbs_Cone:
  case GeomAbs_Sphere:
  case GeomAbs_Torus:
  case GeomAbs_SurfaceOfRevolution:
    return FaceKind::Varies;
  default:
    break;
  }
  // A free-form face: along the axis only if every sampled normal is.
  double u0, u1, v0, v1;
  BRepTools::UVBounds(f, u0, u1, v0, v1);
  BRepLProp_SLProps props(s, 1, 1.0e-9);
  constexpr int n = 6;
  for (int i = 0; i <= n; i++)
    for (int j = 0; j <= n; j++) {
      props.SetParameters(u0 + (u1 - u0) * i / n, v0 + (v1 - v0) * j / n);
      if (!props.IsNormalDefined())
        continue;
      if (std::abs(props.Normal().Z()) > 1.0e-7)
        return FaceKind::Varies;
    }
  return FaceKind::Along;
}

struct Span {
  double lo, hi;
};

std::vector<Span> Merged(std::vector<Span> v) {
  std::sort(v.begin(), v.end(),
            [](const Span& a, const Span& b) { return a.lo < b.lo; });
  std::vector<Span> out;
  for (const Span& s : v) {
    if (!out.empty() && s.lo <= out.back().hi + kEventTol)
      out.back().hi = std::max(out.back().hi, s.hi);
    else
      out.push_back(s);
  }
  return out;
}

Point64 Pt(double x, double y) {
  return Point64(static_cast<int64_t>(std::llround(x * kScale)),
                 static_cast<int64_t>(std::llround(y * kScale)));
}

// The section's edges, each a polyline, chained end to end into loops.
Paths64 Loops(const TopoDS_Shape& section, double deflection) {
  std::vector<std::vector<Point64>> pieces;
  for (TopExp_Explorer e(section, TopAbs_EDGE); e.More(); e.Next()) {
    const TopoDS_Edge& edge = TopoDS::Edge(e.Current());
    if (BRep_Tool::Degenerated(edge))
      continue;
    BRepAdaptor_Curve c(edge);
    GCPnts_TangentialDeflection d(c, 0.05, deflection, 2);
    std::vector<Point64> pts;
    for (int i = 1; i <= d.NbPoints(); i++) {
      const gp_Pnt p = d.Value(i);
      const Point64 q = Pt(p.X(), p.Y());
      if (pts.empty() || pts.back() != q)
        pts.push_back(q);
    }
    if (pts.size() >= 2)
      pieces.push_back(std::move(pts));
  }
  // Ends within this are one point (the section's own tolerance).
  const int64_t tol = static_cast<int64_t>(1.0e-4 * kScale);
  const auto meets = [tol](const Point64& a, const Point64& b) {
    return std::abs(a.x - b.x) <= tol && std::abs(a.y - b.y) <= tol;
  };
  std::vector<bool> used(pieces.size(), false);
  Paths64 loops;
  for (size_t s = 0; s < pieces.size(); s++) {
    if (used[s])
      continue;
    used[s] = true;
    Path64 loop(pieces[s].begin(), pieces[s].end());
    while (loop.size() < 3 || !meets(loop.front(), loop.back())) {
      bool grew = false;
      for (size_t k = 0; k < pieces.size(); k++) {
        if (used[k])
          continue;
        const auto& p = pieces[k];
        if (meets(p.front(), loop.back())) {
          loop.insert(loop.end(), p.begin() + 1, p.end());
        } else if (meets(p.back(), loop.back())) {
          loop.insert(loop.end(), p.rbegin() + 1, p.rend());
        } else {
          continue;
        }
        used[k] = true;
        grew = true;
        break;
      }
      if (!grew)
        break;
    }
    if (loop.size() >= 3)
      loops.push_back(std::move(loop));
  }
  // Even-odd over one solid's loops: a hole is a hole whichever way it ran.
  return Clipper2Lib::Union(loops, Clipper2Lib::FillRule::EvenOdd);
}

Paths64 SectionAt(const TopoDS_Shape& solid, double z, double half,
                  double deflection) {
  const TopoDS_Face plane =
      BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0.0, 0.0, z), gp_Dir(0, 0, 1)),
                              -half, half, -half, half)
          .Face();
  BRepAlgoAPI_Section sec;
  sec.Approximation(Standard_False);
  sec.ComputePCurveOn1(Standard_False);
  sec.ComputePCurveOn2(Standard_False);
  if (!cad::BuildNonDestructive(sec, solid, plane) || sec.Shape().IsNull())
    throw std::runtime_error("sectioning a solid across the axis failed");
  return Loops(sec.Shape(), deflection);
}

Path64 Disk(double r) {
  // Circumscribed, so clipping to it never shaves what is within r.
  const int n = 256;
  const double R = r / std::cos(std::numbers::pi / n);
  Path64 p;
  for (int i = 0; i < n; i++) {
    const double a = 2.0 * std::numbers::pi * i / n;
    p.push_back(Pt(R * std::cos(a), R * std::sin(a)));
  }
  return p;
}

double SegDist(const Point64& a, const Point64& b, const Point64& p) {
  const double ax = static_cast<double>(a.x), ay = static_cast<double>(a.y);
  const double dx = static_cast<double>(b.x) - ax,
               dy = static_cast<double>(b.y) - ay;
  const double px = static_cast<double>(p.x) - ax,
               py = static_cast<double>(p.y) - ay;
  const double l2 = dx * dx + dy * dy;
  double t = l2 > 0.0 ? (px * dx + py * dy) / l2 : 0.0;
  t = std::clamp(t, 0.0, 1.0);
  return std::hypot(px - t * dx, py - t * dy);
}

bool SegsCross(const Point64& a, const Point64& b, const Point64& c,
               const Point64& d) {
  const auto cross = [](const Point64& o, const Point64& p, const Point64& q) {
    return static_cast<double>(p.x - o.x) * static_cast<double>(q.y - o.y) -
           static_cast<double>(p.y - o.y) * static_cast<double>(q.x - o.x);
  };
  const double d1 = cross(c, d, a), d2 = cross(c, d, b), d3 = cross(a, b, c),
               d4 = cross(a, b, d);
  return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}

// Chains segments end to end into loops, by hashed end points: a mesh's
// slice is thousands of them.
Paths64 Chain(const std::vector<std::pair<Point64, Point64>>& segs) {
  // Ends nearer than this are one point (the same mesh node's two
  // interpolations, by two faces' triangulations).
  constexpr int64_t kTol = static_cast<int64_t>(1.0e-4 * kScale);
  const auto cell = [](const Point64& p) {
    return std::pair<int64_t, int64_t>(p.x / kTol, p.y / kTol);
  };
  std::map<std::pair<int64_t, int64_t>, std::vector<size_t>> at;
  for (size_t i = 0; i < segs.size(); i++) {
    at[cell(segs[i].first)].push_back(2 * i);
    at[cell(segs[i].second)].push_back(2 * i + 1);
  }
  const auto end = [&](size_t e) -> const Point64& {
    return e % 2 ? segs[e / 2].second : segs[e / 2].first;
  };
  std::vector<bool> used(segs.size(), false);
  // An unused segment with an end near p, and that end; else npos.
  const auto next = [&](const Point64& p) -> size_t {
    const auto [cx, cy] = cell(p);
    for (int64_t dx = -1; dx <= 1; dx++)
      for (int64_t dy = -1; dy <= 1; dy++) {
        const auto it = at.find({cx + dx, cy + dy});
        if (it == at.end())
          continue;
        for (size_t e : it->second)
          if (!used[e / 2] && std::abs(end(e).x - p.x) <= kTol &&
              std::abs(end(e).y - p.y) <= kTol)
            return e;
      }
    return static_cast<size_t>(-1);
  };
  // Chains, each grown from both ends until nothing meets either.
  std::vector<Path64> chains;
  for (size_t s = 0; s < segs.size(); s++) {
    if (used[s])
      continue;
    used[s] = true;
    std::deque<Point64> chain{segs[s].first, segs[s].second};
    for (bool front : {false, true})
      for (;;) {
        const size_t e = next(front ? chain.front() : chain.back());
        if (e == static_cast<size_t>(-1))
          break;
        used[e / 2] = true;
        if (front)
          chain.push_front(end(e ^ 1));
        else
          chain.push_back(end(e ^ 1)); // the segment's other end
      }
    chains.emplace_back(chain.begin(), chain.end());
  }
  // A triangulation is not always watertight along a free-form face's
  // edges: an open chain is joined to the nearest open end, nearest gaps
  // first, while the gap is under `bridge` -- the slice is one region, and
  // a gap that size changes nothing a slice stands for.
  const double bridge = 50.0 * static_cast<double>(kTol);
  const auto gap = [](const Point64& a, const Point64& b) {
    return std::hypot(static_cast<double>(a.x - b.x),
                      static_cast<double>(a.y - b.y));
  };
  const auto closed = [&](const Path64& c) {
    return c.size() >= 3 && gap(c.front(), c.back()) <= kTol;
  };
  for (;;) {
    double best = bridge;
    size_t bi = 0, bj = 0;
    bool found = false, bFront = false;
    for (size_t i = 0; i < chains.size(); i++) {
      if (chains[i].empty() || closed(chains[i]))
        continue;
      // Its own two ends: closing it.
      const double self = gap(chains[i].front(), chains[i].back());
      if (self < best && chains[i].size() >= 3) {
        best = self;
        bi = bj = i;
        found = true;
      }
      for (size_t j = 0; j < chains.size(); j++) {
        if (j == i || chains[j].empty() || closed(chains[j]))
          continue;
        for (bool jf : {true, false}) {
          const double d =
              gap(chains[i].back(), jf ? chains[j].front() : chains[j].back());
          if (d < best) {
            best = d;
            bi = i;
            bj = j;
            bFront = jf;
            found = true;
          }
        }
      }
    }
    if (!found)
      break;
    if (bi == bj) {
      chains[bi].push_back(chains[bi].front()); // closed
      continue;
    }
    Path64& a = chains[bi];
    Path64 b = std::move(chains[bj]);
    chains[bj].clear();
    if (!bFront)
      std::reverse(b.begin(), b.end());
    a.insert(a.end(), b.begin(), b.end());
  }
  Paths64 loops;
  for (Path64& c : chains)
    if (c.size() >= 3)
      loops.push_back(std::move(c));
  return Clipper2Lib::Union(loops, Clipper2Lib::FillRule::EvenOdd);
}

// The solid's sections at each of `zs` (ascending), from one fine
// triangulation of a copy of it: a boolean per slice is seconds on a
// free-form face (a twisted tooth flank), a mesh slice microseconds. Each
// triangle edge crossing a plane is cut where both its faces cut it -- the
// edge's ends in one order -- so the segments close into loops.
std::vector<Paths64> MeshSlices(const TopoDS_Shape& solid,
                                const std::vector<double>& zs,
                                double deflection) {
  const TopoDS_Shape own = BRepBuilderAPI_Copy(solid, Standard_True).Shape();
  BRepMesh_IncrementalMesh mesh(own, deflection, Standard_False, 0.2,
                                Standard_True);
  size_t triangles = 0;
  for (TopExp_Explorer e(own, TopAbs_FACE); e.More(); e.Next()) {
    TopLoc_Location loc;
    const Handle(Poly_Triangulation) tri =
        BRep_Tool::Triangulation(TopoDS::Face(e.Current()), loc);
    if (!tri.IsNull())
      triangles += static_cast<size_t>(tri->NbTriangles());
  }
  if (triangles > kMeshCeiling)
    throw std::runtime_error(
        "slicing a solid would take a mesh of " + std::to_string(triangles) +
        " triangles, past the " + std::to_string(kMeshCeiling) +
        " the insertion check allows: give the step a coarser resolution "
        "(fewer samples)");
  std::vector<std::vector<std::pair<Point64, Point64>>> segs(zs.size());
  for (TopExp_Explorer e(own, TopAbs_FACE); e.More(); e.Next()) {
    TopLoc_Location loc;
    const Handle(Poly_Triangulation) tri =
        BRep_Tool::Triangulation(TopoDS::Face(e.Current()), loc);
    if (tri.IsNull())
      throw std::runtime_error("a face of the solid would not triangulate");
    const gp_Trsf xf = loc.Transformation();
    std::vector<gp_Pnt> nodes(tri->NbNodes());
    for (int i = 1; i <= tri->NbNodes(); i++)
      nodes[i - 1] = tri->Node(i).Transformed(xf);
    for (int t = 1; t <= tri->NbTriangles(); t++) {
      int n[3];
      tri->Triangle(t).Get(n[0], n[1], n[2]);
      const gp_Pnt* p[3] = {&nodes[n[0] - 1], &nodes[n[1] - 1],
                            &nodes[n[2] - 1]};
      const double lo = std::min({p[0]->Z(), p[1]->Z(), p[2]->Z()});
      const double hi = std::max({p[0]->Z(), p[1]->Z(), p[2]->Z()});
      // A node exactly on a plane counts as above it, so every crossing
      // edge has one end each side.
      auto k = std::upper_bound(zs.begin(), zs.end(), lo) - zs.begin();
      for (; k < static_cast<long>(zs.size()) && zs[k] <= hi; k++) {
        const double z = zs[k];
        Point64 cut[2];
        int m = 0;
        for (int i = 0; i < 3 && m < 2; i++) {
          const gp_Pnt* a = p[i];
          const gp_Pnt* b = p[(i + 1) % 3];
          if ((a->Z() >= z) == (b->Z() >= z))
            continue;
          // One order for the edge, whichever triangle asks.
          if (std::make_tuple(a->Z(), a->X(), a->Y()) >
              std::make_tuple(b->Z(), b->X(), b->Y()))
            std::swap(a, b);
          const double f = (z - a->Z()) / (b->Z() - a->Z());
          cut[m++] = Pt(a->X() + f * (b->X() - a->X()),
                        a->Y() + f * (b->Y() - a->Y()));
        }
        if (m == 2 && cut[0] != cut[1])
          segs[k].emplace_back(cut[0], cut[1]);
      }
    }
  }
  std::vector<Paths64> out(zs.size());
  for (size_t k = 0; k < zs.size(); k++)
    out[k] = Chain(segs[k]);
  return out;
}

} // namespace

Clipper2Lib::Paths64 Rotated(const Clipper2Lib::Paths64& p, double deg) {
  if (deg == 0.0)
    return p;
  const double a = deg * std::numbers::pi / 180.0;
  const double c = std::cos(a), s = std::sin(a);
  Paths64 out;
  out.reserve(p.size());
  for (const Path64& path : p) {
    Path64 q;
    q.reserve(path.size());
    for (const Point64& v : path) {
      const double x = static_cast<double>(v.x), y = static_cast<double>(v.y);
      q.emplace_back(static_cast<int64_t>(std::llround(c * x - s * y)),
                     static_cast<int64_t>(std::llround(s * x + c * y)));
    }
    out.push_back(std::move(q));
  }
  return out;
}

void Refresh(Band& b) {
  b.box = Clipper2Lib::GetBounds(b.region);
  double rmax = 0.0, perim = 0.0;
  for (const Path64& path : b.region) {
    for (size_t i = 0; i < path.size(); i++) {
      const Point64& p = path[i];
      rmax = std::max(
          rmax, std::hypot(static_cast<double>(p.x), static_cast<double>(p.y)));
      const Point64& q = path[(i + 1) % path.size()];
      perim += std::hypot(static_cast<double>(q.x - p.x),
                          static_cast<double>(q.y - p.y));
    }
  }
  // Nearest the axis: zero when the region holds it, else its nearest edge.
  double rmin = 0.0;
  if (Clipper2Lib::PointInPolygonResult::IsOutside == [&] {
        // Non-zero over the region's paths: inside an outer, not a hole.
        int wind = 0;
        for (const Path64& path : b.region) {
          const auto r = Clipper2Lib::PointInPolygon(Point64(0, 0), path);
          if (r == Clipper2Lib::PointInPolygonResult::IsOn)
            return Clipper2Lib::PointInPolygonResult::IsOn;
          if (r == Clipper2Lib::PointInPolygonResult::IsInside)
            wind += Clipper2Lib::IsPositive(path) ? 1 : -1;
        }
        return wind != 0 ? Clipper2Lib::PointInPolygonResult::IsInside
                         : Clipper2Lib::PointInPolygonResult::IsOutside;
      }()) {
    rmin = std::numeric_limits<double>::infinity();
    for (const Path64& path : b.region)
      for (size_t i = 0; i < path.size(); i++)
        rmin = std::min(
            rmin, SegDist(path[i], path[(i + 1) % path.size()], Point64(0, 0)));
  }
  b.rMin = rmin / kScale;
  b.rMax = rmax / kScale;
  b.perimeterMm = perim / kScale;
}

double BoundaryGap(const Clipper2Lib::Paths64& a, const Clipper2Lib::Paths64& b,
                   const Clipper2Lib::Rect64& window, double reachMm) {
  const int64_t g = static_cast<int64_t>(std::ceil(reachMm * kScale));
  const auto in = [&](const Point64& p) {
    return p.x >= window.left - g && p.x <= window.right + g &&
           p.y >= window.top - g && p.y <= window.bottom + g;
  };
  struct Seg {
    Point64 p, q;
  };
  const auto segs = [&](const Paths64& ps) {
    std::vector<Seg> out;
    for (const Path64& path : ps)
      for (size_t i = 0; i < path.size(); i++) {
        const Point64& p = path[i];
        const Point64& q = path[(i + 1) % path.size()];
        if (in(p) || in(q))
          out.push_back({p, q});
      }
    return out;
  };
  const std::vector<Seg> sa = segs(a), sb = segs(b);
  double best = std::numeric_limits<double>::infinity();
  for (const Seg& x : sa)
    for (const Seg& y : sb) {
      if (SegsCross(x.p, x.q, y.p, y.q))
        return 0.0;
      best = std::min({best, SegDist(x.p, x.q, y.p), SegDist(x.p, x.q, y.q),
                       SegDist(y.p, y.q, x.p), SegDist(y.p, y.q, x.q)});
    }
  return best / kScale;
}

Solid Prepare(const TopoDS_Shape& solid) {
  Solid out;
  out.shape = solid;
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(solid, TopAbs_FACE, faces);
  for (int i = 1; i <= faces.Extent(); i++) {
    Bnd_Box box;
    BRepBndLib::AddOptimal(faces(i), box, Standard_False, Standard_False);
    if (box.IsVoid())
      continue;
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    out.half = std::max(
        {out.half, std::abs(x0), std::abs(x1), std::abs(y0), std::abs(y1)});
    out.faces.push_back({i, z0, z1, BoxRadialMin(box)});
  }
  return out;
}

bool ReachSpan(const Solid& s, double reachMm, double& lo, double& hi) {
  lo = std::numeric_limits<double>::infinity();
  hi = -lo;
  for (const Solid::Face& f : s.faces)
    if (f.rMin <= reachMm) {
      lo = std::min(lo, f.z0);
      hi = std::max(hi, f.z1);
    }
  return hi > lo;
}

std::vector<Band> SolidBands(const Solid& solidIn, const BandOptions& opt,
                             int solidIndex, const std::atomic<bool>* cancel) {
  const TopoDS_Shape& solid = solidIn.shape;
  // The events: where a face within reach begins or ends along the axis;
  // and the spans a face varies over.
  std::vector<double> events;
  std::vector<Span> varying;
  double freeFormArea = 0.0; // mm2 of varying free-form faces in reach
  double zMin = std::numeric_limits<double>::infinity(), zMax = -zMin;
  const double half = solidIn.half;
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(solid, TopAbs_FACE, faces);
  for (const Solid::Face& pf : solidIn.faces) {
    const TopoDS_Face& f = TopoDS::Face(faces(pf.index));
    zMin = std::min(zMin, pf.z0);
    zMax = std::max(zMax, pf.z1);
    if (pf.rMin > opt.reachMm)
      continue;
    events.push_back(pf.z0);
    events.push_back(pf.z1);
    for (TopExp_Explorer v(f, TopAbs_VERTEX); v.More(); v.Next())
      events.push_back(BRep_Tool::Pnt(TopoDS::Vertex(v.Current())).Z());
    if (Classify(f, pf.z1 - pf.z0) == FaceKind::Varies) {
      varying.push_back({pf.z0, pf.z1});
      // A free-form face's planar section is a boolean of seconds; its
      // slices come from a mesh, sized by how much of it there is.
      switch (BRepAdaptor_Surface(f).GetType()) {
      case GeomAbs_Plane:
      case GeomAbs_Cylinder:
      case GeomAbs_Cone:
      case GeomAbs_Sphere:
      case GeomAbs_Torus:
        break;
      default: {
        GProp_GProps g;
        BRepGProp::SurfaceProperties(f, g);
        freeFormArea += std::max(0.0, g.Mass());
      }
      }
    }
  }
  if (events.empty() || !(zMax > zMin))
    return {};
  std::sort(events.begin(), events.end());
  const double h = opt.sliceMm;
  varying = Merged(std::move(varying));
  // Inside a span that varies, only the grid cuts it: an event there would
  // cut a slice short off the grid, to be compared with the other side's
  // whole one by a section cut somewhere else.
  const auto inVarying = [&](double z) {
    return std::any_of(varying.begin(), varying.end(),
                       [z](const Span& s) { return z > s.lo && z < s.hi; });
  };
  std::vector<double> cuts;
  for (double z : events)
    if (z >= zMin - kEventTol && z <= zMax + kEventTol && !inVarying(z) &&
        (cuts.empty() || z > cuts.back() + kEventTol))
      cuts.push_back(z);
  // Slice grid points within each varying span, anchored at gridOriginMm;
  // its ends are grid points too.
  const double g0 = opt.gridOriginMm;
  for (const Span& s : varying) {
    cuts.push_back(s.lo);
    cuts.push_back(s.hi);
    for (double k = std::ceil((s.lo - g0) / h); g0 + k * h < s.hi; k += 1.0)
      cuts.push_back(g0 + k * h);
  }
  std::sort(cuts.begin(), cuts.end());
  cuts.erase(std::unique(cuts.begin(), cuts.end(),
                         [](double a, double b) {
                           return std::abs(a - b) <= kEventTol;
                         }),
             cuts.end());
  const Paths64 clip =
      std::isfinite(opt.reachMm) && opt.reachMm < std::hypot(half, half)
          ? Paths64{Disk(opt.reachMm)}
          : Paths64{};
  // The bands' spans, then their sections: independent, so on every core,
  // each worker on its own deep copy of the solid -- a section is a boolean,
  // and a boolean reads lazily built caches on what it is given (Parallel.h).
  std::vector<Band> spans;
  if (cuts.size() > kBandCeiling)
    throw std::runtime_error(
        "a solid would be cut into " + std::to_string(cuts.size()) +
        " bands, past the " + std::to_string(kBandCeiling) +
        " the insertion check allows: give it a coarser resolution (fewer "
        "samples)");
  for (size_t i = 0; i + 1 < cuts.size(); i++) {
    Band b;
    b.z0 = cuts[i];
    b.z1 = cuts[i + 1];
    if (b.z1 - b.z0 < kThinBand)
      continue;
    b.sliced = inVarying(0.5 * (b.z0 + b.z1));
    b.zSection = 0.5 * (b.z0 + b.z1);
    if (b.sliced) {
      // Cut as near its grid cell's middle as it reaches, where the other
      // side's slice in that cell is cut too: the span's end slices,
      // short, compare with the other side's as nearly level as they can.
      const double c = g0 + (std::floor((b.zSection - g0) / h) + 0.5) * h;
      const double e = std::min(1.0e-3, 0.25 * (b.z1 - b.z0));
      b.zSection = std::clamp(c, b.z0 + e, b.z1 - e);
    }
    b.solid = solidIndex;
    spans.push_back(std::move(b));
  }
  // The sliced spans from one fine mesh; the prismatic ones each by its
  // exact section.
  std::vector<size_t> exact;
  {
    std::vector<size_t> sliced;
    std::vector<double> zs;
    for (size_t i = 0; i < spans.size(); i++)
      if (spans[i].sliced && freeFormArea > 0.0) {
        sliced.push_back(i);
        zs.push_back(spans[i].zSection);
      } else {
        exact.push_back(i);
      }
    if (!zs.empty()) {
      try {
        // About area / (4 d) triangles on a millimetre's curvature: the
        // deflection grows with the free-form area so the mesh stays
        // within its budget (a herringbone gear, a threaded rod: 0.5 um).
        const double deflection =
            std::max(opt.meshDeflectionMm, freeFormArea / (4.0 * kMeshBudget));
        std::vector<Paths64> cuts = MeshSlices(solid, zs, deflection);
        for (size_t k = 0; k < sliced.size(); k++) {
          Band& b = spans[sliced[k]];
          b.region = std::move(cuts[k]);
          b.noiseMm = deflection;
          if (!clip.empty() && !b.region.empty())
            b.region = Clipper2Lib::Intersect(b.region, clip,
                                              Clipper2Lib::FillRule::NonZero);
          if (!b.region.empty())
            Refresh(b);
        }
      } catch (const Standard_Failure& e) {
        throw std::runtime_error(
            std::string("triangulating a solid to slice it failed: ") +
            (e.GetMessageString() ? e.GetMessageString() : ""));
      }
    }
  }
  const size_t chunks =
      std::max<size_t>(1, std::min(parallel::Workers(), exact.size() / 2));
  std::vector<std::string> errors(chunks);
  parallel::For(chunks, [&](size_t c) {
    try {
      const TopoDS_Shape own =
          chunks == 1 ? solid
                      : BRepBuilderAPI_Copy(solid, Standard_True).Shape();
      for (size_t x = c; x < exact.size(); x += chunks) {
        if (cancel && cancel->load())
          return;
        Band& b = spans[exact[x]];
        b.region =
            SectionAt(own, b.zSection, 2.0 * half + 1.0, opt.deflectionMm);
        if (!clip.empty() && !b.region.empty())
          b.region = Clipper2Lib::Intersect(b.region, clip,
                                            Clipper2Lib::FillRule::NonZero);
        if (!b.region.empty())
          Refresh(b);
      }
    } catch (const Standard_Failure& e) {
      errors[c] = std::string("sectioning a solid across the axis failed: ") +
                  (e.GetMessageString() ? e.GetMessageString() : "");
    } catch (const std::exception& e) {
      errors[c] = e.what();
    }
  });
  for (const std::string& e : errors)
    if (!e.empty())
      throw std::runtime_error(e);
  if (cancel && cancel->load())
    return {};
  std::vector<Band> out;
  for (Band& b : spans) {
    if (b.region.empty())
      continue;
    // A prismatic band whose neighbour has the very same section is one
    // band: merged, the pair checks run once over its whole length.
    if (!b.sliced && !out.empty() && !out.back().sliced &&
        std::abs(out.back().z1 - b.z0) <= kEventTol &&
        out.back().region == b.region) {
      out.back().z1 = b.z1;
      continue;
    }
    out.push_back(std::move(b));
  }
  return out;
}

std::vector<Band> Moved(const std::vector<Band>& bands,
                        const glm::dmat2& across, double zSign,
                        const glm::dvec3& shift, double reachMm) {
  const double sx = shift.x * kScale, sy = shift.y * kScale;
  const bool mirror = glm::determinant(across) < 0.0;
  const bool identity = across == glm::dmat2(1.0);
  const bool reach = std::isfinite(reachMm);
  const double r2 = reachMm * kScale;
  const Paths64 disk = reach ? Paths64{Disk(reachMm)} : Paths64{};
  std::vector<Band> out;
  out.reserve(bands.size());
  const auto place = [&](const Band& in) -> std::optional<Band> {
    Band b = in;
    if (zSign < 0.0) {
      b.z0 = -in.z1;
      b.z1 = -in.z0;
      b.zSection = -in.zSection;
    }
    b.z0 += shift.z;
    b.z1 += shift.z;
    b.zSection += shift.z;
    for (Path64& path : b.region) {
      for (Point64& v : path) {
        const double x = static_cast<double>(v.x), y = static_cast<double>(v.y);
        const glm::dvec2 q =
            identity ? glm::dvec2(x, y) : across * glm::dvec2(x, y);
        v = Point64(static_cast<int64_t>(std::llround(q.x + sx)),
                    static_cast<int64_t>(std::llround(q.y + sy)));
      }
      if (mirror)
        std::reverse(path.begin(), path.end());
    }
    if (reach) {
      const Clipper2Lib::Rect64 box = Clipper2Lib::GetBounds(b.region);
      // The box's nearest and furthest from the axis, along x and y.
      const auto nearest = [](int64_t lo, int64_t hi) {
        return lo > 0 ? static_cast<double>(lo)
                      : (hi < 0 ? static_cast<double>(-hi) : 0.0);
      };
      const auto furthest = [](int64_t lo, int64_t hi) {
        return std::max(std::abs(static_cast<double>(lo)),
                        std::abs(static_cast<double>(hi)));
      };
      if (std::hypot(nearest(box.left, box.right),
                     nearest(box.top, box.bottom)) > r2)
        return std::nullopt; // wholly beyond reach
      if (std::hypot(furthest(box.left, box.right),
                     furthest(box.top, box.bottom)) > r2)
        b.region = Clipper2Lib::Intersect(b.region, disk,
                                          Clipper2Lib::FillRule::NonZero);
    }
    if (b.region.empty())
      return std::nullopt;
    Refresh(b);
    return b;
  };
  const size_t n = bands.size();
  for (size_t k = 0; k < n; k++) {
    std::optional<Band> b = place(bands[zSign < 0.0 ? n - 1 - k : k]);
    if (!b)
      continue;
    if (!b->sliced && !out.empty() && !out.back().sliced &&
        std::abs(out.back().z1 - b->z0) <= kEventTol &&
        out.back().region == b->region) {
      out.back().z1 = b->z1;
      continue;
    }
    out.push_back(std::move(*b));
  }
  return out;
}

BandCache::Key BandCache::KeyOf(uint64_t key, const glm::dvec3& dir,
                                double sliceMm) {
  const auto q = [](double v) {
    return static_cast<int64_t>(std::llround(v * 1.0e9));
  };
  return {key, q(dir.x), q(dir.y), q(dir.z), q(sliceMm)};
}

const BandCache::Entry* BandCache::Find(uint64_t key, const glm::dvec3& dir,
                                        double sliceMm) {
  std::lock_guard lock(m_mutex);
  auto it = m_entries.find(KeyOf(key, dir, sliceMm));
  if (it == m_entries.end())
    return nullptr;
  it->second.used = ++m_clock;
  return &it->second;
}

double BandCache::SliceFor(const glm::dvec3& dir, double sliceMm) {
  std::lock_guard lock(m_mutex);
  const Key k = KeyOf(0, dir, sliceMm);
  std::vector<double>& cut =
      m_slices[{std::get<1>(k), std::get<2>(k), std::get<3>(k)}];
  double best = 0.0;
  for (double h : cut)
    if (h <= sliceMm * (1.0 + 1e-9) && h >= sliceMm / 16.0 * (1.0 - 1e-9))
      best = std::max(best, h);
  if (best > 0.0)
    return best;
  cut.push_back(sliceMm);
  return sliceMm;
}

void BandCache::Put(uint64_t key, const glm::dvec3& dir, double sliceMm,
                    Entry e) {
  std::lock_guard lock(m_mutex);
  e.points = 0;
  for (const Band& b : e.bands)
    for (const Path64& p : b.region)
      e.points += p.size();
  e.used = ++m_clock;
  const Key k = KeyOf(key, dir, sliceMm);
  if (auto it = m_entries.find(k); it != m_entries.end()) {
    m_points -= it->second.points;
    m_entries.erase(it);
  }
  // Too big to keep at all: not kept.
  if (e.points > kCachePointCeiling)
    return;
  while (m_points + e.points > kCachePointCeiling && !m_entries.empty()) {
    auto oldest = m_entries.begin();
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
      if (it->second.used < oldest->second.used)
        oldest = it;
    m_points -= oldest->second.points;
    m_entries.erase(oldest);
    stats.evicted++;
  }
  m_points += e.points;
  m_entries.emplace(k, std::move(e));
}

} // namespace Measure::sections
