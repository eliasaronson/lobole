#include "core/Measure.h"

#include "Logger.h"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

namespace Measure {

namespace {

// Resolve an ElementRef to its OCC subshape (a face or edge of the owning
// body). Faces are TopExp_Explorer 0-based order, edges are MapShapes 0-based
// — the same conventions ElementMap keys on. Fills `kind`. Null on failure.
TopoDS_Shape ResolveShape(const std::vector<Body>& bodies,
                          const ElementRef& ref, std::string& kind) {
  for (const Body& body : bodies) {
    if (body.createdBy != ref.featureId)
      continue;
    const TopoDS_Shape& shape = body.shape.GetShape();
    if (shape.IsNull())
      return {};
    if (const auto face = body.elementMap.Resolve(ref.name)) {
      int i = 0;
      for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next(), i++) {
        if (i == *face) {
          kind = "face";
          return ex.Current();
        }
      }
    }
    if (const auto edge = body.elementMap.ResolveEdge(ref.name)) {
      TopTools_IndexedMapOfShape edges;
      TopExp::MapShapes(shape, TopAbs_EDGE, edges);
      if (*edge >= 0 && *edge < edges.Extent()) {
        kind = "edge";
        return edges(*edge + 1);
      }
    }
    return {}; // owner found but name no longer resolves
  }
  return {};
}

} // namespace

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
  }
  return out;
}

std::optional<double> CrossSectionArea(const std::vector<Body>& bodies,
                                       const glm::vec3& origin,
                                       const glm::vec3& normal) {
  if (bodies.empty())
    return 0.0;
  const glm::vec3 n = glm::normalize(normal);
  // In-plane axes.
  glm::vec3 u = std::abs(n.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
  u = glm::normalize(u - n * glm::dot(u, n));
  const glm::vec3 v = glm::cross(n, u);

  // A cutting face big enough to span every body.
  glm::vec3 lo(1e30f), hi(-1e30f);
  for (const auto& b : bodies) {
    glm::vec3 bmin, bmax;
    b.shape.GetBoundingBox(bmin, bmax);
    lo = glm::min(lo, bmin);
    hi = glm::max(hi, bmax);
  }
  const float s = glm::length(hi - lo) + 1.0f;
  const glm::vec3 c = origin; // a point on the plane
  auto pt = [&](float su, float sv) {
    const glm::vec3 w = c + u * su + v * sv;
    return gp_Pnt(w.x, w.y, w.z);
  };
  try {
    BRepBuilderAPI_MakePolygon poly(pt(-s, -s), pt(s, -s), pt(s, s),
                                    pt(-s, s), Standard_True);
    TopoDS_Face cutter = BRepBuilderAPI_MakeFace(poly.Wire()).Face();

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
  }
}

std::optional<double> Distance(const std::vector<Body>& bodies,
                               const ElementRef& a, const ElementRef& b) {
  std::string ka, kb;
  const TopoDS_Shape sa = ResolveShape(bodies, a, ka);
  const TopoDS_Shape sb = ResolveShape(bodies, b, kb);
  if (sa.IsNull() || sb.IsNull())
    return std::nullopt;
  try {
    BRepExtrema_DistShapeShape dist(sa, sb);
    if (!dist.IsDone())
      return std::nullopt;
    return dist.Value();
  } catch (const std::exception& e) {
    LOG_ERROR("Measure distance failed: {}", e.what());
    return std::nullopt;
  }
}

} // namespace Measure
