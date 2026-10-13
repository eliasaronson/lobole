// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/AxisRef.h"
#include "core/Feature.h"
#include "core/features/ExtrudeFeature.h" // ExtrudeMode

#include <cstdint>
#include <string>

// Revolves a referenced SketchFeature's closed profile about an axis into a
// solid (modeling M2). Modes new-body / add / cut mirror Extrude (shared
// ApplyBodyMode). The axis (AxisRef: global axis, sketch line, or a
// DatumAxisFeature) must lie in the sketch plane, and the profile must not
// cross it. Angle is in degrees (expression-driven); `symmetric` centers the
// sweep on the profile plane, `reversed` sweeps the opposite hand (no effect
// when symmetric). Regeneration fails if the sketch/axis is missing, the axis
// is out of plane, or the resulting solid is invalid.
class RevolveFeature : public Feature {
public:
  RevolveFeature(const std::string& name, uint64_t sketchId, double angleDeg,
                 ExtrudeMode mode, bool reversed = false);

  [[nodiscard]] std::string TypeName() const override { return "Revolve"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<RevolveFeature>(*this);
  }

  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<RevolveFeature>(executed);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(sketchId);
    VisitRefs(axis, visit);
  }

  void VisitExprs(const ExprVisitor& visit) override {
    visit(angleExpr, angleDeg);
  }

  // As Extrude's: a Pattern of a revolve that cuts or adds (a Sphere placed
  // `on` a body is one) replicates the swept tool. Without it the pattern
  // refused "source body not found" (tools/fuzz.py).
  const CADShape* ToolShape() const override {
    return (mode == ExtrudeMode::Cut || mode == ExtrudeMode::Add) &&
                   m_tool.IsValid()
               ? &m_tool
               : nullptr;
  }
  const ElementMap* ToolMap() const override { return &m_toolMap; }
  bool ToolAdds() const override { return mode == ExtrudeMode::Add; }

  uint64_t sketchId = 0;
  AxisRef axis;
  double angleDeg = 360.0;
  std::string angleExpr;
  bool reversed = false;
  bool symmetric = false;
  ExtrudeMode mode = ExtrudeMode::NewBody;
  // Region selection (U2) — see ExtrudeFeature::regions.
  std::vector<std::vector<uint32_t>> regions;

private:
  // The last-swept cut or add solid and its names (Feature::ToolShape).
  CADShape m_tool;
  ElementMap m_toolMap;
};
