// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "cad/SketchPlane.h"
#include "core/AxisRef.h"
#include "core/Feature.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

enum class PatternKind { Mirror, Linear, Circular };

// Mirrors or arrays a source body (modeling M5). The source is referenced by
// the id of the feature that produced it (Body::createdBy) — whole-body,
// toponaming-free. Every instance (the seed plus its transformed copies) is
// fused, then split back into connected solids: instances that touch merge
// into one body, disjoint ones become separate bodies (the mainstream CAD
// behavior). The seed body is consumed into the result.
//
// Mirror reflects across a plane (base plane or datum plane). Linear/Circular
// array along/around an AxisRef (global axis, sketch line, or datum axis).
// Count is expression-driven (evaluated against the parameter table), as are
// the linear spacing and circular per-step angle.
class PatternFeature : public Feature {
public:
  PatternFeature(const std::string& name, PatternKind kind, uint64_t sourceId);

  [[nodiscard]] std::string TypeName() const override { return "Pattern"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<PatternFeature>(*this);
  }

  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<PatternFeature>(executed);
  }

  bool Execute(RegenContext& ctx) override;
  [[nodiscard]] std::string WhyRefusedOn(const std::vector<Body>& input,
                                         const Document& doc) const override;
  uint64_t MaterialSource() const override { return sourceId; }

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(sourceId); // the seed, by its producing feature
    visit(planeId);  // Mirror kind
    VisitRefs(axis, visit);
  }

private:
  // The count, the reach and the step angle: SetError and false when one is
  // past what a pattern can take.
  bool PatternInRange(int n);
  // The cut-source path: replicate the source's cutter and subtract each
  // transformed copy (the original cut already made instance 0).
  bool ExecuteToolPattern(RegenContext& ctx, const CADShape& tool,
                          const ElementMap& toolMap, bool adds);

public:
  [[nodiscard]] std::string WhyIncomplete() const override {
    return sourceId == 0 ? "pick the body (or cut) to pattern" : "";
  }

  void VisitExprs(const ExprVisitor& visit) override {
    visit({spacing, ExprQuantity::Length, "spacing"});
    visit({angleDeg, ExprQuantity::Angle, "angle_deg"});
    // A count is whole: an expression that comes out fractional (n / 2 of an
    // odd n) is the pattern's error, naming the value, never rounded to a
    // number of copies nobody wrote.
    visit({count, "count"});
  }

  PatternKind kind = PatternKind::Linear;
  uint64_t sourceId = 0; // producing-feature id of the seed body

  // Mirror: the reflection plane (0 = a base plane, else an earlier datum).
  uint64_t planeId = 0;
  PlaneType basePlane = PlaneType::XY;

  // Linear/Circular: the direction / rotation axis.
  AxisRef axis;

  ExprInt count{3}; // total instances including the seed

  ExprValue spacing{5.0}; // Linear: per-step distance along the axis

  ExprValue angleDeg{90.0}; // Circular: per-step angle about the axis
};
