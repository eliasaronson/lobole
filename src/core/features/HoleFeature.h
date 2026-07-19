#pragma once

#include "core/Feature.h"

#include <cstdint>
#include <string>

enum class HoleKind { Simple, Counterbore, Countersink };

// Drills holes at a sketch's Point entities (modeling M8 hole wizard): each
// point is a hole center on the sketch plane, and the hole is cut into the
// bodies below along the plane's -normal. Simple = a plain bore; Counterbore
// adds a larger flat-bottomed recess at the surface; Countersink adds a
// conical recess. A far cheaper, more legible bolt-hole than a construction
// circle + cut-extrude per hole. Cut mode (subtracts from every overlapping
// body), like an extrude-Cut. Regeneration fails per-feature when the sketch
// is missing or has no points, or the diameters/depths are non-positive.
class HoleFeature : public Feature {
public:
  HoleFeature(const std::string& name, uint64_t sketchId, HoleKind kind,
              double diameter, double depth);

  std::string TypeName() const override { return "Hole"; }
  bool Execute(RegenContext& ctx) override;

  void VisitExprs(const ExprVisitor& visit) override {
    visit(diameterExpr, diameter);
    visit(depthExpr, depth);
  }

  uint64_t sketchId = 0;
  HoleKind kind = HoleKind::Simple;
  double diameter = 1.0;
  std::string diameterExpr;
  double depth = 1.0; // bore depth; `through` overrides it
  std::string depthExpr;
  bool through = false; // drill far enough to pierce any body below

  // Counterbore: a `cbDiameter` recess `cbDepth` deep at the surface.
  double cbDiameter = 2.0;
  double cbDepth = 0.5;
  // Countersink: a cone widening to `csDiameter` at the surface over
  // `csDepth` (typical 82/90 deg bolt heads — the caller picks the depth).
  double csDiameter = 2.0;
  double csDepth = 0.5;
};
