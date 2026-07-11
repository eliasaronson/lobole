#pragma once

#include "core/ElementMap.h"
#include "core/Feature.h"

#include <cstdint>

enum class ExtrudeMode { NewBody, Add, Cut };

// End condition (M3.2/M3.3): Blind sweeps `distance` from the sketch plane;
// Symmetric centers `distance` on it; TwoSided sweeps `distance` forward and
// `distance2` backward; UpToFace sweeps to a referenced planar face
// (endTarget), following it across upstream edits. All build ONE prism of
// the computed total length translated back along the normal, so element
// names are identical to a blind extrude's.
enum class ExtrudeEnd { Blind, Symmetric, TwoSided, UpToFace };

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
  // UpToFace target: a planar face by stable element name (toponaming).
  // Planar only in v1 — a curved target needs a trimmed boolean, not a
  // distance.
  ElementRef endTarget;
  bool reversed = false;
  ExtrudeMode mode = ExtrudeMode::NewBody;
};
