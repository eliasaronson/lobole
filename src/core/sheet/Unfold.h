#pragma once

#include "cad/CADKernel.h"
#include "core/ElementMap.h"
#include "core/Feature.h"
#include "core/SheetMetal.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class TopoDS_Face;
class gp_Dir;

// Flattening a sheet body, and folding it back
// (docs/sheetmetal/README.md#how-the-unfolder-works).
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

// The direction the bend's parent panel faces NOW: its recorded `upDir`
// carried through `parentPlace` (which composes, so the result is only unit
// up to rounding and is normalised here) and then through `placement` — the
// occurrence transform for an assembly, identity for a part.
[[nodiscard]] glm::dvec3
CreaseNormal(const BendRegion& bend,
             const glm::dmat4& placement = glm::dmat4(1.0));

// Whether `bend` folds toward `side` — UP as seen from that side of the
// sheet. The wall turns toward the bend's `upDir`, carried to wherever its
// parent panel sits now. Only a crease lying square to `side` has an answer
// a reader would recognise (SquareTo says so); for anything else this is
// just the sign.
[[nodiscard]] bool FoldsToward(const BendRegion& bend, const glm::dvec3& side);

// Does this crease lie square to `side` — flat on, rather than edge-on? The
// one threshold, so a bend line and its table row cannot disagree about
// whether the crease is being looked at.
[[nodiscard]] bool SquareTo(const BendRegion& bend, const glm::dvec3& side,
                            const glm::dmat4& placement = glm::dmat4(1.0));

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

// A cut NORMAL TO THE SHEET everywhere (S10): the planar `profile`, carried
// along `along` to where it meets the folded body, laid onto the blank —
// rigidly on a panel, by the neutral map on a bend — cut straight through
// there, and folded back up. A cut taken straight along a sketch's normal
// through a bend or a leaning wall comes out slanted to the sheet, and no
// laser-then-brake process makes that; this is the cut that process DOES
// make. Returns the folded body with the cut, or nullopt with `error`.
//
// EVERY profile of the sketch in one call, because the expensive half of a
// cut is per BODY, not per hole: the ray tree the outlines are traced
// against, and — only when one of them lands across a bend — the part laid
// flat. Cut one at a time, a thirteen-hole sketch unfolded the part thirteen
// times
// (docs/sheetmetal/README.md#normalcut-shares-its-ray-tree-and-unfolds-only-when-it-must).
[[nodiscard]] std::optional<FlatResult>
NormalCut(const Body& body, const SheetMetalRules& rules,
          const std::vector<TopoDS_Face>& profiles, const gp_Dir& along,
          uint64_t tag, std::string* error);

} // namespace sheet
