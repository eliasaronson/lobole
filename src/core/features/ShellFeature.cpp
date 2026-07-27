#include "core/features/ShellFeature.h"

#include "common/Logger.h"
#include "core/ElementHistory.h"

#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <algorithm>

ShellFeature::ShellFeature(const std::string& name, uint64_t bodyId,
                           std::vector<ElementRef> openFaces, double thickness)
    : bodyId(bodyId), openFaces(std::move(openFaces)), thickness(thickness) {
  m_name = name;
}

bool ShellFeature::Execute(RegenContext& ctx) {
  const auto body =
      std::find_if(ctx.bodies.begin(), ctx.bodies.end(),
                   [&](const Body& b) { return b.createdBy == bodyId; });
  if (body == ctx.bodies.end()) {
    SetError("the body to shell no longer exists");
    return false;
  }
  if (thickness <= 1e-9) {
    SetError("shell thickness must be positive");
    return false;
  }
  for (const ElementRef& ref : openFaces) {
    if (ref.featureId != bodyId) {
      SetError("all open faces must belong to the shelled body");
      return false;
    }
  }

  const TopoDS_Shape& shape = body->shape.GetShape();

  // Collect the faces to remove by their stable names -> current indices ->
  // TopoDS_Face (TopExp_Explorer 0-based order, the ElementMap convention).
  std::vector<int> removeIdx;
  for (const ElementRef& ref : openFaces) {
    const auto idx = body->elementMap.Resolve(ref.name);
    if (!idx) {
      SetError("open face '" + ref.name + "' no longer exists");
      return false;
    }
    removeIdx.push_back(*idx);
  }
  // Shell needs at least one open face to hollow through (SolidWorks/Onshape
  // semantics). A sealed hollow shell is a v2 (a manual offset+cut).
  if (removeIdx.empty()) {
    SetError("shell needs at least one open face to remove");
    return false;
  }
  TopTools_ListOfShape facesToRemove;
  int i = 0;
  for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next(), i++) {
    if (std::find(removeIdx.begin(), removeIdx.end(), i) != removeIdx.end())
      facesToRemove.Append(ex.Current());
  }

  try {
    // Negative offset hollows inward, leaving a wall of `thickness`; the
    // selected faces are removed (open shell).
    BRepOffsetAPI_MakeThickSolid maker;
    maker.MakeThickSolidByJoin(shape, facesToRemove, -thickness, 1.0e-3);
    if (!maker.IsDone()) {
      SetError("shell failed (wall too thick for the geometry?)");
      return false;
    }
    CADShape result(maker.Shape());
    if (!result.IsValid() || result.Volume() < 1e-9) {
      SetError("shell produced an invalid solid");
      return false;
    }
    // Topology is reorganized; give the result fresh primitive-style names so
    // it stays referenceable (a later fillet picks from these).
    ElementMap map;
    topo::NamePrimitiveElements(result, std::to_string(GetId()), map);
    body->shape = std::move(result);
    body->elementMap = std::move(map);
    return true;
  } catch (const Standard_Failure& e) {
    SetError(std::string("shell failed: ") + e.GetMessageString());
    LOG_ERROR("ShellFeature '{}': {}", m_name, GetError());
    return false;
  }
}
