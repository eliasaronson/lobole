# Sheet metal: Fold

A bend made in sheet that is already there: a line sketched on a panel,
and the material on one side of it turned up about that line. Where the
line is interrupted by the panel's outline or by a cut-out, each stretch
frees its own piece, so a tab cut on three sides by a slot inside a plate
folds on its own and the plate round it stays flat.

## The problem

Every bend the sheet features make adds a wall to an edge (`EdgeFlange`,
`Hem`, both through `sheet::Flange`) or steps a whole panel (`Jog`, whose
bends run the panel's full width whatever the line's length:
src/core/features/JogFeature.cpp). Nothing bends a panel along a line
inside it. So the parts a laser and a brake make most cheaply cannot be
modelled the way they are made:

- a tab or lance inside a plate (a cable tie-down, a stop, a louvre's
  tongue), which is a slot cut round three sides and one bend;
- a bracket drawn as its blank and folded, when the blank is what the
  designer has (a DXF, an existing flat part);
- collar tabs round an opening, each folding on its own line.

The model already holds everything such a bend needs. A `BendRegion`
(src/core/Feature.h) has an axis origin and a `width` along the axis, so a
bend need not span its panel; the unfolder (src/core/sheet/Unfold.cpp)
takes as a bend's child only the connected material past its far tangent
plane and leaves "everything that is neither the wall nor the bend" with
the parent, which is how the panel beside an inset flange survives
(`tests/test_refold.cpp`). And `sheet::Refold` already performs the
operation itself: for a crease lying flat it takes the band of flat stock
one allowance wide, wraps it into the bend by the neutral-axis map
(`SectorFor`, carrying every through-cut in the band exactly), and swings
the child round it. A fold is that step applied to a crease nobody
unfolded.

## The shape of the fix

### 1. The feature

`FoldFeature` (`src/core/features/FoldFeature.{h,cpp}`), type `Fold`:

| field | what | unset derives |
|---|---|---|
| `sketch_id` | a sketch holding exactly one line, drawn on a flat panel face of a sheet body (as `Jog` finds its panel) | required |
| `angle_deg` (+`_expr`) | the bend's sweep, (0, 180] | 90, the square bend, as `EdgeFlange` |
| `radius_mm` (+`_expr`) | inside radius | the rules' `EffectiveBendRadius()` |
| `position` | where the line sits in the bend: `centerline`, `bend_start` (the fixed side's tangent line), `bend_end` | `centerline`, where the flat pattern draws the crease |
| `side` | which side of the line turns, `left` or `right` of it seen from the sketched face along the line's first point to its second | resolved once, when the feature is added: the side holding less of the sheet. Stored, never chosen again, so a later edit that changes which side is larger does not move the fold. The two sides are one face until the fold splits it, so neither has a name to give; a call may instead pass `side_at_mm: [x, y]`, a point in the sketch's plane on the side that turns (what a click in the tool sends), which is resolved to `left` or `right` and stored as that. A point on the line, or off the sheet, is refused |
| `reverse` | fold away from the sketched face instead of toward it | false |

K is the rules' for that radius (`EffectiveKFactor`, with its source
recorded on the bend), and the allowance is `BendAllowance(angle, R, K,
t)`: no number of its own.

### 2. Chords

The bend zone is the band of the sketched face one allowance wide, placed
about the line as `position` says, on the line carried to infinity both
ways.

1. **The chords** are the connected pieces of the panel's material inside
   the zone that reach both of its edges: each a stretch of the line
   through material, ending at the outline or at a cut-out. A piece of
   the zone only one edge reaches (a tongue's tip, a sliver between two
   cut-outs) is not a chord.
2. **The line as drawn picks chords**: every chord its two points span any
   part of, each taken whole. A line drawn across a tab's root picks that
   chord; a line drawn across the whole plate picks all of them. This is
   where Fold differs from Jog, whose line only locates the step.
3. **The picked chords must part the panel.** Cut along them, the panel
   must fall into a fixed piece and one or more freed pieces on the
   folding side. Where the material runs round a cut-out from one side of
   a picked chord to the other, the fold is refused, naming how far the
   line would have to be extended to part it; the fold never takes a
   chord the line did not reach.
4. **Each freed piece turns on its own bend**, spanning the chords it
   hangs from. Where it hangs from several (a line across a plate with a
   hole on it), that is one bend with the gap as a cut-out across it,
   which `SectorFor` and `StripFor` already carry. A piece on the folding
   side that still reaches across the line elsewhere (the plate round a
   tab's slot, a second tab the line did not pick) stays.

The order of the freed pieces is their order along the line from its
first point, never the order a boolean lists them in.

### 3. Building it

`sheet::Fold(body, rules, line, face, params, tag, error)` in
src/core/sheet/Unfold.cpp, beside `Refold`, whose per-crease body moves
into one function both call (`FoldCrease`): take the band, find the child
as the connected solid one thickness past the band, wrap the band with
`SectorFor`, swing the child by the inverse of `BendFrame::Local()`, move
every bend the child carries (`Move`), fuse. `Fold` plans the chords,
writes one crease record per freed piece (`flat`, the allowance,
`parentPlace` identity, the frame from the line and the face's normal, or
its opposite face's with `reverse`), and folds them.

Because the band is exactly one allowance of flat stock and the wrap is
the map the unfolder inverts, `Unfold(Fold(x)) == x` to the tolerance
`Refold(Unfold(x)) == x` already holds: the blank of a folded sheet is the
sheet it was folded from. `FlatPattern`, `Unfold`/`Refold`, `NormalCut`,
`list_bends`, `check_sheet_metal`, the DXF's bend layers and a drawing's
bend table read the recorded bends and need no change.

A body with bends laid flat (an `Unfold` not yet refolded) is refused, as
Jog refuses it. A panel of an already folded body is fine: a tab can be
folded out of a flange's wall.

### 4. Names

- A bend's own faces are `<feature id>/bend<k>` with
  `topo::NamePrimitiveElements`' suffixes, `k` the freed piece's ordinal
  along the line. The record's `innerFace`, `outerFace`, `parentFace` and
  `childFace` are filled from them, which is what `check_sheet_metal`
  names in a finding.
- The fixed piece keeps every name it had. A face or edge the fold cuts
  across keeps its name on the fixed piece and gains `;F<feature id>.<k>`
  on freed piece `k`, as a Jog's carried side gains `;S<id>`: both halves
  come out of one split with one name, and a name that resolves to two
  elements resolves to whichever comes first.
- Everything on a freed piece that the fold does not cut keeps its name,
  so a hole in a tab is the same hole after the fold, and a flange already
  on it is still that flange's.

### 5. Refusals

Each names the measured value and what would be accepted.

- No sheet-metal rules; the sketch missing, failed, or not holding exactly
  one line; the line not on a flat panel face of a sheet body; a body with
  bends laid flat (the words Jog uses).
- `angle_deg` outside (0, 180]; `radius_mm` under the material's minimum
  (as a flange refuses it); an unknown `position` or `side`, naming the
  accepted ones.
- **The line does not cross the piece**: no chord under the line as drawn,
  saying where along the line the nearest material is; or picked chords
  that do not part the panel (step 3), saying how far to extend.
- **A cut ends inside the bend zone**: a slot's end or an outline corner
  between the zone's two edges, so that the zone beside a bend's end is
  still joined to the fixed piece and would tear. Named by its position
  on the sheet, with how far to move the line for the zone to clear it,
  either way.
- **Too close to an edge for the bend**: less than one thickness of flat
  material past the zone on either side. The message gives the distance
  the line needs from that edge: the part of the allowance on that side
  plus `t`, in numbers.
- **The fold collides with the sheet**: a freed piece, once turned, shares
  volume with the fixed piece or with another freed piece (a tab longer
  than its slot folded past 90 degrees, two tabs folding into each
  other). Named by the pieces and the overlap; checked with a
  non-destructive common (`cad::BuildNonDestructive`) before the fuse.
- Nothing freed on the folding side, or the fixed side left in more than
  one piece.

What a shop would question but a brake can make stays a finding of
`check_sheet_metal`, not a refusal: a leg under the die's minimum, a hole
near the bend.

### 6. Format, RPC, prose

- A `kFeatureTypes` row (src/core/io/SerializeFeatures.cpp) with its
  typed spec: `sketch_id:int>=0, angle_deg?:number, angle_deg_expr?:string,
  radius_mm?:number, radius_mm_expr?:string,
  position?:'centerline'|'bend_start'|'bend_end', side?:'left'|'right', side_at_mm?:[x,y],
  reverse?:bool`; `FeatureToJson`/`FeatureFromJson`; `VisitExprs` over the
  angle and the radius; `VisitFeatureRefs` over the sketch.
- The format moves: `kVersionPending` raised, `tests/golden/vNEXT-fold.json`
  and its `.expect.json` (a plate with a U-slot, its tab folded, a
  `FlatPattern` after it), a row in format.md's *Pending changes*.
- `FEATURE_FORMATS` in python/lobole/mcp.py gains the Fold entry
  (`mcp_formats_gate` requires it); `python/lobole/rpc.py` is
  regenerated.

### 7. The tool

`part.sheet_fold`, `###tb_fold`, in the sheet-metal toolbar group after
Jog (src/ui/Toolbar.cpp, Commands.cpp, Icons.cpp with a glyph,
src/core/io/Keymap.cpp, FeatureTreePanel.cpp).

- `CreateFold` (src/ui/CreateActions.cpp) opens the preview with the
  newest one-line sketch, the radius unset (so it shows the rules' radius,
  labelled by its source as the Sheet Metal panel labels it) and the side
  resolved from the sheet. With no rules it opens at nothing and the
  preview refuses naming them, as the other sheet tools do
  (`defaults/sheet_tools_without_a_gauge`).
- The editor (src/ui/FeatureEditor.cpp): the line as a combo of one-line
  sketches (Jog's `AcceptJogLine`, shared), Angle and Radius as `ExprDim`,
  Position, "Other side", "Fold away".
- In the viewport the preview shows the zone and each picked chord, and
  the angle is dragged on an arc handle at the bend (`GizmoInput.cpp`'s
  arc, snapping as the mate arc does); the freed pieces follow the drag.
  Clicking a freed piece's side of the line is "Other side".

## Stages

- **A. The two repros as tests of what exists**, fixing what they find:
  a cut corner a hair from a bend's tangent line, and a flange on an edge
  a cut shortened (below). No new feature.
- **B. Core:** `FoldCrease` factored out of `Refold` (the refold tests
  unchanged), the chord planner, `sheet::Fold`, the feature, its
  serialization and golden, the prose.
- **C. The tool** and its GUI tests.

## Files

- New `src/core/features/FoldFeature.{h,cpp}`; `CMakeLists.txt`.
- `src/core/sheet/Unfold.{h,cpp}` (`FoldCrease`, `Fold`, the chord
  planner).
- `src/core/io/SerializeFeatures.cpp`,
  `src/core/io/SerializationInternal.h` (`kVersionPending`).
- `src/ui/CreateActions.cpp`, `FeatureEditor.cpp`, `Toolbar.cpp`,
  `Commands.cpp`, `Icons.cpp`, `FeatureTreePanel.cpp`,
  `PreviewFeature.cpp`; `src/core/io/Keymap.cpp`.
- `python/lobole/mcp.py`, `python/lobole/rpc.py` (regenerated).
- `tests/test_fold.cpp` (new), `tests/test_normal_cut.cpp`,
  `tests/test_flange.cpp`, `tests/test_refold.cpp`,
  `tests/sheetmetal_smoke.py`, `tests/golden/vNEXT-fold.json` and
  `.expect.json`, `src/app/guitests/GuiTestsModeling.cpp`.
- `docs/sheetmetal/README.md` (the feature, the chord rule, the tests),
  `docs/ui/tool-sessions.md` (the defaults line for the sheet tools),
  `docs/modeling/README.md` (the list of sheet features),
  `docs/versioning/format.md`.

## Verified by

`tests/test_fold.cpp`, tagged `[sheetmetal][fold]`:

- **Analytic flat length.** A strip of length L folded at 90 degrees a
  distance d from one end: the two outside dimensions are what the
  allowance says, `a + b - BendDeduction(...) == L`, for a table and a
  rules K, at 45, 90 and 135 degrees; the volume is the strip's less
  nothing but the bend's change. `position: bend_start` and `bend_end`
  move the outside dimensions by the stated part of the allowance.
- **Round trip.** `FlatPattern` after a `Fold` gives the body before the
  fold: same volume, same outline, same thickness; then `Unfold`/`Refold`
  about it gives the folded body back. With a hole on the line, the hole
  comes back in one piece (the existing wrapped-cut property, now reached
  from the fold side).
- **The tab in a slot.** A plate with a U-shaped slot and a line across
  the tab's root: one bend, its width the tab's; the plate's bounding box
  in its own plane unchanged; the tab's free end where the angle and the
  radius put it; `list_bends` one row; the flat pattern equal to the plate
  with its slot; the DXF one bend line the tab's width long. The same
  line drawn across the whole plate folds the whole far side, with the
  tab riding along.
- **Two tabs on one line** fold as two bends with their names distinct; a
  line that reaches only one folds only that one.
- **Names.** A hole in the tab resolves to the same element name before
  and after; the plate's top face resolves on the fixed piece and its
  `;F` name on the tab; every name is unique.
- **Each refusal**, by its text: a line beside the piece; a line whose
  chord does not part the panel (a hole in the plate's middle, the line
  across the hole only); a slot ending inside the zone, with the stated
  move making it build; a line under the stated distance from an edge,
  and at that distance building; a tab folded 180 degrees into the plate;
  a radius under the minimum; a body with bends laid flat.
- **Defaults follow the stock:** the same fold on 16ga and 10ga steel has
  the two gauges' radii and allowances, with no number in the feature.
- A Fold on a flange's wall, and an `EdgeFlange` on a folded tab's edge,
  both unfold.

The two repros, on today's features (stage A):

- **A cut corner a hair from a bend line** (`tests/test_normal_cut.cpp`,
  `tests/test_refold.cpp`): a base flange and an edge flange, a
  `NormalCut` rectangle with one corner 0.001 mm from the bend's tangent
  line on the panel side. `FlatPattern`, and `Unfold` then `Refold`, each
  either build a valid body of the right volume or are refused naming the
  corner; neither fails inside the unfolder with a kernel message. The
  variant with the cut across the whole bend: one bend in `list_bends`,
  in the drawing's bend table and on the DXF's bend layer, not two.
- **A flange on an edge a cut shortened** (`tests/test_flange.cpp`): a
  base flange with a `NormalCut` notch taking 20 mm off one end of an
  edge, then an `EdgeFlange`, and a `Hem`, on what is left of that edge.
  The bend's `width` is the remaining length and the wall stands over the
  remaining edge only.

And:

- `test_golden` reads `vNEXT-fold` and writes it back identical; the
  reader fuzz takes it into its corpus, as it takes every golden.
- `sheetmetal_smoke.py`: the tab built over RPC, `regen` clean, a refused
  fold reported in `add_feature`'s own `regen`, `export_dxf` re-read for
  its bend layer.
- GUI: `sheetmetal/fold_turns_a_tab` (toolbar, pick the line, drag the
  arc, accept: one undo step, the tab folded), `defaults/` coverage of
  `###tb_fold` with and without a gauge.
- The headless and GUI suites.
