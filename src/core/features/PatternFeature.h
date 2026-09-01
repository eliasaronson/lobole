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

  bool Execute(RegenContext& ctx) override;

private:
  // The cut-source path: replicate the source's cutter and subtract each
  // transformed copy (the original cut already made instance 0).
  bool ExecuteCutPattern(RegenContext& ctx, const CADShape& tool,
                         const ElementMap& toolMap);

public:
  [[nodiscard]] std::string WhyIncomplete() const override {
    return sourceId == 0 ? "pick the body (or cut) to pattern" : "";
  }

  void VisitExprs(const ExprVisitor& visit) override {
    visit(spacingExpr, spacing);
    visit(angleExpr, angleDeg);
    // count is an int: round-trip through a double so expression-driven
    // counts land in the serialized feature BEFORE execution — the regen
    // cache (R3) hashes that JSON, and an in-Execute-only evaluation would
    // leave the hash (and therefore the cache) stale on parameter edits.
    double countValue = static_cast<double>(count);
    visit(countExpr, countValue);
    count = std::max(1, static_cast<int>(std::lround(countValue)));
  }

  PatternKind kind = PatternKind::Linear;
  uint64_t sourceId = 0; // producing-feature id of the seed body

  // Mirror: the reflection plane (0 = a base plane, else an earlier datum).
  uint64_t planeId = 0;
  PlaneType basePlane = PlaneType::XY;

  // Linear/Circular: the direction / rotation axis.
  AxisRef axis;

  int count = 3; // total instances including the seed
  std::string countExpr;

  double spacing = 5.0; // Linear: per-step distance along the axis
  std::string spacingExpr;

  double angleDeg = 90.0; // Circular: per-step angle about the axis
  std::string angleExpr;
};
