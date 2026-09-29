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
  `landscape`, and a title block (`project`, `document`, `author`, `date`,
  `notes`). The border margin (10 mm) and title-block rectangle (160 × 32 mm,
  bottom-right) come from `ChromeOf`, the one source the viewport, both
  exporters and the validator share.
- **Views** (`DrawingView`): a source document by stable project-document id
  (part or assembly — a rename cannot break a view), a named orientation, a
  scale (with an optional expression), a sheet position in mm, `showHidden`,
  `quality` (`exact` or `fast`), `hidden` occurrences, a part
  `configuration`, a `rotation_deg`, and `baseViewId` for a projected child.
- **Dimensions** (`DrawingDim`): a kind, a view, one or more `ProjRef`s and a
  label offset in sheet mm.
- **Notes** (`DrawingNote`): text at a sheet position, optionally with a
  leader to projected geometry, or a generated table.

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
occurrenceId, edge | silFace, end}`: a sharp or smooth edge by its source
edge's `ElementRef`, a silhouette by its generating face's, `occurrenceId`
for an assembly source, `end` 0 for the whole edge or 1/2 for an endpoint.
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
(`Ø20 H7`, or an explicit `+0.05/-0.02`), read from the part, so it follows
edits; a `radius` does not, because an ISO class states how a diameter may
vary. See [../tolerances/README.md](../tolerances/README.md).

Text goes through `core::FormatLength` / `FormatAngle` in the **source
document's units**: a drawing of an inch part is an inch drawing. Lengths
carry no suffix, so the title block's UNITS row (`core::UnitsLabel`) states
what the bare numbers are; angles keep `°` (or `rad` on a radians document).
The sheet takes its units from the first view's source
(`ui::ActiveDrawingUnits`); a sheet mixing unit systems gets that one.

A dimension without an `offset` is placed clear of the views, the title
block and the labels already on the sheet. Every visible circle gets a
centre mark, drawn by the same rule in the viewport and both exporters.

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

`check_drawing` (`src/core/drawing/DrawingValidate.cpp`) reports stale
views, dangling dimensions, views off the border or over the title block,
overlapping views and labels, and notes out of bounds, with a receipt.

## Export

`DrawingExport::SaveSVG` and `SavePDF` (`src/core/drawing/DrawingExport.cpp`)
are hand-written, GUI-free, and consume the same projected structures as the
viewport, so export is testable headless without rendering. SVG uses mm user
units, polylines and `<text>`; PDF is one vector page with base-14
Helvetica, mapping Ø, ° and × to WinAnsi. Both write the border, the title
block with its UNITS row, visible and hidden line work, dimensions, notes and
centre marks. `export_drawing {path}` and the toolbar's Export share
`ui::ExportActiveDrawing`; the format is chosen by extension and anything but
`.svg` or `.pdf` is refused.

## Where the code is

| | |
|---|---|
| model, enums, sheet chrome | `src/core/drawing/DrawingDocument.{h,cpp}` |
| HLR, cache, cost | `src/core/drawing/DrawingProjection.{h,cpp}` |
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

- Section, detail, broken and crop views; exploded views.
- Balloons and a BOM table on the sheet.
- Multi-sheet drawings.
- GD&T, weld and surface-finish symbols; templates and styles (one ISO-style
  look, 3.5 mm text).
- DXF of a sheet (DXF exists for flat patterns only).
- Dragging a view on the sheet in the GUI, and folding a child from the GUI
  (`base_view` is RPC-only); adding free notes in the GUI (Add bend table is
  the only note the panel adds).
- Numbering bend lines to match the bend table's rows.
- Parallel projection of a sheet's views; reuse of cached visibility across
  differently placed instances of one shape.
