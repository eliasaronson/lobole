#include "core/Measure.h"

#include "core/Conventions.h"

#include <glm/gtc/quaternion.hpp>

#include <Standard_Failure.hxx>

#include "common/Logger.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_Result.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeHalfSpace.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <ElCLib.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <GeomAbs_CurveType.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <GeomLProp_SLProps.hxx>
#include <Geom_Surface.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Quaternion.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
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
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(shape, TopAbs_EDGE, edges);
  if (index >= 0 && index < edges.Extent())
    return edges(index + 1);
  return {};
}

// Resolve an ElementRef to its OCC subshape (a face or edge of the owning
// body), via FaceAt/EdgeAt above. Fills `kind`. Null on failure.
TopoDS_Shape ResolveShape(const std::vector<Body>& bodies,
                          const ElementRef& ref, std::string& kind) {
  for (const Body& body : bodies) {
    if (body.createdBy != ref.featureId)
      continue;
    const TopoDS_Shape& shape = body.shape.GetShape();
    if (shape.IsNull())
      return {};
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
    return {}; // owner found but name no longer resolves
  }
  return {};
}

// A rigid glm placement (rotation + translation, as occurrence transforms
// are) as an OCC gp_Trsf. glm is column-major: m[col][row].
gp_Trsf ToTrsf(const glm::mat4& m) {
  // Built from a NORMALIZED quaternion rather than by handing OCC the float
  // matrix. SetValues infers a scale factor from the 3x3 block, and the
  // rounding in a float rotation is enough for it to infer one that is not
  // exactly 1 (0.99999998859 for 45 degrees). TopoDS_Shape::Moved() then
  // refuses to move by that location on OpenCASCADE 7.6 — 7.9 accepts the
  // identical input. An occurrence placement is rigid by construction, so
  // say so, and no version has to forgive us. Pinned by
  // "A rigid placement reaches the kernel unscaled" in test_cadkernel.cpp.
  const glm::quat q = glm::normalize(glm::quat_cast(glm::mat3(m)));
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
  glm::vec3 axis{0.0f};
  bool hasAxis = false;
};

ShapeClass ClassifyFace(const TopoDS_Face& face) {
  ShapeClass out;
  BRepAdaptor_Surface surf(face);
  const auto axisOf = [&out](const gp_Ax1& ax) {
    const gp_Dir a = ax.Direction();
    out.axis = glm::vec3(a.X(), a.Y(), a.Z());
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
    out.axis = glm::vec3(a.X(), a.Y(), a.Z());
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
  out.centroid = glm::vec3(c.X(), c.Y(), c.Z());
  if (const auto n = FaceNormal(TopoDS::Face(f), c))
    out.normal = glm::vec3(n->X(), n->Y(), n->Z());
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
  out.midpoint = glm::vec3(mid.X(), mid.Y(), mid.Z());
  const ShapeClass sc = ClassifyEdge(TopoDS::Edge(e));
  out.kind = sc.kind;
  out.radius = sc.radius;
  out.hasRadius = sc.hasRadius;
  out.axis = sc.axis;
  out.hasAxis = sc.hasAxis;
  return out;
}

// Boolean fuzz for contact checks, in millimetres. Big enough to swallow the
// sliver two coincident faces produce, far below any interference a designer
// could intend.
constexpr double kContactFuzz = 1.0e-3;

std::optional<double> InterferenceVolume(const std::vector<Body>& bodiesA,
                                         const glm::mat4& xfA,
                                         const std::vector<Body>& bodiesB,
                                         const glm::mat4& xfB) {
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
        // The two-shape constructor builds immediately, so the fuzz has to
        // go in through the arguments/tools form or it is set too late to
        // matter (it silently was, on the first attempt at this fix).
        BRepAlgoAPI_Common common;
        TopTools_ListOfShape args, tools;
        args.Append(sa);
        tools.Append(sb);
        common.SetArguments(args);
        common.SetTools(tools);
        common.SetFuzzyValue(kContactFuzz);
        common.Build();
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

bool WorldBounds(const std::vector<Body>& bodies, const glm::mat4& xf,
                 glm::vec3& min, glm::vec3& max) {
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
    min = glm::vec3(x0, y0, z0);
    max = glm::vec3(x1, y1, z1);
    return true;
  } catch (const Standard_Failure&) {
    return false;
  }
}

std::optional<SetContact> ContactBetween(const std::vector<Body>& bodiesA,
                                         const glm::mat4& xfA,
                                         const std::vector<Body>& bodiesB,
                                         const glm::mat4& xfB) {
  try {
    const TopLoc_Location la(ToTrsf(xfA)), lb(ToTrsf(xfB));
    struct Placed {
      TopoDS_Shape shape;
      Bnd_Box box;
    };
    const auto place = [](const std::vector<Body>& bodies,
                          const TopLoc_Location& loc) {
      std::vector<Placed> out;
      for (const Body& b : bodies) {
        if (b.shape.GetShape().IsNull())
          continue;
        Placed p;
        p.shape = b.shape.GetShape().Moved(loc);
        BRepBndLib::Add(p.shape, p.box);
        out.push_back(std::move(p));
      }
      return out;
    };
    const std::vector<Placed> pa = place(bodiesA, la);
    const std::vector<Placed> pb = place(bodiesB, lb);
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
        if (d > 0.0)
          continue;
        // Fuzzy, for the reason kContactFuzz documents: parts DESIGNED to
        // touch share a face, and an exact boolean over coincident faces
        // returns a sliver of contact-area x tolerance, which then reads as
        // an interference. This is the path the named-pair `interference`
        // RPC and contact_report take — InterferenceVolume above is the
        // whole-assembly sweep, and both need the same answer.
        BRepAlgoAPI_Common common;
        TopTools_ListOfShape args, tools;
        args.Append(pa[i].shape);
        tools.Append(pb[j].shape);
        common.SetArguments(args);
        common.SetTools(tools);
        common.SetFuzzyValue(kContactFuzz);
        common.Build();
        if (!common.IsDone() || common.Shape().IsNull())
          continue;
        GProp_GProps props;
        BRepGProp::VolumeProperties(common.Shape(), props);
        const double v = std::max(0.0, props.Mass());
        out.volume += v;
        if (v > 0.0) {
          // Weighted by volume, so several overlapping pairs report the
          // centre of the interference rather than the centre of the last
          // one found.
          const gp_Pnt c = props.CentreOfMass();
          weighted += v * glm::dvec3(c.X(), c.Y(), c.Z());
          BRepBndLib::Add(common.Shape(), overlapBox);
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
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
      return a.boxDist < b.boxDist;
    });
    double best = std::numeric_limits<double>::infinity();
    for (const Cand& c : cands) {
      if (c.boxDist >= best)
        break;
      BRepExtrema_DistShapeShape dist(pa[c.i].shape, pb[c.j].shape);
      if (dist.IsDone() && dist.NbSolution() >= 1)
        best = std::min(best, dist.Value());
    }
    if (!std::isfinite(best))
      return std::nullopt;
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

std::optional<SetCompare>
CompareBodySets(const std::vector<Body>& bodiesA, const glm::mat4& xfA,
                const std::vector<Body>& bodiesB, const glm::mat4& xfB,
                CADShape* missingOut, CADShape* extraOut) {
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
      op.SetArguments(args);
      op.SetTools(tools);
      op.Build();
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
                            glm::dvec3& com, glm::vec3& mn, glm::vec3& mx,
                            int& faces, int& edges) {
      CADShape::MassProps mp;
      if (s.ComputeMassProps(mp)) {
        vol = mp.volume;
        surf = mp.surfaceArea;
        com = glm::dvec3(mp.centerOfMass);
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

    const auto volumeOf = [](const TopoDS_Shape& s) {
      if (s.IsNull())
        return 0.0;
      GProp_GProps props;
      BRepGProp::VolumeProperties(s, props);
      return std::max(0.0, props.Mass());
    };
    BRepAlgoAPI_Common common(a.GetShape(), b.GetShape());
    if (common.IsDone())
      out.common = volumeOf(common.Shape());
    BRepAlgoAPI_Cut missing(a.GetShape(), b.GetShape());
    if (missing.IsDone()) {
      out.missing = volumeOf(missing.Shape());
      if (missingOut != nullptr)
        *missingOut = CADShape(missing.Shape());
    }
    BRepAlgoAPI_Cut extra(b.GetShape(), a.GetShape());
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

GeometryReport CheckGeometry(const CADShape& shape) {
  GeometryReport out;
  const TopoDS_Shape& s = shape.GetShape();
  if (s.IsNull()) {
    out.reasons.push_back("null shape");
    return out;
  }
  BRepCheck_Analyzer ana(s);
  out.valid = ana.IsValid();
  if (out.valid)
    return out;

  std::set<std::string> reasons;
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
  out.reasons.assign(reasons.begin(), reasons.end());
  return out;
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
  out.min = glm::vec3(x0, y0, z0);
  out.max = glm::vec3(x1, y1, z1);
  return out;
}

namespace {
// A planar cutting face spanning every body's bounds, centered on the section
// plane. Null if `bodies` is empty. Shared by CrossSectionArea + SectionCaps.
TopoDS_Face BuildSectionCutter(const std::vector<Body>& bodies,
                               const glm::vec3& origin, const glm::vec3& n) {
  glm::vec3 u = std::abs(n.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
  u = glm::normalize(u - n * glm::dot(u, n));
  const glm::vec3 v = glm::cross(n, u);
  glm::vec3 lo(1e30f), hi(-1e30f);
  for (const auto& b : bodies) {
    glm::vec3 bmin, bmax;
    b.shape.GetBoundingBox(bmin, bmax);
    lo = glm::min(lo, bmin);
    hi = glm::max(hi, bmax);
  }
  const float s = glm::length(hi - lo) + 1.0f;
  // Center the quad on the BODIES, projected onto the plane — not on the
  // plane's origin. Sizing it from the bodies' diagonal while centering it
  // at the origin only works for a model that sits near the origin: a part
  // built a metre out (or any assembly occurrence placed away from it) was
  // missed entirely by the cutter, so the cross-section area came back zero
  // and the section view had no cap faces. Same rule the split tool uses to
  // place its knife.
  const glm::vec3 c = (lo + hi) * 0.5f;
  const glm::vec3 center = c - n * glm::dot(c - origin, n);
  auto pt = [&](float su, float sv) {
    const glm::vec3 w = center + u * su + v * sv;
    return gp_Pnt(w.x, w.y, w.z);
  };
  BRepBuilderAPI_MakePolygon poly(pt(-s, -s), pt(s, -s), pt(s, s), pt(-s, s),
                                  Standard_True);
  return BRepBuilderAPI_MakeFace(poly.Wire()).Face();
}
} // namespace

std::optional<std::vector<SectionRegion>>
CrossSectionRegions(const std::vector<Body>& bodies, const glm::vec3& origin,
                    const glm::vec3& normal) {
  std::vector<SectionRegion> out;
  if (bodies.empty())
    return out;
  try {
    const TopoDS_Face cutter =
        BuildSectionCutter(bodies, origin, glm::normalize(normal));
    for (const auto& b : bodies) {
      if (b.shape.GetShape().IsNull())
        continue;
      BRepAlgoAPI_Common common(b.shape.GetShape(), cutter);
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
        out.push_back({props.Mass(), glm::vec3(static_cast<float>(c.X()),
                                               static_cast<float>(c.Y()),
                                               static_cast<float>(c.Z()))});
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

std::optional<double> CrossSectionArea(const std::vector<Body>& bodies,
                                       const glm::vec3& origin,
                                       const glm::vec3& normal) {
  if (bodies.empty())
    return 0.0;
  try {
    const TopoDS_Face cutter =
        BuildSectionCutter(bodies, origin, glm::normalize(normal));
    double area = 0.0;
    for (const auto& b : bodies) {
      if (b.shape.GetShape().IsNull())
        continue;
      BRepAlgoAPI_Common common(b.shape.GetShape(), cutter);
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

std::string CapKey(const Body& b, const glm::vec3& origin,
                   const glm::vec3& normal,
                   const std::vector<glm::vec4>& trimPlanes) {
  std::string key = std::to_string(b.shape.PlacementId());
  key += '|';
  for (int i = 0; i < 3; i++)
    AppendQuantized(key, origin[i]);
  for (int i = 0; i < 3; i++)
    AppendQuantized(key, normal[i]);
  key += '|';
  for (const glm::vec4& t : trimPlanes)
    for (int i = 0; i < 4; i++)
      AppendQuantized(key, t[i]);
  return key;
}

// Big enough that a scrub of a large assembly hits all the way through (the
// engine is 27 parts x one plane), small enough that it cannot grow without
// bound over a session of moving cuts about.
constexpr size_t kMaxCapCache = 512;
} // namespace

std::vector<CADShape> SectionCaps(const std::vector<Body>& bodies,
                                  const glm::vec3& origin,
                                  const glm::vec3& normal,
                                  const std::vector<glm::vec4>& trimPlanes,
                                  SectionCapCache* cache) {
  std::vector<CADShape> caps;
  if (bodies.empty())
    return caps;
  if (cache != nullptr && cache->entries.size() > kMaxCapCache)
    cache->Clear(); // wholesale, never one at a time — see the header
  try {
    const TopoDS_Face cutter =
        BuildSectionCutter(bodies, origin, glm::normalize(normal));
    for (const auto& b : bodies) {
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
      BRepAlgoAPI_Common common(b.shape.GetShape(), cutter);
      // A body that doesn't meet the plane yields an empty compound (not
      // null); keep a cap only when it has real area, so callers can test
      // IsValid() to mean "there is a cut face here".
      if (common.IsDone() && !common.Shape().IsNull()) {
        // Cut the cap back to the kept side of every other section. Each is
        // a half-space solid built from the same oversized quad the cutter
        // uses, with its reference point on the DISCARDED side.
        TopoDS_Shape capShape = common.Shape();
        for (const glm::vec4& t : trimPlanes) {
          const glm::vec3 tn = glm::vec3(t);
          if (glm::length(tn) < 1e-6f)
            continue;
          const glm::vec3 tnu = glm::normalize(tn);
          const glm::vec3 tOrigin = tnu * t.w;
          const TopoDS_Face trimFace = BuildSectionCutter(bodies, tOrigin, tnu);
          // A point well past the plane on the removed side. The distance is
          // the model's own size, so it scales with the part.
          glm::vec3 lo(1e30f), hi(-1e30f);
          for (const auto& bb : bodies) {
            glm::vec3 bmin, bmax;
            bb.shape.GetBoundingBox(bmin, bmax);
            lo = glm::min(lo, bmin);
            hi = glm::max(hi, bmax);
          }
          const glm::vec3 ref = tOrigin + tnu * (glm::length(hi - lo) + 1.0f);
          BRepPrimAPI_MakeHalfSpace halfSpace(trimFace,
                                              gp_Pnt(ref.x, ref.y, ref.z));
          if (!halfSpace.IsDone())
            continue;
          BRepAlgoAPI_Cut trimmed(capShape, halfSpace.Solid());
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
  return Angle(bodies, a, glm::mat4(1.0f), bodies, b, glm::mat4(1.0f));
}

std::optional<AngleResult> Angle(const std::vector<Body>& bodiesA,
                                 const ElementRef& a, const glm::mat4& xfA,
                                 const std::vector<Body>& bodiesB,
                                 const ElementRef& b, const glm::mat4& xfB) {
  const auto da = DirectionOf(bodiesA, a);
  const auto db = DirectionOf(bodiesB, b);
  if (!da || !db)
    return std::nullopt;
  // Occurrence placements are rigid, so only the rotation touches a
  // direction — a translation cannot change an angle.
  const auto placed = [](const glm::dvec3& d, const glm::mat4& xf) {
    return glm::normalize(glm::dmat3(xf) * d);
  };
  const glm::dvec3 va = placed(da->dir, xfA);
  const glm::dvec3 vb = placed(db->dir, xfB);
  if (!std::isfinite(va.x) || !std::isfinite(vb.x))
    return std::nullopt;

  AngleResult out;
  const double dot = std::clamp(glm::dot(va, vb), -1.0, 1.0);
  const double deg = std::acos(dot) * 180.0 / M_PI;
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

std::optional<glm::vec3> AnchorOf(const std::vector<Body>& bodies,
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
      return glm::vec3(c.X(), c.Y(), c.Z());
    }
    BRepAdaptor_Curve curve(TopoDS::Edge(shape));
    const gp_Pnt mid =
        curve.Value(0.5 * (curve.FirstParameter() + curve.LastParameter()));
    return glm::vec3(mid.X(), mid.Y(), mid.Z());
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
               const glm::mat4& xfA, const std::vector<Body>& bodiesB,
               const ElementRef& b, const glm::mat4& xfB, DistanceKind kind) {
  if (kind == DistanceKind::Minimum)
    return DistanceDetailed(bodiesA, a, xfA, bodiesB, b, xfB);

  if (kind == DistanceKind::Centres) {
    const auto ca = CentreOf(bodiesA, a);
    const auto cb = CentreOf(bodiesB, b);
    if (!ca || !cb)
      return std::nullopt;
    DistanceResult out;
    out.pointA = glm::vec3(xfA * glm::vec4(*ca, 1.0f));
    out.pointB = glm::vec3(xfB * glm::vec4(*cb, 1.0f));
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
          out.pointA = glm::vec3(p.X(), p.Y(), p.Z());
          out.pointB = glm::vec3(q.X(), q.Y(), q.Z());
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

std::optional<glm::vec3> CornerOf(const std::vector<Body>& bodies,
                                  const ElementRef& edgeA,
                                  const ElementRef& edgeB) {
  for (const Body& body : bodies) {
    if (body.createdBy != edgeA.featureId || body.shape.GetShape().IsNull())
      continue;
    const auto a = body.elementMap.ResolveEdge(edgeA.name);
    const auto b = body.elementMap.ResolveEdge(edgeB.name);
    if (!a || !b)
      return std::nullopt;
    glm::vec3 p;
    if (!body.shape.CornerPoint(*a, *b, p))
      return std::nullopt;
    return p;
  }
  return std::nullopt;
}

std::optional<glm::vec3> CentreOf(const std::vector<Body>& bodies,
                                  const ElementRef& ref) {
  std::string kind;
  const TopoDS_Shape shape = ResolveShape(bodies, ref, kind);
  if (shape.IsNull())
    return std::nullopt;
  const auto pt = [](const gp_Pnt& p) {
    return std::optional<glm::vec3>(glm::vec3(p.X(), p.Y(), p.Z()));
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

std::optional<DistanceResult>
DimensionAnchors(const std::vector<Body>& bodiesA, const ElementRef& a,
                 const glm::mat4& xfA, const std::vector<Body>& bodiesB,
                 const ElementRef& b, const glm::mat4& xfB, bool parallel) {
  const auto extrema = DistanceDetailed(bodiesA, a, xfA, bodiesB, b, xfB);
  if (!parallel || !extrema)
    return extrema;
  // Parallel: re-anchor on A's own centre. The distance is unchanged (that
  // is what parallel MEANS), so keep the extrema value and move only where
  // the dimension is drawn.
  const auto anchor = AnchorOf(bodiesA, a);
  if (!anchor)
    return extrema;
  const glm::vec3 fromA = glm::vec3(xfA * glm::vec4(*anchor, 1.0f));
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
    out.pointB = glm::vec3(onB.X(), onB.Y(), onB.Z());
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
    nb.shape = b.shape.Transformed(t.xf);
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
  glm::vec3 lo(std::numeric_limits<float>::max());
  glm::vec3 hi(std::numeric_limits<float>::lowest());
  for (const Body& b : placed) {
    CADShape::MassProps mp;
    if (b.shape.ComputeMassProps(mp)) {
      out.volume += mp.volume;
      out.surfaceArea += mp.surfaceArea;
    }
    out.solids += b.shape.SolidCount();
    glm::vec3 bmin, bmax;
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
          const glm::vec3 w = glm::vec3(t.xf * glm::vec4(*p, 1.0f));
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
      out.pointA = glm::vec3(ca->X(), ca->Y(), ca->Z());
      out.pointB = glm::vec3(cb->X(), cb->Y(), cb->Z());
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
            out.pointA = glm::vec3(p.X(), p.Y(), p.Z());
            out.pointB = glm::vec3(q.X(), q.Y(), q.Z());
          }
      out.distance = std::sqrt(best);
      return out;
    }
    BRepExtrema_DistShapeShape dist(sa, sb);
    if (!dist.IsDone() || dist.NbSolution() < 1)
      return std::nullopt;
    DistanceResult out;
    out.distance = dist.Value();
    const gp_Pnt pa = dist.PointOnShape1(1);
    const gp_Pnt pb = dist.PointOnShape2(1);
    out.pointA = glm::vec3(pa.X(), pa.Y(), pa.Z());
    out.pointB = glm::vec3(pb.X(), pb.Y(), pb.Z());
    return out;
  } catch (const Standard_Failure& e) {
    LOG_ERROR("Measure target distance failed: {}",
              e.GetMessageString() ? e.GetMessageString()
                                   : "OpenCASCADE failure");
    return std::nullopt;
  }
}

std::optional<glm::vec3> NearestPointOn(const Target& t, const glm::vec3& p,
                                        DistanceKind kind) {
  if (kind == DistanceKind::Centres) {
    // "Centre to centre" from a point means the other side's centre.
    if (!t.Whole()) {
      const auto c = CentreOf(*t.bodies, t.ref);
      if (!c)
        return std::nullopt;
      return glm::vec3(t.xf * glm::vec4(*c, 1.0f));
    }
  }
  const TopoDS_Shape shape = ResolveTarget(t);
  if (shape.IsNull())
    return std::nullopt;
  try {
    if (kind == DistanceKind::Maximum) {
      std::optional<glm::vec3> best;
      double far = -1.0;
      for (TopExp_Explorer ex(shape, TopAbs_VERTEX); ex.More(); ex.Next()) {
        const gp_Pnt q = BRep_Tool::Pnt(TopoDS::Vertex(ex.Current()));
        const glm::vec3 v(q.X(), q.Y(), q.Z());
        const double d = glm::distance(p, v);
        if (d > far) {
          far = d;
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
    return glm::vec3(q.X(), q.Y(), q.Z());
  } catch (const Standard_Failure& e) {
    LOG_ERROR("Measure nearest point failed: {}", e.GetMessageString()
                                                      ? e.GetMessageString()
                                                      : "OpenCASCADE failure");
    return std::nullopt;
  }
}

std::optional<SetContact> OverlapBetween(const Target& a, const Target& b) {
  if (!a.Whole() || !b.Whole())
    return std::nullopt; // a face has no volume to overlap with
  const std::vector<Body> pa = TargetBodies(a), pb = TargetBodies(b);
  if (pa.empty() || pb.empty())
    return std::nullopt;
  return ContactBetween(pa, glm::mat4(1.0f), pb, glm::mat4(1.0f));
}

std::optional<double> Distance(const std::vector<Body>& bodies,
                               const ElementRef& a, const ElementRef& b) {
  const auto d = DistanceDetailed(bodies, a, b);
  return d ? std::optional<double>(d->distance) : std::nullopt;
}

std::optional<DistanceResult> DistanceDetailed(const std::vector<Body>& bodies,
                                               const ElementRef& a,
                                               const ElementRef& b) {
  return DistanceDetailed(bodies, a, glm::mat4(1.0f), bodies, b,
                          glm::mat4(1.0f));
}

std::optional<DistanceResult>
DistanceDetailed(const std::vector<Body>& bodiesA, const ElementRef& a,
                 const glm::mat4& xfA, const std::vector<Body>& bodiesB,
                 const ElementRef& b, const glm::mat4& xfB) {
  std::string ka, kb;
  TopoDS_Shape sa = ResolveShape(bodiesA, a, ka);
  TopoDS_Shape sb = ResolveShape(bodiesB, b, kb);
  if (sa.IsNull() || sb.IsNull())
    return std::nullopt;
  try {
    sa = sa.Moved(TopLoc_Location(ToTrsf(xfA)));
    sb = sb.Moved(TopLoc_Location(ToTrsf(xfB)));
    BRepExtrema_DistShapeShape dist(sa, sb);
    if (!dist.IsDone() || dist.NbSolution() < 1)
      return std::nullopt;
    DistanceResult out;
    out.distance = dist.Value();
    const gp_Pnt pa = dist.PointOnShape1(1);
    const gp_Pnt pb = dist.PointOnShape2(1);
    out.pointA = glm::vec3(pa.X(), pa.Y(), pa.Z());
    out.pointB = glm::vec3(pb.X(), pb.Y(), pb.Z());
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

} // namespace Measure
