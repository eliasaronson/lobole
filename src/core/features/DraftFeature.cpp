#include "core/features/DraftFeature.h"

#include "common/Logger.h"
#include "core/ElementHistory.h"

#include <BRepBndLib.hxx>
#include <BRepOffsetAPI_DraftAngle.hxx>
#include <Bnd_Box.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>

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
  case PlaneType::XY:
    return {0, 0, 1};
  case PlaneType::XZ:
    return {0, 1, 0};
  case PlaneType::YZ:
    return {1, 0, 0};
  }
  return {0, 0, 1};
}
} // namespace

bool DraftFeature::Execute(RegenContext& ctx) {
  const auto body =
      std::find_if(ctx.bodies.begin(), ctx.bodies.end(),
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
  const double volumeBefore = body->shape.Volume();
  // How far the taper actually travels: the face's extent along the pull axis
  // times tan(angle). This is the number that explains a surprising result —
  // 25 degrees sounds modest until you notice it moves the face nearly a
  // millimetre on a 2 mm wall.
  double maxTaper = 0.0;

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
      {
        // The face's span along the pull axis, from its own bounding box.
        Bnd_Box fbox;
        BRepBndLib::Add(ex.Current(), fbox);
        if (!fbox.IsVoid()) {
          double xmin, ymin, zmin, xmax, ymax, zmax;
          fbox.Get(xmin, ymin, zmin, xmax, ymax, zmax);
          const glm::vec3 lo(xmin, ymin, zmin), hi(xmax, ymax, zmax);
          const double span = std::abs(glm::dot(hi - lo, n));
          maxTaper = std::max(maxTaper, span * std::tan(angleRad));
        }
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
    // OCC can report success and hand back the input solid (the same trap
    // ShellFeature hit): tapering a face always moves material, so an
    // unchanged volume means the draft did not happen.
    if (result.IsValid() &&
        std::abs(result.Volume() - body->shape.Volume()) <
            1e-9 * std::max(1.0, std::abs(body->shape.Volume()))) {
      SetError("draft had no effect: the angle is too small, or these faces "
               "cannot tilt about this neutral plane");
      return false;
    }
    if (!result.IsValid() || result.Volume() < 1e-9) {
      SetError("draft produced an invalid solid");
      return false;
    }
    // A draft that reshapes a large part of the body is valid geometry and
    // almost always a mistake: an angle that sounds modest travels a long way
    // on a tall face, and on a shelled part the taper can cut clean through
    // the wall into the cavity. Advise, with the two numbers that explain it,
    // and let the model stand. (A 2 mm box with 0.25 mm walls drafted 25 deg
    // loses 46% of its volume — the case that prompted this.)
    const double volumeAfter = result.Volume();
    const double changed =
        volumeBefore > 1e-12
            ? std::abs(volumeAfter - volumeBefore) / volumeBefore
            : 0.0;
    if (changed > 0.25) {
      char msg[224];
      std::snprintf(msg, sizeof(msg),
                    "%.3g deg tapers the face by %.3g over its height and "
                    "changes the body by %.0f%% — check the angle and the "
                    "neutral plane",
                    angleDeg, maxTaper, changed * 100.0);
      SetWarning(msg);
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
