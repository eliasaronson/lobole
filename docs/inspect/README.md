# Inspection

The "is it right?" toolset: measuring, mass properties, looking inside,
checking for clashes and floating parts, checking the B-Rep, and comparing
against a reference. Every inspection a person does by clicking, an agent can
do over RPC with the same numbers, and every verdict says how it knows.

Core code is `src/core/Measure.{h,cpp}` (measurement, contact, sections,
comparison, geometry checks) and `src/core/MeshCollide.{h,cpp}` (triangle
clash tests). The RPCs are in `src/remote/RemoteInspection.cpp`; the measure
tool's GUI in `src/ui/MeasureActions.cpp` and `src/ui/MeasureOverlay.{h,cpp}`.
`describe {method}` is the authority for any RPC's parameters and response
shape; this page is what they mean.

Two rules run through all of it:

- **A number says what it is of.** `0.000 mm` between two edges that meet at
  a corner is true and useless; "touching — 90° between the face normals" is
  the same geometry, said. Every readout carries its basis, units and
  qualifier.
- **A verdict says whether it was checked.** A sweep that ran out of budget
  is not a clean sweep. Verdict RPCs attach a `receipt`
  ([../scripting/rpc.md](../scripting/rpc.md#what-a-response-carries)) whose
  standing is `unverified` whenever an oracle could not run.

Kernel queries never modify their arguments: every boolean in `Measure.cpp`
goes through `BuildQuery`, which runs it non-destructive, because an
OpenCASCADE boolean writes p-curves onto its arguments' edges and two threads
querying pairs that share a body otherwise corrupt the heap.
`test_conventions.cpp` fails on a boolean built any other way.

## Measure

**RPC.** `measure {a, b?, kind?, distance_kind?, mode?, part?}`. Each side
names one thing: `{feature_id, element}` for a face or edge,
`{feature_id}` for a whole body, or `{occurrence: id|name|"Unit/Part"}` for a
placed part; each side may carry its own `part` or `occurrence`, so one
measurement can span two parts or two occurrences.

- `{a}` alone: that thing's own measure — face area, edge length, the shape
  kind, radius and diameter for a round face or edge; a body reports volume,
  surface area and bounding size.
- `{a, b}`: distance, the closest points, a signed `delta`, and
  `overlap_volume` / `clash` for two whole shapes.
- `distance_kind`: `minimum` (default), `maximum` (the clearance question),
  `centres` (hole spacing — a circle's or bore's true centre, not a point on
  its rim).
- `kind: angle | both` adds the angle. Between two planar faces it is taken
  between the outward normals (adjacent box faces read 90°, opposite ones
  180°); when either direction's sign is arbitrary, as a line edge's is, it
  is unsigned in 0–90°. `basis` says which. An element with no direction (a
  sphere, a spline) refuses rather than inventing an axis.
- `mode: mesh` takes the minimum distance off the triangles, ± the
  `tolerance_mm` it reports (the two meshes' deflections summed), for pairs
  where the exact extrema search is pathological (every face pair equally
  far, nothing prunes).

**GUI.** The Measure tool (toolbar, `Ctrl+Shift+M`) works on part, assembly
and CAM job tabs. It picks faces, edges, whole bodies, whole occurrences, and
corners.

- It **seeds from the selection**: select two faces, arm it, read the answer.
- **One pick** reads out that element's own measure; **two** their distance
  and angle; **more than two** of one kind their total length or area (a mix
  refuses and says why).
- **The readout leads with the number that is not degenerate.** Touching or
  intersecting elements lead with the angle; parallel ones lead with the
  separation ("parallel, 45.000 mm apart"); otherwise distance then angle.
- **It is drawn on the geometry** on the on-top overlay channel, so a
  thickness through material is visible: a dimension line with arrowheads,
  witness lines, and the value in a pill at its midpoint; an arc for an angle.
  For parallel elements the dimension is placed by projecting one element's
  centroid onto the other, not at the arbitrary point pair the extrema solver
  returns. The picture and the text come from one computation
  (`UpdateMeasureReadout` fills both), so they cannot disagree.
- **Hover** measures the element under the cursor against what is picked
  (or reads its own measure with nothing picked), drawn dimmed; cached on the
  element, the pick and the document version, not recomputed per frame.
- **Pin** keeps a measurement drawn while you take the next; pinned rows
  highlight their dimension on hover and drop themselves when an edit breaks
  a reference. Measurements are session state, never saved.
- The readout uses the document's units. **Copy value**, and **create a
  variable from a measurement** (written in storage units, so it parses as an
  expression).
- Rows name what was picked in words ("Board — top face", "Bracket — Ø25
  bore"); the stable name is the tooltip.
- A **corner** is where two named edges of a face meet. A raw vertex has no
  stable name (the element map names faces and edges), but the pair of edges
  does, so a corner is stored as the two edge refs. One corner reports its
  coordinate; two report the exact distance between them. A pair of edges
  that meets at two vertices (a cylinder's seam) is refused as ambiguous.
- A tab switch, entering sketch mode, or re-arming clears the tool.

`measure_smoke` covers the RPC; a parity phase in `remote_smoke` asserts
that a measurement taken by clicking two occurrence faces and the same one
taken over RPC give the same numbers.

## Mass properties

`mass_properties {body?, part?}` → per body `volume`, `surface_area`,
`center_of_mass`, `inertia` (3×3 about the centre of mass), `material`,
`density`, `mass_g`, and a measured `bbox`; plus `total {volume,
center_of_mass, mass_g}`. Density comes from the body's material; a body with
no material has no mass rather than an assumed density, and the BOM names it
in `warnings`. On an assembly tab without `part` it aggregates every visible
occurrence in world space. Its bbox is measured off the surfaces; `get_state`'s
is a cheap conservative bound that can read well over on a swept surface.

The Properties dock shows the same numbers for the active part (cached by
document version), and a centre-of-gravity marker can be drawn in the
viewport.

## Sections

**The number.** `section {base_plane | origin+normal, offset?, part?,
regions?, loops?}` returns the cross-section area where a plane cuts the
bodies — on an assembly tab, every visible occurrence in place. `offset` is
mm along the normal; omitted, the cut goes through the middle of what is
shown and the chosen offset comes back. `regions: true` adds each connected
island `{area, centroid}` and `region_count`: the total cannot say whether
material is where you think it is, an island can. `loops: true` adds each
island's outline as line/arc/other segments, for recovering the shape of a
part you did not draw.

**The view.** The Section tool (toolbar, both part and assembly tabs) keeps a
list of up to four cuts. A fragment survives only on the kept side of every
enabled cut, so two cuts make a quarter-section. Per cut: a source plane —
a base plane, a datum plane of the active part, or a picked flat face — an
offset with a drag arrow in the viewport, two tilt angles (degrees, about the
source plane's in-plane axes), flip, and visibility.

- The tilt pivots about the cut's offset point, so tilting does not also
  slide the cut off the model.
- Datum- and face-sourced cuts resolve live every frame: move the datum and
  the cut follows. A source that stops resolving stops its cut — the row says
  "source missing — this cut is off" and `list_sections` reports
  `source.resolved: false` — rather than cutting along some other plane.
- Cut faces are capped per body in the body's own colour, each cap trimmed
  by the other cuts. The shader clip previews every frame; the caps (an OCC
  half-space boolean per body) rebuild only when a slider or the drag arrow
  is released.
- Picking honours the cut: nothing on the removed side is pickable, and a ray
  that meets the plane inside a solid hits that body's cap (`RayHit::cap`). A
  cap is a body or occurrence pick, never a face — it has no element name —
  so the tools that need a face refuse it.
- Cuts persist in the project envelope (`project.json`, `"sections"`), not
  per document: a cut is a view of the whole project and applies on whichever
  tab is shown.
- The limit of four is the shadow pass's: it carries its planes in push
  constants beside a `mat4`, and four `vec4` planes fill Vulkan's guaranteed
  128 bytes (`kMaxClipPlanes`, `src/render/ClipPlanes.h`).

RPCs: `add_section`, `list_sections`, `update_section`, `remove_section`
(`{all: true}` clears), and `set_section` as the one-cut shorthand. The
default keeps the half opposite the plane's normal (z ≤ offset for `xy`);
`reverse` keeps the other half, and the response says which as `keeps`.

## Clashes, contact and floating parts

`interference`:

- `{a, b}` (bodies or occurrences; a part inside a sub-assembly by path,
  `"Gangway/Board 19"`, or id chain `"2/17"`) →
  `{clash, volume, distance, state: clash | touching | clear}`. `touch_tol`
  (mm, default 0.001) splits touching from clear, and the contact boolean
  runs with the same 1 µm fuzz, so parts designed to touch report contact,
  not a sliver of clash. Assert every bolted joint `touching` and every
  designed clearance by number; a clash-only check passes exactly when
  nothing touches.
- With no pair, an assembly sweep. It is bounded: pairs whose boxes do not
  overlap cost one box test; the rest are checked closest-first up to
  `max_pairs` (4000) or `deadline_s` (20, checked between pairs — one boolean
  can overrun it). The response says what it checked, box-cleared and left
  unchecked, and a truncated sweep's receipt is `unverified`.
- Each clash carries `centroid_mm` and `bbox_min_mm` / `bbox_max_mm` of the
  overlap in world millimetres — where the solid-in-solid is, not just which
  two parts — in the named-pair check, the sweep and `contact_report` alike,
  from the intersection solid the exact check already builds. A mesh-mode
  sweep knows only that two sides overlap, so its entries carry
  `within_min_mm` / `within_max_mm` instead: the region the two sides' boxes
  share, which holds the overlap somewhere. Motion studies use the same names
  ([assemblies](../assemblies/README.md#motion-studies)).

`contact_report` is the connectivity gate: `connected` (one component = one
structure; anything in its own component is floating), `components`, per-pair
`state`/`distance`, `near_misses` (gap ≤ `near_tol`, default 1 mm — almost
always an arithmetic slip), `clashes`. On an assembly with mates it adds
`unrealized_mates` and `unmated_contacts`. On a part tab it runs over the
part's bodies, which is how a body floating inside one part is found; there
an overlap joins two bodies into one component, like a weld. A pair whose
shapes carry more than `exact_face_budget` faces between them (default 20000
— an imported mesh is one face per triangle) is not started and is listed in
`not_checked`: the deadline cannot preempt a boolean already running, so one
such pair would hang the sweep.

### Mesh mode

`interference {mode: mesh}` (and the motion study's default check) answers
"do these overlap" off triangles with an AABB tree per shape
(`src/core/MeshCollide`). It is about ten times faster than the exact boolean
and is for questions asked over and over.

- It never reports a distance or a volume: those would be the
  tessellation's, not the part's. It returns the verdict and `deflection_mm`;
  a pair closer than the deflection may read either way, so anything that
  must be certain re-runs exact.
- The deflection is 0.2% of each body's bounding diagonal, the renderer's
  own, so a 2 mm screw and a 2 m frame get the same relative fidelity.
- A body wholly inside another is a clash, found by a ray-parity containment
  test that runs only when the two boxes nest. The test starts from a point
  on the inner body's surface, never its box centre: a bearing cap straddling
  a shaft has its box centre inside the shaft.
- The mesh cache keys on placement (`CADShape::PlacementId`), not geometry:
  two occurrences of one part share a shape and sit in different places.
- The triangle test uses the separating-axis theorem with the two face
  normals, the nine edge-edge crosses, **and** each triangle's in-plane edge
  normals. Without the last six, coplanar triangles have no separating axis
  and read as intersecting however far apart they are — and a pin whose end
  cap is flush with the face it passes through is coplanar.

`check_geometry`'s floating-bodies warning uses the mesh answer too: it is a
yes/no riding on a cheap check, and an exact contact on concentric bodies (a
spoked wheel) costs hundreds of times the validity check.

## Geometry checks

`check_geometry {body?}` → `{ok, valid, body_count, feature_errors, bodies[]
{valid, bad_faces, bad_wires, bad_edges, reasons}, receipt}`, plus
`connected` / `body_components` on a multi-body part and `stale_derived` for
derived parts whose source moved. Validity is `BRepCheck_Analyzer`; the
reasons are scanned at the face, wire, edge, shell and solid levels, so a
shape whose faces are each fine but whose shell is not ("not connected")
still names why. A boolean or sweep feature runs the same check on its own
result and fails, naming the reasons, rather than handing an invalid solid to
every later measurement — `CADShape::IsValid` is only a null check.

## Finding and naming geometry

- `list_elements {body?, part?, max_faces?, made_by?, role?, entity?, via?}`
  — stable face and edge names per body, the handles `measure`, fillets and
  mates use, each with the feature that made it (`made_by`) and its `role`;
  the filters ask for a feature's faces in a role (a pocket's floor), and an
  empty answer says `why` ([toponaming](../modeling/toponaming.md)).
  `unnamed_faces` counts faces the element map never named; zero is what
  makes the list complete.
- `query_geometry` — declarative filters (`geometry_type`, `normal`/`axis`
  with `angle_tol_deg`, `radius` ± `radius_tol`, area and length bounds,
  `near_point`) and `expected_count`, which turns the query into a one-call
  assertion. Covers unnamed elements too.
- `frame` — the origin and axes of a named datum frame, a face, a circular
  edge or an occurrence, in part space or (with `occurrence`) world space:
  exactly the frame a mate connector with the same spec uses.
- `geometry_snapshot` / `validate_snapshot` — frozen, revision-bound edge
  measurements ([../scripting/sdk.md](../scripting/sdk.md#measurement-snapshots)).

## Mutating responses and pictures

Every geometry-mutating RPC already returns a `digest` — per-body volume,
bbox and solid count, and a delta — and a `regen` summary
([../scripting/rpc.md](../scripting/rpc.md#what-a-response-carries)), so the
measure step of execute → measure → render needs no extra call.

`render_sheet {path, views?, focus?}` renders up to six named views (default
iso/front/right/top) into one PNG with a numeric footer — bbox, volume, mass,
and on an assembly the `contact_report` summary — so the numbers sit in the
same image as the pixels. `focus` frames one occurrence in every view. It
needs a renderer (GUI or `--offscreen`).

## Comparing against a reference

`compare_geometry {golden_path}` (a STEP or BREP) against the active part, or
`{a, b}` bodies or occurrences. Each side is fused first, so overlapping
bodies are not counted twice.

- `metrics: "all"` (default) runs three booleans (common and both cuts) for
  `iou`, `dice`, `precision`, `recall` and the diff volumes, plus plain
  verdicts ("Under-generating: 17.8% of the reference is missing").
- `metrics: "volume"` runs no boolean and returns size, placement and
  topology: `volume_ratio`, the volumes and surfaces, `center_offset`,
  `bbox_iou`, and the face and edge census. The overlap fields are absent,
  not zero — a zero IoU would mean "no overlap at all". The verdict states
  the census without judging it (a STEP round trip splits faces), and never
  claims a shape match. On parts carrying toroidal fillets the booleans cost
  seconds; volume mode costs milliseconds. `missing_path`, `extra_path` and
  `materialize` need the booleans and are refused with it.
- Volume is a weak oracle: a frustum faked from stacked discs matches a real
  one on bbox, volume and centre of mass. The census separates them
  (`cone:1 plane:2` against `cylinder:6 plane:7`), which is why volume mode
  still returns it.
- `align: true` moves the candidate's centre of mass onto the reference's
  first, so the answer is about shape rather than position (a correct re-draw
  built about its own origin otherwise scores a low IoU against a reference
  placed elsewhere). `center_offset` still reports the shift.
- `missing_path` / `extra_path` export the diff solids as BREP.
  `materialize: true` draws them over the part — missing red, extra green —
  as a renderer overlay that is never pickable, exported or saved and drops
  itself on any edit or tab switch; `clear_geometry_delta` removes it.

`compare_smoke` and `test_measure.cpp` cover both modes.

## Tolerances and fits

`check_fit`, `set_tolerance`, `list_tolerances`, `stack_up`, `check_fits`
and the print allowance (`set_print_process`, `get_print_process`,
`print_allowance`) are described in [../tolerances/README.md](../tolerances/README.md). Declared clearances
between occurrences (`add_assertion`, `verify_assertions`) are assembly
state.

## Before delivering a model

[review-checklist.md](review-checklist.md) is the pre-delivery list, one RPC
per box, with a symptom → cause → fix table. The MCP `modeling-workflow`
prompt points agents at it.

## Query booleans are non-destructive

An OpenCASCADE boolean modifies its arguments. Computing an intersection
computes p-curves, and by default OCC stores them on the input edges
(`BRep_Builder::UpdateEdge`) instead of on a copy. For a modelling feature that
is the right default, because a boolean consumes its operands. For a
measurement it is a write to the document.

The clash sweeps run pairs on worker threads, and one body appears in many
pairs, so two threads asking "does A touch B?" and "does A touch C?" both
write A's edges. The result is heap corruption, detected later at an unrelated
`malloc` (`malloc(): unaligned tcache chunk`), or only an UNKNOWN pair,
because a corrupted edge makes the next boolean on that shape fail.

Warming caches single-threaded (the rule in `core/Parallel.h`) does not cover
this. That rule is for idempotent lazy fills such as bounds and
triangulations; a boolean writes data specific to the pair.

So every boolean in `src/core/Measure.cpp` is default-constructed and passed
to `BuildQuery`, which calls `SetNonDestructive(Standard_True)`: OCC copies a
sub-shape instead of updating it in place. The two-shape constructor
`BRepAlgoAPI_Cut op(a, b)` builds immediately, so any option set afterwards is
silently ignored; `tests/test_conventions.cpp` fails on that constructor form
in Measure.cpp.

The convention is scoped to Measure.cpp. `core/ElementHistory.cpp` and feature
execution (`core/features/*.cpp`) build destructive booleans, and neither runs
on a worker thread. The exception is a refused body boolean's fuzzy retry
(docs/modeling/README.md#a-refused-boolean-gets-one-fuzzy-retry), which runs
on operands the document still owns and so goes through
`cad::BuildNonDestructive`. A sweep that calls into anything else has to carry the
rule with it. `AssemblyStudy`'s fuse copies its shapes and runs
`BOPAlgo_Builder` non-destructive for the same reason. Anywhere else in `src/`, a boolean that must not write to its inputs is
built with `cad::BuildNonDestructive` (`src/cad/NonDestructiveBoolean.h`),
which sets the flag before the build; a second convention test fails on the
two-shape constructor followed by `SetNonDestructive`, where the flag comes
too late to do anything.

`tests/sweep_parallel_smoke.py` diffs a one-thread report
(`DESIGNER_SWEEP_THREADS=1`) against an all-thread one byte for byte; it cannot
see this bug class, because the answer is usually still right. ThreadSanitizer
can: `tools/ci-local.sh --tsan`.

## Interference is box-pruned

`interference` and the contact queries reject a pair by bounding box before
any exact boolean: almost every pair in a real assembly is nowhere near its
partner, and an all-pairs sweep is quadratic.

## Positions are double

The model is double ([../conventions.md](../conventions.md#the-model-is-double)):
a box placed at 164.8 has a face at the double nearest 164.8, and `Measure`
reports it as that, in doubles, like sketch planes and sketch geometry
([../sketching/README.md](../sketching/README.md#plane)). A filter on a
COMPUTED position still wants a tolerance sized to its magnitude, since
arithmetic rounds: a centroid, an intersection or a solved placement is
exact to rounding, not to the decimal a person would write.

## How it is tested

`test_measure.cpp`, `test_meshcollide.cpp` (agreement with the exact boolean
on overlapping, clear and enclosed pairs; deflection scaling; the
placement-keyed cache; a pin in a clearance hole reads clear), and the
headless smokes `measure_smoke`, `contact_smoke`, `compare_smoke`,
`query_smoke`, `strict_inputs_smoke`, `papercuts_smoke`,
`sweep_parallel_smoke` (a one-thread sweep and an all-thread sweep give
byte-identical reports). GUI: the `measure/*` and `section/*` cases in the
GUI suite, and the measure parity phase in `remote_smoke`.

## What it does not do yet

- Pick a raw vertex (corners cover the cases people ask for).
- `measure` and `interference` each resolve an occurrence reference (id,
  name or `"Unit/Part"` path) to world-placed bodies with near-identical
  code. They should be one function; interference's version shares a
  prefetched body map with the contact sweep, so merging them is its own
  change with its own test run.
- A face-sourced section on an assembly tab: the face's plane is part-local
  and needs occurrence-aware resolution, so such a cut reports
  `resolved: false` there.
- Exclude components from a section (keep fasteners whole, as drafting
  convention does); the set is derivable — a part whose only feature is one
  Fastener.
- Hatched cut faces (drawings section independently).
- Draft, wall-thickness or curvature analysis.
- Persistent requirements with revision-bound evidence
  ([../roadmap.md](../roadmap.md#6-the-agent-interface)).
