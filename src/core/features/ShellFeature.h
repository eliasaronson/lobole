#pragma once

#include "core/ElementMap.h"
#include "core/Feature.h"

#include <cstdint>
#include <vector>

// Hollows one body into a thin shell (modeling M8): the faces named by
// `openFaces` (stable ElementRefs into the body's element map) are removed,
// and the remaining boundary is offset inward by `thickness` — the classic
// "make it a wall" operation (a box with its top removed becomes a bin). All
// refs must address the same body (the one produced by ref.featureId). With
// no open faces the whole body is hollowed into a closed shell.
//
// The body is modified in place (createdBy stays the producer's id, like
// fillet/extrude-Add). Shelling reorganizes topology, so the result gets
// fresh primitive-style names ("<id>/faceN") — a later fillet references the
// shelled faces, not the pre-shell ones. Regeneration fails per-feature when
// a ref no longer resolves or OCC rejects the offset (wall thicker than the
// geometry allows).
class ShellFeature : public Feature {
public:
  ShellFeature(const std::string& name, uint64_t bodyId,
               std::vector<ElementRef> openFaces, double thickness);

  std::string TypeName() const override { return "Shell"; }
  bool Execute(RegenContext& ctx) override;

  void VisitExprs(const ExprVisitor& visit) override {
    visit(thicknessExpr, thickness);
  }

  uint64_t bodyId = 0; // producing-feature id of the body to hollow
  std::vector<ElementRef> openFaces;
  double thickness = 1.0;
  std::string thicknessExpr;
};
