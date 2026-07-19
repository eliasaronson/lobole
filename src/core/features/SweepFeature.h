#pragma once

#include "core/BodyMode.h" // ExtrudeMode
#include "core/Feature.h"

#include <cstdint>
#include <string>

// Sweeps a closed profile sketch along an open path sketch (modeling M8):
// BRepOffsetAPI_MakePipe skins the profile down the spine — a pipe from a
// circle down a bent path, a moulding along an edge, ... The profile should
// sit at the path's start and roughly perpendicular to it (as when the two
// sketches are on perpendicular planes sharing the start point). Modes reuse
// ApplyBodyMode. Regeneration fails per-feature when either sketch is
// missing, the profile is not one closed loop, the path is not one open
// chain, or OCC rejects the sweep.
class SweepFeature : public Feature {
public:
  SweepFeature(const std::string& name, uint64_t profileSketchId,
               uint64_t pathSketchId, ExtrudeMode mode = ExtrudeMode::NewBody);

  std::string TypeName() const override { return "Sweep"; }
  bool Execute(RegenContext& ctx) override;

  uint64_t profileSketchId = 0;
  uint64_t pathSketchId = 0;
  ExtrudeMode mode = ExtrudeMode::NewBody;
};
