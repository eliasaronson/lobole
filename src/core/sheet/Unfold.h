// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

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

// Where a sketched line lies on a sheet: the flat face of a body holding both
// ends of the line, one thickness deep (docs/sheetmetal/README.md#features).
// `a` and `b` come back taken onto the face exactly, and `normal` is the
// face's outward normal. What a Jog and a Fold both look for. `alongLine`
// (a fold) also takes a face whose sheet the line misses at its middle but
// meets somewhere along it, carried on both ways, so the fold can say where.
struct Panel {
  size_t body = 0; // index into the bodies searched
  glm::dvec3 a{0.0}, b{0.0};
  glm::dvec3 normal{0.0, 0.0, 1.0};
};
[[nodiscard]] std::optional<Panel>
PanelUnder(const std::vector<Body>& bodies, const glm::dvec3& a,
           const glm::dvec3& b, double thickness, bool alongLine = false);

// Where the line sits in the bend a Fold makes: on its centreline (where the
// flat pattern draws the crease), on the fixed side's tangent line, or on the
// far one.
enum class FoldPosition { Centerline, BendStart, BendEnd };

// A Fold (docs/sheetmetal/README.md#fold): the material on one side of a line
// on a panel turned about it. The line lies on the panel's face (PanelUnder).
struct FoldSpec {
  glm::dvec3 a{0.0}, b{0.0};        // the line, on the face
  glm::dvec3 normal{0.0, 0.0, 1.0}; // the face's outward normal
  double angle = 0.0;               // radians, (0, pi]
  double radius = 0.0;              // inside, mm
  FoldPosition position = FoldPosition::Centerline;
  // Which side turns: left of the line, seen from the face (along `normal`
  // looking back at it) walking from `a` to `b`, or right of it.
  bool left = true;
  bool reverse = false; // fold away from the face instead of toward it
};

// One bend a fold made, as the preview draws it: the zone on the face (four
// corners, fixed side first) before the fold.
struct FoldZone {
  glm::dvec3 corners[4]{};
};

struct FoldResult {
  FlatResult folded;
  std::vector<FoldZone> zones; // one per bend, in order along the line
};

// Folds every piece the line frees (docs/sheetmetal/README.md#fold): the
// chords of the zone under the line part the panel, each piece on the folding
// side turns on its own bend, named `<tag>/bend<k>` with k its place along the
// line from `a`. Returns nullopt with `error` set, naming the measured value
// and what would be accepted.
[[nodiscard]] std::optional<FoldResult> Fold(const Body& body,
                                             const SheetMetalRules& rules,
                                             const FoldSpec& spec, uint64_t tag,
                                             std::string* error);

// Whether the left side of the line holds less of the sheet than the right
// (what a Fold turns when no side is named). nullopt when neither side holds
// any.
[[nodiscard]] std::optional<bool>
FoldSmallerSideIsLeft(const Body& body, const SheetMetalRules& rules,
                      const FoldSpec& spec);

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
