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
| `bend_radius_mm` | the material's minimum: 1 × t for steel, brass, copper; 1.5 × t for aluminium and stainless |
| `k_factor` | the R/t band of the material's chart (R < 2t, 2t ≤ R < 4t, R ≥ 4t) |
| `relief` | `rectangular` (also `none`, `obround`, `tear`; anything else refused) |
| `relief_width_mm`, `relief_depth_mm` | max(1.5 t, 1 mm) wide; R + t deep, which reaches past the tangent line |

**Thickness comes from a gauge, not a literal.** The gauge table
(`sheet::GaugeTable`) carries `aluminum`, `steel`, `stainless_steel`,
`brass` and `copper`, each designated either as a US gauge ("16ga") or a
metric thickness ("1.5mm"); a gauge differs by material (16ga steel is
1.519 mm, 16ga aluminium 1.291). An unstocked designation is refused naming
what is stocked. A derived field left unset follows a later correction to the
tables; a set one stays put. `sheet::Validate` refuses an unknown material,
an unstocked gauge, a radius under the material's minimum and a K outside
(0, 1), at the RPC and at load. Every bend records where its K came from
("table:steel R/t<2", "rules", "override").

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
  edges of a flat panel, one bend per edge. `position` (`material_inside`
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

### How the unfolder works

`src/core/sheet/Unfold.cpp`. Per bend: split the body at the bend's two
tangent planes into the child side, the bend's own slab and everything else
(which stays with the parent, so a panel beside an inset flange is kept);
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
for the laser to etch. The body must BE flat — activate the configuration
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
`src/core/features/{BaseFlange,EdgeFlange,Hem,Jog,CornerRelief,NormalCut,FlatPattern,Unfold,Refold}Feature`,
`src/core/io/ExportDxf.{h,cpp}`, `src/remote/RemoteSheetMetal.cpp`
(`set_sheet_metal_rules`, `get_sheet_metal_rules`, `check_sheet_metal`,
`list_bends`, `export_dxf`).

GUI: the nine tools are one toolbar group (`###tb_base_flange` …
`###tb_refold`, commands `part.sheet_*`). The Variables panel's Sheet Metal
section (`src/ui/SheetMetalPanel.cpp`) shows the stock with each derived number
labelled by its source, a "Show the blank" toggle that activates a `Flat`
configuration unsuppressing the flat pattern (creating it if absent), and one
row per bend. Accepting the Flat pattern tool does the authoring above in one
undo step: the feature goes in suppressed, and a `Flat` configuration that
shows it is added and made active, so the blank stays on screen as previewed
and the base configuration stays folded (`ShowTheBlank`,
`src/ui/PreviewFeature.cpp`).

## Tests

`tests/test_sheetmetal.cpp` (gauges, K bands, allowance, derived rules,
validation, round trip, undo), `test_flange.cpp`, `test_hem_jog.cpp`
(hem, mitred corners, jog), `test_corners.cpp` (closed corners, relief,
non-square refusals), `test_unfold.cpp` (the blank's outline and thickness
over one bend, a tree and a chain; naming; configuration switch; up/down),
`test_refold.cpp` (the involution, holes across creases, refusals),
`test_normal_cut.cpp`, `test_sheetmetal_check.cpp` (every violation kind, and
a clean part firing none), `test_flat_drawing.cpp`, `tests/sheetmetal_smoke.py`
(the RPC surface end to end, DXF re-read for its layers), and the
`sheetmetal/*` and `defaults/*` GUI cases for the tools.

## Not built

- Recognising sheet metal in an imported solid (the reverse of recording
  bends).
- A brake or shop profile: bend length against the brake, back-gauge
  collisions, tonnage.
- Bend lines as a 3D overlay on the folded part; the bend list and the DXF
  carry the same facts.
- Plate that is cut but never formed: the gauge table is a brake's, and stops
  at 5 mm.
