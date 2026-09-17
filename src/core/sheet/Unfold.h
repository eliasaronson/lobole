#pragma once

#include "cad/CADKernel.h"
#include "core/ElementMap.h"
#include "core/Feature.h"
#include "core/SheetMetal.h"

#include <optional>
#include <string>
#include <vector>

// Flattening a sheet body (docs/sheetmetal/plan.md, S4).
//
// The flat pattern does NOT come from developing the cylindrical faces of the
// finished solid. It comes from the bends the features RECORDED as they made
// them — axis, radius, angle, K — so unfolding is a walk over a graph with
// BA = angle * (R + K*t) at each edge, and folding it back up is the same walk
// with the transforms inverted. That is what makes refold(unfold(x)) == x a
// property a test asserts rather than a tolerance to hope about.
//
// The construction, per bend, in the folded body:
//   1. Cut the body at the bend's two tangent planes: the parent side, the
//      bend's own slab, and the child side.
//   2. Rotate the child side about the bend axis until it is coplanar with
//      the parent, then slide it out by the bend allowance.
//   3. Replace the slab with the parent's own cut FACE extruded by the
//      allowance — not a rectangle, which is what makes a notched or
//      profiled seam come out right.
// Then fuse the lot. Panel faces keep their names through every step, so a
// dimension or a hole taken on the folded part still resolves on the flat.
namespace sheet {

struct FlatResult {
  CADShape shape;
  ElementMap map;
  // The bends again, each carrying its allowance and its crease centreline in
  // FLAT coordinates — what a bend line is drawn from.
  std::vector<BendRegion> creases;
};

// Flattens `body` about `fixedFace` (unset = the piece holding the first
// bend's parent panel, i.e. the base flange). Returns nullopt with `error`
// (may be null) set in user-grade prose.
//
// A body with no bends flattens to itself: a blank is already flat, and
// saying so beats refusing.
[[nodiscard]] std::optional<FlatResult>
Unfold(const Body& body, const SheetMetalRules& rules,
       const ElementRef& fixedFace, uint64_t tag, std::string* error);

} // namespace sheet
