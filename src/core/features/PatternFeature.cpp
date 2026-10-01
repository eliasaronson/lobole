#include "core/features/PatternFeature.h"

#include "common/Logger.h"
#include "core/BodyMode.h"
#include "core/Expression.h"
#include "core/features/DatumPlaneFeature.h" // ResolveParentPlane

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

PatternFeature::PatternFeature(const std::string& name, PatternKind kind,
                               uint64_t sourceId)
    : kind(kind), sourceId(sourceId) {
  m_name = name;
}

namespace {

// One instance's names: the source's, with an instance suffix ("4/g7" ->
// "4/g7#2", a mirror's "#m"), so a face of the third bar is as referenceable
// as one of the first. topo::DecodeElementName reads the suffix as the
// element's pattern instance.
ElementMap Suffixed(const ElementMap& src, const std::string& tag) {
  ElementMap m;
  for (const auto& [i, n] : src.names)
    m.names[i] = n + "#" + tag;
  for (const auto& [i, n] : src.edgeNames)
    m.edgeNames[i] = n + "#" + tag;
  return m;
}

} // namespace

bool PatternFeature::Execute(RegenContext& ctx) {
  // Collect the seed body/bodies produced by the referenced feature, each
  // with its names — an instance that arrives unnamed leaves unnamed, and a
  // face with no name cannot be picked, threaded, filleted or fitted.
  std::vector<std::pair<CADShape, ElementMap>> instances;
  for (const Body& b : ctx.bodies) {
    if (b.createdBy == sourceId) {
      // copy: the source stays until we replace it
      instances.emplace_back(b.shape, b.elementMap);
    }
  }
  if (instances.empty()) {
    // A CUT or ADD source leaves no body of its own behind to replicate —
    // replicate its TOOL instead: transformed copies of what the source just
    // swept, each applied in the source's mode (R14 for cuts). Patterned
    // holes used to refuse "source body not found", pushing every repeated
    // hole into one multi-loop sketch; so did a ring of teeth added to a
    // pinion face, which went in as one extrude per tooth.
    const Feature* src = ctx.FindById(sourceId);
    const CADShape* tool = src != nullptr ? src->ToolShape() : nullptr;
    if (tool != nullptr && tool->IsValid()) {
      const ElementMap* toolMap = src->ToolMap();
      return ExecuteToolPattern(ctx, *tool,
                                toolMap != nullptr ? *toolMap : ElementMap{},
                                src->ToolAdds());
    }
    SetError("source body (feature " + std::to_string(sourceId) +
             ") not found — did the source feature fail? (an Extrude that "
             "cuts or adds patterns its tool instead)");
    return false;
  }
  const size_t seedCount = instances.size();

  if (kind == PatternKind::Mirror) {
    const auto plane = ResolveParentPlane(ctx, planeId, basePlane);
    if (!plane) {
      SetError("mirror plane not found");
      return false;
    }
    const glm::dvec3 o = plane->GetOrigin();
    const glm::dvec3 n = plane->GetNormal();
    for (size_t i = 0; i < seedCount; i++) {
      CADShape reflected = instances[i].first;
      reflected.Mirror(o, n);
      instances.emplace_back(std::move(reflected),
                             Suffixed(instances[i].second, "m"));
    }
  } else {
    glm::dvec3 axisOrigin{0.0};
    glm::dvec3 axisDir{1.0, 0.0, 0.0};
    if (!ResolveAxis(ctx, axis, axisOrigin, axisDir)) {
      SetError("pattern axis not found");
      return false;
    }
    // Resolve the count (expression over the parameter table, else literal).
    int n = count;
    if (!countExpr.empty() && ctx.symbols != nullptr) {
      const auto v =
          ExpressionEngine::Default().EvaluateText(countExpr, *ctx.symbols);
      if (v.has_value()) {
        n = static_cast<int>(std::lround(*v));
      }
    }
    n = std::max(1, n);

    for (size_t i = 0; i < seedCount; i++) {
      for (int k = 1; k < n; k++) {
        CADShape copy = instances[i].first;
        if (kind == PatternKind::Linear) {
          copy.Translate(axisDir * (k * spacing));
        } else { // Circular
          copy.RotateAbout(axisOrigin, axisDir,
                           glm::radians(static_cast<double>(k) * angleDeg));
        }
        instances.emplace_back(
            std::move(copy), Suffixed(instances[i].second, std::to_string(k)));
      }
    }
  }

  std::string fuseErr;
  const bool ok =
      FuseInstancesReplacingSource(ctx.bodies, sourceId, std::move(instances),
                                   m_name, m_material, GetId(), &fuseErr);
  if (!ok)
    SetError(fuseErr.empty() ? "pattern failed" : fuseErr);
  return ok;
}

bool PatternFeature::ExecuteToolPattern(RegenContext& ctx, const CADShape& tool,
                                        const ElementMap& toolMap, bool adds) {
  const auto suffixed = [&toolMap](const std::string& tag) {
    return Suffixed(toolMap, tag);
  };
  std::vector<std::pair<CADShape, ElementMap>> cutters;
  if (kind == PatternKind::Mirror) {
    const auto plane = ResolveParentPlane(ctx, planeId, basePlane);
    if (!plane) {
      SetError("mirror plane not found");
      return false;
    }
    CADShape reflected = tool;
    reflected.Mirror(plane->GetOrigin(), plane->GetNormal());
    cutters.emplace_back(std::move(reflected), suffixed("m"));
  } else {
    glm::dvec3 axisOrigin{0.0};
    glm::dvec3 axisDir{1.0, 0.0, 0.0};
    if (!ResolveAxis(ctx, axis, axisOrigin, axisDir)) {
      SetError("pattern axis not found");
      return false;
    }
    int n = count;
    if (!countExpr.empty() && ctx.symbols != nullptr) {
      const auto v =
          ExpressionEngine::Default().EvaluateText(countExpr, *ctx.symbols);
      if (v.has_value())
        n = static_cast<int>(std::lround(*v));
    }
    n = std::max(1, n);
    for (int k = 1; k < n; k++) { // k = 0 is the source cut itself
      CADShape copy = tool;
      if (kind == PatternKind::Linear)
        copy.Translate(axisDir * (k * spacing));
      else
        copy.RotateAbout(axisOrigin, axisDir,
                         glm::radians(static_cast<double>(k) * angleDeg));
      cutters.emplace_back(std::move(copy), suffixed(std::to_string(k)));
    }
  }
  for (auto& [shape, map] : cutters) {
    if (!ApplyBodyModeReporting(ctx.bodies, std::move(shape), std::move(map),
                                adds ? ExtrudeMode::Add : ExtrudeMode::Cut,
                                *this))
      return false;
  }
  return true;
}
