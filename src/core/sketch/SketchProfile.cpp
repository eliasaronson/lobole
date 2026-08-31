#include "core/sketch/SketchProfile.h"

#include "common/Logger.h"
#include "core/ElementHistory.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GC_MakeSegment.hxx>
#include <GeomAbs_CurveType.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Precision.hxx>
#include <ShapeFix_Wire.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Elips.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <memory>

namespace sketch {

namespace {
constexpr double kChainTol = 1e-4;
constexpr int kCircleSegments = 32;

// True if two UV points are within the chaining tolerance.
bool Near(const glm::vec2& a, const glm::vec2& b) {
  return glm::length(a - b) < kChainTol;
}

// A point on a circle at UV angle `ang`.
glm::vec2 OnCircle(const glm::vec2& c, double r, double ang) {
  return {c.x + static_cast<float>(r * std::cos(ang)),
          c.y + static_cast<float>(r * std::sin(ang))};
}

// Tessellates one loop edge into UV points in traversal order (a → b). A line
// is its two endpoints; an arc is sampled along its CCW span, then oriented so
// the first point matches `a`.
std::vector<glm::vec2> TessellateLoopEdge(const LoopEdge& e) {
  if (e.spline)
    return BSplineSampleUV(*e.spline); // poles pre-oriented a → b
  if (e.ellipseArc) {
    std::vector<glm::vec2> pts = EllipseArcSampleUV(*e.ellipseArc); // canonical
    if (!pts.empty() &&
        glm::length(pts.front() - e.a) > glm::length(pts.back() - e.a))
      std::reverse(pts.begin(), pts.end()); // orient to traversal a → b
    return pts;
  }
  if (!e.arc)
    return {e.a, e.b};
  const double sweep = e.angEnd - e.angStart; // normalized positive
  int segs = std::max(
      1, static_cast<int>(std::lround(kCircleSegments * sweep / (2.0 * M_PI))));
  std::vector<glm::vec2> pts;
  pts.reserve(segs + 1);
  for (int i = 0; i <= segs; i++)
    pts.push_back(OnCircle(e.center, e.radius,
                           e.angStart + sweep * i / segs)); // canonical CCW
  if (!pts.empty() &&
      glm::length(pts.front() - e.a) > glm::length(pts.back() - e.a))
    std::reverse(pts.begin(), pts.end()); // orient to traversal a → b
  return pts;
}
} // namespace

double Loop::SignedArea() const {
  if (circle) {
    return M_PI * circle->radius * circle->radius;
  }
  if (ellipse) {
    return M_PI * ellipse->rMajor * ellipse->rMinor;
  }
  // Concatenate the boundary as a polyline (drop each edge's duplicated start),
  // then shoelace. Arcs are tessellated so curved area is counted.
  std::vector<glm::vec2> poly;
  for (const auto& e : edges) {
    std::vector<glm::vec2> seg = TessellateLoopEdge(e);
    for (size_t i = (poly.empty() ? 0 : 1); i < seg.size(); i++)
      poly.push_back(seg[i]);
  }
  if (poly.size() >= 2 && Near(poly.front(), poly.back()))
    poly.pop_back();
  double area = 0.0;
  const size_t n = poly.size();
  for (size_t i = 0; i < n; i++) {
    const glm::vec2& p0 = poly[i];
    const glm::vec2& p1 = poly[(i + 1) % n];
    area += static_cast<double>(p0.x) * p1.y - static_cast<double>(p1.x) * p0.y;
  }
  return area * 0.5;
}

std::vector<Loop> BuildLoops(const std::vector<Entity>& entities) {
  std::vector<Loop> loops;

  // Collect chainable edges (lines + arcs); circles are loops already.
  std::vector<LoopEdge> freeEdges;
  for (const auto& entity : entities) {
    if (IsConstruction(entity))
      continue; // reference geometry: never contributes to a profile
    std::visit(
        [&](const auto& e) {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, Circle>) {
            Loop loop;
            loop.circle = e;
            loops.push_back(loop);
          } else if constexpr (std::is_same_v<T, Ellipse>) {
            Loop loop;
            loop.ellipse = e;
            loops.push_back(loop);
          } else if constexpr (std::is_same_v<T, Line>) {
            freeEdges.push_back({false, e.a, e.b, {}, 0.0, 0.0, 0.0, e.id});
          } else if constexpr (std::is_same_v<T, Arc>) {
            double s = e.startAngle, en = e.endAngle;
            while (en <= s)
              en += 2.0 * M_PI; // CCW-always
            // Endpoints via OnCircle, not ArcPointUV: MakeLoopWire re-derives
            // them with OnCircle, and the two round float32 differently.
            freeEdges.push_back({true, OnCircle(e.center, e.radius, s),
                                 OnCircle(e.center, e.radius, en), e.center,
                                 e.radius, s, en, e.id});
          } else if constexpr (std::is_same_v<T, EllipseArc>) {
            LoopEdge ee;
            ee.a = EllipseArcPointUV(e, 1);
            ee.b = EllipseArcPointUV(e, 2);
            ee.entityId = e.id;
            ee.ellipseArc = e; // canonical sweep; forward[] orients it
            freeEdges.push_back(std::move(ee));
          } else if constexpr (std::is_same_v<T, BSpline>) {
            if (e.poles.size() < 2)
              return; // degenerate: contributes nothing
            LoopEdge se;
            se.a = e.poles.front();
            se.b = e.poles.back();
            se.entityId = e.id;
            se.spline = e;
            if (e.poles.size() >= 3 && Near(se.a, se.b)) {
              // A self-closed spline is its own loop (like a circle).
              Loop loop;
              loop.edges.push_back(std::move(se));
              loops.push_back(std::move(loop));
            } else {
              freeEdges.push_back(std::move(se)); // chains with lines/arcs
            }
          }
          // Point contributes nothing to a profile
        },
        entity);
  }

  // Chain edges into closed loops by endpoint proximity. Reversing an edge to
  // fit the walk only swaps its traversal endpoints; the arc's canonical span
  // is untouched, so its geometry survives.
  std::vector<bool> used(freeEdges.size(), false);
  for (size_t start = 0; start < freeEdges.size(); start++) {
    if (used[start])
      continue;

    std::vector<LoopEdge> chain = {freeEdges[start]};
    used[start] = true;
    glm::vec2 tail = chain.front().b;

    bool extended = true;
    while (extended) {
      extended = false;
      for (size_t i = 0; i < freeEdges.size(); i++) {
        if (used[i])
          continue;
        LoopEdge cand = freeEdges[i];
        if (Near(cand.a, tail)) {
          // already oriented a → b
        } else if (Near(cand.b, tail)) {
          std::swap(cand.a, cand.b); // traverse it backwards
          if (cand.spline)           // keep poles in traversal order a → b
            std::reverse(cand.spline->poles.begin(), cand.spline->poles.end());
        } else {
          continue;
        }
        tail = cand.b;
        chain.push_back(cand);
        used[i] = true;
        extended = true;
        break;
      }
    }

    // Closed if the walk returns to the first edge's start.
    if (chain.size() >= 2 && Near(chain.front().a, tail)) {
      Loop loop;
      loop.edges = std::move(chain);
      loops.push_back(loop);
    } else {
      LOG_WARN("Sketch profile: discarding open chain of {} edges",
               chain.size());
    }
  }

  return loops;
}

namespace {

// Builds an OCC clamped, uniform, non-rational B-spline curve on the plane
// from UV control poles. The knot vector matches the header's BSplineKnots
// (distinct knots 0..(n-p) with clamped end multiplicities), so the OCC edge
// and the UV tessellation (de Boor) trace the same curve — the property that
// keeps a spline profile watertight against its rendered/area polyline.
Handle(Geom_BSplineCurve)
    MakeBSplineCurve(const SketchPlane& plane, const BSpline& s) {
  const int n = static_cast<int>(s.poles.size());
  const int p = BSplineDegree(s);
  TColgp_Array1OfPnt poles(1, n);
  for (int i = 0; i < n; i++) {
    const glm::vec3 w = plane.PlaneToWorld(s.poles[i]);
    poles.SetValue(i + 1, gp_Pnt(w.x, w.y, w.z));
  }
  const int nDistinct = n - p + 1;
  TColStd_Array1OfReal knots(1, nDistinct);
  TColStd_Array1OfInteger mults(1, nDistinct);
  for (int i = 0; i < nDistinct; i++) {
    knots.SetValue(i + 1, static_cast<double>(i));
    mults.SetValue(i + 1, 1);
  }
  mults.SetValue(1, p + 1);
  mults.SetValue(nDistinct, p + 1);
  return new Geom_BSplineCurve(poles, knots, mults, p);
}

// Builds an OCCT wire for one loop in world space on the given plane. Every
// wire is passed through ShapeFix_Wire (reorder / reconnect / close) so greedy
// edge order and small endpoint gaps — especially where arcs meet lines — are
// repaired before face building (FreeCAD Sketch::toShape does the same).
TopoDS_Wire MakeLoopWireImpl(const SketchPlane& plane, const Loop& loop) {
  const glm::vec3 n3 = plane.GetNormal();
  const gp_Dir normal(n3.x, n3.y, n3.z);
  auto toPnt = [&](const glm::vec2& uv) {
    glm::vec3 w = plane.PlaneToWorld(uv);
    return gp_Pnt(w.x, w.y, w.z);
  };

  BRepBuilderAPI_MakeWire wire;
  if (loop.circle) {
    glm::vec3 u = plane.GetUAxis();
    gp_Ax2 axis(toPnt(loop.circle->center), normal, gp_Dir(u.x, u.y, u.z));
    gp_Circ circ(axis, loop.circle->radius);
    BRepBuilderAPI_MakeEdge me(circ);
    if (!me.IsDone())
      return TopoDS_Wire();
    wire.Add(me.Edge());
  } else if (loop.ellipse) {
    const Ellipse& el = *loop.ellipse;
    // Major-axis direction: +U rotated by `rotation` within the plane.
    glm::vec3 u = plane.GetUAxis(), v = plane.GetVAxis();
    const double c = std::cos(el.rotation), s = std::sin(el.rotation);
    glm::vec3 major = u * static_cast<float>(c) + v * static_cast<float>(s);
    gp_Ax2 axis(toPnt(el.center), normal, gp_Dir(major.x, major.y, major.z));
    gp_Elips ell(axis, el.rMajor, el.rMinor);
    BRepBuilderAPI_MakeEdge me(ell);
    if (!me.IsDone())
      return TopoDS_Wire();
    wire.Add(me.Edge());
  } else {
    // Consecutive edges share one explicit vertex per chain joint. BuildLoops
    // matched the endpoints within kChainTol, but solver float noise can
    // leave joints ~1e-7 apart — just past MakeWire's default merge tolerance
    // — and MakeWire then silently DROPS the unconnectable edge (a filleted
    // profile lost its arc and 5% of its area this way). Shared vertices
    // connect by construction. Line geometry snaps to the joints exactly;
    // arcs keep their circle, so each joint's tolerance is the measured
    // distance to the adjacent arc endpoints (bounding boxes include vertex
    // tolerances — keep them as tight as the geometry allows).
    const size_t n = loop.edges.size();

    // A lone closed spline is its own loop: build the closed curve directly,
    // no shared-vertex chaining (like a circle/ellipse loop).
    if (n == 1 && loop.edges[0].spline) {
      Handle(Geom_BSplineCurve) curve =
          MakeBSplineCurve(plane, *loop.edges[0].spline);
      BRepBuilderAPI_MakeEdge me(curve);
      if (!me.IsDone())
        return TopoDS_Wire();
      wire.Add(me.Edge());
      if (!wire.IsDone())
        return TopoDS_Wire();
      ShapeFix_Wire fix;
      fix.Load(wire.Wire());
      fix.SetPrecision(1e-4);
      fix.FixReorder();
      fix.FixConnected();
      fix.FixClosed();
      return fix.Wire();
    }

    // Whether edge i is traversed along its canonical CCW span (BuildLoops
    // may have swapped a/b to fit the walk; the span itself is untouched).
    std::vector<bool> forward(n, true);
    for (size_t i = 0; i < n; i++) {
      const LoopEdge& e = loop.edges[i];
      glm::vec2 canonStart;
      if (e.arc)
        canonStart = OnCircle(e.center, e.radius, e.angStart);
      else if (e.ellipseArc)
        canonStart = EllipseArcPointUV(*e.ellipseArc, 1);
      else
        continue;
      forward[i] =
          glm::length(e.a - canonStart) <= glm::length(e.b - canonStart);
    }
    // World-space point where edge i's traversal starts (atB=false) or ends.
    auto endPointWorld = [&](size_t i, bool atB) {
      const LoopEdge& e = loop.edges[i];
      if (e.arc) {
        const double ang = (atB == forward[i]) ? e.angEnd : e.angStart;
        return toPnt(OnCircle(e.center, e.radius, ang));
      }
      if (e.ellipseArc)
        return toPnt(
            EllipseArcPointUV(*e.ellipseArc, (atB == forward[i]) ? 2 : 1));
      return toPnt(atB ? e.b : e.a);
    };

    BRep_Builder builder;
    std::vector<TopoDS_Vertex> joints(n); // joints[i]: edge i -> edge i+1
    for (size_t i = 0; i < n; i++) {
      const size_t next = (i + 1) % n;
      const glm::vec2 mid = (loop.edges[i].b + loop.edges[next].a) * 0.5f;
      const gp_Pnt p = toPnt(mid);
      double tol = Precision::Confusion();
      // The joint must lie on each adjacent curve within its tolerance. Two
      // terms per neighbour: the distance to the traversal endpoint (angular
      // mismatch; the only term splines/ellipse-arcs need), and for arcs the
      // radial distance to the trimming circle itself. MakeEdge projects the
      // vertex onto the exact-double gp_Circ while every UV point here went
      // through float32, so an exactly-authored profile can sit ~1e-6 off
      // the circle — past the endpoint-only tolerance, which made the edge
      // constructor fail on bit patterns (solver-noised GUI arcs, ~1e-6 off
      // their endpoints, accidentally cleared it).
      auto growForEdge = [&](size_t k, bool atB) {
        const LoopEdge& e = loop.edges[k];
        if (!e.arc && !e.spline && !e.ellipseArc)
          return;
        tol = std::max(tol, p.Distance(endPointWorld(k, atB)) +
                                Precision::Confusion());
        if (e.arc)
          tol = std::max(tol, std::abs(p.Distance(toPnt(e.center)) - e.radius) +
                                  Precision::Confusion());
      };
      growForEdge(i, true);
      growForEdge(next, false);
      builder.MakeVertex(joints[i], p, tol);
    }

    for (size_t i = 0; i < n; i++) {
      const LoopEdge& e = loop.edges[i];
      // .Edge() on a not-done maker throws; check IsDone so a failed edge is
      // a failed wire (feature error), not a Standard_Failure escape.
      auto addEdge = [&](BRepBuilderAPI_MakeEdge& me) {
        if (!me.IsDone()) {
          LOG_ERROR("Sketch profile: edge {} of {} failed to build "
                    "(vertex off curve?)",
                    i, n);
          return false;
        }
        wire.Add(me.Edge());
        return true;
      };
      const TopoDS_Vertex& from = joints[(i + n - 1) % n];
      const TopoDS_Vertex& to = joints[i];
      if (e.spline) {
        // Poles are pre-oriented a → b, so the curve runs from `from` to `to`.
        Handle(Geom_BSplineCurve) curve = MakeBSplineCurve(plane, *e.spline);
        BRepBuilderAPI_MakeEdge me(curve, from, to);
        if (!addEdge(me))
          return TopoDS_Wire();
      } else if (e.arc) {
        // Trim the gp_Circ by the shared vertices, ordered to match the
        // canonical CCW span; ShapeFix_Wire fixes up the in-loop orientation.
        glm::vec3 u = plane.GetUAxis();
        gp_Ax2 axis(toPnt(e.center), normal, gp_Dir(u.x, u.y, u.z));
        gp_Circ circ(axis, e.radius);
        BRepBuilderAPI_MakeEdge me(circ, forward[i] ? from : to,
                                   forward[i] ? to : from);
        if (!addEdge(me))
          return TopoDS_Wire();
      } else if (e.ellipseArc) {
        // Trim a gp_Elips built on the major-axis frame (its parametrisation
        // matches EllipseArc's), ordered to the canonical sweep like the arc.
        const EllipseArc& ea = *e.ellipseArc;
        glm::vec3 u = plane.GetUAxis(), v = plane.GetVAxis();
        const double cr = std::cos(ea.rotation), sr = std::sin(ea.rotation);
        glm::vec3 major =
            u * static_cast<float>(cr) + v * static_cast<float>(sr);
        gp_Ax2 axis(toPnt(ea.center), normal,
                    gp_Dir(major.x, major.y, major.z));
        gp_Elips ell(axis, ea.rMajor, ea.rMinor);
        BRepBuilderAPI_MakeEdge me(ell, forward[i] ? from : to,
                                   forward[i] ? to : from);
        if (!addEdge(me))
          return TopoDS_Wire();
      } else {
        const gp_Pnt p0 = BRep_Tool::Pnt(from), p1 = BRep_Tool::Pnt(to);
        if (p0.Distance(p1) < Precision::Confusion())
          continue; // degenerate sliver after snapping
        Handle(Geom_TrimmedCurve) seg = GC_MakeSegment(p0, p1).Value();
        BRepBuilderAPI_MakeEdge me(seg, from, to);
        if (!addEdge(me))
          return TopoDS_Wire();
      }
      if (wire.Error() != BRepBuilderAPI_WireDone) {
        LOG_ERROR("Sketch profile: wire rejected edge {} of {}", i, n);
        return TopoDS_Wire();
      }
    }
  }
  if (!wire.IsDone())
    return TopoDS_Wire();

  ShapeFix_Wire fix;
  fix.Load(wire.Wire());
  fix.SetPrecision(1e-4);
  fix.FixReorder();
  fix.FixConnected();
  fix.FixClosed();
  return fix.Wire();
}

TopoDS_Wire MakeLoopWire(const SketchPlane& plane, const Loop& loop) {
  // OCCT edge/wire construction can throw (StdFail_NotDone and friends), and
  // several callers sit outside any try (offset, loft, sweep profiles) — an
  // escaping exception aborts the app. Contain it: a throw is a failed wire.
  try {
    return MakeLoopWireImpl(plane, loop);
  } catch (const Standard_Failure& e) {
    LOG_ERROR("Sketch profile: wire construction: OCCT error: {}",
              e.GetMessageString());
    return TopoDS_Wire();
  }
}

// The loop's boundary as a UV polygon (arcs/circles tessellated).
std::vector<glm::vec2> LoopPolygon(const Loop& loop) {
  std::vector<glm::vec2> poly;
  if (loop.circle || loop.ellipse) {
    const glm::vec2 c =
        loop.circle ? loop.circle->center : loop.ellipse->center;
    const double rx = loop.circle ? loop.circle->radius : loop.ellipse->rMajor;
    const double ry = loop.circle ? loop.circle->radius : loop.ellipse->rMinor;
    const double rot = loop.ellipse ? loop.ellipse->rotation : 0.0;
    for (int i = 0; i < kCircleSegments; i++) {
      const double a = 2.0 * M_PI * i / kCircleSegments;
      const double x = rx * std::cos(a), y = ry * std::sin(a);
      poly.push_back({c.x + x * std::cos(rot) - y * std::sin(rot),
                      c.y + x * std::sin(rot) + y * std::cos(rot)});
    }
    return poly;
  }
  for (const auto& e : loop.edges) {
    std::vector<glm::vec2> seg = TessellateLoopEdge(e);
    for (size_t i = (poly.empty() ? 0 : 1); i < seg.size(); i++)
      poly.push_back(seg[i]);
  }
  return poly;
}

// Even-odd point-in-polygon test in UV space.
bool PolygonContains(const std::vector<glm::vec2>& poly, const glm::vec2& p) {
  bool inside = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const glm::vec2& a = poly[i];
    const glm::vec2& b = poly[j];
    if ((a.y > p.y) != (b.y > p.y) &&
        p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
      inside = !inside;
  }
  return inside;
}

// One outer loop with the holes that belong to it (multi-profile, M3.1).
// `key` is the lowest entity id in the outer loop: a stable identity for
// per-group element names (cap faces) that survives loop reordering.
struct ProfileGroup {
  size_t outer;
  std::vector<size_t> holes;
  uint32_t key;
};

uint32_t LoopKey(const Loop& loop) {
  if (loop.circle)
    return loop.circle->id;
  if (loop.ellipse)
    return loop.ellipse->id;
  uint32_t key = UINT32_MAX;
  for (const auto& e : loop.edges)
    key = std::min(key, e.entityId);
  return key == UINT32_MAX ? 0 : key;
}

// Groups loops into outers-with-holes by containment depth: even depth =
// outer boundary, odd depth = hole of its immediate (deepest) container;
// islands inside holes become outers again (the mainstream CAD behavior).
// Empty on partially-overlapping loops — an ambiguous profile is a loud
// error, never a guess.
std::vector<ProfileGroup> ClassifyLoops(const std::vector<Loop>& loops) {
  const size_t n = loops.size();
  std::vector<std::vector<glm::vec2>> polys(n);
  for (size_t i = 0; i < n; i++)
    polys[i] = LoopPolygon(loops[i]);

  // Partial overlap: some but not all of a loop's vertices inside another.
  for (size_t a = 0; a < n; a++) {
    for (size_t b = 0; b < n; b++) {
      if (a == b)
        continue;
      size_t inside = 0;
      for (const glm::vec2& v : polys[a])
        if (PolygonContains(polys[b], v))
          inside++;
      if (inside != 0 && inside != polys[a].size()) {
        LOG_ERROR("Sketch profile: loops {} and {} overlap — profiles must "
                  "be disjoint or fully nested",
                  a, b);
        return {};
      }
    }
  }

  std::vector<int> depth(n, 0);
  std::vector<std::vector<size_t>> containers(n);
  for (size_t i = 0; i < n; i++) {
    for (size_t j = 0; j < n; j++) {
      if (i != j && !polys[i].empty() &&
          PolygonContains(polys[j], polys[i].front()))
        containers[i].push_back(j);
    }
    depth[i] = static_cast<int>(containers[i].size());
  }

  std::vector<ProfileGroup> groups;
  std::vector<int> groupOf(n, -1);
  for (size_t i = 0; i < n; i++) {
    if (depth[i] % 2 == 0) {
      groupOf[i] = static_cast<int>(groups.size());
      groups.push_back({i, {}, LoopKey(loops[i])});
    }
  }
  for (size_t i = 0; i < n; i++) {
    if (depth[i] % 2 == 0)
      continue;
    size_t owner = containers[i][0];
    for (size_t j : containers[i])
      if (depth[j] > depth[owner])
        owner = j;
    if (depth[owner] != depth[i] - 1 || groupOf[owner] < 0) {
      LOG_ERROR("Sketch profile: inconsistent loop nesting at loop {}", i);
      return {};
    }
    groups[groupOf[owner]].holes.push_back(i);
  }
  return groups;
}

// Builds the planar face for one profile group (outer boundary + its holes).
// Null face on failure. Shared by BuildPrism (extrude) and BuildRevolve.
TopoDS_Face BuildGroupFace(const SketchPlane& plane,
                           const std::vector<Loop>& loops,
                           const ProfileGroup& group) {
  glm::vec3 o = plane.GetOrigin();
  glm::vec3 nrm = plane.GetNormal();
  glm::vec3 u = plane.GetUAxis();
  gp_Ax3 ax3(gp_Pnt(o.x, o.y, o.z), gp_Dir(nrm.x, nrm.y, nrm.z),
             gp_Dir(u.x, u.y, u.z));
  gp_Pln pln(ax3);

  TopoDS_Wire outerWire = MakeLoopWire(plane, loops[group.outer]);
  if (outerWire.IsNull())
    return TopoDS_Face();
  BRepBuilderAPI_MakeFace faceMaker(pln, outerWire);
  for (size_t i : group.holes) {
    TopoDS_Wire hole = MakeLoopWire(plane, loops[i]);
    if (hole.IsNull())
      return TopoDS_Face();
    faceMaker.Add(TopoDS::Wire(hole.Reversed()));
  }
  if (!faceMaker.IsDone()) {
    LOG_ERROR("Sketch profile: face construction failed");
    return TopoDS_Face();
  }
  return faceMaker.Face();
}

int CountFaces(const TopoDS_Shape& shape) {
  int count = 0;
  for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next())
    count++;
  return count;
}

} // namespace

TopoDS_Wire BuildProfileWire(const SketchPlane& plane,
                             const std::vector<Loop>& loops) {
  const std::vector<ProfileGroup> groups = ClassifyLoops(loops);
  if (groups.size() != 1) // v1 loft: exactly one outer profile per section
    return TopoDS_Wire();
  return MakeLoopWire(plane, loops[groups[0].outer]);
}

TopoDS_Wire BuildOpenPathWire(const SketchPlane& plane,
                              const std::vector<Entity>& entities) {
  // Collect line/arc edges (circles/points/construction can't be a path).
  std::vector<LoopEdge> edges;
  for (const auto& entity : entities) {
    if (IsConstruction(entity))
      continue;
    if (const auto* l = std::get_if<Line>(&entity)) {
      edges.push_back({false, l->a, l->b, {}, 0.0, 0.0, 0.0, l->id});
    } else if (const auto* a = std::get_if<Arc>(&entity)) {
      double s = a->startAngle, en = a->endAngle;
      while (en <= s)
        en += 2.0 * M_PI;
      edges.push_back({true, ArcPointUV(*a, 1), ArcPointUV(*a, 2), a->center,
                       a->radius, s, en, a->id});
    } else if (const auto* bs = std::get_if<BSpline>(&entity)) {
      // A spline can be a path edge (curves-3d Phase 1 companion). Endpoints
      // are the first/last poles; the curve geometry rides in `spline`.
      if (bs->poles.size() < 2)
        continue;
      LoopEdge e;
      e.a = bs->poles.front();
      e.b = bs->poles.back();
      e.entityId = bs->id;
      e.spline = *bs;
      edges.push_back(e);
    }
  }
  if (edges.empty())
    return TopoDS_Wire();

  // Walk from the one endpoint that no other edge shares (an open chain has
  // exactly two such endpoints; a closed loop or a branch has none/too many).
  // Greedy chain by proximity, reusing MakeLoopWire's edge geometry via a
  // one-loop container.
  std::vector<bool> used(edges.size(), false);
  std::vector<LoopEdge> chain = {edges[0]};
  used[0] = true;
  glm::vec2 head = edges[0].a, tail = edges[0].b;
  bool extended = true;
  while (extended) {
    extended = false;
    for (size_t i = 0; i < edges.size(); i++) {
      if (used[i])
        continue;
      LoopEdge cand = edges[i];
      if (Near(cand.a, tail)) {
        tail = cand.b;
      } else if (Near(cand.b, tail)) {
        std::swap(cand.a, cand.b);
        tail = cand.b;
      } else if (Near(cand.b, head)) {
        head = cand.a;
        chain.insert(chain.begin(), cand);
        used[i] = true;
        extended = true;
        continue;
      } else if (Near(cand.a, head)) {
        std::swap(cand.a, cand.b);
        head = cand.a;
        chain.insert(chain.begin(), cand);
        used[i] = true;
        extended = true;
        continue;
      } else {
        continue;
      }
      chain.push_back(cand);
      used[i] = true;
      extended = true;
    }
  }
  // All edges must be consumed (a disconnected set is not a single path), and
  // the chain must be open (a closed one is a profile, not a path).
  for (bool u : used)
    if (!u)
      return TopoDS_Wire();
  if (Near(head, tail))
    return TopoDS_Wire();

  // Build the open wire directly — MakeLoopWire's shared-vertex scheme assumes
  // a closed loop and collapses a single edge, so it can't be reused here.
  const glm::vec3 n3 = plane.GetNormal();
  const gp_Dir normal(n3.x, n3.y, n3.z);
  const glm::vec3 uAxis = plane.GetUAxis();
  auto toPnt = [&](const glm::vec2& uv) {
    const glm::vec3 w = plane.PlaneToWorld(uv);
    return gp_Pnt(w.x, w.y, w.z);
  };
  // A degenerate segment (coincident endpoints, zero-radius arc) makes
  // MakeEdge fail; calling .Edge() on a failed maker THROWS StdFail_NotDone,
  // which uncaught aborts the whole app. Check IsDone() before Edge(), and
  // catch any OCC failure — a bad path is a rejected feature, not a crash.
  try {
    BRepBuilderAPI_MakeWire wire;
    for (const LoopEdge& e : chain) {
      if (e.spline) {
        // Orient the poles to the walked a->b direction (the chain walker may
        // have swapped endpoints to connect the chain without reversing poles).
        BSpline s = *e.spline;
        if (!Near(s.poles.front(), e.a))
          std::reverse(s.poles.begin(), s.poles.end());
        BRepBuilderAPI_MakeEdge mk(MakeBSplineCurve(plane, s));
        if (!mk.IsDone())
          return TopoDS_Wire();
        wire.Add(mk.Edge());
        if (wire.Error() != BRepBuilderAPI_WireDone)
          return TopoDS_Wire();
        continue;
      }
      BRepBuilderAPI_MakeEdge mk =
          e.arc ? BRepBuilderAPI_MakeEdge(
                      gp_Circ(gp_Ax2(toPnt(e.center), normal,
                                     gp_Dir(uAxis.x, uAxis.y, uAxis.z)),
                              e.radius),
                      toPnt(OnCircle(e.center, e.radius, e.angStart)),
                      toPnt(OnCircle(e.center, e.radius, e.angEnd)))
                : BRepBuilderAPI_MakeEdge(toPnt(e.a), toPnt(e.b));
      if (!mk.IsDone())
        return TopoDS_Wire(); // degenerate edge → reject the path
      wire.Add(mk.Edge());
      if (wire.Error() != BRepBuilderAPI_WireDone)
        return TopoDS_Wire();
    }
    return wire.IsDone() ? wire.Wire() : TopoDS_Wire();
  } catch (const Standard_Failure&) {
    return TopoDS_Wire();
  }
}

CADShape BuildPrism(const SketchPlane& plane, const std::vector<Loop>& loops,
                    double distance, bool reversed) {
  if (loops.empty() || std::abs(distance) < 1e-9) {
    return CADShape();
  }

  try {
    const std::vector<ProfileGroup> groups = ClassifyLoops(loops);
    if (groups.empty()) {
      return CADShape();
    }

    glm::vec3 nrm = plane.GetNormal();
    const double d = reversed ? -distance : distance;
    gp_Vec extrusion(nrm.x * d, nrm.y * d, nrm.z * d);

    // One prism per profile group; several disjoint groups form a compound
    // (booleans and volume handle compounds; ClassifyLoops rejects overlap).
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    TopoDS_Shape single;
    for (const ProfileGroup& group : groups) {
      TopoDS_Face face = BuildGroupFace(plane, loops, group);
      if (face.IsNull()) {
        return CADShape();
      }
      BRepPrimAPI_MakePrism prism(face, extrusion);
      if (!prism.IsDone()) {
        LOG_ERROR("Sketch profile: prism construction failed");
        return CADShape();
      }
      single = prism.Shape();
      builder.Add(compound, single);
    }
    return CADShape(groups.size() == 1 ? single : TopoDS_Shape(compound));
  } catch (const Standard_Failure& e) {
    LOG_ERROR("Sketch profile: OCCT error: {}", e.GetMessageString());
    return CADShape();
  }
}

CADShape BuildRevolve(const SketchPlane& plane, const std::vector<Loop>& loops,
                      const glm::vec3& axisOrigin, const glm::vec3& axisDir,
                      double angleRadians, bool reversed, bool symmetric) {
  double angle = std::abs(angleRadians);
  if (loops.empty() || angle < 1e-9) {
    return CADShape();
  }
  if (angle > 2.0 * M_PI) {
    angle = 2.0 * M_PI;
  }
  const bool full = angle >= 2.0 * M_PI - 1e-9;

  try {
    const std::vector<ProfileGroup> groups = ClassifyLoops(loops);
    if (groups.empty()) {
      return CADShape();
    }

    gp_Ax1 ax(gp_Pnt(axisOrigin.x, axisOrigin.y, axisOrigin.z),
              gp_Dir(axisDir.x, axisDir.y, axisDir.z));

    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    TopoDS_Shape single;
    for (const ProfileGroup& group : groups) {
      TopoDS_Face face = BuildGroupFace(plane, loops, group);
      if (face.IsNull()) {
        return CADShape();
      }

      TopoDS_Shape profile = face;
      double sweep = reversed ? -angle : angle;
      // Symmetric: pre-rotate the profile by -angle/2 about the axis so the
      // sweep straddles the profile plane. Irrelevant at a full revolution,
      // and `reversed` has no effect on a plane-symmetric sweep.
      if (symmetric && !full) {
        gp_Trsf pre;
        pre.SetRotation(ax, -angle / 2.0);
        profile = BRepBuilderAPI_Transform(profile, pre, /*Copy=*/true).Shape();
        sweep = angle;
      }

      BRepPrimAPI_MakeRevol revol(profile, ax, sweep);
      if (!revol.IsDone()) {
        LOG_ERROR("Sketch profile: revolve construction failed");
        return CADShape();
      }
      single = revol.Shape();
      builder.Add(compound, single);
    }
    CADShape out(groups.size() == 1 ? single : TopoDS_Shape(compound));
    // A profile that crosses the axis yields a self-intersecting/degenerate
    // solid; reject it here rather than pre-checking in UV.
    if (!out.IsValid() || out.Volume() < 1e-9) {
      LOG_ERROR("Sketch profile: revolve produced an invalid solid "
                "(does the profile cross the axis?)");
      return CADShape();
    }
    return out;
  } catch (const Standard_Failure& e) {
    LOG_ERROR("Sketch profile: OCCT error: {}", e.GetMessageString());
    return CADShape();
  }
}

std::vector<glm::vec2> TessellateEntity(const Entity& entity) {
  std::vector<glm::vec2> points;
  std::visit(
      [&](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, Line>) {
          points = {e.a, e.b};
        } else if constexpr (std::is_same_v<T, Circle>) {
          points.reserve(kCircleSegments + 1);
          for (int i = 0; i <= kCircleSegments; i++) {
            double a = 2.0 * M_PI * i / kCircleSegments;
            points.push_back(
                {e.center.x + static_cast<float>(e.radius * std::cos(a)),
                 e.center.y + static_cast<float>(e.radius * std::sin(a))});
          }
        } else if constexpr (std::is_same_v<T, Arc>) {
          double s = e.startAngle, en = e.endAngle;
          while (en <= s)
            en += 2.0 * M_PI; // CCW-always
          const double sweep = en - s;
          int segs = std::max(2, static_cast<int>(std::lround(
                                     kCircleSegments * sweep / (2.0 * M_PI))));
          points.reserve(segs + 1);
          for (int i = 0; i <= segs; i++) {
            double a = s + sweep * i / segs;
            points.push_back(
                {e.center.x + static_cast<float>(e.radius * std::cos(a)),
                 e.center.y + static_cast<float>(e.radius * std::sin(a))});
          }
        } else if constexpr (std::is_same_v<T, Ellipse>) {
          points.reserve(kCircleSegments + 1);
          for (int i = 0; i <= kCircleSegments; i++)
            points.push_back(
                EllipsePointUV(e, 2.0 * M_PI * i / kCircleSegments));
        } else if constexpr (std::is_same_v<T, EllipseArc>) {
          points = EllipseArcSampleUV(e);
        } else if constexpr (std::is_same_v<T, BSpline>) {
          points = BSplineSampleUV(e);
        }
        // Point: no polyline
      },
      entity);
  return points;
}

namespace {
// Converts one OCCT edge (line or circle) back to a plane-UV sketch entity.
std::optional<Entity> EdgeToSketchEntity(const TopoDS_Edge& edge,
                                         const SketchPlane& plane) {
  BRepAdaptor_Curve curve(edge);
  auto toUV = [&](const gp_Pnt& p) {
    return plane.WorldToPlane(glm::vec3(p.X(), p.Y(), p.Z()));
  };
  const double f = curve.FirstParameter(), l = curve.LastParameter();
  const glm::vec2 uv1 = toUV(curve.Value(f)), uv2 = toUV(curve.Value(l));
  if (curve.GetType() == GeomAbs_Line) {
    if (glm::length(uv2 - uv1) < 1e-7f)
      return std::nullopt; // degenerate
    return Line{uv1, uv2};
  }
  if (curve.GetType() == GeomAbs_Circle) {
    const gp_Circ circ = curve.Circle();
    const glm::vec2 c = toUV(circ.Location());
    const double r = circ.Radius();
    const glm::vec2 mid = toUV(curve.Value(0.5 * (f + l)));
    auto ang = [&](glm::vec2 p) { return std::atan2(p.y - c.y, p.x - c.x); };
    auto wrap = [](double x) {
      while (x < 0)
        x += 2.0 * M_PI;
      while (x >= 2.0 * M_PI)
        x -= 2.0 * M_PI;
      return x;
    };
    const double a1 = ang(uv1), a2 = ang(uv2), am = ang(mid);
    const double sweep = wrap(a2 - a1); // CCW a1 → a2
    if (wrap(am - a1) <= sweep)         // midpoint lies on that span
      return Arc{c, r, a1, a1 + sweep};
    return Arc{c, r, a2, a2 + (2.0 * M_PI - sweep)}; // the other way (CCW)
  }
  return std::nullopt; // other curve types not represented in sketches
}
} // namespace

std::vector<Entity> OffsetLoop(const SketchPlane& plane,
                               const std::vector<Entity>& chain,
                               double distance, bool joinArc) {
  std::vector<Entity> out;
  if (std::abs(distance) < 1e-9)
    return out;
  auto loops = BuildLoops(chain);
  if (loops.size() != 1)
    return out; // need exactly one closed loop
  TopoDS_Wire wire = MakeLoopWire(plane, loops[0]);
  if (wire.IsNull())
    return out;
  try {
    BRepOffsetAPI_MakeOffset mk(wire,
                                joinArc ? GeomAbs_Arc : GeomAbs_Intersection);
    mk.Perform(distance);
    if (!mk.IsDone())
      return out;
    for (TopExp_Explorer ex(mk.Shape(), TopAbs_EDGE); ex.More(); ex.Next())
      if (auto e = EdgeToSketchEntity(TopoDS::Edge(ex.Current()), plane))
        out.push_back(*e);
  } catch (const Standard_Failure& e) {
    LOG_ERROR("Sketch offset: OCCT error: {}", e.GetMessageString());
    out.clear();
  }
  return out;
}

std::optional<float> SegmentIntersect(glm::vec2 a1, glm::vec2 b1, glm::vec2 a2,
                                      glm::vec2 b2) {
  const glm::vec2 r = b1 - a1, s = b2 - a2;
  auto cross = [](glm::vec2 u, glm::vec2 v) { return u.x * v.y - u.y * v.x; };
  const float rxs = cross(r, s);
  if (std::abs(rxs) < 1e-9f)
    return std::nullopt; // parallel / collinear
  const glm::vec2 qp = a2 - a1;
  const float t = cross(qp, s) / rxs; // param along [a1,b1]
  const float u = cross(qp, r) / rxs; // param along [a2,b2]
  const float eps = 1e-6f;
  if (t < -eps || t > 1.0f + eps || u < -eps || u > 1.0f + eps)
    return std::nullopt;
  return glm::clamp(t, 0.0f, 1.0f);
}

namespace {

// The stable id of the sketch entity that produced `edge` (an edge of the
// profile face), matched by geometry against the entity-id-carrying loops —
// ShapeFix rebuilds the OCC edges, so there is no direct handle. 0 if
// unmatched.
uint32_t MatchEntityId(const TopoDS_Edge& edge, const SketchPlane& plane,
                       const std::vector<Loop>& loops) {
  const std::optional<Entity> ent = EdgeToSketchEntity(edge, plane);
  if (!ent) {
    return 0;
  }
  const float tol = 1e-3f;
  if (const auto* line = std::get_if<Line>(&*ent)) {
    for (const Loop& loop : loops) {
      for (const LoopEdge& le : loop.edges) {
        if (le.arc) {
          continue;
        }
        if ((glm::length(le.a - line->a) < tol &&
             glm::length(le.b - line->b) < tol) ||
            (glm::length(le.a - line->b) < tol &&
             glm::length(le.b - line->a) < tol)) {
          return le.entityId;
        }
      }
    }
  } else if (const auto* arc = std::get_if<Arc>(&*ent)) {
    // Arc or full circle: match by centre + radius.
    for (const Loop& loop : loops) {
      if (loop.circle && glm::length(loop.circle->center - arc->center) < tol &&
          std::abs(loop.circle->radius - arc->radius) < tol) {
        return loop.circle->id;
      }
      for (const LoopEdge& le : loop.edges) {
        if (le.arc && glm::length(le.center - arc->center) < tol &&
            std::abs(le.radius - arc->radius) < tol) {
          return le.entityId;
        }
      }
    }
  }
  return 0;
}

// 0-based index of `face` in TopExp_Explorer(shape, TopAbs_FACE) order — the
// same walk GenerateMesh and picking use. -1 if not found.
int FaceIndexOf(const TopoDS_Shape& shape, const TopoDS_Shape& face) {
  int i = 0;
  for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next(), i++) {
    if (ex.Current().IsSame(face)) {
      return i;
    }
  }
  return -1;
}

} // namespace

namespace {
// Route a builder failure reason to the caller when it wants one, else log
// it (features surface the text via SetError, and Document logs that).
void ProfileFail(std::string* error, std::string message) {
  if (error)
    *error = std::move(message);
  else
    LOG_ERROR("Sketch profile: {}", message);
}
} // namespace

CADShape BuildPrismNamed(const SketchPlane& plane,
                         const std::vector<Loop>& loops, double distance,
                         bool reversed, uint64_t featureTag, ElementMap& out,
                         std::string* error) {
  out.names.clear();
  if (loops.empty() || std::abs(distance) < 1e-9) {
    ProfileFail(error, "empty profile or zero extrude distance");
    return CADShape();
  }

  try {
    const std::vector<ProfileGroup> groups = ClassifyLoops(loops);
    if (groups.empty()) {
      ProfileFail(error,
                  "profile loops overlap or nest inconsistently (see log)");
      return CADShape();
    }

    glm::vec3 nrm = plane.GetNormal();
    const double d = reversed ? -distance : distance;
    gp_Vec extrusion(nrm.x * d, nrm.y * d, nrm.z * d);

    const std::string tag = std::to_string(featureTag) + "/";
    const bool multi = groups.size() > 1;
    // One prism per profile group, assembled into a compound whose face
    // order is the concatenation of the groups' faces — local face indices
    // shift by a running offset. Names stay per-prism, so no boolean/history
    // propagation is needed for disjoint groups.
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    TopoDS_Shape single;
    int faceOffset = 0;
    for (const ProfileGroup& group : groups) {
      TopoDS_Face face = BuildGroupFace(plane, loops, group);
      if (face.IsNull()) {
        ProfileFail(error, "failed to build a face for the profile "
                           "(bad wire geometry, see log)");
        return CADShape();
      }
      BRepPrimAPI_MakePrism prism(face, extrusion);
      if (!prism.IsDone()) {
        ProfileFail(error, "prism construction failed");
        return CADShape();
      }
      const TopoDS_Shape shape = prism.Shape();

      // Side faces: each profile edge sweeps into a lateral face, named from
      // the sketch entity that produced the edge (unique across groups).
      for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) {
        const TopoDS_Edge& edge = TopoDS::Edge(ex.Current());
        const uint32_t entityId = MatchEntityId(edge, plane, loops);
        if (entityId == 0) {
          continue; // unmatched edge (e.g. a seam); leave it for a later pass
        }
        const std::string name = tag + "g" + std::to_string(entityId);
        const TopTools_ListOfShape& gen = prism.Generated(edge);
        for (const TopoDS_Shape& sideFace : gen) {
          const int idx = FaceIndexOf(shape, sideFace);
          if (idx >= 0) {
            out.names[faceOffset + idx] = name;
          }
        }
      }

      // Caps: fixed sentinels, so adding/removing a hole never renames them.
      // With several groups each pair is keyed by its outer loop's lowest
      // entity id ("cap0@g7"), stable against group reordering.
      const std::string capSuffix =
          multi ? "@g" + std::to_string(group.key) : "";
      const int baseIdx = FaceIndexOf(shape, prism.FirstShape());
      if (baseIdx >= 0) {
        out.names[faceOffset + baseIdx] = tag + "cap0" + capSuffix;
      }
      const int topIdx = FaceIndexOf(shape, prism.LastShape());
      if (topIdx >= 0) {
        out.names[faceOffset + topIdx] = tag + "cap1" + capSuffix;
      }

      faceOffset += CountFaces(shape);
      single = shape;
      builder.Add(compound, single);
    }

    // U/L fallback passes: the prism's edges take names from their faces
    // ("<faceName>;U<i>"), which is what makes them referenceable by a fillet.
    CADShape result(multi ? TopoDS_Shape(compound) : single);
    topo::ApplyFallbackPasses(result, out);
    return result;
  } catch (const Standard_Failure& e) {
    ProfileFail(error, std::string("OCCT error: ") + e.GetMessageString());
    return CADShape();
  }
}

CADShape BuildRevolveNamed(const SketchPlane& plane,
                           const std::vector<Loop>& loops,
                           const glm::vec3& axisOrigin,
                           const glm::vec3& axisDir, double angleRadians,
                           bool reversed, bool symmetric, uint64_t featureTag,
                           ElementMap& out, std::string* error) {
  out = {};
  double angle = std::abs(angleRadians);
  if (loops.empty() || angle < 1e-9) {
    ProfileFail(error, "empty profile or zero revolve angle");
    return CADShape();
  }
  if (angle > 2.0 * M_PI) {
    angle = 2.0 * M_PI;
  }
  const bool full = angle >= 2.0 * M_PI - 1e-9;

  try {
    const std::vector<ProfileGroup> groups = ClassifyLoops(loops);
    if (groups.empty()) {
      ProfileFail(error,
                  "profile loops overlap or nest inconsistently (see log)");
      return CADShape();
    }

    gp_Ax1 ax(gp_Pnt(axisOrigin.x, axisOrigin.y, axisOrigin.z),
              gp_Dir(axisDir.x, axisDir.y, axisDir.z));

    const std::string tag = std::to_string(featureTag) + "/";
    const bool multi = groups.size() > 1;
    // One revolve per profile group (see BuildPrismNamed for the compound
    // face-index-offset scheme).
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    TopoDS_Shape single;
    int faceOffset = 0;
    for (const ProfileGroup& group : groups) {
      TopoDS_Face face = BuildGroupFace(plane, loops, group);
      if (face.IsNull()) {
        ProfileFail(error, "failed to build a face for the profile "
                           "(bad wire geometry, see log)");
        return CADShape();
      }

      TopoDS_Shape profile = face;
      double sweep = reversed ? -angle : angle;
      // Symmetric: pre-rotate the profile (see BuildRevolve). Kept alive so
      // the original profile edges can be mapped to their swept copies below.
      std::unique_ptr<BRepBuilderAPI_Transform> xform;
      if (symmetric && !full) {
        gp_Trsf pre;
        pre.SetRotation(ax, -angle / 2.0);
        xform = std::make_unique<BRepBuilderAPI_Transform>(profile, pre,
                                                           /*Copy=*/true);
        profile = xform->Shape();
        sweep = angle;
      }

      BRepPrimAPI_MakeRevol revol(profile, ax, sweep);
      if (!revol.IsDone()) {
        ProfileFail(error, "revolve construction failed (does the profile "
                           "cross the axis?)");
        return CADShape();
      }
      const TopoDS_Shape shape = revol.Shape();

      // Side faces: each profile edge sweeps into a face of revolution, named
      // from the sketch entity that produced the edge. When the profile was
      // pre-rotated, query the swept *copy* of each original edge.
      for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) {
        const TopoDS_Edge& edge = TopoDS::Edge(ex.Current());
        const uint32_t entityId = MatchEntityId(edge, plane, loops);
        if (entityId == 0) {
          continue;
        }
        const TopoDS_Shape query =
            xform ? xform->ModifiedShape(edge) : TopoDS_Shape(edge);
        const std::string name = tag + "g" + std::to_string(entityId);
        for (const TopoDS_Shape& sideFace : revol.Generated(query)) {
          const int idx = FaceIndexOf(shape, sideFace);
          if (idx >= 0) {
            out.names[faceOffset + idx] = name;
          }
        }
      }

      // Caps: the planar ends of a partial revolve. A full revolution has
      // none (FirstShape/LastShape are not part of the result; FaceIndexOf
      // misses). Multi-group caps are keyed like the prism's.
      const std::string capSuffix =
          multi ? "@g" + std::to_string(group.key) : "";
      if (!revol.FirstShape().IsNull()) {
        const int baseIdx = FaceIndexOf(shape, revol.FirstShape());
        if (baseIdx >= 0) {
          out.names[faceOffset + baseIdx] = tag + "cap0" + capSuffix;
        }
      }
      if (!revol.LastShape().IsNull()) {
        const int topIdx = FaceIndexOf(shape, revol.LastShape());
        if (topIdx >= 0) {
          out.names[faceOffset + topIdx] = tag + "cap1" + capSuffix;
        }
      }

      faceOffset += CountFaces(shape);
      single = shape;
      builder.Add(compound, single);
    }

    CADShape result(multi ? TopoDS_Shape(compound) : single);
    if (!result.IsValid() || result.Volume() < 1e-9) {
      ProfileFail(error, "revolve produced an invalid solid "
                         "(does the profile cross the axis?)");
      return CADShape();
    }
    const TopoDS_Shape& shape = result.GetShape();

    // Degenerate pole edges at the axis have no generating parent
    // (Generated() is empty for them) — name them by position so they never
    // go unnamed. Edge indices follow the final (possibly compound) shape.
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    int pole = 0;
    for (int i = 1; i <= edges.Extent(); i++) {
      if (BRep_Tool::Degenerated(TopoDS::Edge(edges(i)))) {
        out.edgeNames[i - 1] = tag + "pole" + std::to_string(pole++);
      }
    }

    topo::ApplyFallbackPasses(result, out);
    return result;
  } catch (const Standard_Failure& e) {
    ProfileFail(error, std::string("OCCT error: ") + e.GetMessageString());
    return CADShape();
  }
}

CADShape BuildThinPrismNamed(const SketchPlane& plane,
                             const std::vector<Loop>& loops, double distance,
                             bool reversed, double thickness, ThinSide side,
                             uint64_t featureTag, ElementMap& out,
                             std::string* error) {
  out = {};
  if (loops.empty() || std::abs(distance) < 1e-9 || thickness <= 0.0) {
    ProfileFail(error,
                "empty profile, zero distance, or non-positive thickness");
    return CADShape();
  }

  try {
    glm::vec3 o = plane.GetOrigin();
    glm::vec3 nrm = plane.GetNormal();
    glm::vec3 u = plane.GetUAxis();
    gp_Ax3 ax3(gp_Pnt(o.x, o.y, o.z), gp_Dir(nrm.x, nrm.y, nrm.z),
               gp_Dir(u.x, u.y, u.z));
    gp_Pln pln(ax3);

    // Offset a closed loop wire by `d` (positive = outward, arc-joined
    // convex corners). Null wire when the offset degenerates.
    auto offsetWire = [&](const TopoDS_Wire& wire, double d) -> TopoDS_Wire {
      if (std::abs(d) < 1e-12)
        return wire;
      TopoDS_Face base = BRepBuilderAPI_MakeFace(pln, wire).Face();
      BRepOffsetAPI_MakeOffset off(base, GeomAbs_Arc);
      off.Perform(d);
      if (!off.IsDone())
        return TopoDS_Wire();
      for (TopExp_Explorer ex(off.Shape(), TopAbs_WIRE); ex.More(); ex.Next())
        return TopoDS::Wire(ex.Current());
      return TopoDS_Wire();
    };

    const double d = reversed ? -distance : distance;
    gp_Vec extrusion(nrm.x * d, nrm.y * d, nrm.z * d);

    const std::string tag = std::to_string(featureTag) + "/";
    const bool multi = loops.size() > 1;
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    TopoDS_Shape single;
    int faceOffset = 0;
    for (const Loop& loop : loops) {
      const TopoDS_Wire wire = MakeLoopWire(plane, loop);
      if (wire.IsNull()) {
        ProfileFail(error, "failed to build the profile wire (see log)");
        return CADShape();
      }
      TopoDS_Wire outer, inner;
      switch (side) {
      case ThinSide::Out:
        outer = offsetWire(wire, thickness);
        inner = wire;
        break;
      case ThinSide::In:
        outer = wire;
        inner = offsetWire(wire, -thickness);
        break;
      case ThinSide::Both:
        outer = offsetWire(wire, thickness / 2.0);
        inner = offsetWire(wire, -thickness / 2.0);
        break;
      }
      if (outer.IsNull() || inner.IsNull()) {
        ProfileFail(error, "thin offset degenerated (wall thicker than the "
                           "profile?)");
        return CADShape();
      }

      BRepBuilderAPI_MakeFace ring(pln, outer);
      ring.Add(TopoDS::Wire(inner.Reversed()));
      if (!ring.IsDone()) {
        ProfileFail(error, "thin ring face construction failed");
        return CADShape();
      }
      BRepPrimAPI_MakePrism prism(ring.Face(), extrusion);
      if (!prism.IsDone()) {
        ProfileFail(error, "prism construction failed");
        return CADShape();
      }
      const TopoDS_Shape shape = prism.Shape();

      const std::string capSuffix =
          multi ? "@g" + std::to_string(LoopKey(loop)) : "";
      const int baseIdx = FaceIndexOf(shape, prism.FirstShape());
      if (baseIdx >= 0) {
        out.names[faceOffset + baseIdx] = tag + "cap0" + capSuffix;
      }
      const int topIdx = FaceIndexOf(shape, prism.LastShape());
      if (topIdx >= 0) {
        out.names[faceOffset + topIdx] = tag + "cap1" + capSuffix;
      }

      faceOffset += CountFaces(shape);
      single = shape;
      builder.Add(compound, single);
    }

    CADShape result(multi ? TopoDS_Shape(compound) : single);
    topo::ApplyFallbackPasses(result, out);
    return result;
  } catch (const Standard_Failure& e) {
    ProfileFail(error, std::string("OCCT error: ") + e.GetMessageString());
    return CADShape();
  }
}

std::vector<uint32_t> LoopEntityIds(const Loop& loop) {
  std::vector<uint32_t> ids;
  if (loop.circle) {
    ids.push_back(loop.circle->id);
  } else if (loop.ellipse) {
    ids.push_back(loop.ellipse->id);
  } else {
    for (const LoopEdge& e : loop.edges)
      ids.push_back(e.entityId);
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

std::vector<glm::vec2> LoopBoundaryPolygon(const Loop& loop) {
  return LoopPolygon(loop); // the internal tessellation, exported
}

bool PointInLoop(const Loop& loop, const glm::vec2& p) {
  if (loop.circle)
    return glm::length(p - loop.circle->center) <= loop.circle->radius;
  const std::vector<glm::vec2> poly = LoopPolygon(loop);
  if (poly.size() < 3)
    return false;
  bool inside = false; // even-odd ray cast toward +x
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const glm::vec2& a = poly[i];
    const glm::vec2& b = poly[j];
    if ((a.y > p.y) != (b.y > p.y) &&
        p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
      inside = !inside;
  }
  return inside;
}

namespace {
// Projects one B-Rep edge onto the sketch plane as a construction entity.
// A curve whose plane is parallel to the sketch keeps its exact type
// (line/circle/arc/ellipse/ellipse-arc); any other curve — an out-of-plane
// shadow (a circle projects to an ellipse) or a B-spline — is sampled into a
// B-spline. Returns nullopt for a degenerate edge.
std::optional<Entity> ProjectEdge(const TopoDS_Edge& edge,
                                  const SketchPlane& plane) {
  const glm::vec3 n3 = plane.GetNormal();
  const gp_Dir sketchN(n3.x, n3.y, n3.z);
  const auto toUV = [&](const gp_Pnt& p) {
    return plane.WorldToPlane(glm::vec3(p.X(), p.Y(), p.Z()));
  };
  BRepAdaptor_Curve curve(edge);
  const double f = curve.FirstParameter(), l = curve.LastParameter();
  const GeomAbs_CurveType type = curve.GetType();

  if (type == GeomAbs_Line) {
    Line ln;
    ln.a = toUV(curve.Value(f));
    ln.b = toUV(curve.Value(l));
    ln.construction = true;
    if (glm::length(ln.b - ln.a) < 1e-9)
      return std::nullopt;
    return ln;
  }

  // A conic keeps its exact 2D form only when its plane is parallel to the
  // sketch (its axis lines up with the sketch normal); otherwise the shadow is
  // a different curve, so fall through to sampling.
  auto parallel = [&](const gp_Dir& d) {
    return std::abs(d.Dot(sketchN)) > 1.0 - 1e-6;
  };
  const bool closed = std::abs((l - f) - 2.0 * M_PI) < 1e-6;
  if (type == GeomAbs_Circle && parallel(curve.Circle().Axis().Direction())) {
    const gp_Circ circ = curve.Circle();
    const glm::vec2 c = toUV(circ.Location());
    if (closed) {
      Circle e;
      e.center = c;
      e.radius = circ.Radius();
      e.construction = true;
      return e;
    }
    const glm::vec2 a = toUV(curve.Value(f)), b = toUV(curve.Value(l));
    Arc arc;
    arc.center = c;
    arc.radius = circ.Radius();
    arc.startAngle = std::atan2(a.y - c.y, a.x - c.x);
    arc.endAngle = std::atan2(b.y - c.y, b.x - c.x);
    while (arc.endAngle <= arc.startAngle)
      arc.endAngle += 2.0 * M_PI;
    arc.construction = true;
    return arc;
  }
  if (type == GeomAbs_Ellipse && parallel(curve.Ellipse().Axis().Direction())) {
    const gp_Elips el = curve.Ellipse();
    const glm::vec2 c = toUV(el.Location());
    const glm::vec2 majTip = toUV(el.Location().Translated(
        gp_Vec(el.XAxis().Direction()) * el.MajorRadius()));
    if (closed) {
      Ellipse e;
      e.center = c;
      e.rMajor = el.MajorRadius();
      e.rMinor = el.MinorRadius();
      e.rotation = std::atan2(majTip.y - c.y, majTip.x - c.x);
      e.construction = true;
      return e;
    }
    EllipseArc ea;
    ea.center = c;
    ea.rMajor = el.MajorRadius();
    ea.rMinor = el.MinorRadius();
    ea.rotation = std::atan2(majTip.y - c.y, majTip.x - c.x);
    ea.startParam = EllipseParamOfPoint(c, ea.rMajor, ea.rMinor, ea.rotation,
                                        toUV(curve.Value(f)));
    ea.endParam = EllipseParamOfPoint(c, ea.rMajor, ea.rMinor, ea.rotation,
                                      toUV(curve.Value(l)));
    while (ea.endParam <= ea.startParam)
      ea.endParam += 2.0 * M_PI;
    ea.construction = true;
    return ea;
  }

  // General fallback: sample the (projected) curve into a B-spline. Enough
  // segments to trace conics/spline shadows smoothly.
  BSpline bs;
  const int segs = 24;
  bs.poles.reserve(segs + 1);
  for (int i = 0; i <= segs; i++)
    bs.poles.push_back(toUV(curve.Value(f + (l - f) * i / segs)));
  bs.construction = true;
  if (bs.poles.size() < 2)
    return std::nullopt;
  return bs;
}
} // namespace

std::vector<Entity> ProjectElement(const CADShape& shape, ExternalKind kind,
                                   int index, const SketchPlane& plane) {
  std::vector<Entity> out;
  if (!shape.IsValid() || index < 0)
    return out;
  if (kind == ExternalKind::Edge) {
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(shape.GetShape(), TopAbs_EDGE, edges);
    if (index >= edges.Extent())
      return out;
    if (auto e = ProjectEdge(TopoDS::Edge(edges.FindKey(index + 1)), plane))
      out.push_back(std::move(*e));
    return out;
  }
  // Face: project each boundary edge.
  TopExp_Explorer faces(shape.GetShape(), TopAbs_FACE);
  for (int i = 0; faces.More() && i < index; faces.Next(), i++) {
  }
  if (!faces.More())
    return out;
  for (TopExp_Explorer edges(faces.Current(), TopAbs_EDGE); edges.More();
       edges.Next())
    if (auto e = ProjectEdge(TopoDS::Edge(edges.Current()), plane))
      out.push_back(std::move(*e));
  return out;
}

std::vector<Entity> ProjectFaceBoundary(const CADShape& shape, int faceIndex,
                                        const SketchPlane& plane) {
  return ProjectElement(shape, ExternalKind::Face, faceIndex, plane);
}

bool FilterLoopsToRegions(std::vector<Loop>& loops,
                          const std::vector<std::vector<uint32_t>>& regions) {
  std::vector<bool> keep(loops.size(), false);
  for (const auto& region : regions) {
    bool matched = false;
    for (size_t i = 0; i < loops.size(); i++) {
      if (LoopEntityIds(loops[i]) == region) {
        keep[i] = true;
        matched = true;
        break;
      }
    }
    if (!matched)
      return false; // a selected region no longer exists in the sketch
  }
  // Holes ride along: an unselected loop nested inside a kept one stays.
  for (size_t i = 0; i < loops.size(); i++) {
    if (keep[i])
      continue;
    const std::vector<glm::vec2> poly = LoopPolygon(loops[i]);
    if (poly.empty())
      continue;
    for (size_t k = 0; k < loops.size(); k++) {
      if (!keep[k] || k == i)
        continue;
      if (PointInLoop(loops[k], poly.front())) {
        keep[i] = true;
        break;
      }
    }
  }
  size_t w = 0;
  for (size_t i = 0; i < loops.size(); i++)
    if (keep[i])
      loops[w++] = std::move(loops[i]);
  loops.resize(w);
  return true;
}

} // namespace sketch
