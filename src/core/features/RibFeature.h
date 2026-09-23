#pragma once

#include "core/Feature.h"

// Rib (docs/modeling/README.md#rib): a thin stiffener wall
// grown from an OPEN sketch profile toward a body. The open profile is
// thickened in-plane (symmetric offset by thickness/2 → closed slab contour),
// prismed along the sketch normal far enough to pass through the target, then
// trimmed "up to the body": Cut(slab, body), split into solids, keep the
// component(s) touching the profile plane — exactly the material between the
// profile and the body wall, conforming to curved walls for free — and fuse
// them into the target. A profile that never reaches the body is a
// per-feature error (regen-failure UX), as is a rib entirely inside the body.
// Deferred: draft angle, per-side thickness, multi-profile networks.
class RibFeature : public Feature {
public:
  RibFeature(const std::string& name, uint64_t sketchId, double thickness,
             bool flipDirection = false, uint64_t bodyId = 0);

  std::string TypeName() const override { return "Rib"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<RibFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;
  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(sketchId); // the open profile
    visit(bodyId);
  }

  void VisitExprs(const ExprVisitor& visit) override {
    visit(thicknessExpr, thickness);
  }

  uint64_t sketchId = 0;  // the open profile
  double thickness = 1.0; // symmetric about the profile curve
  std::string thicknessExpr;
  bool flipDirection = false; // grow along -normal instead of +normal
  // Target body's creating-feature id (the boolean/pattern operand
  // convention); 0 = the nearest overlapping body (the Add rule).
  uint64_t bodyId = 0;
};
