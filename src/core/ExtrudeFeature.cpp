#include "core/ExtrudeFeature.h"

#include "Logger.h"
#include "core/BodyMode.h"
#include "core/SketchFeature.h"
#include "core/SketchProfile.h"

ExtrudeFeature::ExtrudeFeature(const std::string& name, uint64_t sketchId,
                               double distance, ExtrudeMode mode, bool reversed)
    : sketchId(sketchId), distance(distance), reversed(reversed), mode(mode) {
  m_name = name;
}

bool ExtrudeFeature::Execute(RegenContext& ctx) {
  const Feature* ref = ctx.FindById(sketchId);
  const auto* sketch = dynamic_cast<const SketchFeature*>(ref);
  if (!sketch) {
    LOG_ERROR("ExtrudeFeature '{}': sketch id {} not found earlier in history",
              m_name, sketchId);
    return false;
  }

  std::vector<sketch::Loop> loops = sketch::BuildLoops(sketch->entities);
  if (loops.empty()) {
    LOG_ERROR("ExtrudeFeature '{}': sketch has no closed profile", m_name);
    return false;
  }

  CADShape prism =
      sketch::BuildPrism(sketch->plane, loops, distance, reversed);
  if (!prism.IsValid()) {
    LOG_ERROR("ExtrudeFeature '{}': prism construction failed", m_name);
    return false;
  }

  return ApplyBodyMode(ctx.bodies, std::move(prism), mode, m_name, m_material);
}
