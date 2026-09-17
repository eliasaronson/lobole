# Sheet metal

## The problem

A sheet-metal part is a solid of uniform thickness that a brake press can make:
flat panels joined by cylindrical bends, and nothing else. Two things follow,
and this project has neither.

**It must flatten.** The deliverable of a sheet part is not the part, it is the
blank — a 2D outline with bend lines that a laser cuts and a brake folds. A CAD
system that cannot produce it has modelled a picture of a part.

**It must refuse to be unmakeable.** A hole 1.2 mm from a bend, a flange shorter
than the die opening, a bend longer than the brake: all are valid B-Rep and all
come back from the shop as questions. The check belongs where the geometry is.

Today `kFeatureTypes` has no sheet feature, `Body` has no bend, there is no
unfold anywhere in the tree, and OCCT 7.9 ships none either — no develop, no
unroll, no flatten. `grep -rni dxf src/` returns nothing, so even a flat pattern
that existed could not leave the app.

## The model

### Bends are recorded, never recognised

The flat pattern does not come from measuring cylindrical faces of the finished
solid. Every sheet feature knows the bend it just made — axis, radius, angle,
which way — and writes it down. Unfolding is then arithmetic over a graph, not
surface development, and `refold(unfold(x)) == x` is a property a test can
assert rather than a tolerance to hope about.

`Body` grows a `std::vector<BendRegion> bends`, exactly the `threadRegions`
precedent (`src/core/Feature.h:34-39,84`): transient, stamped by the producing
feature on every regeneration, never serialized.

```
struct BendRegion {
  uint64_t featureId;          // who made it
  glm::dvec3 axisOrigin, axisDir;
  double radius;               // INSIDE radius, mm
  double angle;                // radians, always > 0
  bool up;                     // sense about axisDir; drives the DXF layer
  double kFactor;
  std::string kSource;         // "table:al-1.5" | "rules" | "override"
  ElementRef parentFace, childFace;   // the flat panels it joins
  ElementRef innerFace, outerFace;    // the cylindrical faces themselves
};
```

Recognition off arbitrary B-Rep — the "convert this import to sheet metal" path
— is a separate capability that reads this same struct. It is not in this plan.

### Rules are document-level

`SheetMetalRules { thickness, bendRadius, kFactor, material, gauge,
reliefStyle, reliefWidth, reliefDepth }` — one per part, read by every flange,
serialized in the envelope beside tolerances and units
(`src/core/io/Serialization.cpp:120-178`), omitted when unset so every existing
golden re-saves byte-identically. Per-feature overrides use the `Appearance`
"unset inherits" pattern.

Not a Configuration (that is a named variant) and not a Feature (it is not a
step in the history). The `Tolerance` shape, down to the accessors on
`Document` and a `SetSheetMetalRulesCommand` for undo.

**Thickness comes from a gauge, not a literal.** Pick material + gauge and
thickness, minimum bend radius and K-factor arrive together from one table row.
This is what satisfies docs/ui/u10-defaults-and-refusals.md here: a bare
thickness default would be a hard-coded millimetre, and a gauge is derived from
something real. `thickness` is published as a document parameter so
`length = thickness * 4` works in every field.

### A flat pattern is a body

`FlatPattern` is a terminal feature, suppressed by default, that appends the
blank as a second body. An auto-created `Flat` configuration unsuppresses it and
suppresses nothing else, so flattening is a configuration switch — machinery
that already exists, already undoes, already reaches drawings and export.

### How the unfolder works

Given the folded body and its `bends`, BFS from the fixed panel:

1. For each bend, cut the body with the two planes tangent to the bend at its
   start and end. `topo::BooleanNamed` against a half-space box keeps the
   element map, so panel faces keep their names into the flat.
2. Rigid-transform every piece downstream of the bend into the parent's plane.
3. Replace the bend slab with a prism: take the planar cut face left on the
   parent side and extrude it by the bend allowance `BA = θ·(R + K·t)`. Using
   the cut face rather than a rectangle is what makes a notched or profiled
   seam come out right.
4. Fuse the pieces.

The blank is built by taking the part apart and putting it back together, and a
boolean renames what it touches, so the flat carries ITS OWN element names
rather than the folded body's. Every face of it is named — it is a body people
pick on, dimension and cut — but a dimension taken on the folded part is not
the same reference as one taken on the blank. Carrying a name across the fold
is a real thing to want and it is not what this does.

What drives the tests is the blank's OUTLINE, not only its thickness: a wall
folded the wrong way round still lands flat and still has the right volume, and
only the extent of the blank catches it.

**A cut that lies inside a bend face is refused**, naming the bend. Replacing the
slab in step 3 would silently eat it. Tier 2's Unfold/Refold pair is the answer
to that refusal, and is why every CAD system has one.

## Tier 1 — it is sheet metal and it unfolds

**S1 — Rules.** `src/core/SheetMetal.h` (the `Configuration.h` shape),
accessors on `Document`, envelope read/write, `SetSheetMetalRulesCommand`,
`set_sheet_metal_rules` / `get_sheet_metal_rules` with `MethodDoc` rows
(`thickness_mm`, `bend_radius_mm` — `rpc_units_gate.py`), and the gauge table
as vendored data beside `ThreadStandards`/`FitStandards`. Format bump.

**S2 — Base flange.** `BaseFlangeFeature`: a sketch profile thickened by
`rules.thickness`. Geometrically this is `sketch::BuildPrismNamed` at
`distance = thickness` — the cheapest feature in the plan, and the one that
declares a part to be sheet metal. Refuses a sketch with no closed profile,
and refuses when no rules are set, naming `set_sheet_metal_rules`.

**S3 — Edge flange.** `EdgeFlangeFeature`: pick one or more straight edges of a
flat face; build the cross-section in the plane perpendicular to the hinge (the
annular sector from `R` to `R+t` through `angle`, plus the wall rectangle of
`length`), prism it along the hinge by the edge's span, fuse with
`topo::BooleanNamed`, stamp a `BendRegion`. `position` (material inside /
outside / bend outside) sets where the wall lands relative to the picked edge.
Automatic bend relief at each end where the hinge does not span the whole edge:
rectangular or obround, width `max(1.5t, 1mm)`, depth `R + t`, cut before the
flange is fused. Refuses a non-straight edge, an edge not on a flat panel, and
a radius below the material's minimum, each naming what it got and what it
needs. Format bump (S2 + S3 together).

**S4 — Flat pattern.** `FlatPatternFeature` implementing the unfolder above,
plus the `Flat` configuration, plus `fixedFace` (defaults to the base flange's
first face). Format bump.

**S5 — Seeing it.** A Sheet Metal section of the Variables panel: the stock at
the top with every derived number labelled by where it came from, "Show the
blank" as the Flat configuration, and one row per bend below — angle, radius,
direction, K, its provenance and the flat stock it eats. Toolbar buttons, tree
icons, `Commands.cpp` + `Keymap.cpp` rows, pick slots and editor fields, and
`kToolCases` rows for the three new tools.

Bend lines as a 3D `SceneOverlays` layer (red up, blue down, with labels) are
NOT part of this: the bend list carries the same facts in text and the DXF
carries them where a shop reads them. Worth adding when somebody is reading
the folded part on screen rather than the blank.

**S6 — `check_sheet_metal` and DXF.** The check in the shape of `check_geometry`
/ `check_fasteners`: hole-to-bend distance, flange below minimum height,
bend longer than the brake, missing relief, non-uniform thickness — each naming
the element and the required-versus-actual number. A `Violation` list over RPC,
the same list in the panel. Then `export_dxf` over `ProjectedCurve`, which
already carries conic centre, radius and closed-ness: LWPOLYLINE / ARC / CIRCLE
on layers `CUT`, `BEND_UP`, `BEND_DOWN` with a dashed linetype on the bend
layers, `$INSUNITS = 4`. This is the first file this project produces that a
shop can cut.

## Tier 2 — the rest of a working sheet-metal tool

**S7 — Mitre, hem, jog.** One builder (`src/core/sheet/Flange.cpp`) for
every wall bent up from an edge, so the edge flange and the hem cannot
disagree about a bend line or what a bend records. Format bump.

- *Mitre* is not a tool of its own: an edge flange whose picked edges meet at
  a corner trims the flanges that would overlap back from the corner's
  bisector, leaving a tenth of the sheet between them — touching walls fuse,
  and then no flat pattern can say which bend the corner belongs to. Flanges
  bent outside the panel only touch at a point and are left as an open
  corner. Each bend RECORDS its trim: two mitred bends occupy overlapping
  space, and the unfolder bounds each slab by it.
- A mitre is a PLANE through the bend, not a radial cut, so the flat
  pattern lays down its trace on the NEUTRAL surface — the line the bend
  allowance is measured on — rather than on either face; for a cut square to
  the sheet all three agree. Refolding starts from the ideal sector cut by
  its trims, exactly, and takes as its band the very strip the unfolder made
  of that — a plane standing in for the curved trace is exact only where it
  touches it, and the first attempt left a sliver of band behind.
- *Hem*: the builder at 180 degrees, its radius set by the gap the return
  leaves (a tenth of the sheet when closed) and so exempt from the minimum
  bend radius.
- *Jog*: two opposing bends the full width of a panel along a one-line
  sketch drawn on it; the far side is carried, never turned. The two bends
  alone climb `(2R + t)(1 − cos a)`, and a lower jog is refused. A jog puts
  NEW bends between the base and flanges older than it, so the unfolder
  settles which piece each bend folds from by where its material is once
  every cut is made, not by the order it was cut in.

**S8 — Corners.** Two things a corner where two walls meet can need, and
the corners neither is shaped for refused by name. Format bump.

- *Closed corners*, an EdgeFlange option: two flanges that only touch at a
  square corner are each carried past it as far as the other sticks out
  there, then mitred exactly as flanges that overlapped to begin with — so
  the unfolder already knows what to do with them.
- *Corner relief*, a feature: a round or square cut at every corner of a body
  where two bends cross, centred where their bend lines meet and, unless
  sized, one bend radius plus a thickness across — what clears both bends.
  It cuts through the panel and up to where the walls start, and no higher.
  It crosses the bends at a slant, so the flat pattern carries it by its
  neutral trace like a mitre; a refold of a relieved corner wraps it
  radially instead, and is not exact.
- Three bends at one corner, or two meeting at other than a right angle, are
  refused: a relief or a mitre shaped for a square two-bend corner would be
  plausible there, and wrong.

**S9 — Unfold / Refold.** `Unfold` lays the bends made by chosen flange
features (all, by default) flat in place; whatever follows is drawn and cut
flat; `Refold` folds exactly those back, parent side fixed, child side
swinging home. Every crease carries its parent's placement, so a Refold needs
nothing measured off the flat geometry, and a later Unfold that moves a crease
composes onto it. Format bump.

The same step fixed what S4 got wrong: its unfolder rebuilt each bend from
the seam face and silently dropped anything cut through the bend, and it kept
only the parent's connected piece, dropping panel that sat past a tangent
plane beside an inset flange. Now both directions work from the IDEAL shapes —
the full annular sector, the full flat strip — minus the cut-outs carried
across by the neutral-axis map. In a cylinder's own (angle, height)
parameters that map is affine, so a flat outline wraps onto the inside and
outside cylinders as exact curves (poles mapped), joined by exactly radial
ruled walls; bend to flat, the outline is sampled and laid down as a
polygon. A cut that does not go through the sheet has no outline the map can
carry and is refused; so is material added to a bend.

**S10 — Normal cut.** A cut through sheet is normal to the surface, not
projected along the sketch normal — otherwise every cut on a bend comes out
tapered. An option on the existing cut path when the target is a sheet body.

**S11 — Flat pattern on a drawing.** `DrawingView` gains a `configuration`
selector (`src/core/drawing/DrawingDocument.h:81-109`), which is what lets a
sheet name the `Flat` configuration and is worth having for every other
configured part. A `Bend` curve kind so bend lines project dashed, and a bend
table annotation on the sheet. Format bump.

## What it touches

Per feature type, the fixed cost measured on the Rib (`098b4d70`, 18 files, 539
lines) and Thread (`9cb11672`, 20 files, 609) commits, adjusted for the current
UI split: feature `.h`/`.cpp`, `kFeatureTypes` row plus both
`SerializeFeatures.cpp` branches, `CreateActions`, `Commands.cpp`,
`Keymap.cpp`, `Toolbar.cpp`, `FeatureTreePanel.cpp`, `FeatureEditor.{h,cpp}`
(four sites), `tools/designer_mcp.py` `FEATURE_FORMATS`, a unit test, a golden
pair, a `headless_smoke.py` section, a `kToolCases` row with its array-size
bump, and rows in `docs/versioning/format.md` and `docs/ui/gui-coverage.md`.
No new RPC per feature — `add_feature`/`modify_feature` are generic.

New, outside that: `src/core/SheetMetal.{h,cpp}` (rules + gauge table + bend
math), `src/core/sheet/Unfold.{h,cpp}` (the graph walk), `src/core/sheet/
Check.{h,cpp}`, `src/core/io/ExportDxf.{h,cpp}`, `src/remote/RemoteSheetMetal.cpp`,
`src/ui/SheetMetalPanel.cpp`.

Seven feature types (BaseFlange, EdgeFlange, Hem, Jog, CornerRelief, and the
Unfold/Refold pair) plus FlatPattern — the mitre lives inside EdgeFlange: call it 9 × ~600 lines of plumbing, and
5,000-6,000 lines of geometry, check, export and UI on top.

## Format

Five bumps, each with its golden pair and its `docs/versioning/format.md` row:
rules (S1), the two flange features (S2+S3), the flat pattern (S4), the Tier 2
features (S7), Unfold/Refold (S9), and the drawing view's configuration
selector (S11). No migration is owed — there are no users — so tests that
assumed the old behaviour get fixed.

## Verification

- `tests/test_sheetmetal.cpp` — rules round-trip, base flange volume equals
  `area × t`, edge flange volume against the hand-computed sector-plus-wall,
  relief cut where the hinge stops short.
- `tests/test_unfold.cpp` — the blank is exactly one gauge thick, its outline
  matches `Σ panel + Σ BA·width` computed by hand, and it comes out the same
  over a single bend, a tree of flanges off one panel, and a chain of a flange
  on a flange. Flattening a blank twice is refused.
- `tests/test_refold.cpp` — **the involution**, `refold(unfold(x)) == x` to a
  micron over one bend, a tree, a chain and a partial unfold; a hole drawn flat
  across a crease removes exactly `A·t(2R+t) / 2(R+K·t)` once wrapped, and the
  flat pattern lays it down as the same hole again; a hole straddling the
  tangent line; the refusals.
- `tests/test_sheetmetal_check.cpp` — every violation kind fires on a part built
  to trip it, and the clean part fires none. A check nothing fails is not a
  check.
- `tests/sheetmetal_smoke.py` — the RPC surface end to end: rules, two flanges,
  flatten, check, DXF out, and the DXF re-read for its layer set.
- `defaults/*` GUI cases for all eight new tools, and a GUI test that flattens
  and unflattens through the toolbar toggle.
- A dogfood part — a 1.5 mm aluminium electronics enclosure, four walls with
  relief, a hem on the lid edge, holes on two faces — flattened, checked, and
  its DXF read back for outline area against the computed blank.
