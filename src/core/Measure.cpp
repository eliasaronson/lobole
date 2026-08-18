#include "core/Measure.h"

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
#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_Result.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
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
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
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
  gp_Trsf t;
  t.SetValues(m[0][0], m[1][0], m[2][0], m[3][0], //
              m[0][1], m[1][1], m[2][1], m[3][1], //
              m[0][2], m[1][2], m[2][2], m[3][2]);
  return t;
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
  // Outward surface normal at a representative point. Project the centroid onto
  // the surface for its (u,v) — falls back to the UV-bounds midpoint if the
  // projection can't converge — then take the geometric normal there, flipping
  // it when the face is reversed so it points OUT of the solid.
  {
    const TopoDS_Face face = TopoDS::Face(f);
    Standard_Real u0, u1, v0, v1;
    BRepTools::UVBounds(face, u0, u1, v0, v1);
    Standard_Real u = 0.5 * (u0 + u1), v = 0.5 * (v0 + v1);
    const Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
    if (!surf.IsNull()) {
      GeomAPI_ProjectPointOnSurf proj(c, surf, u0, u1, v0, v1);
      if (proj.IsDone() && proj.NbPoints() > 0)
        proj.LowerDistanceParameters(u, v);
      GeomLProp_SLProps sl(surf, u, v, 1, 1e-7);
      if (sl.IsNormalDefined()) {
        gp_Dir n = sl.Normal();
        if (face.Orientation() == TopAbs_REVERSED)
          n.Reverse();
        out.normal = glm::vec3(n.X(), n.Y(), n.Z());
      }
    }
  }
  switch (BRepAdaptor_Surface(TopoDS::Face(f)).GetType()) {
  case GeomAbs_Plane:
    out.kind = "plane";
    break;
  case GeomAbs_Cylinder:
    out.kind = "cylinder";
    break;
  case GeomAbs_Cone:
    out.kind = "cone";
    break;
  case GeomAbs_Sphere:
    out.kind = "sphere";
    break;
  default:
    out.kind = "other";
    break;
  }
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
    break;
  }
  default:
    out.kind = "other";
    break;
  }
  return out;
}

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
        BRepAlgoAPI_Common common(sa, sb);
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
        BRepAlgoAPI_Common common(pa[i].shape, pb[j].shape);
        if (!common.IsDone() || common.Shape().IsNull())
          continue;
        GProp_GProps props;
        BRepGProp::VolumeProperties(common.Shape(), props);
        out.volume += std::max(0.0, props.Mass());
      }
    if (out.volume > 0.0)
      return out; // overlapping sets: separation is 0 by definition

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
  if (kind == "face") {
    BRepGProp::SurfaceProperties(shape, props);
    out.area = props.Mass();
  } else {
    BRepGProp::LinearProperties(shape, props);
    out.length = props.Mass();
    // A circular or arc edge also reports its radius — the top measure use
    // (hole/fillet sizing). Straight edges leave hasRadius false.
    BRepAdaptor_Curve curve(TopoDS::Edge(shape));
    const GeomAbs_CurveType type = curve.GetType();
    if (type == GeomAbs_Circle) {
      out.radius = curve.Circle().Radius();
      out.hasRadius = true;
    }
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
  auto pt = [&](float su, float sv) {
    const glm::vec3 w = origin + u * su + v * sv;
    return gp_Pnt(w.x, w.y, w.z);
  };
  BRepBuilderAPI_MakePolygon poly(pt(-s, -s), pt(s, -s), pt(s, s), pt(-s, s),
                                  Standard_True);
  return BRepBuilderAPI_MakeFace(poly.Wire()).Face();
}
} // namespace

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

std::vector<CADShape> SectionCaps(const std::vector<Body>& bodies,
                                  const glm::vec3& origin,
                                  const glm::vec3& normal) {
  std::vector<CADShape> caps;
  if (bodies.empty())
    return caps;
  try {
    const TopoDS_Face cutter =
        BuildSectionCutter(bodies, origin, glm::normalize(normal));
    for (const auto& b : bodies) {
      if (b.shape.GetShape().IsNull()) {
        caps.emplace_back(); // keep alignment with bodies[]
        continue;
      }
      BRepAlgoAPI_Common common(b.shape.GetShape(), cutter);
      // A body that doesn't meet the plane yields an empty compound (not
      // null); keep a cap only when it has real area, so callers can test
      // IsValid() to mean "there is a cut face here".
      if (common.IsDone() && !common.Shape().IsNull()) {
        GProp_GProps props;
        BRepGProp::SurfaceProperties(common.Shape(), props);
        if (props.Mass() > 1e-9) {
          caps.emplace_back(common.Shape());
          continue;
        }
      }
      caps.emplace_back();
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
