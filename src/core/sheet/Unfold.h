#pragma once

#include "cad/CADKernel.h"
#include "core/ElementMap.h"
#include "core/Feature.h"
#include "core/SheetMetal.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Flattening a sheet body, and folding it back (docs/sheetmetal/plan.md, S4
// and S9).
//
// The flat pattern does NOT come from developing the cylindrical faces of the
// finished solid. It comes from the bends the features RECORDED as they made
// them — axis, radius, angle, K — so unfolding is a walk over a graph with
// BA = angle * (R + K*t) at each edge, and refolding is the same walk with the
// transforms inverted.
//
// Per bend, unfolding:
//   1. Split the body at the bend's two tangent planes into the child side,
//      the bend's own slab (bounded along its axis), and everything else —
//      which stays with the parent, so material beside an inset flange is
//      never dropped.
//   2. Rotate the child side back into the parent's plane about the bend
//      axis, then slide it out by the allowance.
//   3. Replace the slab with the IDEAL flat strip (allowance x thickness x
//      width) minus whatever had been cut from the ideal bend, carried across
//      by the neutral-axis map — so a hole through a bend comes out as a hole
//      in the blank, not as nothing.
// Refolding is those three steps backwards, with the ideal annular sector in
// place of the strip. A cut that does not go through the full thickness
// cannot be carried across by that map and is REFUSED by name; so is material
// ADDED to a bend, which has no flat shape at all.
namespace sheet {

struct FlatResult {
  CADShape shape;
  ElementMap map;
  // Every bend of the body afterwards, in body order: the ones flattened here
  // marked `flat` with their allowance, crease centreline and parent
  // placement; the rest carried along by whatever piece they rode on.
  std::vector<BendRegion> bends;
};

// Flattens the bends of `body` made by the features in `onlyFrom` (empty =
// every bend) about `fixedFace` (unset = the piece holding the first bend's
// parent panel, i.e. the base flange). `tag` names the new geometry and is
// recorded on each crease as `unfoldedBy`. Returns nullopt with `error` (may
// be null) set in user-grade prose.
//
// A body with no bends flattens to itself — a blank is already flat, and
// saying so beats refusing. A body whose selected bends are ALL already flat
// is refused: flattening a blank twice would hand back a longer blank.
[[nodiscard]] std::optional<FlatResult>
Unfold(const Body& body, const SheetMetalRules& rules,
       const ElementRef& fixedFace, uint64_t tag,
       const std::vector<uint64_t>& onlyFrom, std::string* error);

// Folds back up every crease that `unfoldTag` flattened, carrying any
// through-thickness cut made across a crease onto the bend. The parent side
// of each crease stays where it is; the child side swings back. Returns
// nullopt with `error` set when there is nothing of `unfoldTag`'s to fold, or
// a cut across a crease cannot be carried.
[[nodiscard]] std::optional<FlatResult> Refold(const Body& body,
                                               const SheetMetalRules& rules,
                                               uint64_t unfoldTag, uint64_t tag,
                                               std::string* error);

} // namespace sheet
