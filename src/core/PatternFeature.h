#pragma once

#include "SketchPlane.h"
#include "core/AxisRef.h"
#include "core/Feature.h"

#include <cstdint>
#include <string>

enum class PatternKind { Mirror, Linear, Circular };

// Mirrors or arrays a source body (modeling M5). The source is referenced by
// the id of the feature that produced it (Body::createdBy) — whole-body,
// toponaming-free. Every instance (the seed plus its transformed copies) is
// fused, then split back into connected solids: instances that touch merge
// into one body, disjoint ones become separate bodies (Onshape/SolidWorks
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
  bool Execute(RegenContext& ctx) override;

  void VisitExprs(const ExprVisitor& visit) override {
    // count is an int, evaluated separately in Execute against ctx.symbols.
    visit(spacingExpr, spacing);
    visit(angleExpr, angleDeg);
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
