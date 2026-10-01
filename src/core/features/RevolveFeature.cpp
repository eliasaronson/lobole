#include "core/features/RevolveFeature.h"

#include "common/Logger.h"
#include "core/BodyMode.h"
#include "core/features/SketchFeature.h"
#include "core/sketch/SketchProfile.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

RevolveFeature::RevolveFeature(const std::string& name, uint64_t sketchId,
                               double angleDeg, ExtrudeMode mode, bool reversed)
    : sketchId(sketchId), angleDeg(angleDeg), reversed(reversed), mode(mode) {
  m_name = name;
}

bool RevolveFeature::Execute(RegenContext& ctx) {
  const auto* sketch =
      dynamic_cast<const SketchFeature*>(ctx.FindById(sketchId));
  if (!sketch) {
    SetError("profile sketch (id " + std::to_string(sketchId) +
             ") not found earlier in history");
    return false;
  }

  std::vector<sketch::Loop> loops = sketch::BuildLoops(sketch->entities);
  // Single-stroke text makes no profile: refused by name when it is what was
  // picked, or all there is (docs/sketching/README.md#text).
  if (const std::string why = sketch::StrokeTextProfileRefusal(
          sketch->entities, regions, loops.empty());
      !why.empty()) {
    SetError(why);
    return false;
  }
  if (loops.empty()) {
    SetError("sketch has no closed profile");
    return false;
  }
  // Region selection (U2): keep only the chosen regions (+ their holes).
  if (!regions.empty() && !sketch::FilterLoopsToRegions(loops, regions)) {
    SetError("a selected region no longer exists in the sketch - re-pick "
             "the profiles");
    return false;
  }
  if (loops.empty()) {
    SetError("the selected regions leave no profile");
    return false;
  }

  glm::dvec3 axisOrigin{0.0};
  glm::dvec3 axisDir{1.0, 0.0, 0.0};
  if (!ResolveAxis(ctx, axis, axisOrigin, axisDir)) {
    SetError("revolve axis not found (deleted datum axis or sketch line?)");
    return false;
  }

  // The axis must lie in the sketch plane: its direction perpendicular to the
  // plane normal, and a point of it on the plane. Otherwise the revolve is a
  // skew sweep rather than a solid of revolution.
  const glm::dvec3 n = sketch->plane.GetNormal();
  const glm::dvec3 planeO = sketch->plane.GetOrigin();
  if (std::abs(glm::dot(axisDir, n)) > 1e-4 ||
      std::abs(glm::dot(axisOrigin - planeO, n)) > 1e-4) {
    SetError("revolve axis is not in the sketch plane");
    return false;
  }

  ElementMap map;
  std::string buildErr;
  CADShape solid = sketch::BuildRevolveNamed(
      sketch->plane, loops, axisOrigin, axisDir, glm::radians(angleDeg),
      reversed, symmetric, GetId(), map, &buildErr);
  if (!solid.IsValid()) {
    SetError(buildErr.empty() ? "revolve construction failed" : buildErr);
    return false;
  }

  return ApplyBodyModeReporting(ctx.bodies, std::move(solid), std::move(map),
                                mode, *this);
}
