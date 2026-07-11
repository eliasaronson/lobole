#include "core/RevolveFeature.h"

#include "Logger.h"
#include "core/BodyMode.h"
#include "core/SketchFeature.h"
#include "core/SketchProfile.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

RevolveFeature::RevolveFeature(const std::string& name, uint64_t sketchId,
                               double angleDeg, ExtrudeMode mode, bool reversed)
    : sketchId(sketchId),
      angleDeg(angleDeg),
      reversed(reversed),
      mode(mode) {
  m_name = name;
}

bool RevolveFeature::Execute(RegenContext& ctx) {
  const auto* sketch =
      dynamic_cast<const SketchFeature*>(ctx.FindById(sketchId));
  if (!sketch) {
    LOG_ERROR("RevolveFeature '{}': sketch id {} not found earlier in history",
              m_name, sketchId);
    return false;
  }

  std::vector<sketch::Loop> loops = sketch::BuildLoops(sketch->entities);
  if (loops.empty()) {
    LOG_ERROR("RevolveFeature '{}': sketch has no closed profile", m_name);
    return false;
  }

  glm::vec3 axisOrigin{0.0f};
  glm::vec3 axisDir{1.0f, 0.0f, 0.0f};
  if (!ResolveAxis(ctx, axis, axisOrigin, axisDir)) {
    LOG_ERROR("RevolveFeature '{}': axis not found", m_name);
    return false;
  }

  // The axis must lie in the sketch plane: its direction perpendicular to the
  // plane normal, and a point of it on the plane. Otherwise the revolve is a
  // skew sweep rather than a solid of revolution.
  const glm::vec3 n = sketch->plane.GetNormal();
  const glm::vec3 planeO = sketch->plane.GetOrigin();
  if (std::abs(glm::dot(axisDir, n)) > 1e-4f ||
      std::abs(glm::dot(axisOrigin - planeO, n)) > 1e-4f) {
    LOG_ERROR("RevolveFeature '{}': axis is not in the sketch plane", m_name);
    return false;
  }

  ElementMap map;
  CADShape solid = sketch::BuildRevolveNamed(sketch->plane, loops, axisOrigin,
                                             axisDir, glm::radians(angleDeg),
                                             reversed, symmetric, GetId(), map);
  if (!solid.IsValid()) {
    LOG_ERROR("RevolveFeature '{}': revolve construction failed", m_name);
    return false;
  }

  return ApplyBodyMode(ctx.bodies, std::move(solid), std::move(map), mode,
                       m_name, m_material, GetId());
}
