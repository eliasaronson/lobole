#include "core/features/PatternFeature.h"

#include "Logger.h"
#include "core/BodyMode.h"
#include "core/features/DatumPlaneFeature.h" // ResolveParentPlane
#include "core/Expression.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

PatternFeature::PatternFeature(const std::string& name, PatternKind kind,
                               uint64_t sourceId)
    : kind(kind), sourceId(sourceId) {
  m_name = name;
}

bool PatternFeature::Execute(RegenContext& ctx) {
  // Collect the seed body/bodies produced by the referenced feature.
  std::vector<CADShape> instances;
  for (const Body& b : ctx.bodies) {
    if (b.createdBy == sourceId) {
      instances.push_back(b.shape); // copy: source stays until we replace it
    }
  }
  if (instances.empty()) {
    LOG_ERROR("PatternFeature '{}': source body (feature {}) not found", m_name,
              sourceId);
    return false;
  }
  const size_t seedCount = instances.size();

  if (kind == PatternKind::Mirror) {
    const auto plane = ResolveParentPlane(ctx, planeId, basePlane);
    if (!plane) {
      LOG_ERROR("PatternFeature '{}': mirror plane not found", m_name);
      return false;
    }
    const glm::vec3 o = plane->GetOrigin();
    const glm::vec3 n = plane->GetNormal();
    for (size_t i = 0; i < seedCount; i++) {
      CADShape reflected = instances[i];
      reflected.Mirror(o, n);
      instances.push_back(std::move(reflected));
    }
  } else {
    glm::vec3 axisOrigin{0.0f};
    glm::vec3 axisDir{1.0f, 0.0f, 0.0f};
    if (!ResolveAxis(ctx, axis, axisOrigin, axisDir)) {
      LOG_ERROR("PatternFeature '{}': axis not found", m_name);
      return false;
    }
    // Resolve the count (expression over the parameter table, else literal).
    int n = count;
    if (!countExpr.empty() && ctx.symbols != nullptr) {
      const auto v = ExpressionEngine::Default().EvaluateText(
          countExpr, *ctx.symbols, nullptr);
      if (v.has_value()) {
        n = static_cast<int>(std::lround(*v));
      }
    }
    n = std::max(1, n);

    for (size_t i = 0; i < seedCount; i++) {
      for (int k = 1; k < n; k++) {
        CADShape copy = instances[i];
        if (kind == PatternKind::Linear) {
          copy.Translate(axisDir * static_cast<float>(k * spacing));
        } else { // Circular
          copy.RotateAbout(axisOrigin, axisDir,
                           glm::radians(static_cast<double>(k) * angleDeg));
        }
        instances.push_back(std::move(copy));
      }
    }
  }

  return FuseInstancesReplacingSource(ctx.bodies, sourceId, std::move(instances),
                                      m_name, m_material, GetId());
}
