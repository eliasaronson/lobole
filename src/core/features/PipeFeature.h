#pragma once

#include "core/BodyMode.h" // ExtrudeMode
#include "core/Feature.h"

#include <cstdint>
#include <string>

// Sweeps a circular, optionally hollow, profile along a path — the common
// "pipe / tube" convenience over Sweep (curves-3d Phase 2). The path
// (`pathId`) is a sketch OR a Curve3D (fit-spline / helix / control-poly), via
// the same ResolveSweepPathWire as Sweep. A circle of `outerDiameter` is built
// normal to the path at its start and skinned down the spine; when
// `wallThickness` > 0 an inner pipe is subtracted to make a tube. Reuses
// SweepProfileAlongSpine — no duplicate sweep code. Modes reuse ApplyBodyMode.
// Regeneration fails per-feature when the path is missing/unusable, the wall is
// thicker than the radius, or OCC rejects the sweep.
// The OUTER solid a tube occupies: `pathId` swept by a circle of
// `outerDiameter` grown by `extra` on the radius, ignoring any wall. What a
// COPE cuts with — the outside of the neighbour is what a member has to
// clear, whether or not the neighbour is hollow, and subtracting a hollow
// tube would leave the member filling its bore. Returns an invalid CADShape
// with `error` (may be null) set when the path or the sweep will not build.
[[nodiscard]] CADShape PipeOuterSolid(const RegenContext& ctx, uint64_t pathId,
                                      double outerDiameter, double extra,
                                      std::string* error);

class PipeFeature : public Feature {
public:
  PipeFeature(const std::string& name, uint64_t pathId, double outerDiameter,
              double wallThickness = 0.0,
              ExtrudeMode mode = ExtrudeMode::NewBody);

  std::string TypeName() const override { return "Pipe"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<PipeFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;
  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(pathId); // a sketch or a Curve3D
  }

  void VisitExprs(const ExprVisitor& visit) override;

  uint64_t pathId = 0; // a sketch OR a Curve3D
  double outerDiameter = 2.0;
  double wallThickness = 0.0; // 0 = solid rod
  std::string outerDiameterExpr, wallThicknessExpr;
  ExtrudeMode mode = ExtrudeMode::NewBody;
};
