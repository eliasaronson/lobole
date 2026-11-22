# Sheet metal

A sheet-metal part is a solid of uniform thickness that a brake can make:
flat panels joined by cylindrical bends. Its deliverable is the BLANK — a flat
outline with bend lines that a laser cuts and a brake folds — so the app
models the part, flattens it, checks it against the stock, and writes the
blank as a DXF. OpenCASCADE has no unfolder; this one is ours.

## Rules: the stock a part is cut from

`sheet::SheetMetalRules` (`src/core/SheetMetal.h`) is one per part, read by
every sheet feature, set with `set_sheet_metal_rules` and read with
`get_sheet_metal_rules` (`Document::SheetMetal()`, serialized as the
document's `sheet_metal` key and omitted when unset; `SetSheetMetalRulesCommand`
makes a change one undo step). It is neither a feature (it is not a step in
the history) nor a configuration (it is not a variant).

| field | unset (0) derives |
|---|---|
| `material` + `gauge` | required; together they fix `thickness` |
| `bend_radius_mm` | the material's minimum: 1 × t for steel, brass, copper; 1.5 × t for 6061-T6 and stainless (6061-T6's is not yet sourced: [known-bugs](../known-bugs.md#sheet-metal-6061-t6s-bend-radius-is-unsourced)) |
| `k_factor` | the R/t band of the material's chart (R < 2t, 2t ≤ R < 4t, R ≥ 4t) |
| `relief` | `rectangular` (also `none`, `obround`, `tear`; anything else refused) |
| `relief_width_mm`, `relief_depth_mm` | max(1.5 t, 1 mm) wide; R + t deep, which reaches past the tangent line |

**Thickness comes from a gauge, not a literal.** The gauge table
(`sheet::GaugeTable`) carries `aluminum_6061_t6`, `steel`, `stainless_steel`,
`brass` and `copper`, each designated either as a US gauge ("16ga") or a
metric thickness ("1.5mm"); a gauge differs by material (16ga steel is
1.519 mm, 16ga aluminium 1.291). An unstocked designation is refused naming
what is stocked. A derived field left unset follows a later correction to the
tables; a set one stays put. `sheet::Validate` refuses an unknown material,
an unstocked gauge, a radius under the material's minimum and a K outside
(0, 1), at the RPC and at load. Every bend records where its K came from
("table:steel R/t<2", "rules", "override").

### Rules that are expressions

The four numbers a part may set -- `bend_radius`, `k_factor`,
`relief_width` and `relief_depth` -- are `ExprValue`s
([variables](../variables/README.md#numbers-that-are-expressions)): each
may be an expression of the part's variables, then the project's.
`set_sheet_metal_rules` takes `bend_radius_mm_expr`, `k_factor_expr`,
`relief_width_mm_expr` and `relief_depth_mm_expr`, evaluated when the call
arrives and refused naming the key when the text does not parse or names a
variable the scope does not have; a number alone unbinds.
`get_sheet_metal_rules` answers each text beside its number. The file
writes `<key>_expr` beside the value (format v222).

A variable change re-evaluates them in `Document::Regenerate`, before the
chain hash the rules seed, so every bend rebuilds. A value
`sheet::Validate` refuses (a radius under the material's minimum, a K
outside (0, 1)) keeps the rules' last good values, and the reason is
reported as `{owner: "sheet-metal rules", error}` in the `owners` list of
the regen every mutating call answers (`set_variable` included). The
thickness takes none: the gauge is the stock, and it fixes the thickness.

## Bends are recorded, never recognised

Every sheet feature knows the bend it made and stamps a `BendRegion` on the
body (`src/core/Feature.h`): axis, INSIDE radius, width along the axis, angle,
K and its source, the panels it joins, its inner and outer faces, and the
frame it was built in (`parentDir`, `upDir`). It is transient — re-stamped on
every regeneration, never serialized, like `threadRegions`. Unfolding is
therefore arithmetic over a graph, with BA = θ·(R + K·t) at each edge, not
surface development of the finished solid, and refold(unfold(x)) == x is a
property the tests assert.

A bend stores no up/down sense: it is only meaningful from one side of the
blank, and `sheet::FoldsToward(bend, side)` answers it for the side asked
about. A hem is `crushed` — folded flat rather than bent round a die — so its
radius is deliberately under the material's minimum and it is exempt from
that check.

## Features

All take `add_feature` / `modify_feature`; the rows in
`src/core/io/SerializeFeatures.cpp` (`kFeatureTypes`) are the parameter
reference.

- **BaseFlange** `{sketch_id, reversed?, regions?}` — the first wall: a closed
  profile thickened by the document's thickness. It takes no thickness of its
  own and refuses a part with no rules.
- **EdgeFlange** `{edges, length, angle_deg, radius?, position?, length_mode?,
  offset_start?/offset_end?, relief?, corners?}` — a wall bent up from STRAIGHT
  edges of a flat panel, one bend per edge (an edge listed twice is flanged
  once, as a fillet blends it once; so for a Hem). `position` (`material_inside`
  default, `material_outside`, `bend_outside`) says which mold line the picked
  edge marks; `length_mode` (`flat` default = the wall past the bend,
  `outside`, `inside`) says which dimension `length` states. Offsets stop the
  bend line inside the panel and cut a relief at each end. Edges meeting at a
  corner whose flanges would overlap are always MITRED: each is trimmed back
  from the corner's bisector leaving a tenth of the sheet between them, since
  touching walls would fuse and no flat pattern could say which bend the
  corner belongs to; each bend records its trim. `corners: closed` carries two
  flanges that only touch at a square corner past it and mitres them shut.
- **Hem** `{edges, length, kind?: closed|open, gap?, outside?,
  offset_start?/offset_end?}` — the same builder at 180°: closed leaves the
  return a tenth of the sheet off the panel. A hem running the full length of
  a wall meets the wall beside it and fuses, so a hemmed tray insets its hems.
- **Jog** `{sketch_id (one line on a panel), height, angle_deg?, flip?}` — two
  opposing bends across the whole panel; the far side is carried, never
  turned. A height under (2R + t)(1 − cos a), what the two bends alone climb,
  is refused.
- **Fold** `{sketch_id (one line on a panel), angle_deg?, radius_mm?,
  position?, side? | side_at_mm?, reverse?}` — a bend made in sheet that is
  already there; [below](#fold).
- **CornerRelief** `{body_id, style?: circular|rectangular, size?}` — a cut at
  every corner where two bends cross, centred where their bend lines meet,
  sized R + t unless given, through the panel and up to where the walls start.
  Three bends at one corner, or two at other than a right angle, are refused.
- **NormalCut** `{sketch_id, body_id?, regions?}` — a cut through the sheet
  NORMAL to the metal wherever it lands: square to a panel, radially through a
  bend. An extrude cut goes along the sketch normal and comes out slanted on a
  bend. Each outline point is carried along the sketch normal to the sheet (a
  panel in front of the sketch plane wins over a nearer one behind it), laid
  on the blank, cut and folded back. All regions of the sketch are cut in one
  call, sharing one ray intersector, and the part is unfolded only when some
  outline lands off a single planar face — a hole in one panel is placed
  straight back onto it and stays an exact circle. An outline that lands on
  two separate parts of the sheet is refused.
- **FlatPattern** `{body_id, fixed_face?}` — the blank: the body laid flat,
  REPLACING its shape rather than adding a second body (a folded part and its
  blank occupy the same space). It must come after every feature that
  reshapes the sheet. Author it SUPPRESSED and unsuppress it in a
  configuration (conventionally `Flat`), so switching folded/flat is a
  configuration switch with one undo stack. A cut across a bend that the
  flat pattern cannot carry is refused, not dropped; flattening a blank twice
  is refused.
- **Unfold** `{body_id, fixed_face?, bends?}` / **Refold** `{unfold_id}` — lay
  chosen flanges' bends flat in place, draw and cut in the flat, fold back.
  Every crease carries its parent's placement, so a Refold needs nothing
  measured off the flat geometry. A cut that does not go right through the
  sheet, or material added to a bend, is refused.

### Fold

A line sketched on a flat panel, and the material on one side of it turned
about that line (`FoldFeature`, `sheet::Fold` in `src/core/sheet/Unfold.cpp`).
Where the panel's outline or a cut-out interrupts the line, each stretch
frees its own piece: a tab cut on three sides inside a plate folds on its own
and the plate round it stays flat, and a blank drawn flat is folded up one
line at a time.

| field | what | unset derives |
|---|---|---|
| `sketch_id` | a sketch holding exactly one line, drawn on a flat face of a sheet body (`sheet::PanelUnder`, which a Jog uses too) | required |
| `angle_deg` (+`_expr`) | the bend's sweep, (0, 180] | 90 |
| `radius_mm` (+`_expr`) | inside radius; under the material's minimum is refused, as a flange refuses it | the rules' `EffectiveBendRadius()` |
| `position` | where the line sits in the bend: `centerline` (the crease the flat pattern draws), `bend_start` (the fixed side's tangent line), `bend_end` (the far one) | `centerline` |
| `side` | `left` or `right` of the line, seen from the sketched face walking from its first point to its second | resolved ONCE, the first time the fold sees its sheet, to the side whose freed material is less (the tab, not the plate round it), and stored: a later edit that changes which side is larger does not move the fold |
| `side_at_mm` | `[x, y]`, a point in the sketch's plane on the side that turns: what a click sends. Resolved to `side`, stored as that, never written; a point on the line, or off the sheet, is refused with the document unchanged | |
| `reverse` | fold away from the sketched face | false |

K is the rules' for the radius, with its source on the bend, and the
allowance `BendAllowance(angle, R, K, t)`: the fold has no number of its own.
The same fold on 16 and 10 gauge takes each gauge's radius and allowance.

**Chords.** The bend zone is the band of the panel one allowance wide, placed
about the line as `position` says, on the line carried on both ways. Its
connected pieces of panel material that reach BOTH of its edges are the
chords (taken from the faces in the sketched plane carried one thickness
down, so the start of a bend already on the panel's edge, whose metal dips
into the sheet's thickness there, is not mistaken for panel). The line picks
every chord its two points span any part of, each whole, and the picked
chords must part the panel into one fixed piece and the pieces on the
folding side. Each freed piece turns on its own bend spanning the chords it
hangs from, a gap between them (a hole on the line) a cut-out across that
bend. Pieces whose spans interleave along the line -- a line across a whole
plate through the root of a tab cut in it frees the plate's far side and the
tab -- turn as ONE bend, since two bends over one stretch of one axis cannot
be. Bends are ordered along the line from its first point.

**Building.** Each bend is written as a crease lying flat (its allowance,
identity `parentPlace`, the frame from the line and the face's normal, or
the opposite face's with `reverse`) and folded by `FoldCrease`, the step
`Refold` takes per crease: the band of flat stock wrapped into the bend by
the neutral-axis map, the freed piece swung round it, every bend it carries
moved with it. A fold is that step applied to a crease nobody unfolded, so
the blank of a folded sheet is the sheet it was folded from, and
`FlatPattern`, `Unfold`/`Refold`, `NormalCut`, `list_bends`,
`check_sheet_metal`, the DXF and a drawing's bend table read the recorded
bends unchanged. A body with bends laid flat is refused, as a Jog refuses
it; a panel of an already folded body is fine (a tab folded out of a
flange's wall, and a flange on a folded tab's edge, both unfold).

**Names.** A bend's own faces are `<id>/bend<k>/face<i>`, k from 1 along
the line. The fixed piece keeps every name it had; a face or edge the fold
cuts across keeps its name there and gains `;F<id>.<k>` on freed piece k
(`topo::DecodeElementName` reads it as a move, like a Jog's `;S<id>`);
everything on a freed piece the fold does not cut keeps its name, so a hole
in a tab is the same hole standing up. The fold's own splits and fuses leave
no `;M<id>` modifiers. The bend's `innerFace`/`outerFace` are its cylinders
and `parentFace`/`childFace` the faces it joins, which `check_sheet_metal`
names.

**Refusals**, each naming the measured value and what would be accepted:
no rules; the sketch missing, failed, or not exactly one line; the line not
on a flat panel; bends laid flat; an angle outside (0, 180]; a radius under
the material's minimum; an unknown `position` or `side` (naming the
accepted ones); and

- the line not crossing the sheet: "the nearest material along it is X mm
  past its first point";
- picked chords that do not part the panel ("the material runs round the
  end of the line"), with how far past each end the line would have to reach
  for the chords it then picks to part it;
- a cut ending inside the bend zone (a slot's end, a tongue): its position,
  and how far to move the line toward the side that folds or away from it
  for the zone to clear it -- the metal beside it would tear;
- less than one thickness of flat sheet past the zone on either side: "this
  bend needs N mm there: P mm of its allowance on that side, plus one
  thickness";
- the turned piece running into the rest of the sheet (a non-destructive
  common before the fuse), with the overlap and where. On a flat plate this
  cannot happen -- every piece one fold frees turns about one axis, by one
  angle, away from the plate -- so it is a fold against a wall standing on
  the plate, or a fold of a part already folded.

What a shop would question but a brake can make (a leg under the die's
minimum, a hole near the bend) stays a finding of `check_sheet_metal`.

**The tool** (`###tb_fold`, `part.sheet_fold`, after Jog) opens on the
newest sketch holding only one line, at the rules' radius (the tool window
says which number and where it came from), a square bend and the side that
frees less. The viewport draws each bend's zone and the line; an arc at the
bend drags the angle, snapping as every angle drag does, with the freed
pieces following; and a click on the sheet on one side of the line turns
that side -- "Other side" in the tool window, done by pointing. The tool
window also holds the line, Angle, Radius, Position and "Fold away".

### How the unfolder works

`src/core/sheet/Unfold.cpp`. Per bend: split the body at the bend's two
tangent planes into the child side, the bend's own slab and everything else
(which stays with the parent, so a panel beside an inset flange is kept; of
the region between the tangent planes only what is joined to the bend is
the bend's, so the plate beyond a folded tab's slot stays plate);
rotate the child side into the parent's plane and slide it out by the
allowance; replace the slab with the IDEAL flat strip minus what had been cut
from the ideal bend, carried across by the neutral-axis map. In a cylinder's
(angle, height) parameters that map is affine, so an outline wraps onto the
inner and outer cylinders as exact curves joined by radial walls; bend to
flat, it is sampled and laid down as a polygon. A slanted cut (a mitre, a
corner relief) is carried by its trace on the NEUTRAL surface. A jog puts new
bends between the base and older flanges, so which piece a bend folds from is
settled by where its material is once every cut is made. Refold runs the same
steps backwards from the ideal annular sector.

The blank carries its own element names, not the folded body's: every face is
named, but a reference taken on the folded part is not the same reference on
the blank.

## NormalCut shares its ray tree and unfolds only when it must

`sheet::NormalCut` takes every profile of the sketch in one call, with one
`CutShared` context for the feature: one `IntCurvesFace_ShapeIntersector` ray
tree, and the flat pattern behind a lazy `Flat()`. Where an outline lands is
decided from the sample hits themselves (the outline is sampled every 0.25 mm);
all samples on one planar face is a panel cut with no unfold, and `Flat()` is
forced only when they are not. `tests/test_normal_cut.cpp` asserts twelve holes
in one sketch give the same body as twelve sketches cut one at a time.

## Checks and data

- `check_sheet_metal {part?, body?}` — `bend_below_minimum_radius`
  (re-checked, since changing the gauge leaves existing bends where they
  were), `flange_below_minimum_height`, `missing_bend_relief`,
  `hole_too_close_to_bend`, each naming the element and required-versus-actual.
  `not_checked` names bend length against the brake and back-gauge
  collisions, which need a shop profile the build does not have.
- `list_bends {part?, body?, side?}` — a row per bend as DATA: radius against
  the material minimum (`below_minimum`, hems exempt), angle, width, K and its
  source, the allowance once flattened, the axis, and `folds` up/down for a
  given side. Its count equals `check_sheet_metal`'s `bends_checked`.
- Both answer from any tab when given a part.

## DXF and drawings

`export_dxf {path, part?, body?, mark?}` (`src/core/io/ExportDxf.cpp`) writes
the flat body's blank as R12 ASCII in millimetres (`$INSUNITS 4`): the outline
and every hole on `CUT` as LINE / ARC / CIRCLE with arcs kept as arcs, each
crease on `BEND_UP` or `BEND_DOWN` (dashed, UP as seen from the side the first
flange was picked on), and `mark` as a TEXT entity on `MARK` inside the blank
for the laser to etch. The outline is the boundary of every face in the
blank's plane, not of its largest face alone: an unfolded part's side is the
base, each bend's strip and each wall as separate coplanar faces, and an edge
two of them share is a seam inside the blank, not a cut (writing the largest
face's wires cut the base out and dropped every flange; reading our own DXF
back in found it, `tests/test_sketch_import.cpp`). The body must BE flat — activate the configuration
that unsuppresses its FlatPattern first. R12 has no ELLIPSE, so a curve that
is neither line nor circle (an angled cylinder's trace) is written as line
segments.

On a drawing, a view's `configuration` names a configuration of its part
(`DrawingView::configuration`, `src/core/drawing/DrawingDocument.h`); the view
draws a copy of the part evaluated in it, cached per (document,
configuration) in `AppState::configuredSources`, so the model tab's active
configuration never changes. A view square onto the blank draws each crease
as a chain line (`CurveKind::Bend`) in the GUI, SVG and PDF. A note with
`table: bends` is a bend table whose rows come from `drawing::BendTableText`
at every resolve. RPCs: `configuration` on `add_drawing_view` /
`set_drawing_view` / `list_drawing_views` (which reports `bend_lines`), and
`table` on `add_drawing_note`.

## Code and GUI

`src/core/SheetMetal.{h,cpp}` (rules, gauge table, bend arithmetic),
`src/core/sheet/` (`Flange` — one builder for every wall bent up from an edge,
so a flange and a hem cannot disagree about a bend; `Unfold`; `Check`),
`src/core/features/{BaseFlange,EdgeFlange,Hem,Jog,Fold,CornerRelief,NormalCut,FlatPattern,Unfold,Refold}Feature`,
`src/core/io/ExportDxf.{h,cpp}`, `src/remote/RemoteSheetMetal.cpp`
(`set_sheet_metal_rules`, `get_sheet_metal_rules`, `check_sheet_metal`,
`list_bends`, `export_dxf`).

GUI: the ten tools are one toolbar group (`###tb_base_flange` …
`###tb_refold`, commands `part.sheet_*`). The Variables panel's Sheet Metal
section (`src/ui/SheetMetalPanel.cpp`) shows the stock with each derived number
labelled by its source, the four settable ones as fields
(`###sm_bend_radius`, `###sm_k_factor`, `###sm_relief_width`,
`###sm_relief_depth`) that keep an expression typed into them and refuse a
value the material cannot take, a "Show the blank" toggle that activates a `Flat`
configuration unsuppressing the flat pattern (creating it if absent), and one
row per bend. Accepting the Flat pattern tool does the authoring above in one
undo step: the feature goes in suppressed, and a `Flat` configuration that
shows it is added and made active, so the blank stays on screen as previewed
and the base configuration stays folded (`ShowTheBlank`,
`src/ui/PreviewFeature.cpp`).

## Tests

`tests/test_sheetmetal.cpp` (gauges, K bands, allowance, derived rules,
validation, round trip, undo), `test_flange.cpp` (with a flange and a hem
on an edge a cut shortened), `test_hem_jog.cpp`
(hem, mitred corners, jog), `test_corners.cpp` (closed corners, relief,
non-square refusals), `test_unfold.cpp` (the blank's outline and thickness
over one bend, a tree and a chain; naming; configuration switch; up/down),
`test_part_exprs.cpp` (a bend radius bound to a variable rebuilding the bend,
one the material refuses keeping the last good one), `tests/part_exprs_smoke.py`
(the same over the RPC), GUI `sheetmetal/a_bend_radius_takes_an_expression`,
`test_refold.cpp` (the involution, holes across creases, refusals),
`test_fold.cpp` (a strip's outside dimensions against the bend deduction at
45, 90 and 135 degrees, table and rules K; the blank of a fold and the
unfold/refold round it; a tab in a plate, two tabs, a line across a whole
plate; names; every refusal by its words, and the stated move building;
defaults on two gauges; a fold on a flange's wall and a flange on a folded
tab), `test_normal_cut.cpp` (with a cut a hair from a bend line, flattened
and unfolded, and one through a whole bend that stays one bend),
`test_sheetmetal_check.cpp` (every violation kind, and
a clean part firing none), `test_flat_drawing.cpp`, `tests/sheetmetal_smoke.py`
(the RPC surface end to end, DXF re-read for its layers), and the
`sheetmetal/*` and `defaults/*` GUI cases for the tools
(`sheetmetal/fold_turns_a_tab`: the side clicked, the arc dragged, one undo
step).

## Not built

- Recognising sheet metal in an imported solid (the reverse of recording
  bends).
- A brake or shop profile: bend length against the brake, back-gauge
  collisions, tonnage.
- Bend lines as a 3D overlay on the folded part; the bend list and the DXF
  carry the same facts.
- Plate that is cut but never formed: the gauge table is a brake's, and stops
  at 5 mm.
