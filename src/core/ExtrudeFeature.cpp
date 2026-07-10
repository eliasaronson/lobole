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

  // NewBody: the prism *is* the body, so its face indices survive into the
  // rendered mesh — name its faces (topological naming, T2) and attach the map.
  // Add/Cut run a boolean that renumbers faces, so leave those unnamed (T5).
  if (mode == ExtrudeMode::NewBody) {
    ElementMap map;
    CADShape prism = sketch::BuildPrismNamed(sketch->plane, loops, distance,
                                             reversed, GetId(), map);
    if (!prism.IsValid()) {
      LOG_ERROR("ExtrudeFeature '{}': prism construction failed", m_name);
      return false;
    }
    ctx.bodies.push_back({m_name, std::move(prism), m_material, GetId()});
    ctx.bodies.back().elementMap = std::move(map);
    return true;
  }

  CADShape prism =
      sketch::BuildPrism(sketch->plane, loops, distance, reversed);
  if (!prism.IsValid()) {
    LOG_ERROR("ExtrudeFeature '{}': prism construction failed", m_name);
    return false;
  }

  return ApplyBodyMode(ctx.bodies, std::move(prism), mode, m_name, m_material,
                       GetId());
}
