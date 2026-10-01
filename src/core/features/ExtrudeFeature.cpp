#include "core/features/ExtrudeFeature.h"

#include "common/Logger.h"
#include "core/BodyMode.h"
#include "core/features/SketchFeature.h"
#include "core/sketch/SketchProfile.h"

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
    const glm::dvec3 n = sketch->plane.GetNormal();
    total = glm::dot(target->GetOrigin() - sketch->plane.GetOrigin(), n);
    sweepReversed = false;
  } else if (endMode == ExtrudeEnd::ThroughAll) {
    if (mode != ExtrudeMode::Cut) {
      SetError("through_all requires mode 'cut'");
      return false;
    }
    if (ctx.bodies.empty()) {
      SetError("through_all cut needs an existing body");
      return false;
    }
    // Sweep from the sketch plane to just past the farthest body extent
    // along the cut direction (`reversed` picks the side, like a blind cut).
    const glm::dvec3 n = sketch->plane.GetNormal();
    const glm::dvec3 dir = reversed ? -n : n;
    const glm::dvec3 origin = sketch->plane.GetOrigin();
    double reach = 0.0;
    for (const auto& b : ctx.bodies) {
      glm::vec3 lo, hi;
      b.shape.GetBoundingBox(lo, hi);
      for (int c = 0; c < 8; ++c) {
        const glm::dvec3 corner{c & 1 ? hi.x : lo.x, c & 2 ? hi.y : lo.y,
                                c & 4 ? hi.z : lo.z};
        reach = std::max(reach, glm::dot(corner - origin, dir));
      }
    }
    total = reach + 1.0;
  }
  // A tool that starts ON its sketch plane starts a hair behind it when a
  // face of the body it meets lies there within single-precision rounding (a
  // face placed through float): otherwise a pocket cut from that face can
  // keep a sub-micron skin over it (CoplanarStartSlack).
  if ((mode == ExtrudeMode::Cut || mode == ExtrudeMode::Add) &&
      (endMode == ExtrudeEnd::Blind || endMode == ExtrudeEnd::ThroughAll) &&
      total > 0.0) {
    const glm::dvec3 n = sketch->plane.GetNormal();
    const double slack =
        CoplanarStartSlack(ctx.bodies, sketch->plane.GetOrigin(),
                           reversed ? -n : n, mode == ExtrudeMode::Cut);
    total += slack;
    backOffset += slack;
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
  std::string buildErr;
  CADShape prism =
      thin ? sketch::BuildThinPrismNamed(sketch->plane, loops, total,
                                         sweepReversed, thinThickness, thinSide,
                                         GetId(), map, &buildErr)
           : sketch::BuildPrismNamed(sketch->plane, loops, total, sweepReversed,
                                     GetId(), map, &buildErr);
  if (!prism.IsValid()) {
    SetError(buildErr.empty() ? "profile could not be extruded" : buildErr);
    return false;
  }
  if (backOffset != 0.0) {
    const double sign = reversed ? 1.0 : -1.0;
    prism.Translate(sketch->plane.GetNormal() * (sign * backOffset));
  }

  if (mode == ExtrudeMode::Cut || mode == ExtrudeMode::Add) {
    m_tool = prism; // for a Pattern of this cut or add (ToolShape)
    m_toolMap = map;
  }
  return ApplyBodyModeReporting(ctx.bodies, std::move(prism), std::move(map),
                                mode, *this);
}
