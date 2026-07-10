#pragma once

#include "core/Feature.h"

#include <cstdint>

enum class ExtrudeMode { NewBody, Add, Cut };

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
  }

  uint64_t sketchId = 0;
  double distance = 1.0;
  // Optional expression driving `distance` (empty = plain literal),
  // evaluated against the document's parameters on every regeneration.
  std::string distanceExpr;
  bool reversed = false;
  ExtrudeMode mode = ExtrudeMode::NewBody;
};
