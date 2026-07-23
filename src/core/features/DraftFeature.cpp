#include "core/features/DraftFeature.h"

#include "common/Logger.h"
#include "core/ElementHistory.h"

#include <BRepOffsetAPI_DraftAngle.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <Standard_Failure.hxx>

#include <algorithm>
#include <cmath>

DraftFeature::DraftFeature(const std::string& name, uint64_t bodyId,
                           std::vector<ElementRef> faces, double angleDeg,
                           PlaneType neutralPlane)
    : bodyId(bodyId), faces(std::move(faces)), angleDeg(angleDeg),
      neutralPlane(neutralPlane) {
  m_name = name;
}

namespace {
glm::vec3 PlaneNormal(PlaneType t) {
  switch (t) {
  case PlaneType::XY: return {0, 0, 1};
  case PlaneType::XZ: return {0, 1, 0};
  case PlaneType::YZ: return {1, 0, 0};
  }
  return {0, 0, 1};
}
} // namespace

bool DraftFeature::Execute(RegenContext& ctx) {
  const auto body = std::find_if(
      ctx.bodies.begin(), ctx.bodies.end(),
      [&](const Body& b) { return b.createdBy == bodyId; });
  if (body == ctx.bodies.end()) {
    SetError("the body to draft no longer exists");
    return false;
  }
  if (faces.empty()) {
    return true; // previewing: no faces picked yet
  }
  for (const ElementRef& ref : faces) {
    if (ref.featureId != bodyId) {
      SetError("all drafted faces must belong to the same body");
      return false;
    }
  }

  // Resolve face names -> current indices -> TopoDS_Face (TopExp_Explorer
  // 0-based order, the ElementMap convention).
  std::vector<int> idx;
  for (const ElementRef& ref : faces) {
    const auto i = body->elementMap.Resolve(ref.name);
    if (!i) {
      SetError("drafted face '" + ref.name + "' no longer exists");
      return false;
    }
    idx.push_back(*i);
  }

  const TopoDS_Shape& shape = body->shape.GetShape();
  const glm::vec3 n = PlaneNormal(neutralPlane);
  const gp_Dir pull(n.x, n.y, n.z);
  // Neutral plane at the body's bbox min along the pull axis: the base stays
  // put and faces taper away from it.
  glm::vec3 bmin, bmax;
  body->shape.GetBoundingBox(bmin, bmax);
  const glm::vec3 base = n * glm::dot(bmin, n); // point on the pull axis
  const gp_Pln neutral(gp_Pnt(base.x, base.y, base.z), pull);
  const double angleRad = angleDeg * M_PI / 180.0;

  try {
    BRepOffsetAPI_DraftAngle draft(shape);
    int applied = 0;
    int i = 0;
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next(), i++) {
      if (std::find(idx.begin(), idx.end(), i) == idx.end())
        continue;
      // A face perpendicular to the pull (normal parallel to it) can't be
      // tapered — DraftAngle silently no-ops it, so reject it up front.
      glm::vec3 fc, fn, fu;
      if (body->shape.FacePlane(i, fc, fn, fu) &&
          std::abs(glm::dot(glm::normalize(fn), n)) > 0.99) {
        SetError("a drafted face is perpendicular to the pull direction "
                 "(pick a side face, or a different neutral plane)");
        return false;
      }
      draft.Add(TopoDS::Face(ex.Current()), pull, angleRad, neutral);
      if (!draft.AddDone()) {
        SetError("draft rejected a face (not tiltable about the neutral "
                 "plane?)");
        return false;
      }
      applied++;
    }
    if (applied == 0) {
      SetError("no drafted faces resolved");
      return false;
    }
    draft.Build();
    if (!draft.IsDone()) {
      SetError("draft failed (angle too large for the geometry?)");
      return false;
    }
    CADShape result(draft.Shape());
    if (!result.IsValid() || result.Volume() < 1e-9) {
      SetError("draft produced an invalid solid");
      return false;
    }
    ElementMap map;
    topo::NamePrimitiveElements(result, std::to_string(GetId()), map);
    body->shape = std::move(result);
    body->elementMap = std::move(map);
    return true;
  } catch (const Standard_Failure& e) {
    SetError(std::string("draft failed: ") + e.GetMessageString());
    LOG_ERROR("DraftFeature '{}': {}", m_name, GetError());
    return false;
  }
}
