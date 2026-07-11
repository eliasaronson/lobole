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
    SetError("sketch has no closed profile");
    LOG_ERROR("ExtrudeFeature '{}': sketch has no closed profile", m_name);
    return false;
  }

  // Name the prism's faces from the sketch entities that generate them
  // (topological naming, T2), for every mode: NewBody attaches the map as-is,
  // and Add/Cut propagate it through their boolean (T5) — a face the boolean
  // leaves alone keeps its name, a pierced one gains ";M<id>".
  ElementMap map;
  CADShape prism = sketch::BuildPrismNamed(sketch->plane, loops, distance,
                                           reversed, GetId(), map);
  if (!prism.IsValid()) {
    SetError("profile could not be extruded (overlapping loops? see log)");
    LOG_ERROR("ExtrudeFeature '{}': prism construction failed", m_name);
    return false;
  }

  return ApplyBodyMode(ctx.bodies, std::move(prism), std::move(map), mode,
                       m_name, m_material, GetId());
}
