#pragma once

#include "core/Feature.h"

#include <cstdint>

enum class ExtrudeMode { NewBody, Add, Cut };

// End condition (M3.2): Blind sweeps `distance` from the sketch plane;
// Symmetric centers `distance` on it; TwoSided sweeps `distance` forward and
// `distance2` backward. All build ONE prism of the total length translated
// back along the normal, so element names are identical to a blind extrude.
enum class ExtrudeEnd { Blind, Symmetric, TwoSided };

// Extrudes a referenced SketchFeature's closed profile into a solid.
//   NewBody: append the prism as its own body
//   Add:     fuse the prism into an existing body (a boss)
//   Cut:     subtract the prism from existing bodies (a pocket/hole)
// References its sketch by stable id, so it survives feature reordering and
// undo. Regeneration fails (stopping the history there) if the sketch is
// missing or its profile is not a closed loop.
class ExtrudeFeature : public Feature {
public:
  ExtrudeFeature(const std::string& name, uint64_t sketchId, double distance,
                 ExtrudeMode mode, bool reversed = false);

  std::string TypeName() const override { return "Extrude"; }
  bool Execute(RegenContext& ctx) override;

  void VisitExprs(const ExprVisitor& visit) override {
    visit(distanceExpr, distance);
    visit(distance2Expr, distance2);
  }

  uint64_t sketchId = 0;
  double distance = 1.0;
  // Optional expression driving `distance` (empty = plain literal),
  // evaluated against the document's parameters on every regeneration.
  std::string distanceExpr;
  // The backward side of a TwoSided extrude (with its expression twin).
  double distance2 = 1.0;
  std::string distance2Expr;
  ExtrudeEnd endMode = ExtrudeEnd::Blind;
  bool reversed = false;
  ExtrudeMode mode = ExtrudeMode::NewBody;
};
