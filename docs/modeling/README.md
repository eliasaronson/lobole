# Modeling

How a part is built: an ordered list of features that regenerates the part's
solid bodies from scratch, deterministically, on every edit. Sketching (the 2D
profiles features consume), sheet metal and tube frames are their own areas;
this page covers the feature model, regeneration, and the general solid
features.

Topic pages:

- [toponaming.md](toponaming.md) — how a face or edge keeps its name across
  regenerations, which is what every face/edge reference relies on.
- [holes-and-threads.md](holes-and-threads.md) — the Hole feature and what it
  records (fit, tapping), the cosmetic Thread, and thread engagement.
- [derived.md](derived.md) — the Derived feature: another part's bodies copied
  into this one as a snapshot.

## The model

- A **`Document`** (`src/core/Document.{h,cpp}`) holds the parameters
  (variables), the ordered feature list, and the bodies the last regeneration
  produced. Tolerances, studies and CAM setups also live on the document, but
  outside the regeneration chain.
- A **`Feature`** (`src/core/Feature.h`, concrete types in
  `src/core/features/`) has a stable id (`GetId()`), never an index, and
  `Execute(RegenContext&)` which reads and modifies the bodies built so far.
  `RegenContext` exposes only the features *before* the executing one, so a
  reference can only point backwards.
- A **`Body`** (`Feature.h`) is a `CADShape` plus transient data stamped each
  regeneration: `createdBy` (the id of the feature that created it — the
  whole-body identity booleans, patterns, ribs and CAM use), `elementMap`
  (face/edge names, [toponaming.md](toponaming.md)), `material`,
  `appearance`, `reference` (a ghost from a Derived feature), thread regions
  and bend records.
- References between features are stable ids: `sketch_id`, `source_id`,
  `body_id` (a `createdBy`), `ElementRef {feature_id, element}` for a face or
  edge. Every feature reports what it names through `VisitFeatureRefs`, and
  `Document::FirstConsumerIndex` uses that to find a feature's first consumer.
- Every numeric field a person would want to drive is an expression twin
  (`distance` + `distance_expr`). `VisitExprs` evaluates them against the
  parameter table before execution, so the evaluated value is part of the
  feature's JSON and therefore of the cache key. An expression that fails to
  evaluate is a feature error, not a silent fall-back to the literal.
- The kernel is OpenCASCADE behind `CADShape` (`src/cad/CADKernel.{h,cpp}`).
  `CADShape::Volume` is cached by TShape identity (plus the location's scale),
  because every mutating RPC digests every body's volume before and after, and
  an unchanged body — a large Derived snapshot especially — must not be
  re-measured each time. The cache holds the forward figure; a shape reversed
  in place reads it negated, which is what an inside-out solid encloses.

## Regeneration

`Document::Regenerate()` (`src/core/Document.cpp`):

1. Evaluates parameters, then every feature's expressions.
2. Computes a **chain hash** per feature: `h_i = H(h_{i-1}, FeatureToJson(f_i),
   effective suppression)`, seeded with the sheet-metal rules when the part has
   them. The serialized feature is exactly the state that determines its
   output, and chaining makes any upstream change invalidate everything after
   it. Stripped from the hash because they do not change geometry: sketch
   reference images, `appearance`, `material` (both are re-stamped onto bodies
   by a post-pass keyed on `createdBy`). An Import hashes its file's content
   instead of its path, so a save that moves the file into the project is not
   a changed part.
3. **Prefix cache.** Each cache entry holds the hash and the body list after
   that feature. Regeneration restores the bodies at the first divergence and
   executes from there; editing the last feature re-runs one feature. Retained
   feature-internal state (solved sketches, cached datum planes) is safe
   because a matching prefix implies identical inputs — this is why the cache
   is chain-invalidated rather than dependency-tracked. `LastRegenStats()`
   reports `{executed, cached}`; `ClearRegenCache()` resets it.
   `TipChainHash()` is the last entry's hash; baked geometry and the Derived
   stale check key on it (baked geometry is described in the versioning area).
4. **Continue past failure.** A feature that fails records its error, adds
   nothing, and regeneration continues; downstream features see the bodies as
   they were and fail on their own if they needed the missing output. A
   failure after an earlier failure is tagged (`after_earlier_failure` in the
   RPC `regen` block) so a client can point at the first red feature.
   `Regenerate()` returns false if any feature errored.
5. **Suppression.** A feature's `suppressed` flag (serialized) or suppression by the
   active configuration (`EffectiveSuppressed`) skips execution and keeps the
   parameters. A feature referencing a suppressed feature's output fails with
   its normal missing-reference error — the same rule as deletion.
6. **Rollback bar.** `Document::Rollback()` (persisted, -1 = end) executes
   features `[0, rollback)` only; the rest keep their parameters and hold no
   errors. A second, unpersisted marker, `ViewRollback()`, is set by sketch
   edit to the sketch's first consumer so the solid built from the sketch is
   not in front of it while editing; regeneration stops at the earlier of the
   two.

`Document::BulkEdit` defers regeneration while a loader adds many features, so
a load is one regeneration rather than one per feature.

A mutating RPC reports what it broke in its own response (`regen`), not only in
a later `get_state` (`tests/regen_honesty_smoke.py`).

## Features

The authoritative list, with every field, is `kFeatureTypes` in
`src/core/io/SerializeFeatures.cpp`, served as the RPC `describe`
`feature_types` rows. General solid features:

| type | what it does |
|---|---|
| `Box`, `Sphere`, `Cylinder` | primitives; faces named `<id>/face<i>` |
| `Import` | STEP/STL/BREP bodies; centred on their bbox unless `keep_origin` |
| `Sketch` | 2D profile on a base plane, a `DatumPlane` (`planeRef`) or a body face (`face_ref`); see the sketching area |
| `Extrude` | see [Extrude](#extrude) |
| `Revolve` | profile about an `AxisRef` (`x`, `y`, `z` through the world origin, `sketch_line`, `datum_axis`, `custom {origin, direction}`); `angle_deg`, `symmetric`, `reversed`. The axis must lie in the sketch plane and the profile must not cross it; a joint within 1e-4 mm of it is ON it, so an arc may end on the axis |
| `DatumPlane` | `offset` from a plane, `angle` about an axis, or `through_points`; sketches that reference it move with it |
| `DatumAxis` | two points, a sketch line, or a global axis |
| `Frame` | a named datum frame that mates anchor to (assemblies area) |
| `Boolean` | `union`, `subtract` or `intersect` of two bodies by `createdBy`; both operands consumed into one result, so booleans chain. The result must pass BRepCheck and [the boolean check](#a-boolean-is-checked-before-it-is-kept) |
| `Pattern` | `mirror` (about a base or datum plane), `linear`, `circular` of a source; see [Patterns](#patterns) |
| `Fillet`, `Chamfer` | edges by `ElementRef`; blend faces named from the spine edge |
| `Shell` | hollows a body, `open_faces` by `ElementRef` |
| `Draft` | tapers `faces` about a neutral plane |
| `Split` | a body by a face, a datum or an offset base plane |
| `Refine` | merges coplanar faces and boolean seams |
| `Rib` | see [Rib](#rib) |
| `Hole`, `Thread` | see [holes-and-threads.md](holes-and-threads.md) |
| `Curve3D`, `Sweep`, `Loft`, `Pipe` | see [3D curves, sweep, loft and pipe](#3d-curves-sweep-loft-and-pipe) |
| `Derived` | see [derived.md](derived.md) |
| `Fastener` | a generated standard part; only in a part of its own, placed in an assembly (the parts area) |

Sheet-metal features (`BaseFlange`, `EdgeFlange`, `Hem`, `Jog`, `Unfold`,
`Refold`, `FlatPattern`, `CornerRelief`, `NormalCut`) and `Cope` are
documented in the sheet-metal and tube areas.

Body modes: extrude, revolve, sweep, loft and pipe take `mode:
new_body|add|cut`, applied by `ApplyBodyMode` (`src/core/BodyMode.{h,cpp}`),
which propagates element names through the boolean. `add` fuses into the last
body its bounding box overlaps, and every other body the new solid actually
touches joins that one — a neck strung between two turned pieces makes one
body of them, where it used to overlap the second as a separate solid and the
part weighed the overlap twice. A body that only shares a bounding box with it
stays separate. `add` never fuses into a reference (ghost) body, and `cut`
passes through one. A cut warns when it
removes no material, and when it consumes EVERY body: a part left with
nothing is a tool the wrong way round or a boolean the kernel could not do,
and without the warning the first sign is "no bodies" from `mass_properties`
with no feature named. Every add and cut result is checked before it is kept
([below](#a-boolean-is-checked-before-it-is-kept)); a refused one leaves every
body as it was.

### Extrude

`ExtrudeFeature` (`src/core/features/ExtrudeFeature.{h,cpp}`,
`src/core/sketch/SketchProfile.cpp`):

- **Profiles.** Every closed loop of the sketch is classified by containment
  depth: even depth is an outer, odd depth a hole of its immediate container,
  and an island inside a hole is an outer again. Each outer with its holes is
  one prism; the prisms are assembled into one result. Loops that partially
  overlap are refused ("profile loops overlap"). `regions` restricts the
  extrude to picked regions (each named by the sorted entity ids of its outer
  loop); a region that no longer matches a loop is an error.
- **End conditions** (`end_mode`): `blind` (`distance`), `symmetric` (centred
  on the plane), `two_sided` (`distance` forward, `distance2` back),
  `up_to_face` (`end_target`, a **planar** face by `ElementRef`, followed
  across upstream edits), `through_all` (cut only; past every body's extent).
  Every variant builds one prism of the computed length, translated back along
  the normal, so element names are the same as a blind extrude's. A positive
  `distance` runs along the sketch plane's normal; for a feature on the far
  side of a part, face the plane the other way (the SDK's
  `Plane.yz(x, flip=True)`, [axes](../scripting/sdk.md#the-principal-planes))
  rather than passing a negative distance.
- **Thin** (`thin: {thickness, side: out|in|both}`): every loop becomes a wall
  ring (OCC `MakeOffset`), composing with any end condition.
- A cut or add extrude keeps the prism it swept (`ToolShape()`), which is what
  lets a pattern repeat it; a Hole keeps its drilled tool the same way, so a
  bolt circle is one Hole and a pattern.
- A cut that removes nothing, or an add that adds nothing, leaves the body as
  it was, with a warning: the boolean would still imprint the tool's outline
  where the two touch, splitting a face and renaming it.

### Patterns

`PatternFeature` (`src/core/features/PatternFeature.{h,cpp}`) references its
source by feature id. `count`, `spacing` and `angle_deg` are expressions.

- A **body source**: every instance (seed plus copies) is fused and split back
  into connected solids, so touching instances merge and disjoint ones become
  separate bodies. The seed is consumed.
- A **cut, add or Hole source** leaves no body of its own, so the pattern
  repeats the source's tool (`Feature::ToolShape`) and subtracts or fuses each
  transformed copy. In the GUI the pick for this is the cut's tree row with the
  Source slot armed; a body pick resolves to the body's `createdBy`, which is
  the plate, not the cut.
- Every instance carries the source's element names with an instance suffix
  (`4/g7` becomes `4/g7#2`, a mirror's `#m`), so a face of the third copy is as
  referenceable as one of the first.

### Rib

`RibFeature` (`src/core/features/RibFeature.{h,cpp}`) grows a wall from an
**open** sketch profile toward a body: the profile is offset by `thickness/2`
each side into a closed slab, prismed along the sketch normal (`flip` for the
other way) far enough to pass through, cut against the target body, and the
component touching the profile plane is fused in. That component is exactly
the material between the profile and the body wall, so it conforms to curved
walls. A profile that never reaches the body is an error. The target is
`body_id` (a `createdBy`), or the nearest overlapping body when 0.

### Thread

`ThreadFeature` is a cosmetic thread: an annotation on a cylindrical face
that changes no geometry, with pitch and diameters from the ISO metric table
in `src/core/ThreadStandards.{h,cpp}`. Its `modeled` flag (a real helical cut)
is reserved and errors. Details, and how threads engage in interference
checks: [holes-and-threads.md](holes-and-threads.md#thread).

### 3D curves, sweep, loft and pipe

- **`Curve3D`** (`CurveFeature`, `src/core/features/CurveFeature.{h,cpp}`) is
  a free curve that is neither a sketch entity nor a body. Like a datum it
  contributes no body; `Execute` caches an OCC edge and a polyline, and a
  degenerate definition is an error. Kinds (`Curve3DKindName` is the one
  spelling): `fit_spline` (`GeomAPI_Interpolate` through `points`, `closed`
  for periodic), `control_poly` (clamped B-spline over `points` as poles,
  `degree`), `helix` (`origin`, `axis`, `radius`, `pitch`, `turns`,
  `left_handed`, `start_deg` measured from `HelixReference(axis)`), and
  `bent_path` (the corners of a tube's centreline and one bend `radius`, built
  line-arc-line with exact circular arcs; a straight too short for its two
  arcs or a corner that doubles back is refused, and refused where it is
  ADDED: `add_feature` asks `WhyIncomplete`, so nothing is committed for a
  pipe to be swept along). A bent path reports its
  `Bends()` — angle, radius, straight before, roll — and its cut `Length()`.
  RPCs: `curve_points` (the polyline actually built, with arc length) and
  `tube_schedule` (cut list and bend schedule per tube member).
- **`Sweep`** (`SweepFeature`) skins a closed profile sketch along `path_id`,
  a sketch (an open chain of lines, arcs and splines) or a `Curve3D`
  (`ResolveSweepPathWire`), with `BRepOffsetAPI_MakePipeShell` and rounded
  corner transitions. `frame`: `corrected_frenet` (default; cannot self-twist
  on a helix), `frenet`, `fixed`. An optional `guide_id` reorients the section
  along a second curve (`BRepFill_NoContact`).
- **`Loft`** skins `sketch_ids` (two or more sections, in order) with
  `BRepOffsetAPI_ThruSections`. With `path_id` the sections are instead swept
  along that spine by `MakePipeShell` with several sections, and an optional
  `guide_id` scales them to keep contact with the guide
  (`BRepFill_ContactOnBorder`) — `ThruSections` has no guide support.
- **`Pipe`** is a circle of `outer_diameter` (hollow when `wall_thickness` >
  0) swept along a path.

All four share `SweepSectionsAlongSpine` (`SweepFeature.h`). A path or guide
that gives no spine is reported by `SweepPathError`: when the path feature is
itself broken (a bent path edited into one that cannot be bent) the consumer
names it and defers to its error, instead of claiming the path is the wrong
kind of thing.

## A boolean is checked before it is kept

OpenCASCADE can return a boolean result that is not null, passes
`BRepCheck_Analyzer`, and is wrong. The case that found it: an intake flange's
paired-port slot whose round ends were the same cylinders as the two branch
bores (swept round elbows), drawn 1 mm on past the pad face. Drawn from one
face the cut "removed no material"; drawn from the other it kept the bores'
floors inside the slot and stood the slot's far end in the empty bores. That
is the bores turned inside out: a section is solid where the holes are, and
the part came out 13,800 mm3 lighter than any cut could have made it.

`BooleanRefusal` (`src/core/BodyMode.{h,cpp}`) runs on the result of every
`ApplyBodyMode` add and cut and every `Boolean` feature, before anything is
kept:

- **Volume.** Not negative; a cut no larger than its input; an intersection no
  larger than either operand; a union no smaller than its input (the old
  "add LOST material" refusal). Tolerances are relative to the volumes
  compared. These read volumes the callers compute anyway.
- **Shells.** Per solid, exactly one closed shell encloses positive volume and
  every other (a void) negative. A single-shell solid is covered by the
  total's sign; otherwise the shell with the most faces is inferred from the
  total, and the rest go through the volume cache, so a void the boolean did
  not touch costs nothing.
- **Faces.** A cut or a union keeps no face strictly inside its tool, and an
  intersection none outside it. Judged at one interior point
  (`BOPTools_AlgoTools3D::PointInFace`) for every face the boolean made and
  every face of the target it kept whole that has a vertex in the tool's box,
  against the TOOL — a sound solid, and for a feature a small one.
  Classifying against the target instead (hundreds of faces) cost 8% of the
  OM615's build; this costs about 2%.

The refusal is the feature's error, so it reaches the `regen` block of the
call that made it. It names the feature, the operation, the body, both
volumes and a point, and says what usually causes it: a tool that shares a
surface with the body. Stop the tool at the face where the surfaces meet, or
make it clearly larger or smaller than what it coincides with.

The check is not a proof. It samples one point per face, and a target face the
kernel should have split but did not is judged only if a vertex of it lies in
the tool's box.

### A refused boolean gets one fuzzy retry

Most refusals are coincident geometry, which OpenCASCADE's exact booleans
handle badly, and the model is usually reasonable: a slot exactly as wide as
the seat it opens, with its floor on the seat's axis (the cut came back
uncut), or two equal spokes whose axes cross 8 µm apart (the union counted
their overlap twice). So a refused result is not the last word.
`CheckedBooleanNamed` (`src/core/BodyMode.{h,cpp}`) runs the boolean, checks
it, and on a refusal runs it ONCE more with OCC's fuzzy option
(`SetFuzzyValue`), then checks that result the same way. It serves every path
the check runs on: `ApplyBodyMode` add and cut, and the `Boolean` feature
(union, subtract and intersect), where the retry must also pass the feature's
BRepCheck, and a result that fails BRepCheck is retried too.

- **Kept:** the feature carries a warning, so it reaches the `regen` block of
  the call that made it: the operation "needed OpenCASCADE's fuzzy option at
  0.0029 mm", followed by what the exact result got wrong.
- **Refused again:** the refusal is the exact result's, followed by
  "Retried with OpenCASCADE's fuzzy option at X mm (1e-05 of the smaller
  operand's size), and that was refused too:" and the retry's own finding.

**The tolerance** is 1e-5 of the smaller operand's bounding-box diagonal
(`FuzzyTolerance`), so it scales with the feature and stays far below any
dimension of it (0.0029 mm for a 290 mm spoke). The fraction was measured,
because both ways of getting it wrong produce a result that PASSES the check:

- too small, and the kernel still misreads the near-coincidence. The jig slot
  came out 50-70 mm3 heavy at a fuzzy value of 1e-5 to 1e-4 mm, and right
  from 1e-3 mm up;
- too large, and it merges geometry it should keep. The flange and two spokes
  came out right up to 0.01 mm and 14 mm3 light at 0.029 mm (1e-4 of their
  size).

Both operands in those cases are about 290 mm across, so 1e-5 sits inside the
window with room either way. `tests/test_boolean_volume.cpp` pins both volumes
against an independent answer: the jig's section, and the wheel with the spoke
moved 0.01 mm off the crossing, built exactly.

**The retry is non-destructive.** It goes through `cad::BuildNonDestructive`
(`src/cad/NonDestructiveBoolean.h`), which sets the fuzzy value and
`SetNonDestructive` before the build. A refused retry leaves the operands in
the document, and a destructive fuzzy build would have written what it
computed at the fuzzy tolerance onto their edges for every later feature to
inherit.
A test counts the curve representations on the operands' edges across a
retry, and checks the same count sees the exact build's writes.

A retry runs only after a refusal, so a document that builds cleanly pays
nothing for it.

## Bounding boxes are memoized on the shape

`CADShape::GetBoundingBox` is memoized on the `CADShape` itself, one slot per
flavour (conservative, tight). A slot is filled lazily and dropped by
`CADShape::SetShape`, the only thing that replaces `m_shape` (gated by
`tests/test_conventions.cpp`), so an in-place transform cannot bypass it. A
regeneration builds new shapes, which start cold.

A copy made by `Transformed` through quarter turns and a translation (every
basis entry 0 or +-1) carries the tight box, permuted: the permutation is exact,
and CAM frames every body of a job through such moves (laid on a face, flipped,
a placement turned). Any other angle starts cold. The conservative box is not
carried: it is bounded in the frame it was taken in. A job's bodies are warmed
on their SOURCE documents' bodies (`ui::JobBodies`), which outlive the job's
copies, so returning to a job tab framed by quarter turns pays nothing; the
stock and machine boxes a job tab draws every frame are memoized in
`CamTool.cpp` on the job's geometry and the setup less its operations.

A copy at any other angle (a rotary index of 30 degrees) is a new object each
time a job tab frames it, so the TIGHT box of a LOCATED shape is also
remembered beside the object, keyed on its TShape and its location's
transformation: the same TShape at the same placement is the same point set,
so a framing done again reads the box the first one paid for, and returning to
such a job costs what a quarter-turned one does
(docs/cam/gui.md#job-tab-frame-cost). `CADShape::RememberTightBox` puts a box
computed elsewhere -- by the worker that pays a project's job tabs off the
frame, on detached copies -- under a framing's identity.
`frametime/job_tab_idle_and_switch` bounds both costs
(docs/rendering/frame-pacing.md#tests).

The per-shape memo lives inside the object: a shape's address-based id is
recycled by the allocator, so an outside cache keyed on the address alone
could hand a freed shape's box to whatever lands at that address next. The
located-box memo beside it holds a handle to each TShape it keys on, as the
volume cache does, so an address it keys on is never reused while the entry
lives; it is a bounded ring (1024), and opening a project empties it.

The conservative box uses `BRepBndLib::Add` with `useTriangulation = false` and
the tight one `AddOptimal`. Neither is mesh-derived: a box from the
tessellation is inflated by the deflection, so a part's size would change the
first time anything drew it.

Per-frame callers (preview handles, the section tool's anchor and hit test,
`ui::VisibleSceneBounds`) ask for the same bounds several times a frame;
without the memo each call walks the whole B-Rep. Filling a slot writes to the
shape, so it follows the OCC lazy-cache rule: the parallel sweeps warm bounds
single-threaded before workers start.

## Hole cutters: compound when disjoint

`HoleFeature` combines its cutters into one compound when no two holes overlap
(checked on each hole's outer radius, counterbore and countersink included);
fusing them one at a time is O(n²) in the accumulated tool. If any pair
overlaps it fuses all the cutters, because a boolean against a compound whose
pieces interfere fails outright.

## Code map

| where | what |
|---|---|
| `src/core/Document.{h,cpp}` | feature list, parameters, `Regenerate`, cache, rollback |
| `src/core/Feature.h` | `Feature`, `Body`, `RegenContext`, primitives, `ImportFeature` |
| `src/core/features/` | one file pair per feature type |
| `src/core/BodyMode.{h,cpp}` | new-body/add/cut, pattern instance fusing |
| `src/core/ElementMap.h`, `src/core/ElementHistory.{h,cpp}` | toponaming |
| `src/core/sketch/SketchProfile.{h,cpp}` | loops, profile faces, named prism/revolve builders |
| `src/core/AxisRef.h` | the shared axis reference |
| `src/core/io/SerializeFeatures.cpp` | feature JSON, `kFeatureTypes` |
| `src/cad/CADKernel.{h,cpp}`, `src/cad/FaceSelector.cpp` | OCC wrapper, face/edge picking |
| `src/remote/RemoteModeling.cpp` | modeling RPCs |
| `src/ui/FeatureEditor.cpp`, `src/ui/FeatureTreePanel.cpp`, `src/ui/PreviewFeature.cpp`, `src/ui/Toolbar.cpp` | editors, tree, create-with-preview, toolbar |

## RPCs

`add_feature` (JSON dialect = `feature_types`), `modify_feature` (replaces the
whole spec, keeps the id), `delete_feature`, `suppress_feature {feature_id,
suppressed}`, `set_rollback {feature_id | index}`, `set_material`,
`set_appearance`, `project_geometry`, `update_derived`, `derive_occurrence`
(`src/remote/RemoteModeling.cpp`); `list_elements`, `curve_points`,
`tube_schedule` (`src/remote/RemoteInspection.cpp`). `get_state` lists each
feature with its error, warning, `suppressed` and, for a Derived, `stale`.

## GUI

The part toolbar (`DrawModelToolbar`, `src/ui/Toolbar.cpp`) has one
`###tb_*` button per creation tool; each opens a preview with a tool window
and Accept. The feature tree's context menu has Edit, Rename,
Suppress/Unsuppress, Rollback to here / Roll to end, and, where they apply,
Reattach face… (a face-attached sketch) and Pick edges… (a fillet or chamfer
with a dangling edge). An errored feature is red with its message as the
tooltip. View > Show Element Names labels the hovered face with its name.

## Tests

Headless, one file per feature (`tests/test_extrude.cpp`, `test_revolve`,
`test_datum`, `test_boolean`, `test_pattern`, `test_fillet`, `test_shell`,
`test_draft`, `test_split`, `test_refine`, `test_rib`, `test_thread`,
`test_hole`, `test_curve3d`, `test_sweep`, `test_loft`, `test_pipe`,
`test_derived`, `test_toponaming`); the boolean check in
`tests/test_boolean_volume.cpp`, with the intake flange reduced to a block,
two swept bores and the slot, and the same case over the RPC in
`tests/regen_honesty_smoke.py`; regeneration semantics (cache stats,
suppression, rollback) in `tests/test_document.cpp`. `tests/test_golden.cpp`
loads the golden corpus (`tests/golden/`), and `tests/test_determinism.cpp`
checks that every golden regenerates to the same volumes and the same element
names. GUI coverage is in `src/app/guitests/GuiTestsModeling.cpp` and the
`defaults/*` suite, which asserts every creation tool either changes the
document or refuses visibly.

## Not built

- Split pieces all share one `createdBy`, so a later fillet, boolean or
  shell that names a piece binds to the first; a body sub-ordinal would tell
  them apart. Splitting by a sketch (sketch as knife) is not built.

- A modelled (helical) thread: the `Thread` feature's `modeled` flag is
  reserved and errors — [../plans/modeled-thread.md](../plans/modeled-thread.md).
- Draft angle on an extrude or a rib; per-side rib thickness; multi-profile
  rib networks.
- Up-to-face against a curved face (only planar targets).
- A tapered (conical) helix; more than one loft guide; a scaling
  (`BRepFill_Contact`) guide on a sweep.
- Picking 3D points in the viewport for a fit or control curve (points are
  typed or dragged in the panel).
- Dependency-graph cache invalidation: an edit re-runs everything after it,
  related or not.
- Inserting a feature at the rollback marker.
