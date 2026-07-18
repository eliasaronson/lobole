#include "core/ExtrudeFeature.h"

#include "Logger.h"
#include "core/BodyMode.h"
#include "core/SketchFeature.h"
#include "core/SketchProfile.h"

#include <cmath>

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

  // End condition: every variant is ONE prism of the total length, shifted
  // back along the normal — geometry-identical to sweeping both ways, and
  // the element map stays exactly a blind extrude's.
  double total = distance;
  double backOffset = 0.0; // along the sweep direction, before `reversed`
  bool sweepReversed = reversed;
  if (endMode == ExtrudeEnd::Symmetric) {
    backOffset = distance / 2.0;
  } else if (endMode == ExtrudeEnd::TwoSided) {
    total = distance + distance2;
    backOffset = distance2;
  } else if (endMode == ExtrudeEnd::UpToFace) {
    if (!endTarget.IsSet()) {
      SetError("no target face set for up-to-face");
      return false;
    }
    const auto target = ResolveFacePlane(ctx, endTarget);
    if (!target) {
      SetError("target face '" + endTarget.name +
               "' no longer exists (or is not planar)");
      return false;
    }
    // Signed sweep along the sketch normal to the target's plane — the sign
    // picks the side, so `reversed` does not apply.
    const glm::vec3 n = sketch->plane.GetNormal();
    total = glm::dot(target->GetOrigin() - sketch->plane.GetOrigin(), n);
    sweepReversed = false;
  }
  if (std::abs(total) < 1e-9) {
    SetError(endMode == ExtrudeEnd::UpToFace
                 ? "target face lies on the sketch plane"
                 : "extrude distance is zero");
    return false;
  }

  // Name the prism's faces from the sketch entities that generate them
  // (topological naming, T2), for every mode: NewBody attaches the map as-is,
  // and Add/Cut propagate it through their boolean (T5) — a face the boolean
  // leaves alone keeps its name, a pierced one gains ";M<id>".
  ElementMap map;
  CADShape prism =
      thin ? sketch::BuildThinPrismNamed(sketch->plane, loops, total,
                                         sweepReversed, thinThickness,
                                         thinSide, GetId(), map)
           : sketch::BuildPrismNamed(sketch->plane, loops, total,
                                     sweepReversed, GetId(), map);
  if (!prism.IsValid()) {
    SetError(thin ? "thin profile could not be built (wall thicker than the "
                    "profile, or thickness <= 0?)"
                  : "profile could not be extruded (overlapping loops? "
                    "see log)");
    LOG_ERROR("ExtrudeFeature '{}': prism construction failed", m_name);
    return false;
  }
  if (backOffset != 0.0) {
    const glm::vec3 n = sketch->plane.GetNormal();
    const float sign = reversed ? 1.0f : -1.0f;
    prism.Translate(n * (sign * static_cast<float>(backOffset)));
  }

  return ApplyBodyMode(ctx.bodies, std::move(prism), std::move(map), mode,
                       m_name, m_material, GetId());
}
