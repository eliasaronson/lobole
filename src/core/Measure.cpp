#include "core/Measure.h"

#include "Logger.h"

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
