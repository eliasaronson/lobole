// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/Measure.h"
#include "core/Angles.h"
#include "core/FitBands.h"

#include "cad/MeshShape.h"
#include "core/MeshCollide.h"
#include "core/MeshMeasure.h"
#include "core/Parallel.h"
#include "core/mesh/MeshBoolean.h"
#include "core/mesh/MeshBvh.h"
#include "core/mesh/Slice.h"

#include "core/Conventions.h"
#include "core/ThreadStandards.h"
#include <BRepPrimAPI_MakeCylinder.hxx>

#include <glm/gtc/quaternion.hpp>

#include <Standard_Failure.hxx>

#include "common/Logger.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_BooleanOperation.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_Result.hxx>
#include <BRepClass3d.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepPrimAPI_MakeHalfSpace.hxx>
#include <BRepTools.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Bnd_OBB.hxx>
#include <ElCLib.hxx>
#include <Extrema_ExtAlgo.hxx>
#include <Extrema_ExtFlag.hxx>
#include <Extrema_ExtPC.hxx>
#include <GProp_GProps.hxx>
#include <Geom2d_Curve.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <GeomAbs_CurveType.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <GeomLProp_SLProps.hxx>
#include <Geom_Surface.hxx>
#include <Precision.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Quaternion.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <set>

namespace Measure {

namespace {

// The face / edge at an ElementMap index: faces are TopExp_Explorer 0-based
// order, edges are MapShapes 0-based — the same conventions ElementMap keys
// on. Null when out of range. Single source of truth for index -> subshape.
TopoDS_Shape FaceAt(const TopoDS_Shape& shape, int index) {
  int i = 0;
  for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next(), i++) {
    if (i == index)
      return ex.Current();
  }
  return {};
}
TopoDS_Shape EdgeAt(const TopoDS_Shape& shape, int index) {
  // The map of the shape asked about last, per thread: list_elements asks
  // for every edge of one body in turn, and building the map each time made
  // describing a body's edges quadratic. The cache holds the shape itself,
  // so the TShape it is keyed on cannot be freed and its address reused.
  thread_local TopoDS_Shape last;
  thread_local TopTools_IndexedMapOfShape edges;
  if (last.IsNull() || !last.IsSame(shape) ||
      last.Orientation() != shape.Orientation()) {
    edges.Clear();
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    last = shape;
  }
  if (index >= 0 && index < edges.Extent())
    return edges(index + 1);
  return {};
}

// Resolve an ElementRef to its OCC subshape (a face or edge of the owning
// body), via FaceAt/EdgeAt above. Fills `kind`. Null on failure.
// Bodies can share a creator (a multi-body import's `<id>/b<k>/...`, a
// split's pieces): the name says which, so every one is asked, not just the
// first -- stopping there left every face of an import's second body
// unmeasurable.
TopoDS_Shape ResolveShape(const std::vector<Body>& bodies,
                          const ElementRef& ref, std::string& kind) {
  for (const Body& body : bodies) {
    if (body.createdBy != ref.featureId)
      continue;
    const TopoDS_Shape& shape = body.shape.GetShape();
    if (shape.IsNull())
      continue;
    if (const auto face = body.elementMap.Resolve(ref.name)) {
      const TopoDS_Shape f = FaceAt(shape, *face);
      if (!f.IsNull()) {
        kind = "face";
        return f;
      }
    }
    if (const auto edge = body.elementMap.ResolveEdge(ref.name)) {
      const TopoDS_Shape e = EdgeAt(shape, *edge);
      if (!e.IsNull()) {
        kind = "edge";
        return e;
      }
    }
  }
  return {}; // no body of that creator holds the name
}

// A rigid glm placement (rotation + translation, as occurrence transforms
// are) as an OCC gp_Trsf. glm is column-major: m[col][row].
gp_Trsf ToTrsf(const glm::dmat4& m) {
  // Built from a NORMALIZED quaternion rather than by handing OCC the float
  // matrix. SetValues infers a scale factor from the 3x3 block, and the
  // rounding in a float rotation is enough for it to infer one that is not
  // exactly 1 (0.99999998859 for 45 degrees). TopoDS_Shape::Moved() then
  // refuses to move by that location on OpenCASCADE 7.6 — 7.9 accepts the
  // identical input. An occurrence placement is rigid by construction, so
  // say so, and no version has to forgive us. Pinned by
  // "A rigid placement reaches the kernel unscaled" in test_cadkernel.cpp.
  const glm::dquat q = glm::normalize(glm::quat_cast(glm::dmat3(m)));
  gp_Trsf t;
  t.SetRotation(gp_Quaternion(q.x, q.y, q.z, q.w));
  t.SetTranslationPart(gp_Vec(m[3][0], m[3][1], m[3][2]));
  return t;
}

// What a face or an edge IS: the kind vocabulary, plus the radius and axis
// wherever the surface/curve defines them. ONE switch per shape type, shared
// by the two entry points that classify — DescribeFaceAt/DescribeEdgeAt
// (which resolve by element-map INDEX) and Describe (which resolves by
// stable NAME). They had drifted: Describe classified circular edges only,
// so a cylindrical face reported its area and never its radius.
struct ShapeClass {
  std::string kind;
  double radius = 0.0;
  bool hasRadius = false;
  glm::dvec3 axis{0.0};
  bool hasAxis = false;
};

ShapeClass ClassifyFace(const TopoDS_Face& face) {
  ShapeClass out;
  BRepAdaptor_Surface surf(face);
  const auto axisOf = [&out](const gp_Ax1& ax) {
    const gp_Dir a = ax.Direction();
    out.axis = glm::dvec3(a.X(), a.Y(), a.Z());
    out.hasAxis = true;
  };
  switch (surf.GetType()) {
  case GeomAbs_Plane:
    out.kind = "plane";
    break;
  case GeomAbs_Cylinder:
    out.kind = "cylinder";
    out.radius = surf.Cylinder().Radius();
    out.hasRadius = true;
    axisOf(surf.Cylinder().Axis());
    break;
  case GeomAbs_Cone:
    out.kind = "cone";
    axisOf(surf.Cone().Axis());
    break;
  case GeomAbs_Sphere:
    out.kind = "sphere";
    out.radius = surf.Sphere().Radius();
    out.hasRadius = true;
    break;
  case GeomAbs_Torus:
    // Round-8 retro: bent tubes are toruses; "other" made them
    // unassertable. radius = the MINOR (tube) radius — what a bent-rod
    // query wants; the axis is the ring's.
    out.kind = "torus";
    out.radius = surf.Torus().MinorRadius();
    out.hasRadius = true;
    axisOf(surf.Torus().Axis());
    break;
  default:
    out.kind = "other";
    break;
  }
  return out;
}

// The outward unit normal of a planar-or-not face at a representative point:
// project the face's centre of mass onto the surface for its (u,v) — falling
// back to the UV-bounds midpoint when the projection cannot converge — take
// the geometric normal there, and flip it when the face is REVERSED so it
// points out of the solid. Shared by DescribeFaceAt (which reports it) and
// DirectionOf (which takes angles against it), in double throughout: an
// angle read off a float normal carries ~1e-4 degrees of noise, which is
// enough to make a right angle print as 89.9999.
std::optional<gp_Dir> FaceNormal(const TopoDS_Face& face, const gp_Pnt& com) {
  Standard_Real u0, u1, v0, v1;
  BRepTools::UVBounds(face, u0, u1, v0, v1);
  Standard_Real u = 0.5 * (u0 + u1), v = 0.5 * (v0 + v1);
  const Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
  if (surf.IsNull())
    return std::nullopt;
  GeomAPI_ProjectPointOnSurf proj(com, surf, u0, u1, v0, v1);
  if (proj.IsDone() && proj.NbPoints() > 0)
    proj.LowerDistanceParameters(u, v);
  GeomLProp_SLProps sl(surf, u, v, 1, 1e-7);
  if (!sl.IsNormalDefined())
    return std::nullopt;
  gp_Dir n = sl.Normal();
  if (face.Orientation() == TopAbs_REVERSED)
    n.Reverse();
  return n;
}

ShapeClass ClassifyEdge(const TopoDS_Edge& edge) {
  ShapeClass out;
  BRepAdaptor_Curve curve(edge);
  switch (curve.GetType()) {
  case GeomAbs_Line:
    out.kind = "line";
    break;
  case GeomAbs_Circle: {
    // Full circle vs arc: the trimmed parameter span of a circular edge is
    // its swept angle.
    const double sweep =
        std::abs(curve.LastParameter() - curve.FirstParameter());
    out.kind = sweep >= 2.0 * M_PI - 1e-6 ? "circle" : "arc";
    out.radius = curve.Circle().Radius();
    out.hasRadius = true;
    const gp_Dir a = curve.Circle().Axis().Direction();
    out.axis = glm::dvec3(a.X(), a.Y(), a.Z());
    out.hasAxis = true;
    break;
  }
  default:
    out.kind = "other";
    break;
  }
  return out;
}

} // namespace

std::optional<FaceInfo> DescribeFaceAt(const Body& body, int faceIndex) {
  if (body.shape.GetShape().IsNull())
    return std::nullopt;
  const TopoDS_Shape f = FaceAt(body.shape.GetShape(), faceIndex);
  if (f.IsNull())
    return std::nullopt;
  FaceInfo out;
  GProp_GProps props;
  BRepGProp::SurfaceProperties(f, props);
  out.area = props.Mass();
  const gp_Pnt c = props.CentreOfMass();
  out.centroid = glm::dvec3(c.X(), c.Y(), c.Z());
  if (const auto n = FaceNormal(TopoDS::Face(f), c))
    out.normal = glm::dvec3(n->X(), n->Y(), n->Z());
  const ShapeClass sc = ClassifyFace(TopoDS::Face(f));
  out.kind = sc.kind;
  out.radius = sc.radius;
  out.hasRadius = sc.hasRadius;
  out.axis = sc.axis;
  out.hasAxis = sc.hasAxis;
  return out;
}

std::optional<EdgeInfo> DescribeEdgeAt(const Body& body, int edgeIndex) {
  if (body.shape.GetShape().IsNull())
    return std::nullopt;
  const TopoDS_Shape e = EdgeAt(body.shape.GetShape(), edgeIndex);
  if (e.IsNull())
    return std::nullopt;
  EdgeInfo out;
  GProp_GProps props;
  BRepGProp::LinearProperties(e, props);
  out.length = props.Mass();
  BRepAdaptor_Curve curve(TopoDS::Edge(e));
  const double midParam =
      0.5 * (curve.FirstParameter() + curve.LastParameter());
  const gp_Pnt mid = curve.Value(midParam);
  out.midpoint = glm::dvec3(mid.X(), mid.Y(), mid.Z());
  const ShapeClass sc = ClassifyEdge(TopoDS::Edge(e));
  out.kind = sc.kind;
  out.radius = sc.radius;
  out.hasRadius = sc.hasRadius;
  out.axis = sc.axis;
  out.hasAxis = sc.hasAxis;
  return out;
}

std::optional<double> DistanceToElementAt(const Body& body, bool face,
                                          int index, const glm::dvec3& point,
                                          double cutoff) {
  if (body.shape.GetShape().IsNull())
    return std::nullopt;
  const TopoDS_Shape s = face ? FaceAt(body.shape.GetShape(), index)
                              : EdgeAt(body.shape.GetShape(), index);
  if (s.IsNull())
    return std::nullopt;
  try {
    const gp_Pnt p(point.x, point.y, point.z);
    Bnd_Box box;
    BRepBndLib::Add(s, box);
    if (box.IsVoid() || box.Distance(Bnd_Box(p, p)) > cutoff)
      return std::nullopt;
    const TopoDS_Vertex v = BRepBuilderAPI_MakeVertex(p);
    BRepExtrema_DistShapeShape dist(v, s, Extrema_ExtFlag_MIN);
    if (!dist.IsDone() || dist.NbSolution() < 1)
      return std::nullopt;
    if (dist.Value() > cutoff)
      return std::nullopt;
    return dist.Value();
  } catch (const Standard_Failure& e) {
    LOG_ERROR("Measure element distance failed: {}",
              e.GetMessageString() ? e.GetMessageString()
                                   : "OpenCASCADE failure");
    return std::nullopt;
  }
}

namespace {

double SegmentDistance(const gp_Pnt& p, const gp_Pnt& a, const gp_Pnt& b) {
  const gp_Vec ab(a, b), ap(a, p);
  const double len2 = ab.SquareMagnitude();
  double t = len2 > 0.0 ? ap.Dot(ab) / len2 : 0.0;
  t = std::clamp(t, 0.0, 1.0);
  return p.Distance(a.Translated(ab * t));
}

// The outward normal of `face` (its adaptor `surf`) at parameter fraction
// `t` along `edge`.
std::optional<gp_Dir> NormalOnFaceAlong(const TopoDS_Edge& edge,
                                        const TopoDS_Face& face,
                                        const BRepAdaptor_Surface& surf,
                                        double t) {
  double f = 0.0, l = 0.0;
  gp_Pnt2d uv;
  const Handle(Geom2d_Curve) pc = BRep_Tool::CurveOnSurface(edge, face, f, l);
  if (!pc.IsNull()) {
    uv = pc->Value(f + (l - f) * t);
  } else {
    // No p-curve: project the 3-D point.
    BRepAdaptor_Curve c(edge);
    const gp_Pnt p = c.Value(c.FirstParameter() +
                             (c.LastParameter() - c.FirstParameter()) * t);
    const Handle(Geom_Surface) s = BRep_Tool::Surface(face);
    if (s.IsNull())
      return std::nullopt;
    GeomAPI_ProjectPointOnSurf proj(p, s);
    if (!proj.IsDone() || proj.NbPoints() < 1)
      return std::nullopt;
    double u = 0.0, v = 0.0;
    proj.LowerDistanceParameters(u, v);
    uv.SetCoord(u, v);
  }
  BRepLProp_SLProps props(surf, uv.X(), uv.Y(), 1, 1e-9);
  if (!props.IsNormalDefined())
    return std::nullopt;
  gp_Dir n = props.Normal();
  if (face.Orientation() == TopAbs_REVERSED)
    n.Reverse();
  return n;
}

constexpr double kSmoothAngle = 1e-4; // rad

bool SmoothBetween(const TopoDS_Edge& edge, const TopoDS_Face& a,
                   const BRepAdaptor_Surface& sa, const TopoDS_Face& b,
                   const BRepAdaptor_Surface& sb) {
  if (BRep_Tool::Degenerated(edge) || a.IsSame(b))
    return true;
  try {
    // Two planes meet tangentially only where they are one plane.
    if (sa.GetType() == GeomAbs_Plane && sb.GetType() == GeomAbs_Plane) {
      const auto na = NormalOnFaceAlong(edge, a, sa, 0.5);
      const auto nb = NormalOnFaceAlong(edge, b, sb, 0.5);
      return na && nb && na->Angle(*nb) <= kSmoothAngle;
    }
    // Interior samples: an end of the edge is a vertex where a third face
    // may meet and the normal there belongs to nobody in particular.
    for (const double t : {0.1, 0.3, 0.5, 0.7, 0.9}) {
      const auto na = NormalOnFaceAlong(edge, a, sa, t);
      const auto nb = NormalOnFaceAlong(edge, b, sb, t);
      if (!na || !nb || na->Angle(*nb) > kSmoothAngle)
        return false;
    }
    return true;
  } catch (const Standard_Failure&) {
    return false;
  }
}

// The exact distance from `p` to a bounded edge: the curve's interior
// extrema and its two ends.
double EdgeDistance(const gp_Pnt& p, const BRepAdaptor_Curve& c) {
  const double f = c.FirstParameter(), l = c.LastParameter();
  double best = std::min(p.Distance(c.Value(f)), p.Distance(c.Value(l)));
  Extrema_ExtPC ext(p, c, f, l);
  if (ext.IsDone())
    for (int i = 1; i <= ext.NbExt(); i++)
      best = std::min(best, std::sqrt(ext.SquareDistance(i)));
  return best;
}

} // namespace

bool EdgeIsSmooth(const TopoDS_Edge& edge, const TopoDS_Face& a,
                  const TopoDS_Face& b) {
  return SmoothBetween(edge, a, BRepAdaptor_Surface(a), b,
                       BRepAdaptor_Surface(b));
}

std::vector<bool> SmoothEdges(const Body& body) {
  const TopoDS_Shape& shape = body.shape.GetShape();
  if (shape.IsNull())
    return {};
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(shape, TopAbs_EDGE, edges);
  TopTools_IndexedDataMapOfShapeListOfShape parents;
  TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, parents);
  // One adaptor per face, not one per sample.
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(shape, TopAbs_FACE, faces);
  std::vector<std::optional<BRepAdaptor_Surface>> adaptors(
      static_cast<size_t>(faces.Extent()));
  const auto adaptor = [&](const TopoDS_Face& f) -> const BRepAdaptor_Surface& {
    auto& a = adaptors[static_cast<size_t>(faces.FindIndex(f) - 1)];
    if (!a)
      a.emplace(f);
    return *a;
  };
  std::vector<bool> out(static_cast<size_t>(edges.Extent()), false);
  for (int e = 1; e <= edges.Extent(); e++) {
    const TopoDS_Edge& edge = TopoDS::Edge(edges(e));
    const int at = parents.FindIndex(edge);
    std::vector<TopoDS_Face> around;
    if (at != 0)
      for (const TopoDS_Shape& f : parents(at))
        if (std::none_of(around.begin(), around.end(),
                         [&](const TopoDS_Face& g) { return g.IsSame(f); }))
          around.push_back(TopoDS::Face(f));
    if (BRep_Tool::Degenerated(edge) || around.size() == 1)
      out[e - 1] = true; // degenerated, or a seam
    else if (around.size() == 2 && faces.Contains(around[0]) &&
             faces.Contains(around[1]))
      out[e - 1] = SmoothBetween(edge, around[0], adaptor(around[0]), around[1],
                                 adaptor(around[1]));
  }
  return out;
}

std::optional<FaceInset> FaceInsetOf(const TopoDS_Face& face) {
  try {
    const Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
    if (surf.IsNull())
      return std::nullopt;
    // The boundary: every edge but a seam or a degenerated one, as a
    // polyline for the search and as curves for the exact answer.
    std::vector<std::pair<gp_Pnt, gp_Pnt>> segments;
    std::vector<BRepAdaptor_Curve> boundary;
    for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) {
      const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
      if (BRep_Tool::Degenerated(e) || BRep_Tool::IsClosed(e, face))
        continue;
      const BRepAdaptor_Curve& c = boundary.emplace_back(e);
      const double f = c.FirstParameter(), l = c.LastParameter();
      const int n = c.GetType() == GeomAbs_Line ? 1 : 32;
      gp_Pnt prev = c.Value(f);
      for (int k = 1; k <= n; k++) {
        const gp_Pnt p = c.Value(f + (l - f) * k / n);
        segments.emplace_back(prev, p);
        prev = p;
      }
    }
    const bool any = !boundary.empty();
    const auto nearest = [&](const gp_Pnt& p) {
      double best = std::numeric_limits<double>::infinity();
      for (const auto& [a, b] : segments)
        best = std::min(best, SegmentDistance(p, a, b));
      return best;
    };
    double u0 = 0.0, u1 = 0.0, v0 = 0.0, v1 = 0.0;
    BRepTools::UVBounds(face, u0, u1, v0, v1);
    BRepTopAdaptor_FClass2d inside(face, Precision::PConfusion());
    bool found = false;
    double bestU = 0.0, bestV = 0.0, bestD = -1.0;
    const auto consider = [&](double u, double v) {
      if (u < u0 || u > u1 || v < v0 || v > v1)
        return;
      // The distance first: it is cheap, and a sample no further from the
      // boundary than the best so far need not be classified at all.
      const double d = any ? nearest(surf->Value(u, v)) : 0.0;
      if (found && d <= bestD)
        return;
      if (inside.Perform(gp_Pnt2d(u, v)) != TopAbs_IN)
        return;
      if (!found || d > bestD) {
        found = true;
        bestD = d;
        bestU = u;
        bestV = v;
      }
    };
    // Coarse first; a sliver or a ring the coarse grid misses gets a fine
    // one.
    double du = 0.0, dv = 0.0;
    for (const int grid : {8, 32}) {
      du = (u1 - u0) / grid;
      dv = (v1 - v0) / grid;
      for (int i = 0; i < grid; i++)
        for (int j = 0; j < grid; j++)
          consider(u0 + (i + 0.5) * du, v0 + (j + 0.5) * dv);
      if (found)
        break;
    }
    if (!found)
      return std::nullopt;
    // Refine about the best sample: a 5 x 5 at half the spacing, three times.
    for (int round = 0; round < 3 && any; round++) {
      const double cu = bestU, cv = bestV;
      du *= 0.5;
      dv *= 0.5;
      for (int i = -2; i <= 2; i++)
        for (int j = -2; j <= 2; j++)
          consider(cu + i * du, cv + j * dv);
    }
    FaceInset out;
    const gp_Pnt p = surf->Value(bestU, bestV);
    out.point = glm::dvec3(p.X(), p.Y(), p.Z());
    if (any) {
      double exact = std::numeric_limits<double>::infinity();
      for (const BRepAdaptor_Curve& c : boundary)
        exact = std::min(exact, EdgeDistance(p, c));
      out.inset = exact;
      out.hasInset = true;
    }
    return out;
  } catch (const Standard_Failure&) {
    return std::nullopt;
  }
}

std::optional<FaceInset> FaceInsetAt(const Body& body, int faceIndex) {
  if (body.shape.GetShape().IsNull())
    return std::nullopt;
  const TopoDS_Shape f = FaceAt(body.shape.GetShape(), faceIndex);
  if (f.IsNull())
    return std::nullopt;
  return FaceInsetOf(TopoDS::Face(f));
}

// Boolean fuzz for contact checks, in millimetres. Big enough to swallow the
// sliver two coincident faces produce, far below any interference a designer
// could intend.
constexpr double kContactFuzz = 1.0e-3;

// Run a boolean that MEASURES rather than models.
//
// Two OpenCASCADE defaults are wrong for a question and right for an edit,
// and both are silent:
//
//   - A boolean MODIFIES its arguments. It writes the p-curves it computes
//     onto the input edges (BRep_Builder::UpdateEdge). A modelling feature
//     builds on a copy of its cached operands instead (topo::BooleanNamed);
//     a measurement must not write either, because the operands are the
//     document. Two sweep threads
//     asking about two pairs that SHARE a body then write to the same TEdge
//     at once — which is NOT the lazy-cache hazard core/Parallel.h describes
//     and is not fixed by the warm-up the sweeps do, because what gets
//     written is specific to the PAIR rather than an idempotent fill. It
//     cost about one run in five of tests/cncest6090_smoke.py, surfacing as
//     a heap corruption inside an unrelated malloc, several frames from
//     anything of ours. ThreadSanitizer names it exactly: UpdateEdge racing
//     ~BRep_CurveOnSurface.
//   - The two-shape CONSTRUCTOR builds immediately, so anything set
//     afterwards is set too late to matter. It silently was, on the first
//     attempt at the contact fuzz below.
//
// Both disappear if no boolean in this file is built any other way, which is
// what test_conventions.cpp checks — a `BRepAlgoAPI_X op(a, b)` here is a
// build that skipped this function.
void BuildQuery(BRepAlgoAPI_BooleanOperation& op,
                const TopTools_ListOfShape& args,
                const TopTools_ListOfShape& tools, double fuzz = 0.0) {
  op.SetArguments(args);
  op.SetTools(tools);
  if (fuzz > 0.0)
    op.SetFuzzyValue(fuzz);
  op.SetNonDestructive(Standard_True);
  op.Build();
}

void BuildQuery(BRepAlgoAPI_BooleanOperation& op, const TopoDS_Shape& a,
                const TopoDS_Shape& b, double fuzz = 0.0) {
  TopTools_ListOfShape args, tools;
  args.Append(a);
  tools.Append(b);
  BuildQuery(op, args, tools, fuzz);
}

std::optional<double> InterferenceVolume(const std::vector<Body>& bodiesA,
                                         const glm::dmat4& xfA,
                                         const std::vector<Body>& bodiesB,
                                         const glm::dmat4& xfB) {
  const auto threaded = [](const auto& bodies) {
    return std::any_of(bodies.begin(), bodies.end(), [](const Body& b) {
      return !b.threadRegions.empty() || !b.ringGrips.empty() ||
             !b.fitBands.empty();
    });
  };
  if (threaded(bodiesA) || threaded(bodiesB)) {
    const auto c = ContactBetween(bodiesA, xfA, bodiesB, xfB, false);
    return c ? std::optional<double>(c->volume) : std::nullopt;
  }
  try {
    const TopLoc_Location la(ToTrsf(xfA)), lb(ToTrsf(xfB));
    double total = 0.0;
    // Bounding boxes first. A whole-assembly clash sweep is O(n^2) pairs, and
    // almost all of them are nowhere near each other — a boolean intersection
    // is thousands of times dearer than a box test. 50 occurrences took 7.5 s
    // before this (1225 booleans); the scale round measured it.
    const auto boxOf = [](const TopoDS_Shape& s) {
      Bnd_Box box;
      BRepBndLib::Add(s, box);
      return box;
    };
    for (const Body& a : bodiesA) {
      if (a.shape.GetShape().IsNull())
        continue;
      const TopoDS_Shape sa = a.shape.GetShape().Moved(la);
      const Bnd_Box boxA = boxOf(sa);
      for (const Body& b : bodiesB) {
        if (b.shape.GetShape().IsNull())
          continue;
        const TopoDS_Shape sb = b.shape.GetShape().Moved(lb);
        if (boxA.IsOut(boxOf(sb)))
          continue; // cannot touch
        // FUZZY, not exact. Two solids designed to TOUCH share a face, and
        // an exact boolean over coincident faces returns a sliver whose
        // volume is the contact area times the modelling tolerance — for the
        // dock's ladder resting on its end joist that is 0.134 mm3 of
        // nothing, which then read as a clash. Measured: backing the part
        // out by 0.0001 mm left the volume unchanged (a real overlap would
        // shrink continuously) and 0.001 mm took it to exactly zero.
        //
        // A micron of fuzz makes coincident faces cancel while leaving any
        // interference a designer meant — the smallest real one here is a
        // 15 mm standoff — untouched.
        BRepAlgoAPI_Common common;
        BuildQuery(common, sa, sb, kContactFuzz);
        if (!common.IsDone() || common.Shape().IsNull())
          continue;
        GProp_GProps props;
        BRepGProp::VolumeProperties(common.Shape(), props);
        total += std::max(0.0, props.Mass());
      }
    }
    return total;
  } catch (const std::exception& e) {
    LOG_ERROR("Interference check failed: {}", e.what());
    return std::nullopt;
  } catch (const Standard_Failure& e) {
    // OCC throws Standard_Failure, which does NOT derive from
    // std::exception, so the catch above never sees it. On OCC 7.6
    // these calls do throw for transformed shapes, and the escape
    // reached the RPC boundary as an "unknown exception".
    LOG_ERROR("Interference check failed: {}", e.GetMessageString()
                                                   ? e.GetMessageString()
                                                   : "OpenCASCADE failure");
    return std::nullopt;
  }
}

bool WorldBounds(const std::vector<Body>& bodies, const glm::dmat4& xf,
                 glm::dvec3& min, glm::dvec3& max) {
  try {
    const TopLoc_Location loc(ToTrsf(xf));
    Bnd_Box box;
    for (const Body& b : bodies) {
      if (b.shape.GetShape().IsNull())
        continue;
      BRepBndLib::Add(b.shape.GetShape().Moved(loc), box);
    }
    if (box.IsVoid())
      return false;
    Standard_Real x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    min = glm::dvec3(x0, y0, z0);
    max = glm::dvec3(x1, y1, z1);
    return true;
  } catch (const Standard_Failure&) {
    return false;
  }
}

namespace {

// The oriented box of a placed body set, or a void box when it has no shape.
// Off the B-Rep, never the triangulation: AddOBB reads a mesh when one is
// there, and a chord sits inside the surface by up to the deflection, so
// the same pair was pruned meshed and kept unmeshed, and a pruned one could
// be closer than the limit (docs/tolerances/README.md#checking-a-print).
Bnd_OBB PlacedOBB(const std::vector<Body>& bodies, const glm::dmat4& xf) {
  const TopLoc_Location loc(ToTrsf(xf));
  Bnd_OBB obb;
  for (const Body& b : bodies) {
    if (b.shape.GetShape().IsNull())
      continue;
    Bnd_OBB one;
    BRepBndLib::AddOBB(b.shape.GetShape().Moved(loc), one,
                       /*theIsTriangulationUsed=*/false);
    if (!one.IsVoid())
      obb.Add(one);
  }
  return obb;
}

} // namespace

OrientedBox OrientedBoxOf(const std::vector<Body>& bodies,
                          const glm::dmat4& xf) {
  try {
    auto obb = std::make_shared<Bnd_OBB>(PlacedOBB(bodies, xf));
    if (!obb->IsVoid())
      return OrientedBox{std::move(obb)};
  } catch (const Standard_Failure&) {
  }
  return {}; // nothing to judge by
}

bool SeparatedByOBB(const OrientedBox& a, const OrientedBox& b, double gap) {
  if (!a.box || !b.box)
    return false; // nothing to judge by: let the exact check decide
  try {
    Bnd_OBB ba = *a.box;
    Bnd_OBB bb = *b.box;
    // Half the gap on each box, so two boxes exactly `gap` apart still meet.
    if (gap > 0.0) {
      ba.Enlarge(gap * 0.5);
      bb.Enlarge(gap * 0.5);
    }
    return ba.IsOut(bb);
  } catch (const Standard_Failure&) {
    return false; // never reject a pair because the prefilter failed
  }
}

bool SeparatedByOBB(const std::vector<Body>& a, const glm::dmat4& xfA,
                    const std::vector<Body>& b, const glm::dmat4& xfB,
                    double gap) {
  return SeparatedByOBB(OrientedBoxOf(a, xfA), OrientedBoxOf(b, xfB), gap);
}

// Only explicitly annotated, matching coaxial thread spans may be relieved.
// Resolve against the CURRENT shape (including any occurrence transforms), so
// copied/transformed bodies never carry stale world-space interface
// coordinates.
static std::vector<Measure::ThreadSpan>
ThreadSpans(const Body& body, const TopLoc_Location& loc) {
  std::vector<Measure::ThreadSpan> out;
  for (const auto& region : body.threadRegions) {
    const auto spec = threads::Lookup(region.designation);
    const auto index = body.elementMap.Resolve(region.face.name);
    if (!spec || !index)
      continue;
    const auto face = FaceAt(body.shape.GetShape().Moved(loc), *index);
    if (face.IsNull())
      continue;
    BRepAdaptor_Surface surface(TopoDS::Face(face));
    if (surface.GetType() != GeomAbs_Cylinder)
      continue;
    const auto cylinder = surface.Cylinder();
    const double expected =
        region.external ? spec->majorDia : threads::TapDrill(*spec);
    if (std::abs(2 * cylinder.Radius() - expected) > 1e-4)
      continue;
    const auto origin = cylinder.Location();
    const auto dir = cylinder.Axis().Direction();
    glm::dvec3 axis(dir.X(), dir.Y(), dir.Z());
    double lo = surface.FirstVParameter(), hi = surface.LastVParameter();
    if (lo > hi)
      std::swap(lo, hi);
    if (region.length > 0)
      hi = std::min(hi, lo + region.length);
    out.push_back(
        {*spec, glm::dvec3(origin.X(), origin.Y(), origin.Z()) + lo * axis,
         axis, hi - lo, std::max(region.minimumEngagement, spec->pitch),
         region.external});
  }
  return out;
}

static TopoDS_Shape RemoveThreadEngagement(
    TopoDS_Shape overlap, const std::vector<Measure::ThreadSpan>& a,
    const std::vector<Measure::ThreadSpan>& b, size_t& engaged) {
  for (const auto& x : a)
    for (const auto& y : b) {
      if (x.external == y.external || x.spec.majorDia != y.spec.majorDia ||
          x.spec.pitch != y.spec.pitch)
        continue;
      // Measured in the EXTERNAL thread's own frame, whichever body was
      // passed first. Working in x's frame made the answer depend on the
      // argument order: a quad's camera-cage screw in its spacer came back
      // clean as (screw, spacer) and 6.8 mm3 of clash as (spacer, screw) —
      // the same two solids, and the sweep picks the order.
      const Measure::ThreadSpan& ext = x.external ? x : y;
      const Measure::ThreadSpan& in = x.external ? y : x;
      const double dot = glm::dot(ext.axis, in.axis);
      if (std::abs(std::abs(dot) - 1) > 1e-7)
        continue;
      const auto delta = in.start - ext.start;
      const double at = glm::dot(delta, ext.axis);
      if (glm::length(delta - at * ext.axis) > 1e-4)
        continue;
      const double end = at + dot * in.length;
      const double lo = std::max(0.0, std::min(at, end));
      const double hi = std::min(ext.length, std::max(at, end));
      if (hi - lo + 1e-5 < std::max(ext.minimum, in.minimum))
        continue;
      const auto start = ext.start + lo * ext.axis;
      const auto tool = BRepPrimAPI_MakeCylinder(
                            gp_Ax2(gp_Pnt(start.x, start.y, start.z),
                                   gp_Dir(ext.axis.x, ext.axis.y, ext.axis.z)),
                            ext.spec.majorDia / 2, hi - lo)
                            .Shape();
      BRepAlgoAPI_Cut cut;
      BuildQuery(cut, overlap, tool);
      if (!cut.IsDone())
        throw Standard_Failure("thread engagement subtraction failed");
      overlap = cut.Shape();
      ++engaged;
    }
  return overlap;
}

// A retaining ring's grip or its groove's (RingGrip), placed: the grip
// face's axis, radius and the span it runs along that axis.
struct GripSpan {
  const RingGrip* grip = nullptr;
  glm::dvec3 origin{0.0}, axis{0.0, 0.0, 1.0};
  double radius = 0.0, lo = 0.0, hi = 0.0;
};

static std::vector<GripSpan> GripSpans(const Body& body,
                                       const TopLoc_Location& loc) {
  std::vector<GripSpan> out;
  for (const RingGrip& grip : body.ringGrips) {
    const auto index = body.elementMap.Resolve(grip.face.name);
    if (!index)
      continue;
    const auto face = FaceAt(body.shape.GetShape().Moved(loc), *index);
    if (face.IsNull())
      continue;
    BRepAdaptor_Surface surface(TopoDS::Face(face));
    if (surface.GetType() != GeomAbs_Cylinder)
      continue;
    const gp_Cylinder cylinder = surface.Cylinder();
    const gp_Pnt o = cylinder.Location();
    const gp_Dir d = cylinder.Axis().Direction();
    double lo = surface.FirstVParameter(), hi = surface.LastVParameter();
    if (lo > hi)
      std::swap(lo, hi);
    out.push_back({&grip, glm::dvec3(o.X(), o.Y(), o.Z()),
                   glm::dvec3(d.X(), d.Y(), d.Z()), cylinder.Radius(), lo, hi});
  }
  return out;
}

// Removes from `overlap` the annulus between radii rIn and rOut about the
// axis through `origin` along `axis`, from `lo` to `hi` along it: the
// designed overlap of a ring in its groove or of a press fit, both of which
// are an annulus between two coaxial cylinders over the length they engage.
// The caller pads the radii and the span by a hair, so the cut does not
// leave a sliver of the two cylinders' own coincident faces behind.
static TopoDS_Shape CutAnnulus(const TopoDS_Shape& overlap,
                               const glm::dvec3& origin, const glm::dvec3& axis,
                               double rIn, double rOut, double lo, double hi,
                               const char* what) {
  const glm::dvec3 start = origin + lo * axis;
  const gp_Ax2 ax(gp_Pnt(start.x, start.y, start.z),
                  gp_Dir(axis.x, axis.y, axis.z));
  const double h = hi - lo;
  BRepAlgoAPI_Cut tube;
  BuildQuery(tube, BRepPrimAPI_MakeCylinder(ax, rOut, h).Shape(),
             BRepPrimAPI_MakeCylinder(ax, std::max(rIn, 1e-3), h).Shape());
  if (!tube.IsDone())
    throw Standard_Failure((std::string(what) + " tool failed").c_str());
  BRepAlgoAPI_Cut cut;
  BuildQuery(cut, overlap, tube.Shape());
  if (!cut.IsDone())
    throw Standard_Failure((std::string(what) + " subtraction failed").c_str());
  return cut.Shape();
}

static double VolumeOf(const TopoDS_Shape& s) {
  GProp_GProps props;
  BRepGProp::VolumeProperties(s, props);
  return std::max(0.0, props.Mass());
}

// Removes from `overlap` the grip of a ring seated in its groove
// (docs/parts/README.md#a-ring-in-its-groove): the annulus between the
// free ring's grip face and the groove's floor, over the ring's thickness —
// and only for a ring and a groove of one standard and size, on one axis,
// the ring within the groove's width. Whatever else overlaps is left.
static TopoDS_Shape RemoveRingGrip(TopoDS_Shape overlap,
                                   const std::vector<GripSpan>& a,
                                   const std::vector<GripSpan>& b,
                                   size_t& gripped) {
  for (const GripSpan& x : a)
    for (const GripSpan& y : b) {
      if (x.grip->groove == y.grip->groove ||
          x.grip->standard != y.grip->standard ||
          std::abs(x.grip->size - y.grip->size) > 1e-6)
        continue;
      const GripSpan& groove = x.grip->groove ? x : y;
      const GripSpan& ring = x.grip->groove ? y : x;
      const double dot = glm::dot(groove.axis, ring.axis);
      if (std::abs(std::abs(dot) - 1.0) > 1e-7)
        continue;
      // The ring's span along the groove's axis, in the groove's frame.
      const glm::dvec3 delta = ring.origin - groove.origin;
      const double along = glm::dot(delta, groove.axis);
      if (glm::length(delta - along * groove.axis) > 1e-4)
        continue;
      const double r0 = along + dot * ring.lo, r1 = along + dot * ring.hi;
      const double lo = std::min(r0, r1), hi = std::max(r0, r1);
      constexpr double kTol = 1e-4;
      if (lo < groove.lo - kTol || hi > groove.hi + kTol)
        continue; // not in its groove: its overlap is a clash
      // A hair past the two cylinders and the ring's sides, so the cut does
      // not leave a sliver of their own coincident faces behind.
      constexpr double kPad = 1e-3;
      const double rIn = std::min(groove.radius, ring.radius) - kPad;
      const double rOut = std::max(groove.radius, ring.radius) + kPad;
      overlap = CutAnnulus(overlap, groove.origin, groove.axis, rIn, rOut,
                           lo - kPad, hi + kPad, "ring grip");
      ++gripped;
    }
  return overlap;
}

// One round face of a placed body, for the press-fit step: its band (null
// for a face that carries none, read only to say so), whether it is a bore,
// and its cylinder.
struct BandSpan {
  const FitBand* band = nullptr;
  bool bore = false;
  glm::dvec3 origin{0.0}, axis{0.0, 0.0, 1.0};
  double radius = 0.0, lo = 0.0, hi = 0.0;
};

static std::optional<BandSpan> SpanOfFace(const TopoDS_Face& face) {
  const auto bore = fitband::IsBore(face);
  if (!bore)
    return std::nullopt;
  BRepAdaptor_Surface surface(face);
  const gp_Cylinder cylinder = surface.Cylinder();
  const gp_Pnt o = cylinder.Location();
  const gp_Dir d = cylinder.Axis().Direction();
  double lo = surface.FirstVParameter(), hi = surface.LastVParameter();
  if (lo > hi)
    std::swap(lo, hi);
  BandSpan s;
  s.bore = *bore;
  s.origin = {o.X(), o.Y(), o.Z()};
  s.axis = {d.X(), d.Y(), d.Z()};
  s.radius = cylinder.Radius();
  s.lo = lo;
  s.hi = hi;
  return s;
}

// Every round face of `body`, placed as `placed`, with the band it carries
// (FitBand). The unbanded ones are the far side of a banded face, read only
// to say that they carry none.
static std::vector<BandSpan> BandSpans(const Body& body,
                                       const TopoDS_Shape& placed) {
  std::map<int, const FitBand*> banded;
  for (const FitBand& band : body.fitBands)
    if (const auto index = body.elementMap.Resolve(band.face.name))
      banded[*index] = &band;
  std::vector<BandSpan> out;
  int index = 0;
  for (TopExp_Explorer ex(placed, TopAbs_FACE); ex.More(); ex.Next(), ++index)
    if (auto s = SpanOfFace(TopoDS::Face(ex.Current()))) {
      const auto it = banded.find(index);
      s->band = it != banded.end() ? it->second : nullptr;
      out.push_back(*s);
    }
  return out;
}

// A millimetre value as a person writes it: 47, 8.2, 46.982.
static std::string Mm(double v) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.4f", v);
  std::string s = buf;
  while (!s.empty() && s.back() == '0')
    s.pop_back();
  if (!s.empty() && s.back() == '.')
    s.pop_back();
  return s == "-0" ? "0" : s;
}

// How far off the bore's axis the shaft's may lie, over the length the two
// engage, and still be ONE fit (docs/tolerances/README.md#press-fits): a
// tenth of a micrometre, the ring grip's own. A mate places a fit exactly
// coaxial; anything further off is a part that is not where its fit says.
constexpr double kFitCoaxialMm = 1e-4;

// The press-fit half of the designed-overlap step
// (docs/tolerances/README.md#press-fits). For a bore of one side and a shaft
// of the other, coaxial and engaged, the shaft larger than the bore: when
// both carry a band and the fit they make allows the interference the model
// shows, the annulus between them over the engaged length is relieved and
// listed in `pressFits`; otherwise the overlap stays a clash and
// `fitsRefused` says why. A pair of faces neither of which carries a band is
// none of this step's business.
//
// A refusal is said only when an overlap is left once every fit is
// relieved: it explains a clash. Two faces can be coaxial and engaged
// without being a fit's -- a bearing's bore and the outside of the plate its
// outer ring is pressed into -- and with the real fit relieved there is no
// clash for them to explain.
static TopoDS_Shape RelievePressFits(TopoDS_Shape overlap,
                                     const std::vector<BandSpan>& a,
                                     const std::vector<BandSpan>& b,
                                     Measure::SetContact& out) {
  std::vector<Measure::SetContact::FitRefused> refusals;
  for (const BandSpan& x : a)
    for (const BandSpan& y : b) {
      if (x.band == nullptr && y.band == nullptr)
        continue;
      if (x.bore == y.bore)
        continue;
      const BandSpan& bore = x.bore ? x : y;
      const BandSpan& shaft = x.bore ? y : x;
      // A printed press (docs/tolerances/README.md#printed-press-fits) is a
      // press at whatever the model shows, an interference or none: FDM
      // prints the bore small and the pin large, so a press allowance of 0
      // still holds. Within its band it is reported, overlap or not.
      const bool printed = bore.band != nullptr && shaft.band != nullptr &&
                           bore.band->printedPress && shaft.band->printedPress;
      if (printed) {
        const double least = shaft.band->lower - bore.band->upper;
        if (2.0 * (shaft.radius - bore.radius) < least - 1e-7)
          continue; // looser than the band: a clearance, not this press
      } else if (shaft.radius - bore.radius <= 1e-7) {
        continue; // the shaft is not larger: these two do not overlap
      }
      const double dot = glm::dot(bore.axis, shaft.axis);
      if (std::abs(dot) < 0.99)
        continue; // crossing, not one inside the other
      // The engaged length, along the bore's axis.
      const glm::dvec3 p0 = shaft.origin + shaft.lo * shaft.axis;
      const glm::dvec3 p1 = shaft.origin + shaft.hi * shaft.axis;
      const double s0 = glm::dot(p0 - bore.origin, bore.axis);
      const double s1 = glm::dot(p1 - bore.origin, bore.axis);
      const double lo = std::max(bore.lo, std::min(s0, s1));
      const double hi = std::min(bore.hi, std::max(s0, s1));
      if (hi - lo <= 1e-6)
        continue; // side by side on one axis: not engaged
      // How far the shaft's axis strays from the bore's at either end of it.
      const auto offAxis = [&](double s) {
        const double t = std::abs(s1 - s0) > 1e-12 ? (s - s0) / (s1 - s0) : 0.0;
        const glm::dvec3 p = p0 + t * (p1 - p0);
        const glm::dvec3 d = p - bore.origin;
        return glm::length(d - glm::dot(d, bore.axis) * bore.axis);
      };
      const double off = std::max(offAxis(lo), offAxis(hi));
      if (off >= bore.radius)
        continue; // not one inside the other at all
      const bool coaxial = off <= kFitCoaxialMm;

      Measure::SetContact::FitRefused refused;
      const auto refuse = [&](std::string reason) {
        refused.reason = std::move(reason);
        refusals.push_back(refused);
      };
      if (bore.band == nullptr || shaft.band == nullptr) {
        // One side banded, the other not: the overlap has no fit behind it.
        // Said only of a coaxial pair, the one a fit could have been.
        if (!coaxial)
          continue;
        const BandSpan& has = bore.band != nullptr ? bore : shaft;
        refused.fit = "Ø" + Mm(2.0 * has.radius) + " " + has.band->label;
        refuse(std::string("the ") + (has.bore ? "shaft" : "bore") + " (Ø" +
               Mm(2.0 * (has.bore ? shaft.radius : bore.radius)) +
               " modelled) carries no size band, so the " +
               Mm(2.0 * (shaft.radius - bore.radius)) + " mm it overlaps the " +
               (has.bore ? "bore" : "shaft") +
               " is not a fit; set_tolerance on that face");
        continue;
      }
      // As made: a printed face prints at its modelled size less the
      // process's allowance.
      const double db = 2.0 * bore.radius - bore.band->allowance;
      const double ds = 2.0 * shaft.radius - shaft.band->allowance;
      const double interference = ds - db;
      const double maxInterference = shaft.band->upper - bore.band->lower;
      // The basic size the two bands share: a size B with both made
      // diameters inside their bands about it, the roundest such number.
      // Empty past the band, where the bore's made size names the fit.
      double basic = db;
      {
        const double bLo =
            std::max(ds - shaft.band->upper, db - bore.band->upper);
        const double bHi =
            std::min(ds - shaft.band->lower, db - bore.band->lower);
        for (double scale : {1.0, 10.0, 100.0, 1000.0}) {
          const double c = std::ceil(bLo * scale - 1e-6) / scale;
          if (bLo <= bHi + 1e-9 && c <= bHi + 1e-9) {
            basic = c;
            break;
          }
        }
      }
      refused.fit = printed
                        ? "Ø" + Mm(ds) + " printed press " + Mm(interference) +
                              " (" + bore.band->label + ")"
                        : "Ø" + Mm(basic) + " " + bore.band->label + " / " +
                              shaft.band->label;
      refused.interference = interference;
      refused.maxInterference = maxInterference;
      if (!coaxial) {
        char buf[200];
        std::snprintf(buf, sizeof buf,
                      "not coaxial: the shaft's axis is %.4f mm off the "
                      "bore's over the length they engage, and a fit is "
                      "relieved only within %.4f mm",
                      off, kFitCoaxialMm);
        refuse(buf);
        continue;
      }
      if (bore.band->printedPress != shaft.band->printedPress) {
        const bool borePrinted = bore.band->printedPress;
        refuse(std::string("a printed press pairs with a printed press, and "
                           "the ") +
               (borePrinted ? "shaft" : "bore") + " carries " +
               (borePrinted ? shaft.band->label : bore.band->label) +
               "; set_tolerance printed: press on both faces");
        continue;
      }
      if (bore.band->side == FitBand::Side::Shaft ||
          shaft.band->side == FitBand::Side::Hole) {
        const bool boreWrong = bore.band->side == FitBand::Side::Shaft;
        refuse(std::string("the ") + (boreWrong ? "bore" : "shaft") +
               " carries a " + (boreWrong ? "shaft" : "hole") + " band (" +
               (boreWrong ? bore.band->label : shaft.band->label) +
               "), so the pair is not a fit");
        continue;
      }
      if (printed && interference <= 1e-7) {
        // Within the band and no solid in solid: nothing to relieve, and
        // the press is still the press.
        Measure::SetContact::PressFit fit;
        fit.fit = refused.fit;
        fit.diameter = ds;
        fit.interference = interference;
        fit.maxInterference = maxInterference;
        fit.length = hi - lo;
        fit.volume = 0.0;
        out.pressFits.push_back(std::move(fit));
        continue;
      }
      if (maxInterference <= 1e-9) {
        char buf[200];
        std::snprintf(buf, sizeof buf,
                      "a clearance fit allows no interference (at least "
                      "%.4f mm clearance), and the model overlaps by %.4f mm "
                      "on the diameter",
                      -maxInterference, 2.0 * (shaft.radius - bore.radius));
        refuse(buf);
        continue;
      }
      if (interference > maxInterference + 1e-7) {
        refused.excess = interference - maxInterference;
        char buf[240];
        std::snprintf(buf, sizeof buf,
                      "past the band: %.4f mm interference on the diameter "
                      "where the fit allows at most %.4f mm — %.4f mm too "
                      "much",
                      interference, maxInterference, refused.excess);
        refuse(buf);
        continue;
      }
      // Axially, the engaged length and a little past each end of it: a
      // ring's or a pin's chamfer runs from its cylinder down through the
      // annulus, and the part of it inside the bore is the same press. Ten
      // times the radial interference takes a lead-in down to about 6
      // degrees; the radii bound the cut either way.
      constexpr double kPad = 1e-3;
      const double ends = kPad + 10.0 * (shaft.radius - bore.radius);
      const double before = VolumeOf(overlap);
      overlap =
          CutAnnulus(overlap, bore.origin, bore.axis, bore.radius - kPad,
                     shaft.radius + kPad, lo - ends, hi + ends, "press fit");
      const double relieved = std::max(0.0, before - VolumeOf(overlap));
      if (relieved <= 1e-9)
        continue; // the faces overlap, their solids did not: nothing to say
      Measure::SetContact::PressFit fit;
      fit.fit = refused.fit;
      fit.diameter = printed ? ds : basic;
      fit.interference = interference;
      fit.maxInterference = maxInterference;
      fit.length = hi - lo;
      fit.volume = relieved;
      out.pressFits.push_back(std::move(fit));
      out.pressFitVolume += relieved;
    }
  if (!refusals.empty() && VolumeOf(overlap) > 1e-9)
    out.fitsRefused.insert(out.fitsRefused.end(), refusals.begin(),
                           refusals.end());
  return overlap;
}

// The exact minimum distance between two placed shapes, asked of the FACES
// that can hold it rather than of every face against every other.
//
// BRepExtrema prunes face pairs by their boxes against the best distance so
// far, which works when one pair is obviously closest. A tessellation prunes
// at triangle granularity — the granularity this problem has — so: ask the
// meshes where the answer is, then ask the B-Rep what it is, over the faces
// the meshes point at. The triangles never reach the answer; they only
// choose the candidates, so this is the exact distance and not an
// approximation of it. A tessellation lies within its deflection of its
// surface, so the face pair holding the true minimum has triangles within
// `mesh + 2 deflections` of each other, and collecting to that cannot miss
// it.
//
// Measured on a quadcopter (propellers of 53 spline faces each, 2208
// triangles): propeller to camera plate 3.3 s -> 0.6 s, propeller to
// battery 0.8 s, camera to battery 7 ms.
//
// THE NARROWED SET. Nearest collects every face pair within `mesh + 2
// deflections`. A pair's true distance is at least its triangles' less the
// two meshes' deflections summed, so once an exact answer is in hand, a pair
// whose floor is past it cannot beat it and is skipped. The rest are
// independent single face-pair extrema, so they run on every core
// (parallel::For).
//
// Meshing teeth are the case this is for: many nearly parallel flanks, all
// within a deflection of each other, which no tessellation separates. Two
// herringbone gears of a 70 mm planetary put 140-300 pairs in the set, and
// this was capped at 64 and handed to BRepExtrema whole above that: 15-30 s
// a pair, lofted or twisted. Refined in parallel it is 0.4 s for a sun and
// planet and about 4 s for a planet in the coarser-meshed ring.
//
// Extrema WRITE nothing onto the shapes (unlike a boolean,
// docs/inspect/README.md#query-booleans-are-non-destructive): each worker
// reads its two faces' geometry and owns its BRepExtrema object, as
// OpenCASCADE's own BRepExtrema_DistShapeShape::SetMultiThread does over the
// same sub-shapes. What a worker must not meet is a MESHER writing a
// triangulation onto a face it reads; the mesh is built (under its cache's
// lock, by Nearest) before the workers start, a finer one below is built on a
// copy, and a sweep that calls this from its own workers warms every mesh
// first (collide::Warm).
//
// THE CAP is a cost estimate, not a correctness limit: refining N pairs costs
// N extrema spread over the cores, asking OpenCASCADE about the whole shapes
// costs whatever its own pruning makes it, and on the cases measured here it
// pruned no better (a sun inside its ring, 16 mm apart: minutes whole). It
// is a FIXED number, never derived from the thread count, so a one-thread run
// (LOBOLE_SWEEP_THREADS=1) refines exactly the pairs an all-thread run does
// and answers bit for bit the same (sweep_parallel_smoke).
constexpr size_t kMaxRefinedPairs = 4096;

// A list longer than this is asked again of meshes cut kRemeshFactor times
// finer, built on copies (collide::Mesh::Build with a stated deflection, so
// the document's triangulation is untouched and nothing is cached). A coarse
// mesh is one reason a list is long (a 70 mm ring is meshed at 0.2 mm, a
// gap between teeth is 0.1), and a finer one narrows it: a sun and planet's
// 144 pairs come down to about 70. Concentric faces are the
// other, and no mesh separates those: a sun 16 mm inside its ring keeps
// 2166 of its 3212, and costs 12 s on 22 cores.
constexpr size_t kRemeshAbove = 128;
constexpr double kRemeshFactor = 10.0;

double ExactDistance(const CADShape& shapeA, const TopoDS_Shape& a,
                     const glm::dmat4& xfA, const CADShape& shapeB,
                     const TopoDS_Shape& b, const glm::dmat4& xfB,
                     gp_Pnt* pointA = nullptr, gp_Pnt* pointB = nullptr) {
  constexpr double kInf = std::numeric_limits<double>::infinity();
  // MIN, not the default MINMAX: every BRepExtrema here wants the minimum,
  // and the maxima are work nobody reads.
  const auto extrema = [](const TopoDS_Shape& x, const TopoDS_Shape& y,
                          gp_Pnt* px, gp_Pnt* py) {
    BRepExtrema_DistShapeShape d(x, y, Extrema_ExtFlag_MIN,
                                 Extrema_ExtAlgo_Grad);
    if (!d.IsDone() || d.NbSolution() < 1)
      return kInf;
    if (px != nullptr) {
      *px = d.PointOnShape1(1);
      *py = d.PointOnShape2(1);
    }
    return d.Value();
  };
  const auto whole = [&] {
    gp_Pnt pa, pb;
    const double d = extrema(a, b, &pa, &pb);
    if (pointA != nullptr && std::isfinite(d)) {
      *pointA = pa;
      *pointB = pb;
    }
    return d;
  };
  const collide::Gap gap = collide::Nearest(shapeA, xfA, shapeB, xfB, 2.0);
  if (!gap.known || gap.pairs.empty())
    return whole();
  // Tessellations that touch or cross do NOT make the solids touch. A chord
  // of a concave face (a bore, a ring's tooth space) stands proud of it by
  // up to the deflection, so a shaft 0.016 mm inside a bore meshed at 0.1
  // crosses it. This returned 0 there, with no points, and a planet pushed
  // into its ring read as touching 0.04 mm early
  // (docs/inspect/README.md#crossing-tessellations-are-not-contact). The
  // face pairs within the slack are refined exactly as any others; a pair
  // whose surfaces really cross answers 0 itself.
  const bool crossing = gap.distance <= 0.0;

  // A long list asks again, finer: see kRemeshAbove. Not when the meshes
  // cross: the finer meshes cost seconds on a gear, and the pairs in hand
  // are refined until one whose surfaces cross ends the search, which for
  // two solids that really interpenetrate is among the first.
  std::vector<collide::Mesh::FacePair> pairs = gap.pairs;
  double tolerance = gap.tolerance;
  if (!crossing && pairs.size() > kRemeshAbove) {
    const double fine = gap.tolerance / (2.0 * kRemeshFactor);
    const auto ma = collide::Mesh::Build(shapeA, fine);
    const auto mb = collide::Mesh::Build(shapeB, fine);
    if (ma && mb && !ma->Empty() && !mb->Empty()) {
      const double fineTol = ma->Deflection() + mb->Deflection();
      const collide::Mesh::Near finer =
          ma->Nearest(xfA, *mb, xfB, 2.0 * fineTol);
      if (!finer.pairs.empty()) {
        pairs = finer.pairs;
        tolerance = fineTol;
      }
    }
  }
  // Past the cap, two shapes whose meshes cross overlap widely: 0, the
  // overlap pass's question, rather than minutes of BRepExtrema over two
  // interpenetrating solids.
  if (pairs.size() > kMaxRefinedPairs)
    return crossing ? 0.0 : whole();

  // The faces, once, in the walk order `triangleFaces` indexes.
  std::vector<TopoDS_Shape> fa, fb;
  for (TopExp_Explorer ex(a, TopAbs_FACE); ex.More(); ex.Next())
    fa.push_back(ex.Current());
  for (TopExp_Explorer ex(b, TopAbs_FACE); ex.More(); ex.Next())
    fb.push_back(ex.Current());
  for (const collide::Mesh::FacePair& pair : pairs)
    if (pair.a >= fa.size() || pair.b >= fb.size())
      return whole(); // the mesh and the face walk disagree: ask everything

  struct Hit {
    double d = kInf;
    gp_Pnt pa, pb;
    bool failed = false;
  };
  std::vector<Hit> hits(pairs.size());
  std::atomic<double> best{kInf};
  // How far a pair's triangles can sit from its surfaces: the two meshes'
  // deflections SUMMED (collide::Gap::tolerance), each surface off its own
  // tessellation by up to its own deflection, in opposite directions at
  // worst.
  const double margin = tolerance;
  parallel::For(pairs.size(), [&](size_t k) {
    const collide::Mesh::FacePair& pair = pairs[k];
    // This pair's TRUE distance is at least its triangles' less the two
    // deflections. STRICTLY past the best: a pair that could tie is still
    // asked, so which pairs hold the minimum does not depend on which worker
    // got there first, and neither do the points reported.
    if (pair.distance - margin > best.load(std::memory_order_relaxed))
      return;
    // Meshes that cross, and a pair already found whose SURFACES cross:
    // the answer is 0 and no pair can change it. Without this, two solids
    // that interpenetrate refined every pair in the slack (seconds a
    // measurement for a gear pushed into its mate). Which crossing pair
    // supplies the (degenerate) points then depends on the workers.
    if (crossing &&
        best.load(std::memory_order_relaxed) <= Precision::Confusion())
      return;
    Hit& h = hits[k];
    try {
      h.d = extrema(fa[pair.a], fb[pair.b], &h.pa, &h.pb);
    } catch (const Standard_Failure&) {
      h.failed = true; // a worker must not throw; the whole shapes are asked
      return;
    } catch (const std::exception&) {
      h.failed = true;
      return;
    }
    double seen = best.load(std::memory_order_relaxed);
    while (h.d < seen &&
           !best.compare_exchange_weak(seen, h.d, std::memory_order_relaxed))
      ;
  });
  // The minimum, and among equal minima the first in the (sorted) list.
  const Hit* winner = nullptr;
  for (const Hit& h : hits) {
    if (h.failed)
      return whole();
    if (winner == nullptr || h.d < winner->d)
      winner = &h;
  }
  if (winner == nullptr || !std::isfinite(winner->d))
    return whole();
  if (pointA != nullptr) {
    *pointA = winner->pa;
    *pointB = winner->pb;
  }
  return winner->d;
}

namespace {

// Whether any solid of `inner` lies inside `outer`, for two shapes whose
// SURFACES are known to be apart: each solid of `inner` is then wholly in or
// wholly out, so one of its vertices answers for all of it. Gated on the
// boxes nesting, which is the cheap half.
bool AnySolidInside(const TopoDS_Shape& inner, const TopoDS_Shape& outer) {
  Bnd_Box outerBox;
  BRepBndLib::Add(outer, outerBox);
  for (TopExp_Explorer s(inner, TopAbs_SOLID); s.More(); s.Next()) {
    TopExp_Explorer v(s.Current(), TopAbs_VERTEX);
    if (!v.More())
      continue;
    const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
    if (outerBox.IsOut(p))
      continue;
    BRepClass3d_SolidClassifier classifier(outer, p, Precision::Confusion());
    if (classifier.State() == TopAbs_IN)
      return true;
  }
  return false;
}

// ContactBetween and ClearanceBetween: the overlap pass (a boolean per
// box-overlapping body pair) when `wantOverlap`, then the distance pass when
// nothing overlapped and `wantDistance`.
std::optional<SetContact> ContactImpl(const std::vector<Body>& bodiesA,
                                      const glm::dmat4& xfA,
                                      const std::vector<Body>& bodiesB,
                                      const glm::dmat4& xfB, bool wantOverlap,
                                      bool wantDistance) {
  try {
    const TopLoc_Location la(ToTrsf(xfA)), lb(ToTrsf(xfB));
    struct Placed {
      TopoDS_Shape shape;
      const CADShape* source = nullptr; // for the mesh cache, keyed on it
      const Body* body = nullptr;
      Bnd_Box box;
      std::vector<Measure::ThreadSpan> threads;
      std::vector<GripSpan> grips;
      // Its round faces and their bands, read the first time an overlap
      // with a banded body asks (BandSpans).
      std::optional<std::vector<BandSpan>> spans;
      const std::vector<BandSpan>& Spans() {
        if (!spans)
          spans = BandSpans(*body, shape);
        return *spans;
      }
    };
    const auto place = [](const std::vector<Body>& bodies,
                          const TopLoc_Location& loc) {
      std::vector<Placed> out;
      for (const Body& b : bodies) {
        if (b.shape.GetShape().IsNull())
          continue;
        Placed p;
        p.source = &b.shape;
        p.body = &b;
        p.shape = b.shape.GetShape().Moved(loc);
        p.threads = ThreadSpans(b, loc);
        p.grips = GripSpans(b, loc);
        BRepBndLib::Add(p.shape, p.box);
        out.push_back(std::move(p));
      }
      return out;
    };
    std::vector<Placed> pa = place(bodiesA, la);
    std::vector<Placed> pb = place(bodiesB, lb);
    if (pa.empty() || pb.empty())
      return std::nullopt;

    // Volume pass: only box-overlapping pairs can intersect (same pruning
    // rationale as InterferenceVolume — a boolean is thousands of box tests).
    SetContact out;
    glm::dvec3 weighted(0.0);
    Bnd_Box overlapBox;
    struct Cand {
      double boxDist;
      size_t i, j;
    };
    std::vector<Cand> cands;
    cands.reserve(pa.size() * pb.size());
    for (size_t i = 0; i < pa.size(); ++i)
      for (size_t j = 0; j < pb.size(); ++j) {
        const double d = pa[i].box.Distance(pb[j].box);
        cands.push_back({d, i, j});
        if (d > 0.0 || !wantOverlap)
          continue;
        // Surfaces PROVEN apart need no boolean: their solids overlap only
        // if one is wholly inside the other. The meshes bound it -- a pair's
        // true distance is at least its tessellations' less the two
        // deflections summed -- and a modelled thread's nut turned clear of
        // its pillar is exactly that, at 1.1 s a boolean of two helicoids
        // (the clock's nut search, docs/inspect/README.md#threads). Only
        // where the distance pass will mesh them anyway (wantDistance:
        // ExactDistance asks Nearest of the same cache), so no new caller
        // meshes on a worker; and not for a pair the designed-overlap step
        // reports whether it overlaps or not (two printed presses, two
        // engaging thread regions, a ring and its groove).
        const auto printedPress = [](const Body& b) {
          return std::any_of(b.fitBands.begin(), b.fitBands.end(),
                             [](const FitBand& f) { return f.printedPress; });
        };
        const bool designed =
            (printedPress(*pa[i].body) && printedPress(*pb[j].body)) ||
            (!pa[i].threads.empty() && !pb[j].threads.empty()) ||
            (!pa[i].grips.empty() && !pb[j].grips.empty());
        if (wantDistance && !designed) {
          const collide::Gap gap =
              collide::Nearest(*pa[i].source, xfA, *pb[j].source, xfB, 2.0);
          bool apart = gap.known && gap.distance - gap.tolerance > kContactFuzz;
          // Meshes that do not cross but are within their deflections of
          // each other: a nut's thread 0.17 mm clear of its pillar's, meshed
          // at 0.15 mm. Asked again of copies meshed ten times finer, as the
          // distance pass narrows a long list (kRemeshFactor): 0.2-0.4 s on
          // the clock's threads, against the boolean's 1-3 s.
          if (gap.known && !apart && gap.distance > 0.0) {
            const double fine = gap.tolerance / (2.0 * kRemeshFactor);
            const auto ma = collide::Mesh::Build(*pa[i].source, fine);
            const auto mb = collide::Mesh::Build(*pb[j].source, fine);
            if (ma && mb && !ma->Empty() && !mb->Empty())
              apart = ma->Nearest(xfA, *mb, xfB, 0.0).distance -
                          (ma->Deflection() + mb->Deflection()) >
                      kContactFuzz;
          }
          if (apart && !AnySolidInside(pa[i].shape, pb[j].shape) &&
              !AnySolidInside(pb[j].shape, pa[i].shape))
            continue;
        }
        // Fuzzy, for the reason kContactFuzz documents: parts DESIGNED to
        // touch share a face, and an exact boolean over coincident faces
        // returns a sliver of contact-area x tolerance, which then reads as
        // an interference. This is the path the named-pair `interference`
        // RPC and contact_report take — InterferenceVolume above is the
        // whole-assembly sweep, and both need the same answer.
        BRepAlgoAPI_Common common;
        BuildQuery(common, pa[i].shape, pb[j].shape, kContactFuzz);
        if (!common.IsDone() || common.Shape().IsNull())
          return std::nullopt;
        const auto threaded =
            RemoveThreadEngagement(common.Shape(), pa[i].threads, pb[j].threads,
                                   out.engagedThreadPairs);
        if (!pa[i].threads.empty() && !pb[j].threads.empty())
          out.threadEngagementVolume +=
              std::max(0.0, VolumeOf(common.Shape()) - VolumeOf(threaded));
        // The designed-overlap step: what the model says is MEANT to be
        // solid in solid. A ring in its groove (RingGrip,
        // docs/parts/README.md#a-ring-in-its-groove) and a press fit
        // (FitBand, docs/tolerances/README.md#press-fits) are both an
        // annulus between two coaxial cylinders, relieved exactly; anything
        // else either pair overlaps is left, a clash. Every contact check
        // comes through here, so every one honours both.
        TopoDS_Shape overlap = threaded;
        if (!pa[i].grips.empty() && !pb[j].grips.empty()) {
          const double before = VolumeOf(overlap);
          overlap = RemoveRingGrip(overlap, pa[i].grips, pb[j].grips,
                                   out.ringGripPairs);
          out.ringGripVolume += std::max(0.0, before - VolumeOf(overlap));
        }
        // A printed press is reported overlapping or not, so a pair that
        // both carry one is looked at either way.
        const auto pressed = [](const Body& b) {
          return std::any_of(b.fitBands.begin(), b.fitBands.end(),
                             [](const FitBand& f) { return f.printedPress; });
        };
        if ((!pa[i].body->fitBands.empty() || !pb[j].body->fitBands.empty()) &&
            (VolumeOf(overlap) > 1e-9 ||
             (pressed(*pa[i].body) && pressed(*pb[j].body))))
          overlap =
              RelievePressFits(overlap, pa[i].Spans(), pb[j].Spans(), out);
        GProp_GProps props;
        BRepGProp::VolumeProperties(overlap, props);
        const double v = std::max(0.0, props.Mass());
        out.volume += v;
        if (v > 0.0) {
          // Weighted by volume, so several overlapping pairs report the
          // centre of the interference rather than the centre of the last
          // one found.
          const gp_Pnt c = props.CentreOfMass();
          weighted += v * glm::dvec3(c.X(), c.Y(), c.Z());
          BRepBndLib::Add(overlap, overlapBox);
          out.overlaps = true;
        }
      }
    if (out.volume > 0.0) {
      out.centroid = weighted / out.volume;
      if (out.overlaps && !overlapBox.IsVoid()) {
        double lo[3], hi[3];
        overlapBox.Get(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
        out.overlapMin = {lo[0], lo[1], lo[2]};
        out.overlapMax = {hi[0], hi[1], hi[2]};
      }
      return out; // overlapping sets: separation is 0 by definition
    }

    // Distance pass: probe in ascending box-distance order and stop once a
    // pair's box bound (a lower bound on its exact distance) can no longer
    // beat the best exact distance found.
    //
    // Skipped outright when the caller did not ask, because it is the
    // EXPENSIVE half — 299 ms a pair on an engine against the boolean's 165
    // — and a caller that only wants "does this overlap" has its answer by
    // here. It says so rather than reporting a 0 that would read as contact.
    if (!wantDistance) {
      out.distanceKnown = false;
      return out;
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
      return a.boxDist < b.boxDist;
    });
    double best = std::numeric_limits<double>::infinity();
    for (const Cand& c : cands) {
      if (c.boxDist >= best)
        break;
      best = std::min(best, ExactDistance(*pa[c.i].source, pa[c.i].shape, xfA,
                                          *pb[c.j].source, pb[c.j].shape, xfB));
    }
    if (!std::isfinite(best))
      return std::nullopt;
    if (!wantOverlap) {
      // Asked without the boolean: only an answer that PROVES no overlap
      // is one. Surfaces apart meet nowhere, so the sets overlap only if a
      // solid of one is wholly inside the other.
      if (best <= 0.0)
        return std::nullopt;
      for (const Placed& x : pa)
        for (const Placed& y : pb)
          if (AnySolidInside(x.shape, y.shape) ||
              AnySolidInside(y.shape, x.shape))
            return std::nullopt;
    }
    out.distance = best;
    return out;
  } catch (const std::exception& e) {
    LOG_ERROR("Contact check failed: {}", e.what());
    return std::nullopt;
  } catch (const Standard_Failure& e) {
    // OCC throws Standard_Failure, which does NOT derive from std::exception
    // (same trap as the siblings above).
    LOG_ERROR("Contact check failed: {}", e.GetMessageString()
                                              ? e.GetMessageString()
                                              : "OpenCASCADE failure");
    return std::nullopt;
  }
}

} // namespace

std::optional<SetContact> ContactBetween(const std::vector<Body>& bodiesA,
                                         const glm::dmat4& xfA,
                                         const std::vector<Body>& bodiesB,
                                         const glm::dmat4& xfB,
                                         bool wantDistance) {
  return ContactImpl(bodiesA, xfA, bodiesB, xfB, true, wantDistance);
}

std::optional<SetContact> ClearanceBetween(const std::vector<Body>& bodiesA,
                                           const glm::dmat4& xfA,
                                           const std::vector<Body>& bodiesB,
                                           const glm::dmat4& xfB) {
  return ContactImpl(bodiesA, xfA, bodiesB, xfB, false, true);
}

std::optional<SetCompare>
CompareBodySets(const std::vector<Body>& bodiesA, const glm::dmat4& xfA,
                const std::vector<Body>& bodiesB, const glm::dmat4& xfB,
                CADShape* missingOut, CADShape* extraOut, bool overlap) {
  try {
    const TopLoc_Location la(ToTrsf(xfA)), lb(ToTrsf(xfB));
    // Each side is FUSED, not compounded: with overlapping bodies a compound
    // double-counts the overlap in GProp volumes and every derived metric
    // (IoU, precision, ...) silently breaks. Fusing makes vol(side) mean
    // "occupied space" — dearer than CompoundOf, but a compare op runs once,
    // not per frame.
    const auto fuse = [](const std::vector<Body>& bodies,
                         const TopLoc_Location& loc) -> TopoDS_Shape {
      TopTools_ListOfShape placed;
      for (const Body& b : bodies)
        if (!b.shape.GetShape().IsNull())
          placed.Append(b.shape.GetShape().Moved(loc));
      if (placed.IsEmpty())
        return TopoDS_Shape();
      if (placed.Extent() == 1)
        return placed.First();
      BRepAlgoAPI_Fuse op;
      TopTools_ListOfShape args, tools;
      args.Append(placed.First());
      bool first = true;
      for (const TopoDS_Shape& s : placed) {
        if (first) {
          first = false;
          continue;
        }
        tools.Append(s);
      }
      BuildQuery(op, args, tools);
      if (!op.IsDone() || op.Shape().IsNull())
        return TopoDS_Shape();
      return op.Shape();
    };
    const CADShape a(fuse(bodiesA, la));
    const CADShape b(fuse(bodiesB, lb));
    if (a.GetShape().IsNull() || b.GetShape().IsNull())
      return std::nullopt;

    SetCompare out;
    const auto measure = [](const CADShape& s, double& vol, double& surf,
                            glm::dvec3& com, glm::dvec3& mn, glm::dvec3& mx,
                            int& faces, int& edges) {
      CADShape::MassProps mp;
      if (s.ComputeMassProps(mp)) {
        vol = mp.volume;
        surf = mp.surfaceArea;
        com = mp.centerOfMass;
      }
      s.GetBoundingBox(mn, mx);
      for (TopExp_Explorer ex(s.GetShape(), TopAbs_FACE); ex.More(); ex.Next())
        faces++;
      TopTools_IndexedMapOfShape edgeMap;
      TopExp::MapShapes(s.GetShape(), TopAbs_EDGE, edgeMap);
      edges = edgeMap.Extent();
    };
    measure(a, out.volumeA, out.surfaceA, out.comA, out.minA, out.maxA,
            out.facesA, out.edgesA);
    measure(b, out.volumeB, out.surfaceB, out.comB, out.minB, out.maxB,
            out.facesB, out.edgesB);

    // Everything above came off the two shapes directly. Everything below is
    // three booleans, which is where the seconds are.
    out.hasOverlap = overlap;
    if (!overlap)
      return out;

    const auto volumeOf = [](const TopoDS_Shape& s) {
      if (s.IsNull())
        return 0.0;
      GProp_GProps props;
      BRepGProp::VolumeProperties(s, props);
      return std::max(0.0, props.Mass());
    };
    BRepAlgoAPI_Common common;
    BuildQuery(common, a.GetShape(), b.GetShape());
    if (common.IsDone())
      out.common = volumeOf(common.Shape());
    BRepAlgoAPI_Cut missing;
    BuildQuery(missing, a.GetShape(), b.GetShape());
    if (missing.IsDone()) {
      out.missing = volumeOf(missing.Shape());
      if (missingOut != nullptr)
        *missingOut = CADShape(missing.Shape());
    }
    BRepAlgoAPI_Cut extra;
    BuildQuery(extra, b.GetShape(), a.GetShape());
    if (extra.IsDone()) {
      out.extra = volumeOf(extra.Shape());
      if (extraOut != nullptr)
        *extraOut = CADShape(extra.Shape());
    }
    return out;
  } catch (const std::exception& e) {
    LOG_ERROR("Shape compare failed: {}", e.what());
    return std::nullopt;
  } catch (const Standard_Failure& e) {
    LOG_ERROR("Shape compare failed: {}", e.GetMessageString()
                                              ? e.GetMessageString()
                                              : "OpenCASCADE failure");
    return std::nullopt;
  }
}

ShellReport AnalyzeWatertight(const CADShape& shape) {
  ShellReport out;
  const TopoDS_Shape& s = shape.GetShape();
  if (s.IsNull())
    return out;
  for (TopExp_Explorer ex(s, TopAbs_SHELL); ex.More(); ex.Next())
    out.shells++;
  // Free-edge scan: a closed (watertight) B-Rep uses every non-degenerate
  // edge in exactly two faces. Seam edges list their face twice, so they do
  // not count as free.
  TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
  TopExp::MapShapesAndAncestors(s, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
  for (int i = 1; i <= edgeFaces.Extent(); i++) {
    const TopoDS_Edge& e = TopoDS::Edge(edgeFaces.FindKey(i));
    if (BRep_Tool::Degenerated(e))
      continue;
    if (edgeFaces(i).Extent() < 2)
      out.freeEdges++;
  }
  out.watertight = out.shells > 0 && out.freeEdges == 0;
  return out;
}

namespace {
const char* CheckStatusStr(BRepCheck_Status s) {
  switch (s) {
  case BRepCheck_InvalidPointOnCurve:
  case BRepCheck_InvalidPointOnCurveOnSurface:
  case BRepCheck_InvalidPointOnSurface:
    return "vertex not on its curve/surface";
  case BRepCheck_No3DCurve:
    return "edge has no 3D curve";
  case BRepCheck_Multiple3DCurve:
    return "edge has multiple 3D curves";
  case BRepCheck_Invalid3DCurve:
    return "invalid 3D curve";
  case BRepCheck_NoCurveOnSurface:
  case BRepCheck_InvalidCurveOnSurface:
  case BRepCheck_InvalidCurveOnClosedSurface:
    return "invalid curve on surface";
  case BRepCheck_InvalidSameRangeFlag:
  case BRepCheck_InvalidSameParameterFlag:
    return "bad same-range/parameter flag";
  case BRepCheck_InvalidDegeneratedFlag:
    return "bad degenerated flag";
  case BRepCheck_FreeEdge:
    return "free edge (used by one face)";
  case BRepCheck_InvalidMultiConnexity:
    return "invalid multi-connexity";
  case BRepCheck_InvalidRange:
    return "invalid parameter range";
  case BRepCheck_EmptyWire:
    return "empty wire";
  case BRepCheck_RedundantEdge:
    return "redundant edge";
  case BRepCheck_SelfIntersectingWire:
    return "self-intersecting wire";
  case BRepCheck_NoSurface:
    return "face has no surface";
  case BRepCheck_InvalidWire:
  case BRepCheck_RedundantWire:
    return "invalid/redundant wire";
  case BRepCheck_IntersectingWires:
    return "intersecting wires";
  case BRepCheck_InvalidImbricationOfWires:
    return "bad wire nesting";
  case BRepCheck_EmptyShell:
    return "empty shell";
  case BRepCheck_RedundantFace:
    return "redundant face";
  case BRepCheck_InvalidImbricationOfShells:
    return "bad shell nesting";
  case BRepCheck_UnorientableShape:
    return "unorientable shape";
  case BRepCheck_NotClosed:
    return "shell/solid not closed";
  case BRepCheck_NotConnected:
    return "not connected";
  case BRepCheck_BadOrientation:
  case BRepCheck_BadOrientationOfSubshape:
    return "bad orientation";
  case BRepCheck_InvalidToleranceValue:
    return "invalid tolerance";
  case BRepCheck_CheckFail:
    return "check failed (fatal)";
  default:
    return "invalid geometry";
  }
}
} // namespace

namespace {
// Does a cavity of some solid in `s` reach outside that solid's outer skin?
// BRepCheck_Analyzer asks it with ONE point per shell (0.432 of the way along
// the first edge in face order), so its verdict on a void poking through the
// wall followed the order of the faces -- which the offset builder takes from
// a map hashed on addresses: a Draft that tilted a shelled cavity's wall
// through the outer wall was refused in some processes and kept in others
// (tests/fuzz/cases/rebuild-139.jsonl). Every vertex and edge midpoint of
// every inner shell is classified against the outer one instead.
bool CavityReachesOutside(const TopoDS_Shape& s) {
  for (TopExp_Explorer sol(s, TopAbs_SOLID); sol.More(); sol.Next()) {
    const TopoDS_Solid& solid = TopoDS::Solid(sol.Current());
    int shells = 0;
    for (TopExp_Explorer sh(solid, TopAbs_SHELL); sh.More(); sh.Next())
      shells++;
    if (shells < 2)
      continue;
    const TopoDS_Shell outer = BRepClass3d::OuterShell(solid);
    if (outer.IsNull())
      continue; // BRepCheck says what is wrong with that one
    TopoDS_Solid skin;
    BRep_Builder builder;
    builder.MakeSolid(skin);
    builder.Add(skin, outer);
    BRepClass3d_SolidClassifier inside(skin);
    const auto out = [&](const gp_Pnt& p, double tol) {
      inside.Perform(p, std::max(tol, Precision::Confusion()));
      return inside.State() == TopAbs_OUT;
    };
    for (TopExp_Explorer sh(solid, TopAbs_SHELL); sh.More(); sh.Next()) {
      if (sh.Current().IsSame(outer))
        continue;
      for (TopExp_Explorer v(sh.Current(), TopAbs_VERTEX); v.More(); v.Next()) {
        const TopoDS_Vertex& vx = TopoDS::Vertex(v.Current());
        if (out(BRep_Tool::Pnt(vx), BRep_Tool::Tolerance(vx)))
          return true;
      }
      for (TopExp_Explorer e(sh.Current(), TopAbs_EDGE); e.More(); e.Next()) {
        const TopoDS_Edge& edge = TopoDS::Edge(e.Current());
        if (BRep_Tool::Degenerated(edge))
          continue;
        const BRepAdaptor_Curve c(edge);
        if (out(c.Value(0.5 * (c.FirstParameter() + c.LastParameter())),
                BRep_Tool::Tolerance(edge)))
          return true;
      }
    }
  }
  return false;
}
} // namespace

GeometryReport CheckGeometry(const CADShape& shape) {
  GeometryReport out;
  const TopoDS_Shape& s = shape.GetShape();
  if (s.IsNull()) {
    out.reasons.push_back("null shape");
    return out;
  }
  BRepCheck_Analyzer ana(s);
  out.valid = ana.IsValid();
  const bool escapes = CavityReachesOutside(s);
  if (out.valid && !escapes)
    return out;
  out.valid = false;

  std::set<std::string> reasons;
  if (escapes)
    reasons.insert("a cavity reaches outside the body");
  const auto collect = [&](const TopoDS_Shape& sub) {
    Handle(BRepCheck_Result) res = ana.Result(sub);
    if (res.IsNull())
      return;
    for (const BRepCheck_Status st : res->Status())
      if (st != BRepCheck_NoError)
        reasons.insert(CheckStatusStr(st));
  };
  const auto scan = [&](TopAbs_ShapeEnum type, int& counter) {
    for (TopExp_Explorer ex(s, type); ex.More(); ex.Next()) {
      if (ana.IsValid(ex.Current()))
        continue;
      counter++;
      collect(ex.Current());
    }
  };
  scan(TopAbs_FACE, out.badFaces);
  scan(TopAbs_WIRE, out.badWires);
  scan(TopAbs_EDGE, out.badEdges);
  // The SHELL and SOLID levels have no counter of their own, but they must
  // still be able to SPEAK. A shape whose faces, wires and edges are each
  // fine while the shell they form is not — an unclosed or badly oriented
  // shell, which is what a sweep with an off-perpendicular profile produces —
  // used to report `valid: false` with every counter zero and `reasons: []`.
  // A verdict that names nothing is not a diagnosis, and it is the shape this
  // codebase refuses everywhere else.
  int ignored = 0;
  scan(TopAbs_SHELL, ignored);
  scan(TopAbs_SOLID, ignored);
  out.reasons.assign(reasons.begin(), reasons.end());
  return out;
}

std::string InvalidReasons(const CADShape& shape) {
  const GeometryReport r = CheckGeometry(shape);
  if (r.valid)
    return {};
  std::string out;
  for (const std::string& why : r.reasons)
    out += (out.empty() ? "" : ", ") + why;
  return out.empty() ? std::string("not a valid solid") : out;
}

std::optional<Element> Describe(const std::vector<Body>& bodies,
                                const ElementRef& ref) {
  std::string kind;
  const TopoDS_Shape shape = ResolveShape(bodies, ref, kind);
  if (shape.IsNull())
    return std::nullopt;

  Element out;
  out.kind = kind;
  GProp_GProps props;
  // The same classifier both index-keyed describes use, so a face resolved by
  // NAME reports the cylinder it is — a bore measured in the GUI used to come
  // back as 4712.390 mm² of surface area and no diameter at all.
  const ShapeClass sc = kind == "face" ? ClassifyFace(TopoDS::Face(shape))
                                       : ClassifyEdge(TopoDS::Edge(shape));
  out.shape = sc.kind;
  out.radius = sc.radius;
  out.hasRadius = sc.hasRadius;
  out.axis = sc.axis;
  out.hasAxis = sc.hasAxis;
  if (kind == "face") {
    BRepGProp::SurfaceProperties(shape, props);
    out.area = props.Mass();
  } else {
    BRepGProp::LinearProperties(shape, props);
    out.length = props.Mass();
  }
  return out;
}

std::optional<Bounds> ElementBounds(const std::vector<Body>& bodies,
                                    const ElementRef& ref) {
  std::string kind;
  const TopoDS_Shape shape = ResolveShape(bodies, ref, kind);
  if (shape.IsNull())
    return std::nullopt;
  Bnd_Box box;
  BRepBndLib::Add(shape, box);
  if (box.IsVoid())
    return std::nullopt;
  Standard_Real x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  Bounds out;
  out.min = glm::dvec3(x0, y0, z0);
  out.max = glm::dvec3(x1, y1, z1);
  return out;
}

namespace {
// A planar cutting face spanning every body's bounds, centered on the section
// plane. Null if `bodies` is empty. Shared by CrossSectionArea + SectionCaps.
TopoDS_Face BuildSectionCutter(const std::vector<Body>& bodies,
                               const glm::dvec3& origin, const glm::dvec3& n) {
  glm::dvec3 u =
      std::abs(n.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0);
  u = glm::normalize(u - n * glm::dot(u, n));
  const glm::dvec3 v = glm::cross(n, u);
  glm::dvec3 lo(1e30), hi(-1e30);
  for (const auto& b : bodies) {
    glm::dvec3 bmin, bmax;
    b.shape.GetBoundingBox(bmin, bmax);
    lo = glm::min(lo, bmin);
    hi = glm::max(hi, bmax);
  }
  const double s = glm::length(hi - lo) + 1.0;
  // Center the quad on the BODIES, projected onto the plane — not on the
  // plane's origin. Sizing it from the bodies' diagonal while centering it
  // at the origin only works for a model that sits near the origin: a part
  // built a metre out (or any assembly occurrence placed away from it) was
  // missed entirely by the cutter, so the cross-section area came back zero
  // and the section view had no cap faces. Same rule the split tool uses to
  // place its knife.
  const glm::dvec3 c = (lo + hi) * 0.5;
  const glm::dvec3 center = c - n * glm::dot(c - origin, n);
  auto pt = [&](double su, double sv) {
    const glm::dvec3 w = center + u * su + v * sv;
    return gp_Pnt(w.x, w.y, w.z);
  };
  BRepBuilderAPI_MakePolygon poly(pt(-s, -s), pt(s, -s), pt(s, s), pt(-s, s),
                                  Standard_True);
  return BRepBuilderAPI_MakeFace(poly.Wire()).Face();
}
// A cut face's boundary, loop by loop. The outer wire is flagged so a caller
// can tell the outline from the holes in it without re-deriving containment.
std::vector<SectionLoop> SectionLoopsOf(const TopoDS_Face& face) {
  std::vector<SectionLoop> loops;
  // The outer wire is the one with the biggest bounding box, not the one
  // `BRepTools::OuterWire` names: after the boolean that made this face, the
  // wire it hands back is not `IsSame` as any wire the explorer walks, so
  // every loop came back flagged inner. A planar face's outline is the one
  // that contains the rest, and on a plane that is simply the largest.
  std::vector<double> spans;
  for (TopExp_Explorer we(face, TopAbs_WIRE); we.More(); we.Next()) {
    Bnd_Box box;
    BRepBndLib::Add(we.Current(), box);
    spans.push_back(box.IsVoid() ? 0.0 : box.SquareExtent());
  }
  const size_t widest =
      spans.empty()
          ? 0
          : static_cast<size_t>(std::max_element(spans.begin(), spans.end()) -
                                spans.begin());
  size_t index = 0;
  for (TopExp_Explorer we(face, TopAbs_WIRE); we.More(); we.Next(), index++) {
    const TopoDS_Wire& wire = TopoDS::Wire(we.Current());
    SectionLoop loop;
    loop.outer = (index == widest);
    for (TopExp_Explorer ee(wire, TopAbs_EDGE); ee.More(); ee.Next()) {
      const TopoDS_Edge& edge = TopoDS::Edge(ee.Current());
      SectionSegment seg;
      const gp_Pnt a = BRep_Tool::Pnt(TopExp::FirstVertex(edge, Standard_True));
      const gp_Pnt b = BRep_Tool::Pnt(TopExp::LastVertex(edge, Standard_True));
      seg.from = glm::dvec3(a.X(), a.Y(), a.Z());
      seg.to = glm::dvec3(b.X(), b.Y(), b.Z());
      BRepAdaptor_Curve curve(edge);
      if (curve.GetType() == GeomAbs_Line) {
        seg.kind = "line";
      } else if (curve.GetType() == GeomAbs_Circle) {
        seg.kind = "arc";
        const gp_Circ circle = curve.Circle();
        const gp_Pnt cc = circle.Location();
        seg.center = glm::dvec3(cc.X(), cc.Y(), cc.Z());
        seg.radius = circle.Radius();
      } else {
        seg.kind = "other";
      }
      loop.segments.push_back(std::move(seg));
    }
    if (!loop.segments.empty())
      loops.push_back(std::move(loop));
  }
  return loops;
}

} // namespace

std::optional<std::vector<SectionRegion>>
CrossSectionRegions(const std::vector<Body>& bodies, const glm::dvec3& origin,
                    const glm::dvec3& normal, bool withLoops) {
  std::vector<SectionRegion> out;
  if (bodies.empty())
    return out;
  // A mesh body's regions come from its slice (MeshMeasure.h).
  out = meshmeasure::Section(bodies, origin, normal, withLoops).regions;
  if (std::none_of(bodies.begin(), bodies.end(),
                   [](const Body& b) { return b.shape.IsValid(); }))
    return out;
  try {
    const TopoDS_Face cutter =
        BuildSectionCutter(bodies, origin, glm::normalize(normal));
    for (const auto& b : bodies) {
      if (b.shape.GetShape().IsNull())
        continue;
      BRepAlgoAPI_Common common;
      BuildQuery(common, b.shape.GetShape(), cutter);
      if (!common.IsDone())
        continue;
      // One FACE per connected island. A face with inner wires is still one
      // island — an annulus is connected — which is the distinction that
      // makes this worth reporting rather than counting wires.
      for (TopExp_Explorer ex(common.Shape(), TopAbs_FACE); ex.More();
           ex.Next()) {
        GProp_GProps props;
        BRepGProp::SurfaceProperties(ex.Current(), props);
        if (props.Mass() <= 1e-9)
          continue;
        const gp_Pnt c = props.CentreOfMass();
        SectionRegion region;
        region.area = props.Mass();
        region.centroid = glm::dvec3(c.X(), c.Y(), c.Z());
        if (withLoops)
          region.loops = SectionLoopsOf(TopoDS::Face(ex.Current()));
        out.push_back(std::move(region));
      }
    }
    std::sort(out.begin(), out.end(),
              [](const SectionRegion& a, const SectionRegion& b) {
                return a.area > b.area; // biggest first: the part, then the
                                        // bits that may not belong
              });
    return out;
  } catch (const std::exception& e) {
    LOG_ERROR("Cross-section regions failed: {}", e.what());
    return std::nullopt;
  } catch (const Standard_Failure& e) {
    LOG_ERROR("Cross-section regions failed: {}", e.GetMessageString());
    return std::nullopt;
  }
}

std::optional<double> SectionEnclosedArea(const std::vector<Body>& bodies,
                                          const glm::dvec3& origin,
                                          const glm::dvec3& normal) {
  if (bodies.empty())
    return 0.0;
  try {
    const TopoDS_Face cutter =
        BuildSectionCutter(bodies, origin, glm::normalize(normal));
    // Each body's cut face, the material the plane crosses.
    TopTools_ListOfShape material;
    for (const auto& b : bodies) {
      if (b.shape.GetShape().IsNull())
        continue;
      BRepAlgoAPI_Common common;
      BuildQuery(common, b.shape.GetShape(), cutter);
      if (!common.IsDone() || common.Shape().IsNull())
        continue;
      GProp_GProps props;
      BRepGProp::SurfaceProperties(common.Shape(), props);
      if (props.Mass() > 1e-9)
        material.Append(common.Shape());
    }
    if (material.IsEmpty())
      return 0.0;
    // The plane with ALL the material taken out at once, so faces of
    // separate bodies that meet or overlap act as one: what is left is the
    // open plane around the part plus one face per region the material
    // rings. The cutter is the bodies' diagonal larger than them on every
    // side, so the open plane reaches past their bounds and an enclosed
    // region cannot.
    TopTools_ListOfShape plane;
    plane.Append(cutter);
    BRepAlgoAPI_Cut cut;
    BuildQuery(cut, plane, material);
    if (!cut.IsDone() || cut.Shape().IsNull())
      return std::nullopt;
    Bnd_Box within;
    for (TopTools_ListIteratorOfListOfShape it(material); it.More(); it.Next())
      BRepBndLib::Add(it.Value(), within);
    within.Enlarge(1e-3 + 1e-6 * std::sqrt(within.SquareExtent()));
    Standard_Real x0, y0, z0, x1, y1, z1;
    within.Get(x0, y0, z0, x1, y1, z1);
    double area = 0.0;
    for (TopExp_Explorer ex(cut.Shape(), TopAbs_FACE); ex.More(); ex.Next()) {
      Bnd_Box box;
      BRepBndLib::Add(ex.Current(), box);
      if (box.IsVoid())
        continue;
      Standard_Real a0, b0, c0, a1, b1, c1;
      box.Get(a0, b0, c0, a1, b1, c1);
      if (a0 < x0 || b0 < y0 || c0 < z0 || a1 > x1 || b1 > y1 || c1 > z1)
        continue; // the open plane around the part
      GProp_GProps props;
      BRepGProp::SurfaceProperties(ex.Current(), props);
      area += props.Mass();
    }
    return area;
  } catch (const std::exception& e) {
    LOG_ERROR("Enclosed section area failed: {}", e.what());
    return std::nullopt;
  } catch (const Standard_Failure& e) {
    LOG_ERROR("Enclosed section area failed: {}", e.GetMessageString()
                                                      ? e.GetMessageString()
                                                      : "OpenCASCADE failure");
    return std::nullopt;
  }
}

std::optional<double> CrossSectionArea(const std::vector<Body>& bodies,
                                       const glm::dvec3& origin,
                                       const glm::dvec3& normal) {
  if (bodies.empty())
    return 0.0;
  // A mesh body's area is its closed loops' (MeshMeasure.h); an open one's
  // cut encloses nothing and adds nothing, which the `section` RPC says.
  const double meshArea =
      meshmeasure::Section(bodies, origin, normal, false).area;
  if (std::none_of(bodies.begin(), bodies.end(),
                   [](const Body& b) { return b.shape.IsValid(); }))
    return meshArea;
  try {
    const TopoDS_Face cutter =
        BuildSectionCutter(bodies, origin, glm::normalize(normal));
    double area = meshArea;
    for (const auto& b : bodies) {
      if (b.shape.GetShape().IsNull())
        continue;
      BRepAlgoAPI_Common common;
      BuildQuery(common, b.shape.GetShape(), cutter);
      if (!common.IsDone())
        continue;
      GProp_GProps props;
      BRepGProp::SurfaceProperties(common.Shape(), props);
      area += props.Mass();
    }
    return area;
  } catch (const std::exception& e) {
    LOG_ERROR("Cross-section failed: {}", e.what());
    return std::nullopt;
  } catch (const Standard_Failure& e) {
    // OCC throws Standard_Failure, which does NOT derive from
    // std::exception, so the catch above never sees it. On OCC 7.6
    // these calls do throw for transformed shapes, and the escape
    // reached the RPC boundary as an "unknown exception".
    LOG_ERROR("Cross-section failed: {}", e.GetMessageString()
                                              ? e.GetMessageString()
                                              : "OpenCASCADE failure");
    return std::nullopt;
  }
}

namespace {
// Quantized so a plane that differs in the last bits of a float is the same
// plane, for the same reason PlacementId quantizes its own.
void AppendQuantized(std::string& key, double v) {
  key += std::to_string(static_cast<int64_t>(std::llround(v * 1.0e6)));
  key += ',';
}

std::string CapKey(const Body& b, const glm::dvec3& origin,
                   const glm::dvec3& normal,
                   const std::vector<glm::dvec4>& trimPlanes) {
  std::string key = std::to_string(b.shape.PlacementId());
  key += '|';
  for (int i = 0; i < 3; i++)
    AppendQuantized(key, origin[i]);
  for (int i = 0; i < 3; i++)
    AppendQuantized(key, normal[i]);
  key += '|';
  for (const glm::dvec4& t : trimPlanes)
    for (int i = 0; i < 4; i++)
      AppendQuantized(key, t[i]);
  return key;
}

// Big enough that a scrub of a large assembly hits all the way through (the
// engine is 27 parts x one plane), small enough that it cannot grow without
// bound over a session of moving cuts about.
constexpr size_t kMaxCapCache = 512;
} // namespace

static CADShape MeshSectionCap(const Body& body, const glm::dvec3& origin,
                               const glm::dvec3& normal) {
  if (!body.IsMesh() || !body.mesh->Closed())
    return CADShape();
  const mesh::SliceResult s = mesh::Slice(*body.mesh, origin, normal);
  const mesh::SliceCap cap = mesh::Cap(s, origin, normal);
  return cad::TriangulationShape(cap.positions, cap.triangles);
}

std::vector<CADShape> SectionCaps(const std::vector<Body>& bodies,
                                  const glm::dvec3& origin,
                                  const glm::dvec3& normal,
                                  const std::vector<glm::dvec4>& trimPlanes,
                                  SectionCapCache* cache) {
  std::vector<CADShape> caps;
  if (bodies.empty())
    return caps;
  if (cache != nullptr && cache->entries.size() > kMaxCapCache)
    cache->Clear(); // wholesale, never one at a time — see the header
  // Only a B-Rep body needs the cutter; a part of mesh bodies alone has no
  // box to size it from.
  const bool anyBRep =
      std::any_of(bodies.begin(), bodies.end(),
                  [](const Body& b) { return b.shape.IsValid(); });
  try {
    const TopoDS_Face cutter =
        anyBRep ? BuildSectionCutter(bodies, origin, glm::normalize(normal))
                : TopoDS_Face();
    for (const auto& b : bodies) {
      if (b.IsMesh()) {
        // A closed mesh body's cap is its slice's loops, triangulated, in
        // the plane: no boolean. An open one gets none -- its cut closes
        // nothing (`list_sections` says so).
        caps.push_back(MeshSectionCap(b, origin, normal));
        continue;
      }
      if (b.shape.GetShape().IsNull()) {
        caps.emplace_back(); // keep alignment with bodies[]
        continue;
      }
      // The cutter is an oversized quad covering EVERY body, so a body's cap
      // does not depend on the quad's exact size — only on the body and the
      // planes. That is what lets the key leave the other bodies out, and it
      // matters: in a mechanism something always moves, so a key that
      // included the scene's extent would miss on every step.
      std::string key;
      if (cache != nullptr) {
        key = CapKey(b, origin, normal, trimPlanes);
        const auto it = cache->entries.find(key);
        if (it != cache->entries.end()) {
          cache->hits++;
          caps.push_back(it->second.cap);
          continue;
        }
        cache->misses++;
      }
      const auto remember = [&](const CADShape& cap) {
        if (cache != nullptr)
          cache->entries.emplace(key, SectionCapCache::Entry{b.shape, cap});
        caps.push_back(cap);
      };
      BRepAlgoAPI_Common common;
      BuildQuery(common, b.shape.GetShape(), cutter);
      // A body that doesn't meet the plane yields an empty compound (not
      // null); keep a cap only when it has real area, so callers can test
      // IsValid() to mean "there is a cut face here".
      if (common.IsDone() && !common.Shape().IsNull()) {
        // Cut the cap back to the kept side of every other section. Each is
        // a half-space solid built from the same oversized quad the cutter
        // uses, with its reference point on the DISCARDED side.
        TopoDS_Shape capShape = common.Shape();
        for (const glm::dvec4& t : trimPlanes) {
          const glm::dvec3 tn = glm::dvec3(t);
          if (glm::length(tn) < 1e-6)
            continue;
          const glm::dvec3 tnu = glm::normalize(tn);
          const glm::dvec3 tOrigin = tnu * t.w;
          const TopoDS_Face trimFace = BuildSectionCutter(bodies, tOrigin, tnu);
          // A point well past the plane on the removed side. The distance is
          // the model's own size, so it scales with the part.
          glm::dvec3 lo(1e30), hi(-1e30);
          for (const auto& bb : bodies) {
            glm::dvec3 bmin, bmax;
            bb.shape.GetBoundingBox(bmin, bmax);
            lo = glm::min(lo, bmin);
            hi = glm::max(hi, bmax);
          }
          const glm::dvec3 ref = tOrigin + tnu * (glm::length(hi - lo) + 1.0);
          BRepPrimAPI_MakeHalfSpace halfSpace(trimFace,
                                              gp_Pnt(ref.x, ref.y, ref.z));
          if (!halfSpace.IsDone())
            continue;
          BRepAlgoAPI_Cut trimmed;
          BuildQuery(trimmed, capShape, halfSpace.Solid());
          if (trimmed.IsDone() && !trimmed.Shape().IsNull())
            capShape = trimmed.Shape();
        }
        GProp_GProps props;
        BRepGProp::SurfaceProperties(capShape, props);
        if (props.Mass() > 1e-9) {
          remember(CADShape(capShape));
          continue;
        }
      }
      remember(CADShape()); // no cut here, and that is worth caching too
    }
  } catch (const std::exception& e) {
    LOG_ERROR("Section caps failed: {}", e.what());
    caps.clear();
  } catch (const Standard_Failure& e) {
    // OCC throws Standard_Failure, which does NOT derive from
    // std::exception, so the catch above never sees it. On OCC 7.6
    // these calls do throw for transformed shapes, and the escape
    // reached the RPC boundary as an "unknown exception".
    LOG_ERROR("Section caps failed: {}", e.GetMessageString()
                                             ? e.GetMessageString()
                                             : "OpenCASCADE failure");
    caps.clear();
  }
  return caps;
}

std::optional<Direction> DirectionOf(const std::vector<Body>& bodies,
                                     const ElementRef& ref) {
  std::string kind;
  const TopoDS_Shape shape = ResolveShape(bodies, ref, kind);
  if (shape.IsNull())
    return std::nullopt;
  const auto make = [](const gp_Dir& d, bool oriented, const char* from,
                       const char* plural) {
    Direction out;
    out.dir = glm::dvec3(d.X(), d.Y(), d.Z());
    out.oriented = oriented;
    out.from = from;
    out.plural = plural;
    return std::optional<Direction>(out);
  };
  try {
    if (kind == "face") {
      const TopoDS_Face face = TopoDS::Face(shape);
      BRepAdaptor_Surface surf(face);
      switch (surf.GetType()) {
      case GeomAbs_Plane: {
        GProp_GProps props;
        BRepGProp::SurfaceProperties(shape, props);
        if (const auto n = FaceNormal(face, props.CentreOfMass()))
          return make(*n, /*oriented=*/true, "the face normal", "face normals");
        return std::nullopt;
      }
      case GeomAbs_Cylinder:
        return make(surf.Cylinder().Axis().Direction(), false,
                    "the cylinder axis", "cylinder axes");
      case GeomAbs_Cone:
        return make(surf.Cone().Axis().Direction(), false, "the cone axis",
                    "cone axes");
      case GeomAbs_Torus:
        return make(surf.Torus().Axis().Direction(), false, "the torus axis",
                    "torus axes");
      default:
        // A sphere has no direction, and neither does a general surface.
        // Refuse rather than hand back a plausible axis nobody asked for.
        return std::nullopt;
      }
    }
    BRepAdaptor_Curve curve(TopoDS::Edge(shape));
    switch (curve.GetType()) {
    case GeomAbs_Line:
      // A line's sign is whichever way OCC parameterised it, which is not a
      // fact about the model — hence oriented=false.
      return make(curve.Line().Direction(), false, "the edge", "edges");
    case GeomAbs_Circle:
      return make(curve.Circle().Axis().Direction(), false, "the circle axis",
                  "circle axes");
    default:
      return std::nullopt;
    }
  } catch (const Standard_Failure& e) {
    LOG_ERROR("Measure direction failed: {}", e.GetMessageString()
                                                  ? e.GetMessageString()
                                                  : "OpenCASCADE failure");
    return std::nullopt;
  }
}

std::optional<AngleResult> Angle(const std::vector<Body>& bodies,
                                 const ElementRef& a, const ElementRef& b) {
  return Angle(bodies, a, glm::dmat4(1.0), bodies, b, glm::dmat4(1.0));
}

std::optional<AngleResult> Angle(const std::vector<Body>& bodiesA,
                                 const ElementRef& a, const glm::dmat4& xfA,
                                 const std::vector<Body>& bodiesB,
                                 const ElementRef& b, const glm::dmat4& xfB) {
  const auto da = DirectionOf(bodiesA, a);
  const auto db = DirectionOf(bodiesB, b);
  if (!da || !db)
    return std::nullopt;
  // Occurrence placements are rigid, so only the rotation touches a
  // direction — a translation cannot change an angle.
  const auto placed = [](const glm::dvec3& d, const glm::dmat4& xf) {
    return glm::normalize(glm::dmat3(xf) * d);
  };
  const glm::dvec3 va = placed(da->dir, xfA);
  const glm::dvec3 vb = placed(db->dir, xfB);
  if (!std::isfinite(va.x) || !std::isfinite(vb.x))
    return std::nullopt;

  AngleResult out;
  const double dot = std::clamp(glm::dot(va, vb), -1.0, 1.0);
  const double deg = angles::ToDegrees(std::acos(dot));
  // Fold to [0, 90] when either side's sign is arbitrary: an edge that OCC
  // happened to parameterise the other way must not turn 30° into 150°.
  const bool signed_ = da->oriented && db->oriented;
  out.degrees = signed_ ? deg : std::min(deg, 180.0 - deg);
  out.acute = std::min(deg, 180.0 - deg);
  // A ten-thousandth of a degree: far below anything a designer means, far
  // above the noise in an analytic surface's axis.
  constexpr double kTol = 1.0e-4;
  out.parallel = out.acute <= kTol;
  out.perpendicular = std::abs(out.acute - 90.0) <= kTol;
  // Read it aloud before changing it: "between the edge and the edge" is
  // what a naive join produces, and a basis nobody can read is a basis
  // nobody checks.
  if (signed_) {
    out.basis = "between the face normals";
  } else {
    const std::string what = da->plural == db->plural
                                 ? "the " + da->plural
                                 : da->from + " and " + db->from;
    out.basis = "between " + what + " (unsigned, so 0-90\xc2\xb0)";
  }
  return out;
}

const char* FacingWord(const glm::dvec3& unitDir) {
  constexpr double kAxisAligned = 0.9; // ~25 degrees of slop
  const double upness = glm::dot(unitDir, conv::kWorldUp);
  if (upness > kAxisAligned)
    return "top";
  if (upness < -kAxisAligned)
    return "bottom";
  if (std::abs(glm::dot(unitDir, conv::kWorldFront)) > kAxisAligned)
    return "side";
  if (std::abs(glm::dot(unitDir, conv::kWorldRight)) > kAxisAligned)
    return "side";
  return nullptr;
}

std::string HumanName(const std::vector<Body>& bodies, const ElementRef& ref,
                      const units::System& sys) {
  const auto e = Describe(bodies, ref);
  if (!e)
    return {};
  // Which way a planar face points, in the words a person uses about a part
  // on a bench. Only for planes: a cylinder has no "top".
  const auto facing = [&]() -> const char* {
    const auto n = DirectionOf(bodies, ref);
    if (!n || !n->oriented)
      return nullptr;
    return FacingWord(glm::normalize(n->dir));
  };

  if (e->kind == "face") {
    if (e->shape == "cylinder" && e->hasRadius) {
      // A cylindrical face is a bore or a boss and either way the number
      // people say about it is the diameter.
      return "\xc3\x98" + units::FormatLength(e->radius * 2.0, sys) + " bore";
    }
    if (e->shape == "sphere")
      return "spherical face";
    if (e->shape == "cone")
      return "conical face";
    if (e->shape == "torus")
      return "toroidal face";
    if (const char* where = facing())
      return std::string(where) + " face";
    return "face";
  }
  if (e->shape == "circle" || e->shape == "arc") {
    return "\xc3\x98" + units::FormatLength(e->radius * 2.0, sys) +
           (e->shape == "arc" ? " arc" : " circle");
  }
  return units::FormatLength(e->length, sys) + " edge";
}

std::optional<glm::dvec3> AnchorOf(const std::vector<Body>& bodies,
                                   const ElementRef& ref) {
  std::string kind;
  const TopoDS_Shape shape = ResolveShape(bodies, ref, kind);
  if (shape.IsNull())
    return std::nullopt;
  try {
    if (kind == "face") {
      GProp_GProps props;
      BRepGProp::SurfaceProperties(shape, props);
      const gp_Pnt c = props.CentreOfMass();
      return glm::dvec3(c.X(), c.Y(), c.Z());
    }
    BRepAdaptor_Curve curve(TopoDS::Edge(shape));
    const gp_Pnt mid =
        curve.Value(0.5 * (curve.FirstParameter() + curve.LastParameter()));
    return glm::dvec3(mid.X(), mid.Y(), mid.Z());
  } catch (const Standard_Failure&) {
    return std::nullopt;
  }
}

const char* ToString(DistanceKind k) {
  switch (k) {
  case DistanceKind::Maximum:
    return "maximum";
  case DistanceKind::Centres:
    return "centres";
  case DistanceKind::Minimum:
    break;
  }
  return "minimum";
}

bool DistanceKindFromString(const std::string& s, DistanceKind& out) {
  if (s == "minimum") {
    out = DistanceKind::Minimum;
    return true;
  }
  if (s == "maximum") {
    out = DistanceKind::Maximum;
    return true;
  }
  if (s == "centres" || s == "centers") { // both spellings; one meaning
    out = DistanceKind::Centres;
    return true;
  }
  return false;
}

std::vector<const char*> DistanceKindNames() {
  return {"minimum", "maximum", "centres"};
}

std::optional<DistanceResult>
DistanceOfKind(const std::vector<Body>& bodiesA, const ElementRef& a,
               const glm::dmat4& xfA, const std::vector<Body>& bodiesB,
               const ElementRef& b, const glm::dmat4& xfB, DistanceKind kind) {
  if (kind == DistanceKind::Minimum)
    return DistanceDetailed(bodiesA, a, xfA, bodiesB, b, xfB);

  if (kind == DistanceKind::Centres) {
    const auto ca = CentreOf(bodiesA, a);
    const auto cb = CentreOf(bodiesB, b);
    if (!ca || !cb)
      return std::nullopt;
    DistanceResult out;
    out.pointA = glm::dvec3(xfA * glm::dvec4(*ca, 1.0));
    out.pointB = glm::dvec3(xfB * glm::dvec4(*cb, 1.0));
    out.distance = glm::distance(out.pointA, out.pointB);
    return out;
  }

  // Maximum. BRepExtrema only minimises, and the furthest points of two
  // curved faces are not in general vertices, so this samples the
  // tessellated boundary of each element and takes the furthest pair. On the
  // planar/linear geometry that dominates real use the extreme IS a vertex
  // and the answer is exact; on a curved face it is a tight lower bound,
  // which is the honest thing a sampled method can offer.
  std::string ka, kb;
  TopoDS_Shape sa = ResolveShape(bodiesA, a, ka);
  TopoDS_Shape sb = ResolveShape(bodiesB, b, kb);
  if (sa.IsNull() || sb.IsNull())
    return std::nullopt;
  try {
    sa = sa.Moved(TopLoc_Location(ToTrsf(xfA)));
    sb = sb.Moved(TopLoc_Location(ToTrsf(xfB)));
    const auto sample = [](const TopoDS_Shape& s) {
      std::vector<gp_Pnt> pts;
      // Vertices first — on a box or a straight edge they are the answer.
      for (TopExp_Explorer ex(s, TopAbs_VERTEX); ex.More(); ex.Next())
        pts.push_back(BRep_Tool::Pnt(TopoDS::Vertex(ex.Current())));
      // Then points along every edge, which is what catches a circular rim
      // whose extreme is between its (one or two) vertices.
      constexpr int kPerEdge = 24;
      for (TopExp_Explorer ex(s, TopAbs_EDGE); ex.More(); ex.Next()) {
        BRepAdaptor_Curve curve(TopoDS::Edge(ex.Current()));
        const double t0 = curve.FirstParameter(), t1 = curve.LastParameter();
        for (int i = 0; i <= kPerEdge; i++)
          pts.push_back(curve.Value(t0 + (t1 - t0) * i / kPerEdge));
      }
      return pts;
    };
    const std::vector<gp_Pnt> pa = sample(sa), pb = sample(sb);
    if (pa.empty() || pb.empty())
      return std::nullopt;
    DistanceResult out;
    double best = -1.0;
    for (const gp_Pnt& p : pa) {
      for (const gp_Pnt& q : pb) {
        const double d2 = p.SquareDistance(q);
        if (d2 > best) {
          best = d2;
          out.pointA = glm::dvec3(p.X(), p.Y(), p.Z());
          out.pointB = glm::dvec3(q.X(), q.Y(), q.Z());
        }
      }
    }
    out.distance = std::sqrt(best);
    return out;
  } catch (const Standard_Failure& e) {
    LOG_ERROR("Measure maximum distance failed: {}",
              e.GetMessageString() ? e.GetMessageString()
                                   : "OpenCASCADE failure");
    return std::nullopt;
  }
}

std::optional<SetTotal> TotalOf(const std::vector<Body>& bodies,
                                const std::vector<ElementRef>& refs) {
  if (refs.empty())
    return std::nullopt;
  SetTotal out;
  for (const ElementRef& r : refs) {
    const auto e = Describe(bodies, r);
    if (!e)
      return std::nullopt;
    if (out.kind.empty())
      out.kind = e->kind;
    else if (out.kind != e->kind)
      return std::nullopt; // an area plus a length is not a quantity
    out.total += e->kind == "face" ? e->area : e->length;
    out.count++;
  }
  return out;
}

std::optional<glm::dvec3> CornerOf(const std::vector<Body>& bodies,
                                   const ElementRef& edgeA,
                                   const ElementRef& edgeB) {
  for (const Body& body : bodies) {
    if (body.createdBy != edgeA.featureId || body.shape.GetShape().IsNull())
      continue;
    const auto a = body.elementMap.ResolveEdge(edgeA.name);
    const auto b = body.elementMap.ResolveEdge(edgeB.name);
    if (!a || !b)
      continue; // another body of the same creator may hold them
    glm::dvec3 p;
    if (!body.shape.CornerPoint(*a, *b, p))
      return std::nullopt;
    return p;
  }
  return std::nullopt;
}

std::optional<glm::dvec3> CentreOf(const std::vector<Body>& bodies,
                                   const ElementRef& ref) {
  std::string kind;
  const TopoDS_Shape shape = ResolveShape(bodies, ref, kind);
  if (shape.IsNull())
    return std::nullopt;
  const auto pt = [](const gp_Pnt& p) {
    return std::optional<glm::dvec3>(glm::dvec3(p.X(), p.Y(), p.Z()));
  };
  try {
    if (kind == "edge") {
      BRepAdaptor_Curve curve(TopoDS::Edge(shape));
      if (curve.GetType() == GeomAbs_Circle)
        return pt(curve.Circle().Location());
      return AnchorOf(bodies, ref);
    }
    const TopoDS_Face face = TopoDS::Face(shape);
    BRepAdaptor_Surface surf(face);
    // For a bore, the centre that matters is on the AXIS, level with the
    // face — the point a drawing would put the centre mark on.
    const auto onAxis = [&](const gp_Ax1& ax) {
      GProp_GProps props;
      BRepGProp::SurfaceProperties(shape, props);
      const gp_Pnt com = props.CentreOfMass();
      const gp_Lin axis(ax);
      return pt(ElCLib::Value(ElCLib::Parameter(axis, com), axis));
    };
    switch (surf.GetType()) {
    case GeomAbs_Cylinder:
      return onAxis(surf.Cylinder().Axis());
    case GeomAbs_Cone:
      return onAxis(surf.Cone().Axis());
    case GeomAbs_Torus:
      return pt(surf.Torus().Location());
    case GeomAbs_Sphere:
      return pt(surf.Sphere().Location());
    default:
      // A planar or general face's centre of mass IS its centre.
      return AnchorOf(bodies, ref);
    }
  } catch (const Standard_Failure&) {
    return AnchorOf(bodies, ref);
  }
}

std::optional<RimCircle> RimCircleOf(const std::vector<Body>& bodies,
                                     const ElementRef& ref) {
  const auto owner =
      std::find_if(bodies.begin(), bodies.end(),
                   [&](const Body& b) { return b.createdBy == ref.featureId; });
  if (owner == bodies.end())
    return std::nullopt;
  std::string kind;
  const TopoDS_Shape picked = ResolveShape(bodies, ref, kind);
  if (picked.IsNull() || kind != "edge")
    return std::nullopt;
  try {
    const BRepAdaptor_Curve pickedCurve(TopoDS::Edge(picked));
    if (pickedCurve.GetType() != GeomAbs_Circle)
      return std::nullopt;
    const gp_Circ circle = pickedCurve.Circle();
    const double r = circle.Radius();
    // "The same circle" to a tolerance that scales with it: a rim is a rim
    // whether it is 3 mm or 300 mm across.
    const double tol = 1e-5 * r;
    const gp_Dir axis = circle.Axis().Direction();

    // Each arc as an angular interval on the PICKED circle's own frame. An
    // edge's parameters are angles in ITS circle's frame, which may start
    // elsewhere or run the other way round, so the interval is placed by
    // where its ends and middle fall on this one.
    constexpr double kTau = 2.0 * M_PI;
    std::vector<std::pair<double, double>> spans;
    int arcs = 0;
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(owner->shape.GetShape(), TopAbs_EDGE, edges);
    for (int i = 1; i <= edges.Extent(); i++) {
      const BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
      if (c.GetType() != GeomAbs_Circle)
        continue;
      const gp_Circ other = c.Circle();
      if (std::abs(other.Radius() - r) > tol ||
          other.Location().Distance(circle.Location()) > tol ||
          std::abs(std::abs(other.Axis().Direction().Dot(axis)) - 1.0) >
              tol / std::max(r, tol))
        continue;
      const double span =
          std::min(kTau, std::abs(c.LastParameter() - c.FirstParameter()));
      const double a0 = ElCLib::Parameter(circle, c.Value(c.FirstParameter()));
      const double a1 = ElCLib::Parameter(circle, c.Value(c.LastParameter()));
      const double am = ElCLib::Parameter(
          circle, c.Value(0.5 * (c.FirstParameter() + c.LastParameter())));
      const auto wrapped = [&](double a) {
        a = std::fmod(a, kTau);
        return a < 0.0 ? a + kTau : a;
      };
      // Whichever end the middle is half a span ahead of is the start.
      const double gap0 = std::abs(wrapped(a0 + 0.5 * span - am + M_PI) - M_PI);
      const double gap1 = std::abs(wrapped(a1 + 0.5 * span - am + M_PI) - M_PI);
      const double start = wrapped(gap0 <= gap1 ? a0 : a1);
      arcs++;
      if (start + span <= kTau) {
        spans.emplace_back(start, start + span);
      } else {
        spans.emplace_back(start, kTau);
        spans.emplace_back(0.0, start + span - kTau);
      }
    }
    std::sort(spans.begin(), spans.end());
    double covered = 0.0, from = 0.0, to = -1.0;
    for (const auto& [a, b] : spans) {
      if (a > to) {
        if (to > from)
          covered += to - from;
        from = a;
        to = b;
      } else {
        to = std::max(to, b);
      }
    }
    if (to > from)
      covered += to - from;

    RimCircle out;
    const gp_Pnt c = circle.Location();
    out.centre = {c.X(), c.Y(), c.Z()};
    out.axis = {axis.X(), axis.Y(), axis.Z()};
    out.radius = r;
    out.arcs = arcs;
    // Arc ends meet to within the edge tolerance, not exactly; a gap that
    // small is not a gap in the rim.
    out.coveredDeg = std::min(360.0, angles::ToDegrees(covered));
    if (kTau - covered < tol / r * arcs)
      out.coveredDeg = 360.0;
    return out;
  } catch (const Standard_Failure&) {
    return std::nullopt;
  }
}

std::optional<DistanceResult>
DimensionAnchors(const std::vector<Body>& bodiesA, const ElementRef& a,
                 const glm::dmat4& xfA, const std::vector<Body>& bodiesB,
                 const ElementRef& b, const glm::dmat4& xfB, bool parallel) {
  const auto extrema = DistanceDetailed(bodiesA, a, xfA, bodiesB, b, xfB);
  if (!parallel || !extrema)
    return extrema;
  // Parallel: re-anchor on A's own centre. The distance is unchanged (that
  // is what parallel MEANS), so keep the extrema value and move only where
  // the dimension is drawn.
  const auto anchor = AnchorOf(bodiesA, a);
  if (!anchor)
    return extrema;
  const glm::dvec3 fromA = glm::dvec3(xfA * glm::dvec4(*anchor, 1.0));
  std::string kb;
  TopoDS_Shape sb = ResolveShape(bodiesB, b, kb);
  if (sb.IsNull())
    return extrema;
  try {
    sb = sb.Moved(TopLoc_Location(ToTrsf(xfB)));
    const TopoDS_Vertex v =
        BRepBuilderAPI_MakeVertex(gp_Pnt(fromA.x, fromA.y, fromA.z));
    BRepExtrema_DistShapeShape dist(v, sb);
    if (!dist.IsDone() || dist.NbSolution() < 1)
      return extrema;
    const gp_Pnt onB = dist.PointOnShape2(1);
    DistanceResult out;
    out.distance = extrema->distance;
    out.pointA = fromA;
    out.pointB = glm::dvec3(onB.X(), onB.Y(), onB.Z());
    return out;
  } catch (const Standard_Failure& e) {
    LOG_ERROR("Measure dimension anchors failed: {}",
              e.GetMessageString() ? e.GetMessageString()
                                   : "OpenCASCADE failure");
    return extrema;
  }
}

namespace {
// The placed OCC shape a Target resolves to: a named face or edge, or —
// when the name is empty — the bodies of the set fused into one compound,
// which is what "this whole part" means to the extrema solver.
TopoDS_Shape ResolveTarget(const Target& t) {
  if (!t.bodies)
    return {};
  if (!t.Whole()) {
    std::string kind;
    TopoDS_Shape s = ResolveShape(*t.bodies, t.ref, kind);
    if (s.IsNull())
      return {};
    return s.Moved(TopLoc_Location(ToTrsf(t.xf)));
  }
  TopoDS_Compound comp;
  BRep_Builder builder;
  builder.MakeCompound(comp);
  int added = 0;
  for (const Body& b : *t.bodies) {
    // featureId 0 = every body (an occurrence); otherwise just the one body
    // that feature created.
    if (t.ref.featureId != 0 && b.createdBy != t.ref.featureId)
      continue;
    const TopoDS_Shape& s = b.shape.GetShape();
    if (s.IsNull())
      continue;
    builder.Add(comp, s);
    added++;
  }
  if (added == 0)
    return {};
  return comp.Moved(TopLoc_Location(ToTrsf(t.xf)));
}

// The bodies a WHOLE target covers, already placed — what ContactBetween
// and the mass properties need, as opposed to the fused compound the extrema
// solver wants.
std::vector<Body> TargetBodies(const Target& t) {
  std::vector<Body> out;
  if (!t.bodies || !t.Whole())
    return out;
  for (const Body& b : *t.bodies) {
    if (t.ref.featureId != 0 && b.createdBy != t.ref.featureId)
      continue;
    if (b.shape.GetShape().IsNull())
      continue;
    Body nb = b;
    nb = PlacedBody(b, t.xf);
    out.push_back(std::move(nb));
  }
  return out;
}
} // namespace

std::optional<SetMeasure> DescribeTarget(const Target& t) {
  const std::vector<Body> placed = TargetBodies(t);
  if (placed.empty())
    return std::nullopt;
  SetMeasure out;
  glm::dvec3 lo(std::numeric_limits<double>::max());
  glm::dvec3 hi(std::numeric_limits<double>::lowest());
  for (const Body& b : placed) {
    CADShape::MassProps mp;
    if (b.shape.ComputeMassProps(mp)) {
      out.volume += mp.volume;
      out.surfaceArea += mp.surfaceArea;
    }
    out.solids += b.shape.SolidCount();
    glm::dvec3 bmin, bmax;
    b.shape.GetBoundingBox(bmin, bmax);
    lo = glm::min(lo, bmin);
    hi = glm::max(hi, bmax);
  }
  out.size = hi - lo;
  return out;
}

std::optional<DistanceResult> DistanceBetween(const Target& a, const Target& b,
                                              DistanceKind kind) {
  // Elements keep the paths that already know about them — including the
  // maximum sampler and the centre anchors, which read the B-Rep rather than
  // a fused compound.
  if (!a.Whole() && !b.Whole()) {
    return DistanceOfKind(*a.bodies, a.ref, a.xf, *b.bodies, b.ref, b.xf, kind);
  }
  const TopoDS_Shape sa = ResolveTarget(a);
  const TopoDS_Shape sb = ResolveTarget(b);
  if (sa.IsNull() || sb.IsNull())
    return std::nullopt;
  try {
    if (kind == DistanceKind::Centres) {
      // Centre of MASS for a whole shape, centroid/midpoint for an element —
      // both are "the middle of this thing", which is what the caller asked
      // for.
      const auto centre = [](const Target& t,
                             const TopoDS_Shape& s) -> std::optional<gp_Pnt> {
        if (!t.Whole()) {
          const auto p = CentreOf(*t.bodies, t.ref);
          if (!p)
            return std::nullopt;
          const glm::dvec3 w = glm::dvec3(t.xf * glm::dvec4(*p, 1.0));
          return gp_Pnt(w.x, w.y, w.z);
        }
        GProp_GProps props;
        BRepGProp::VolumeProperties(s, props);
        if (props.Mass() <= 0.0) // a sheet or a wire: fall back to area
          BRepGProp::SurfaceProperties(s, props);
        return props.CentreOfMass();
      };
      const auto ca = centre(a, sa);
      const auto cb = centre(b, sb);
      if (!ca || !cb)
        return std::nullopt;
      DistanceResult out;
      out.pointA = glm::dvec3(ca->X(), ca->Y(), ca->Z());
      out.pointB = glm::dvec3(cb->X(), cb->Y(), cb->Z());
      out.distance = glm::distance(out.pointA, out.pointB);
      return out;
    }
    if (kind == DistanceKind::Maximum) {
      // Same sampled walk DistanceOfKind uses, over whichever shapes these
      // are. Exact wherever the extreme is a vertex, which is everywhere the
      // geometry is planar or straight.
      const auto sample = [](const TopoDS_Shape& s) {
        std::vector<gp_Pnt> pts;
        for (TopExp_Explorer ex(s, TopAbs_VERTEX); ex.More(); ex.Next())
          pts.push_back(BRep_Tool::Pnt(TopoDS::Vertex(ex.Current())));
        return pts;
      };
      const std::vector<gp_Pnt> pa = sample(sa), pb = sample(sb);
      if (pa.empty() || pb.empty())
        return std::nullopt;
      DistanceResult out;
      double best = -1.0;
      for (const gp_Pnt& p : pa)
        for (const gp_Pnt& q : pb)
          if (const double d2 = p.SquareDistance(q); d2 > best) {
            best = d2;
            out.pointA = glm::dvec3(p.X(), p.Y(), p.Z());
            out.pointB = glm::dvec3(q.X(), q.Y(), q.Z());
          }
      out.distance = std::sqrt(best);
      return out;
    }
    // Body pair by body pair, through the mesh-guided refinement — the same
    // exact answer, asked of the faces that can hold it. Asking the fused
    // compounds instead was 26 s on two lofted propellers, and it is what
    // `measure` between two whole occurrences actually calls (the sweep's
    // path is ContactBetween, which was already going through it).
    const std::vector<Body> ba = TargetBodies(a), bb = TargetBodies(b);
    DistanceResult out;
    double best = std::numeric_limits<double>::infinity();
    for (const Body& x : ba)
      for (const Body& y : bb) {
        const TopoDS_Shape sx = x.shape.GetShape(), sy = y.shape.GetShape();
        if (sx.IsNull() || sy.IsNull())
          continue;
        gp_Pnt pa, pb;
        const double d = ExactDistance(x.shape, sx, glm::dmat4(1.0), y.shape,
                                       sy, glm::dmat4(1.0), &pa, &pb);
        if (d < best) {
          best = d;
          out.pointA = glm::dvec3(pa.X(), pa.Y(), pa.Z());
          out.pointB = glm::dvec3(pb.X(), pb.Y(), pb.Z());
        }
      }
    if (std::isfinite(best)) {
      out.distance = best;
      return out;
    }
    BRepExtrema_DistShapeShape dist(sa, sb, Extrema_ExtFlag_MIN,
                                    Extrema_ExtAlgo_Grad);
    if (!dist.IsDone() || dist.NbSolution() < 1)
      return std::nullopt;
    out.distance = dist.Value();
    const gp_Pnt pa = dist.PointOnShape1(1);
    const gp_Pnt pb = dist.PointOnShape2(1);
    out.pointA = glm::dvec3(pa.X(), pa.Y(), pa.Z());
    out.pointB = glm::dvec3(pb.X(), pb.Y(), pb.Z());
    return out;
  } catch (const Standard_Failure& e) {
    LOG_ERROR("Measure target distance failed: {}",
              e.GetMessageString() ? e.GetMessageString()
                                   : "OpenCASCADE failure");
    return std::nullopt;
  }
}

std::optional<glm::dvec3> NearestPointOn(const Target& t, const glm::dvec3& p,
                                         DistanceKind kind) {
  if (kind == DistanceKind::Centres) {
    // "Centre to centre" from a point means the other side's centre.
    if (!t.Whole()) {
      const auto c = CentreOf(*t.bodies, t.ref);
      if (!c)
        return std::nullopt;
      return glm::dvec3(t.xf * glm::dvec4(*c, 1.0));
    }
  }
  const TopoDS_Shape shape = ResolveTarget(t);
  if (shape.IsNull())
    return std::nullopt;
  try {
    if (kind == DistanceKind::Maximum) {
      std::optional<glm::dvec3> best;
      double farthest = -1.0;
      for (TopExp_Explorer ex(shape, TopAbs_VERTEX); ex.More(); ex.Next()) {
        const gp_Pnt q = BRep_Tool::Pnt(TopoDS::Vertex(ex.Current()));
        const glm::dvec3 v(q.X(), q.Y(), q.Z());
        const double d = glm::distance(p, v);
        if (d > farthest) {
          farthest = d;
          best = v;
        }
      }
      return best;
    }
    const TopoDS_Vertex v = BRepBuilderAPI_MakeVertex(gp_Pnt(p.x, p.y, p.z));
    BRepExtrema_DistShapeShape dist(v, shape);
    if (!dist.IsDone() || dist.NbSolution() < 1)
      return std::nullopt;
    const gp_Pnt q = dist.PointOnShape2(1);
    return glm::dvec3(q.X(), q.Y(), q.Z());
  } catch (const Standard_Failure& e) {
    LOG_ERROR("Measure nearest point failed: {}", e.GetMessageString()
                                                      ? e.GetMessageString()
                                                      : "OpenCASCADE failure");
    return std::nullopt;
  }
}

std::optional<SetContact> OverlapBetween(const Target& a, const Target& b,
                                         double apart) {
  if (!a.Whole() || !b.Whole())
    return std::nullopt; // a face has no volume to overlap with
  const std::vector<Body> pa = TargetBodies(a), pb = TargetBodies(b);
  if (pa.empty() || pb.empty())
    return std::nullopt;
  if (apart > 0.0) {
    try {
      bool nested = false;
      for (const Body& x : pa)
        for (const Body& y : pb)
          nested = nested ||
                   AnySolidInside(x.shape.GetShape(), y.shape.GetShape()) ||
                   AnySolidInside(y.shape.GetShape(), x.shape.GetShape());
      if (!nested) {
        SetContact clear;
        clear.distance = apart;
        return clear; // apart, and neither inside the other: no overlap
      }
    } catch (const Standard_Failure&) {
      // fall through to the boolean, which reports its own failure
    }
  }
  // No distance: this verb exists for the overlap VOLUME, and its callers
  // read `volume` alone. Asking for the distance as well doubled the cost of
  // every `measure` between two whole shapes — the handler had already
  // computed one through DistanceBetween.
  return ContactBetween(pa, glm::dmat4(1.0), pb, glm::dmat4(1.0), false);
}

collide::Gap MeshGap(const Target& a, const Target& b) {
  collide::Gap out;
  if (!a.Whole() || !b.Whole())
    return out;
  const std::vector<Body> ba = TargetBodies(a), bb = TargetBodies(b);
  if (ba.empty() || bb.empty())
    return out;
  // Body by body, keeping the nearest — a target can be a whole occurrence,
  // and an occurrence can be several bodies.
  for (const Body& x : ba)
    for (const Body& y : bb) {
      if (x.shape.GetShape().IsNull() || y.shape.GetShape().IsNull())
        continue;
      const collide::Gap gap = collide::Nearest(x.shape, glm::dmat4(1.0),
                                                y.shape, glm::dmat4(1.0), 2.0);
      if (!gap.known)
        continue;
      if (!out.known || gap.distance < out.distance)
        out = gap;
      out.known = true;
    }
  return out;
}

std::optional<double> Distance(const std::vector<Body>& bodies,
                               const ElementRef& a, const ElementRef& b) {
  const auto d = DistanceDetailed(bodies, a, b);
  return d ? std::optional<double>(d->distance) : std::nullopt;
}

std::optional<DistanceResult> DistanceDetailed(const std::vector<Body>& bodies,
                                               const ElementRef& a,
                                               const ElementRef& b) {
  return DistanceDetailed(bodies, a, glm::dmat4(1.0), bodies, b,
                          glm::dmat4(1.0));
}

std::optional<DistanceResult>
DistanceDetailed(const std::vector<Body>& bodiesA, const ElementRef& a,
                 const glm::dmat4& xfA, const std::vector<Body>& bodiesB,
                 const ElementRef& b, const glm::dmat4& xfB) {
  std::string ka, kb;
  TopoDS_Shape sa = ResolveShape(bodiesA, a, ka);
  TopoDS_Shape sb = ResolveShape(bodiesB, b, kb);
  if (sa.IsNull() || sb.IsNull())
    return std::nullopt;
  try {
    sa = sa.Moved(TopLoc_Location(ToTrsf(xfA)));
    sb = sb.Moved(TopLoc_Location(ToTrsf(xfB)));
    // MIN, as ExactDistance asks: the default MINMAX also searches for the
    // maxima, which nobody reads and which on two spline faces cost as much
    // again as the minimum.
    BRepExtrema_DistShapeShape dist(sa, sb, Extrema_ExtFlag_MIN,
                                    Extrema_ExtAlgo_Grad);
    if (!dist.IsDone() || dist.NbSolution() < 1)
      return std::nullopt;
    DistanceResult out;
    out.distance = dist.Value();
    const gp_Pnt pa = dist.PointOnShape1(1);
    const gp_Pnt pb = dist.PointOnShape2(1);
    out.pointA = glm::dvec3(pa.X(), pa.Y(), pa.Z());
    out.pointB = glm::dvec3(pb.X(), pb.Y(), pb.Z());
    return out;
  } catch (const std::exception& e) {
    LOG_ERROR("Measure distance failed: {}", e.what());
    return std::nullopt;
  } catch (const Standard_Failure& e) {
    // OCC throws Standard_Failure, which does NOT derive from
    // std::exception, so the catch above never sees it. On OCC 7.6
    // these calls do throw for transformed shapes, and the escape
    // reached the RPC boundary as an "unknown exception".
    LOG_ERROR("Measure distance failed: {}", e.GetMessageString()
                                                 ? e.GetMessageString()
                                                 : "OpenCASCADE failure");
    return std::nullopt;
  }
}

std::vector<ThreadSpan> ThreadSpansOf(const Body& body,
                                      const glm::dmat4& world) {
  return ThreadSpans(body, TopLoc_Location(ToTrsf(world)));
}

// The placed shape a target resolves to, for meshmeasure::Distance.
TopoDS_Shape ResolveTargetShape(const Target& t) { return ResolveTarget(t); }

} // namespace Measure

namespace meshmeasure {

namespace {

// A target as triangles in its placed frame: a mesh body's own, a B-Rep's
// tessellated (on a copy) at a thousandth of its size, which `deflection`
// reports. Empty when there is nothing.
std::shared_ptr<const mesh::TriMesh> TargetTriangles(const Measure::Target& t,
                                                     double& deflection) {
  auto out = std::make_shared<mesh::TriMesh>();
  deflection = 0.0;
  if (t.bodies == nullptr)
    return nullptr;
  const auto append = [&](const std::vector<glm::dvec3>& pos,
                          const std::vector<std::array<uint32_t, 3>>& tris) {
    const auto base = uint32_t(out->positions.size());
    out->positions.insert(out->positions.end(), pos.begin(), pos.end());
    for (const auto& tr : tris)
      out->triangles.push_back({tr[0] + base, tr[1] + base, tr[2] + base});
  };
  bool brep = false;
  for (const Body& b : *t.bodies) {
    if (b.reference || !b.IsMesh())
      continue;
    if (t.ref.featureId != 0 && b.createdBy != t.ref.featureId)
      continue;
    if (!t.Whole() && !b.elementMap.Resolve(t.ref.name))
      continue;
    const mesh::TriMesh placed =
        t.xf == glm::dmat4(1.0) ? *b.mesh : mesh::Transformed(*b.mesh, t.xf);
    append(placed.positions, placed.triangles);
  }
  // The B-Rep part of the target, if any.
  std::vector<Body> breps;
  for (const Body& b : *t.bodies)
    if (!b.IsMesh())
      breps.push_back(b);
  if (!breps.empty() && (t.Whole() || !IsMeshElement(*t.bodies, t.ref))) {
    const TopoDS_Shape s =
        Measure::ResolveTargetShape(Measure::Target{&breps, t.xf, t.ref});
    if (!s.IsNull()) {
      const CADShape shape(s);
      glm::dvec3 lo, hi;
      shape.GetBoundingBox(lo, hi);
      deflection = std::max(glm::length(hi - lo) * 1e-3, 1e-6);
      const CADShape::Mesh m = shape.GenerateMeshDetached(deflection);
      std::vector<glm::dvec3> pos;
      for (const auto& v : m.vertices)
        pos.emplace_back(v.position);
      std::vector<std::array<uint32_t, 3>> tris;
      for (size_t k = 0; k + 2 < m.indices.size(); k += 3)
        tris.push_back({m.indices[k], m.indices[k + 1], m.indices[k + 2]});
      append(pos, tris);
      brep = true;
    }
  }
  (void)brep;
  if (out->triangles.empty())
    return nullptr;
  return out;
}

} // namespace

std::optional<Gap> Distance(const Measure::Target& a, const Measure::Target& b,
                            std::string& why) {
  double da = 0.0, db = 0.0;
  const auto ma = TargetTriangles(a, da), mb = TargetTriangles(b, db);
  if (!ma || !mb) {
    why = std::string("side ") + (!ma ? "a" : "b") +
          " has no geometry to measure";
    return std::nullopt;
  }
  const mesh::Bvh ta(ma), tb(mb);
  const mesh::Bvh::Gap g = ta.Distance(tb, glm::dmat4(1.0));
  Gap out;
  out.distance = g.distance;
  out.pointA = g.onThis;
  out.pointB = g.onOther;
  out.toleranceMm = da + db;
  return out;
}

std::optional<Overlap> Interference(const std::vector<Body>& a,
                                    const std::vector<Body>& b,
                                    std::string& why) {
  for (const std::vector<Body>* side : {&a, &b})
    for (const Body& x : *side)
      if (x.IsMesh() && !x.mesh->Closed()) {
        why = "body '" + x.name +
              "' is an open mesh, which bounds no volume "
              "to overlap: close its holes in MeshLab, Blender or the "
              "scanning app";
        return std::nullopt;
      }
  const Measure::Target ta{&a, glm::dmat4(1.0), {}};
  const Measure::Target tb{&b, glm::dmat4(1.0), {}};
  double da = 0.0, db = 0.0;
  auto ma = TargetTriangles(ta, da), mb = TargetTriangles(tb, db);
  if (!ma || !mb) {
    why = "a side has no geometry";
    return std::nullopt;
  }
  Overlap out;
  out.toleranceMm = da + db;
  const mesh::Bvh tra(ma), trb(mb);
  out.distance = tra.Distance(trb, glm::dmat4(1.0)).distance;
  // Welded, so a tessellated B-Rep (one block of vertices a face) closes.
  const auto weld = [](const mesh::TriMesh& m) {
    std::vector<glm::dvec3> soup;
    soup.reserve(m.triangles.size() * 3);
    for (uint32_t t = 0; t < m.triangles.size(); t++)
      for (int c = 0; c < 3; c++)
        soup.push_back(m.Corner(t, c));
    return mesh::FromSoup(soup, mesh::Float32Quantum(m.positions));
  };
  const mesh::TriMesh wa = weld(*ma), wb = weld(*mb);
  mesh::TriMesh common;
  std::string err;
  if (!mesh::Boolean(wa, wb, mesh::BooleanOp::Intersect, common, err)) {
    why = "the overlap could not be computed: " + err;
    return std::nullopt;
  }
  out.volume = common.triangles.empty() ? 0.0 : common.Volume();
  return out;
}

} // namespace meshmeasure
