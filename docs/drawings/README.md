# Drawings

2D drafting output: a sheet of hidden-line projected views of a part or an
assembly, with driven dimensions, callouts, notes and a title block, exported
as SVG or PDF. The workflow, in the GUI and over RPC, is in
[guide.md](guide.md); this page is how it works.

## The model

A drawing is the third document kind, `DocKind::Drawing`, beside parts and
assemblies (`DrawingDocument`, `src/core/drawing/DrawingDocument.h`). It
neither runs features nor places occurrences; it projects other documents
onto one sheet. A project holds any number of drawings.

- **Sheet**: size (`a4`, `a3`, `a2`, `a1`, `a0`, `letter`, case-insensitive),
  `landscape`, and a title block (`project`, `document`, `material`,
  `scale`, `author`, `date`, `notes`). The border margin (10 mm) and
  title-block rectangle (160 × 48 mm, eight 6 mm rows, bottom-right) come
  from `ChromeOf`, the one source the viewport, both exporters and the
  validator share; the rows themselves from `DrawingExport::TitleBlockRows`.
  A blank MATERIAL is the first view's part's material grade (`6082-T6`
  from `aluminum_6082_t6`; a generic material prints its name, `ALUMINUM`,
  which says no grade was chosen —
  [grades](../rendering/materials.md#grades)), a blank SCALE its
  scale (`2:1`, with `UNLESS SHOWN` when another view differs), filled by
  `ui::DrawingSheetForOutput` for the screen and the file alike; anything
  else (a plate's thickness and standard) is typed. NOTES carries the
  general tolerance. A misspelt title-block key is refused.
- **Views** (`DrawingView`): a source document by stable project-document id
  (part or assembly — a rename cannot break a view), a named orientation, a
  scale (with an optional expression), a sheet position in mm, `showHidden`,
  `quality` (`exact` or `fast`), `hidden` occurrences, a part
  `configuration`, a `rotation_deg`, `baseViewId` for a projected child, a
  `section` and a `detail` (below).
- **Dimensions** (`DrawingDim`): a kind, a view, one or more `ProjRef`s and a
  label offset in sheet mm.
- **Notes** (`DrawingNote`): text at a sheet position, optionally with a
  leader to projected geometry, or a generated table; drawn as text, a
  balloon, a datum feature symbol or a feature control frame (`style`).

Views, dimensions and notes share one never-reused id counter. Undo is a
full-document snapshot stack (`DrawingHistory`, `ui::SnapshotDrawing`).
Unknown sheet sizes, orientations, dimension kinds and note tables are
refused by name, on load and over RPC.

**Projected geometry and measured values are derived, never stored.** A
saved drawing is its setup: `drawings/<name>/index.json` with `kind:
"drawing"`, the sheet inline and notes inline, and one file per view
(`views/`) and per dimension (`dims/`) in the directory format
(`src/core/io/Serialization.cpp`). Opening a drawing re-projects it.

### Orientations

The seven orientations — `front`, `back`, `left`, `right`, `top`, `bottom`,
`iso` — come from the one named-view table `conv::kNamedViews`
(`src/core/Conventions.h`) through `OrientationAxes`, so a drawing's front
is the view cube's front and the `view` RPC's. The world is Z-up with front
looking along +Y (the camera sits at −Y). Top and bottom are rolled so the
part's front edge is at the bottom of the sheet, which is what lets a top
view sit directly above its front view. `tests/test_drawing_hlr.cpp` checks
the orientations against the model's up axis.

### Rotation

`rotation_deg` turns a view on the sheet, counter-clockwise about its own
view direction (`ViewAxes`, which every projection of a view goes through
instead of `OrientationAxes`). It is for a part modelled lying down — a
turned piece along X draws upright as its front view at 90 — without a
rotated copy of the model. It turns the picture, not the view: a front view
at 90 still looks along +Y, and a child folds from it only along an axis the
two still share. The panel's view row offers 0/90/180/270
(`Rotate`); `add_drawing_view` and `set_drawing_view` take any angle.

### Section views

A view with a `section` {`point`, `label`} is cut by the plane through
`point` (world mm, the source's frame) square to the view's own direction:
what lies between the reader and the plane is taken away
(`drawing::CutBody`, `src/core/drawing/DrawingSection.{h,cpp}`, a
non-destructive boolean against a box on the reader's side), and the rest is
projected as any view is. The cut is per body, in its own frame, and kept
while body and plane are the same (`CachedCut` in `ui/DrawingActions.cpp`,
1024 entries): a new cut every update would also be a new shape to the
projection cache, and a head's two assembly sections re-cut 120 bodies per
update until the cache held them all.

The cut's elements are not the source's, but a dimension still names the
source's: `SectionCut` maps each of its edges and faces back to the source
edge or face it is (a shortened edge is still that edge), and an edge the
plane made to the face it was cut out of — the wall of a bore in section is
named by the bore's face, as its silhouette would be, so a `diameter` across
the two walls resolves. A body wholly in front of the plane is left out.

On the section itself, the faces the cut exposed are hatched (thin lines at
45°, 2.5 mm on the sheet, alternating direction from part to part so two
parts that touch read as two; `drawing::HatchRegion`, even-odd) and the view
is captioned `SECTION A-A`. Every other view of the same source that sees the
plane edge-on draws it as a cutting-plane line — a chain line, thick at its
ends, arrows pointing the way the section looks, the letter at both ends —
once: on the view the section folds from when that view sees it, else on
each that does. All of it is `SheetView::extras`, the `SheetGraphics` the
viewport and both exporters draw the same way.

### Detail views

A view with a `detail` {`view`, `center` [x, y, z] world mm, `radius_mm`,
`label`} draws part of another view larger: what the PARENT projects — its
source, orientation, turn, section, hidden occurrences and configuration,
read at every update, so an edit to the parent carries over — at the
detail's own scale, kept inside the circle about `center`
(`ProjectDrawingView`, `ClipToDetail` in `ui/DrawingActions.cpp`). The
circle is a world point and a model radius, like a section's point, so it
stays on its feature when the parent moves or is rescaled; the detail's
`position` is where the circle's centre lands on the sheet
(`DrawingFoldedPosition`). Curves are clipped at the circle (a circle cut
by it is an arc, `closed` false, so it marks no centre), the hatching and
thread lines with them, and the projection is the parent's, so it is a
cache hit. The detail draws its boundary in thin line and is captioned
`DETAIL B (5:1)`; the parent draws the circle and its letter
(`DetailCircles`). A detail of a section is captioned as the detail.

Its dimensions measure the part as any view's do (the value divides by the
detail's scale). `add_drawing_view {detail, scale, position}` refuses the
parameters that are the parent's (`source`, `orientation`, `section`, ...),
a detail of a detail, and a radius of 0; `label` defaults to the sheet's
next free view letter (`ui::NextViewLetter`: A to Z without I, O and Q,
skipping those its sections and details use).

### Threads

A thread nothing modelled — a Thread feature's cosmetic thread, a Hole's
tapped fit, both written on the body as `ThreadMark`s — is drawn by ISO
6410's convention in thin line (`ThreadDecorations`, `ui/DrawingActions.cpp`):
seen down its axis, three quarters of a circle at a tapped hole's major
diameter (a screw's root) wherever the drilled rim shows; seen from the side,
two lines along the threaded length — solid where a section cuts the thread
open (the hatching stops at the drill, the thick line), dashed where the hole
is hidden, and left off when the view hides hidden lines. A modelled thread
draws its own geometry instead.

### Projected children

A view with a `baseViewId` folds from its base: when the two views map the
same world direction to sheet X, the child's X is locked to the base's, and
likewise for Y (`ui::DrawingFoldedPosition`). Front and top share X; front
and right share Y; iso shares neither and is placed freely. The coordinate
along the fold axis is the child's own.

## Projection (HLR)

`drawing::ProjectView` (`src/core/drawing/DrawingProjection.{h,cpp}`) runs
OpenCASCADE hidden-line removal over all the view's bodies at once, so
occlusion is global. Two modes, chosen per view and never substituted for
each other:

- **`exact`** (default): `HLRBRep_Algo` on the curves. Every output curve
  carries a `SourceRef` to the 3D edge that produced it, which is what a
  dimension attaches to. Conics stay symbolic (`curveType`, `conicCenter`,
  `conicRadius`, and `closed` for a full sweep, which is how a bore rim is
  told from a corner fillet); other curves are discretized at 0.05 mm
  deflection in model space.
- **`fast`**: the same algorithm over the shapes' triangulations. Orders of
  magnitude cheaper and a *look*: polylines with no source references, so
  nothing on it can be dimensioned.

**Silhouettes** are the curves a projection creates (a shaft's side view).
A curve is a silhouette when it resolves to no model edge — by identity,
then by shared stored curve and location, because the outliner splits input
edges. HLR's `OutLine` flag is not the signal: it is also set on a face's
real edges when the face is seen edge-on. A silhouette's generating face is
the HLR face whose wire blocks contain it, and it is referenced as that face.

A **seam** — where a closed surface's parameters wrap, a model edge that is
no edge of the part — is drawn only where it lies on its face's horizon,
and is then that face's silhouette, named by the face (`SeamFace` in
`DrawingProjection.cpp`). So a cylinder's two side lines are one face's two
silhouettes whichever side carries the seam, and a seam seen across its face
(a chamfer cone's, from above) draws nothing rather than a tick.

Exact HLR is priced by **surface complexity, not part count**: `Hide()`
intersects every projected edge against every face, and a swept B-spline
surface (a spring, a pipe, an imported driver) is orders of magnitude dearer
than planes and cylinders. Sixteen bored boxes are milliseconds a view; one
four-turn helical spring is seconds; one or two such parts can be nearly all
of an assembly view. The levers are `hidden` (leave occurrences off this
view), `quality: "fast"`, and `update_drawing {budget_s}`, which bounds the
whole update: a view still running when the budget is spent stops, keeps its
previous curves and says so. OCC's `Hide()` has no cancel hook, so the
projection drives HLR pairwise itself to be stoppable. Failures are caught
per view; a failed view keeps its last good geometry and an error string.

The projection is split so that the expensive half could leave the main
thread: `drawing::ProjectView` needs only its input shapes, while
`AssembleSheetView` (`src/ui/DrawingActions.cpp`), which maps the curves back
to stable element names, needs the live documents and the sheet layout. A
worker could not project the documents' own shapes — `fast` HLR meshes them,
writing triangulation into the shared faces — so it would project deep
copies, which give the same curves and edge names
(`tests/test_drawing_hlr.cpp`). The update still runs on the main thread.

### Staleness and updates

Each projected view records its source's version (`DrawingSourceVersion`).
When the source changes the view is **stale** until updated. Projection is
explicit — `update_drawing`, the toolbar's Update views — and also runs when
a drawing tab is entered and when a view is added with automatic placement.
It never runs per frame.

`ExportActiveDrawing` calls `UpdateDrawingRenders`, so an exported file cannot
be stale, yet HLR is skipped for unchanged views: `state.drawingProjectionCache`
checks each view's leaf geometry and placements.

### The incremental projection cache

`drawing::ProjectionCache` (session-local, owned by `AppState`, cleared when
the project is replaced) makes repeated and incremental updates cheap; first
projection costs the same as uncached.

- A **complete view result** is kept in model coordinates, visible and
  hidden curves both. Placement, scale, folding and joining curves to
  `ElementRef`s happen afterwards (`src/ui/DrawingActions.cpp`), so moving or
  scaling a view, editing annotations, or toggling hidden lines does no HLR
  work.
- On a miss in exact mode a fresh OCC algorithm rebuilds outlines, then
  **self-visibility** per body and **directed pair visibility** per body pair
  are replayed from the cache where their keys match. A missing pair starts
  from the body's self-visibility, independent of other occluders, and hidden
  intervals are merged through OCC's tolerance-aware edge status, so removing
  a body reveals edges again.
- Keys hold shape identity (`IsEqual`), revision (`Document::Version()`),
  exact placement, view axes, mode and deflection. Every update gathers the
  current leaf shapes and placements, so a nested part edit invalidates even
  when the root assembly's version did not move. Source ids are rebound to
  the current input list; a positional attribution is never replayed. Local
  edge signatures (bounds, tolerances, curve type, sampled points) are
  checked before intervals are replayed.
- Bounds: 16 complete views and 4,096 visibility entries within a 64 MiB
  payload budget, most-recently-used first; visibility entries are evicted
  before views. The accounting excludes OCC geometry held by shape handles,
  so it is not a process-memory cap.
- Cancellation never publishes a completed view; finished self/pair entries
  stay reusable.

The cache is single-threaded, and the views of a sheet project one after
another. Projecting them in parallel needs isolated shape snapshots first:
an OCC algorithm writes to the shapes it reads.

### What a view costs

The projection times its self and pair work per `InputBody::groupId` (the
occurrence, set by the drawing actions) and returns
`Stats::costBySource`. **A pair's cost counts for both of its members**,
because the question is "what would leaving this one out save", and leaving
out either member avoids that pair. So the shares overlap and can sum past
the total; they are not normalised. Pairs inside one occurrence (an imported
part made of several solids) are charged to that occurrence once.

`list_drawing_views` reports each view's `cost` (`outline_ms`, `hide_ms`,
`self_ms`, `pair_ms`, call and hit counts, `projection_hit`, `cache_bytes`,
and `by_occurrence`, dearest first, named in the ids `hidden` accepts). A
cached re-projection reports zero, which is what it spent. When a view took
over a second and one occupant carried at least half of it, `warnings` names
it and the two parameters that act on it (`hidden`, `quality`).
`add_drawing_view` in its auto-placed form returns `cost_ms`,
`by_occurrence` and the same warning.

## Dimensions and annotation

A dimension references projected geometry through `ProjRef {viewId,
occurrenceId, edge | silFace, end, extreme}`: a sharp or smooth edge by its
source edge's `ElementRef`, a silhouette by its generating face's,
`occurrenceId` for an assembly source, `end` 0 for the whole edge or 1/2 for
an endpoint, or `extreme` (`left`, `right`, `top`, `bottom`) for the curve's
point furthest toward that side of the sheet. That last is the top of a
curved outline — a knight's head, a dome — which is neither end of its curve
nor its midpoint; `list_drawing_geometry` gives each curve's `bbox` so a
caller can see where it reaches.
`drawing::ResolveDim` (`src/core/drawing/DrawingDimension.{h,cpp}`) matches
the refs against the view's current curves on every update and measures. A
reference that no longer resolves is **dangling**: flagged, drawn in the
error style in the viewport, left out of the export, and never rebound to a
different edge.

Dimensions are **driven**: measurements, read-only. There is no driving a
model from the sheet. Kinds: `distance`, `distance_x`, `distance_y`,
`radius`, `diameter`, `angle`, and three callouts whose text comes from the
model:

- `hole_callout`: the `HoleFeature` that drilled the picked circle.
- `pattern_callout`: two or more picked circles, their count and uniform
  pitch.
- `fastener_callout` (the toolbar's Part note): the standard part an
  occurrence's Fastener feature describes ("ISO 4762 M6x50"). Assembly views
  only.

A `diameter` dimension carries the tolerance the model records for that bore
(`Ø20 H7 (+0.021/0)` — a class with its ISO 286 limits, which are what a
shop holds — or an explicit `+0.05/-0.02`), read from the part, so it
follows edits; a `radius` does not, because an ISO class states how a
diameter may vary. See [../tolerances/README.md](../tolerances/README.md).
On a circle it is radial; on two parallel lines of one ref — a shaft's
silhouettes, a bore's walls in section — it is a LINEAR dimension across
them with a Ø, its line moved along the walls by the offset's component
along them.

A tapped `hole_callout` reads the thread the hole's bores carry
(`FindThreadForHole`): its designation and class, how deep it is threaded,
and the drill under it — `3/8-16 UNC-2B DEEP 12.7, DRILL Ø7.94 DEEP 12.7`.

Text goes through `core::FormatLength` / `FormatAngle` in the **source
document's units**: a drawing of an inch part is an inch drawing. Lengths
are written to a fixed number of decimals for the unit (mm 2, in 3) and a
deviation to one more (`core::FormatDeviation`) — three significant figures
wrote a 37.34 hole spacing as 37.3. Lengths carry no suffix, so the title
block's UNITS row (`core::UnitsLabel`) states what the bare numbers are;
angles keep `°` (or `rad` on a radians document). The sheet takes its units
from the first view's source (`ui::ActiveDrawingUnits`); a sheet mixing unit
systems gets that one.

A linear value — `distance`, `distance_x`, `distance_y`, and a Ø across
two lines — is written along its dimension line, above it, readable from
the bottom or the right of the sheet (ISO 129-1, method 1): a vertical one
is turned 90° and stands left of its line, and none breaks its line
(`ResolvedDim::aligned`, `textAngle`; `drawing::AlignedLabel`). Its box —
what placement avoids and `check_drawing` judges — is the turned text's
(`DimLabelBox`, from the four corners `DimTextQuad` gives), and its
knockout is turned with it. A radius, an angle and a callout keep a
horizontal value at their leader's end. `SheetText` carries the turn, so
the viewport (`AddTurnedText`), the SVG (`rotate`) and the PDF (a text
matrix) set it the same way.

A dimension without an `offset` is placed clear of the views, the title
block and the labels already on the sheet. A Ø across two lines is placed
as the linear dimension it is: its line square to the walls, beyond the
view along them on the side their ends reach nearer, in lanes outward
until its value is clear. Every circle seen whole
(`HasCentreMark`: visible and closed — an arc of a circle mostly hidden
marked a centre in empty air) gets a centre mark, drawn by the same rule in
the viewport and both exporters. A label is centred on its position, its
text knocked out of what it sits on (`TextKnockout`), and a leader stops at
the side of the text facing it (`LeaderEnd`) rather than running through
the words to their centre.

**Notes** are text with `{{expression}}` resolved against project variables
(or the source part's, with a view), and an optional leader to a projected
edge. A note with `table: "bends"` is a **bend table** of its view's part,
regenerated at every resolve in the configuration the view draws: one row per
bend with angle, inside radius, K factor and bend allowance, plus UP or DOWN
when the view looks square onto a flat blank — UP is relative to the side the
view looks from. The DXF writer answers the same question from the side the
first flange was picked on, so the two can label a crease differently
(`src/core/io/ExportDxf.h`). A view of a flat blank also draws its bend
lines as chain lines. See [../sheetmetal/README.md](../sheetmetal/README.md).

### Parts lists and balloons

A note with `table: "bom"` on an ASSEMBLY view is its parts list: one row
per part the view draws — `ITEM`, `QTY`, `PART`, `KIND` (`MADE`, `PRINTED`
for a part with a print process, `BOUGHT` for a standard part), `MATERIAL` (its
grade, `6082-T6`, or a generic material's name), unit `MASS g` — from `ui::ComputeBomOf` over the source's leaves less the
view's `hidden` occurrences (so the camera a head carries is not listed as
part of the head). Any note whose text holds tabs is a ruled table (rows by
`\n`, cells by `\t`, a row with no tab spans it), standing on its
bottom-right corner at the note's position — on the title block.

A note with `style: "balloon"` and no text, its leader on an occurrence of
an assembly view, shows that occurrence's item number in the sheet's parts
list of the same assembly (else in the view's own), so a balloon and its row
cannot disagree; one pointing at an occurrence the list leaves out is
refused. It is drawn as the number in a circle with a dot on the part. A
`style: "datum"` note is a datum feature symbol: its letter in a box, the
leader ending in a filled triangle on the feature. A leader ref may name an
`extreme` or an `end`, and an extreme is honoured on a circle too (a balloon
on a bearing seen end-on landed in its bore). Both, and tables, are
`NoteGraphics` — the one description every surface draws. The table is
computed once per set of leaves (part, version, configuration, material,
occurrence): seventeen balloons on a 60-part head each re-ran the masses.

### Balloon layout

`layout_balloons {views}` (`ui::LayoutDrawingBalloons`) puts a balloon on
every row of the sheet's parts list, round the views it is given:

- **Which view.** Each row on the view that draws the most of it in solid
  line, among its occurrences.
- **Where the leader ends.** On a visible edge of the part: points its
  refs resolve to — a line's middle, a curve's ends, its four extremes —
  that lie on a VISIBLE piece of that edge and at least 0.8 mm from any
  other part's lines, since a dot where two parts meet says neither. An
  end costs more than a middle (a vertex is shared).
- **Where the balloon goes** (`drawing::LayoutBalloons`,
  `src/core/drawing/DrawingBalloons.{h,cpp}`). Slots on three rings round
  the view's box, a balloon's width apart, kept where a balloon touches
  nothing else: the other views, the title block, tables, every label and
  the balloons already placed. The parts and the slots are matched in the
  order they go round the view (the cheapest monotone match over every
  rotation of both orders), each part's anchor re-picked nearest its
  balloon, and then any two leaders that cross, or a leader through
  another balloon, swap balloons. Swapping two crossing segments shortens
  their total, so the untangling ends.
- The balloons already on those views are replaced, so it runs again
  after the model moves; one undo step. It answers `placed`, `unplaced`
  (a row no view shows an edge of clear of its neighbours) and
  `crossings`, which is 0 when it succeeded.

`check_drawing` reports any two balloon leaders that cross
(`balloon_leaders_cross`), however they were placed.

### Feature control frames

A `style: "fcf"` note is a feature control frame (ISO 1101; ASME Y14.5
draws the same symbols): `fcf` {`symbol`, `tolerance_mm`, `diameter`,
`modifier`, `datums`} (`FeatureControlFrame`, `DrawingDocument.h`). The
fourteen symbols — straightness, flatness, circularity, cylindricity,
profile of a line and of a surface, parallelism, perpendicularity,
angularity, position, coaxiality, symmetry, circular and total runout —
each say what the frame may carry (`GeoSymbolRules`, one table): a form
tolerance takes no datum, orientation, coaxiality, symmetry and runout
need one, position and profile may stand alone; a Ø zone only where the
feature can be an axis, a material modifier (Ⓜ, Ⓛ) only where it can be a
size. `ValidateFcf` refuses anything else, and at most three datums, each
once.

**The datums are datum features.** A `datum` note's text is its letter:
one capital, not I, O or Q (ISO 5459: they read as 1, 0 and 0), once per
sheet. Every letter a frame names must be one of them, or the frame does
not resolve (`ResolveDrawingNote`, naming the datums the sheet has), and
removing a datum a frame names is refused naming the frame.

It is drawn as the ruled frame, two lines of lettering tall: the symbol as
line work (the PDF's Helvetica has no glyph for any of them), the
tolerance with its Ø and its circled modifier, then one compartment per
datum, primary first, knocked out of what it stands on (`NoteGraphics`).
With a `ref` it stands at its position with a leader from the end nearer
the feature, an arrow on it; with `dim` (a dimension id) it stands under
that dimension's value with no leader, which is how a size's position
tolerance is written. A frame's tolerance is written in the source's
units to a deviation's decimals (`0.02`).

`check_drawing` (`src/core/drawing/DrawingValidate.cpp`) reports stale
views, dangling dimensions, views off the border or over the title block,
overlapping views and labels, notes out of bounds, a frame naming a datum
the sheet lacks, and crossing balloon leaders, with a receipt. A
label sits ON a view when it crosses a line the view draws or lies inside
its outline (a line found every way from its centre) — not merely inside its
box, which flagged every callout beside a round part; inside a section it
stands on knocked-out hatching, which is how a section is dimensioned.

## Export

`DrawingExport::SaveSVG` and `SavePDF` (`src/core/drawing/DrawingExport.cpp`)
are hand-written, GUI-free, and consume the same projected structures as the
viewport, so export is testable headless without rendering. SVG uses mm user
units, polylines and `<text>`; PDF is one vector page with base-14
Helvetica, mapping Ø, ° and × to WinAnsi, centring text by Helvetica's
widths (`drawing::TextWidthMM`, which also sizes a table's columns), and
turning a line of text with a text matrix (the SVG with `rotate`). A
`SheetGraphics` knockout is filled white before the rest of it. Both
write the border, the title block, visible and hidden line work, every
view's `extras` (hatching, cutting planes, captions, threads), dimensions,
notes with their balloons, datums and tables, and centre marks. `export_drawing {path}` and the toolbar's Export share
`ui::ExportActiveDrawing`; the format is chosen by extension and anything but
`.svg` or `.pdf` is refused.

## Where the code is

| | |
|---|---|
| model, enums, sheet chrome | `src/core/drawing/DrawingDocument.{h,cpp}` |
| HLR, cache, cost | `src/core/drawing/DrawingProjection.{h,cpp}` |
| section cut, hatching | `src/core/drawing/DrawingSection.{h,cpp}` |
| balloon layout | `src/core/drawing/DrawingBalloons.{h,cpp}` |
| dimension and note resolution, bend tables | `src/core/drawing/DrawingDimension.{h,cpp}`, `DrawingCallout.{h,cpp}` |
| sheet-space render structures | `src/core/drawing/DrawingRender.h` |
| validation | `src/core/drawing/DrawingValidate.{h,cpp}` |
| SVG / PDF | `src/core/drawing/DrawingExport.{h,cpp}` |
| update, placement, folding, export glue | `src/ui/DrawingActions.{h,cpp}` |
| panel, sheet viewport, dimension picking | `src/ui/DrawingPanel.cpp`, toolbar in `src/ui/Toolbar.cpp` |
| RPCs | `src/remote/RemoteDrawings.cpp` |

## Tests

- `tests/test_drawing_hlr.cpp` (`[drawing][hlr]`): every curve of a box
  traced to a source edge, silhouettes attributed to their face, blind-bore
  hidden lines, instances hiding each other, closed conics, cost attribution
  per source, orientations against the up axis. `[hlr-cache]`: source
  rebinding, hidden-line filtering without HLR, movement, removal and
  reordering, coincident instances, view changes, cancellation, poles and
  rotated occluders — each compared against uncached recomputation curve by
  curve. Manual benchmarks: `[hlr-cache-benchmark]`, and
  `[hlr-project-benchmark]` with `DESIGNER_HLR_PROJECT=<copy of a project>`.
- `tests/test_drawing.cpp` (`[drawing]`): model ids, round-trips and
  goldens, formatters, dimension resolution and callouts, SVG and PDF output
  (parsed, never byte-compared, since HLR discretization is not stable across
  OCC versions), layout validation, auto-placement, note expressions,
  refusals of unknown enums.
- `tests/test_drawing_sheet.cpp` (`[drawing]`): a section cut names its
  edges by the source's (the bore's walls by its face), even-odd hatching,
  seams as silhouettes, balloons/datums/tables as `NoteGraphics`, leader
  ends, the tapped callout with its thread, class limits, a Ø across two
  walls, an extreme on a circle, centre marks, the label-on-view rule, the
  title block's rows, the v147 golden and its refusals.
- `tests/test_drawing_frames.cpp` (`[drawing]`): what each symbol may
  carry and every refusal, a frame's compartments and its leader, aligned
  values (their turn, their box, the exporters' rotate and text matrix), a
  Ø across two walls placed along them, the balloon layout on a case a
  column per side tangles, crossing leaders reported, the v151 golden and
  its refusals.
- `tests/drawing_frames_smoke.py`: datum features and the frames naming
  them (a missing datum, a datum named twice or a frame's datum removed
  refused), a frame under a Ø, a detail of a section, a vertical value
  turned, balloons laid out with none crossing and laid out again, an
  ASME B18.3 screw BOUGHT and a 6082-T6 grade in the list and the title
  block, and a reopen.
- `tests/drawing_sheet_smoke.py`: a section view, its Ø across the walls
  with class and limits, a datum, the title block, a parts list numbered
  for its balloons with a hidden occurrence left out, export, and a reopen.
- `tests/test_flat_drawing.cpp`: a view draws its configuration, a dimension
  across a blank reads the developed length, bend tables.
- `tests/test_actions.cpp`: layout reuse and nested-part invalidation through
  the drawing actions.
- `tests/drawing_smoke.py`, `drawing_assembly_smoke.py`,
  `drawing_quality_smoke.py`: the RPC chain headless (create, views, update,
  dimensions, dangling after an edit, cost reporting, `fast`, `hidden`,
  `budget_s`, export).
- GUI: `drawing/build_a_drawing_by_clicking`.

## Not done

- Broken, crop, offset-section and half-section views; exploded views. A
  section plane is square to one of the seven orientations; a detail's
  boundary is a circle.
- Multi-sheet drawings.
- Weld and surface-finish symbols; a datum target; a frame's composite
  (two-row) form, projected zones and datum modifiers; templates and
  styles (one ISO-style look, 3.5 mm text).
- Balloons placed round several views at once as one problem: each view's
  ring is laid out in turn, clear of the ones before.
- DXF of a sheet (DXF exists for flat patterns only).
- Dragging a view on the sheet in the GUI, and folding a child from the GUI
  (`base_view` is RPC-only); adding free notes in the GUI (Add bend table is
  the only note the panel adds).
- Numbering bend lines to match the bend table's rows.
- Parallel projection of a sheet's views; reuse of cached visibility across
  differently placed instances of one shape.
