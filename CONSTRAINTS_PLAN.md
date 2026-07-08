# Sketch constraints v2 — plan (rev. 2026-07-06)

The constraints v1 milestone (coincident, H/V, parallel, perpendicular, equal,
distance, radius; picking + endpoint drag-solve; a constraints toolbar) shipped.
This revision folds in two things:

1. **New requirements**: rectangles must be 4 real lines with shared corner
   points; any segment must be individually cuttable (drawing onto a line makes
   both resulting pieces trimmable); snap-to horizontal/vertical/coincident/
   point-on-line while drawing.
2. **Findings from the reference repos** (`~/Documents/CAD/FreeCAD`,
   `~/Documents/CAD/solvespace` — OpenSCAD and Blender have no constraint
   sketchers and taught nothing here. KiCad has no constraint solver either;
   its one useful bit is the snap-anchor ranking + snap-indicator UX in
   `kicad-source-mirror/common/tool/grid_helper.cpp` — worth a 15-minute look
   when building Phase E's snapping cursor behavior, nothing more).

PlaneGCS still covers every solver primitive we need; the work remains in our
data model, UI, and profile building. Sequenced so each phase ends green
(`cmake --build build && ctest`) and is independently useful.

## What the reference repos taught us

Pointers are into the checkouts under `~/Documents/CAD/`.

- **Entity identity.** FreeCAD uses raw vector indices (GeoId) exactly like our
  `PointRef.entity`, and pays for it with a centralized remap-every-constraint
  pass on each insert/delete (`Sketcher/App/SketchObjectConstraints.cpp`,
  `changeConstraintAfterDeletingGeo`). SolveSpace instead gives each user action
  a stable handle and regenerates the index-addressed entity list from it every
  solve (`solvespace/src/sketch.h`, `generate.cpp`). **Decision: stable IDs**
  (monotonic per-sketch counter, never reused — SolveSpace's `max+1` allocation
  recycles the top ID after a delete; don't copy that). Stable IDs matter extra
  for us because the remote/LLM interface references entities across mutations.
- **Split constraint-transfer policy** (FreeCAD `SketchObjectOperations.cpp`
  `split()` + `deriveConstraintsForPieces()` — the definitive reference):
  H/V/Parallel → copied to *both* pieces; Tangent/Perpendicular → the piece that
  intersects the other curve; Angle → the piece containing the reference point
  (param-range test); whole-edge Distance → re-anchored `front.start↔back.end`
  spanning both pieces; Radius/Equal → first piece. A Coincident joins the two
  pieces at the cut; split *arcs* additionally get a center–center Coincident.
  **Neither FreeCAD nor SolveSpace adds a collinear between split line pieces**
  — an unconstrained split line may bend at the joint. We follow suit (H/V
  transfer keeps rectangle sides straight, which is our main case).
- **Trim** (FreeCAD `trim()`): cut boundaries are the two intersections with
  other curves that bracket the click; no intersections → trim degrades to
  delete-the-whole-entity. At each cut: Coincident if the cut lands on the
  cutting curve's vertex, else PointOnObject binding the new endpoint to the
  cutting edge. SolveSpace picks among multiple intersections by cursor
  proximity (`modify.cpp` `SplitLinesOrCurves`).
- **Deletion machinery** (prerequisite v1 punted on): before erasing, transfer
  constraints from a dying vertex onto a coincident surviving vertex (FreeCAD
  `delGeometry`); repair coincidence transitivity — A~B and B~C, delete B ⇒
  re-add A~C (SolveSpace `FixConstraintsForPointBeingDeleted`); then sweep and
  drop orphaned constraints, warning only about non-trivial ones (SolveSpace
  `PruneRequestsAndConstraints` skips coincident/H/V in the warning).
- **Dragging — the PlaneGCS-native recipe** (FreeCAD `App/Sketch.cpp`
  `initMove`/`moveGeometries`): build the system ONCE on mouse-down; pin the
  dragged point(s) with *temporary* coincident constraints against free helper
  params, **down-weighted via `rescaleConstraint(id, 0.01)`** so real
  constraints dominate (this is the anti-oscillation trick); per mouse-move,
  only overwrite the helper params and re-solve with **DogLeg** (no fallback
  chain while dragging); re-init the solution if the cursor strays far from the
  linearization point; refuse to start a drag while the system has conflicts.
  Whole line = pin both endpoints; circle body = pin center + a PointOnCircle
  holding the radius. Our current per-frame `Solver::Load()` rebuild + hard
  coincident pin (`SketchInteraction.cpp` Select-drag) is the naive version of
  this — replace it in Phase F.
- **Auto-constraint inference**: create the constraint on *click*, never on
  hover, and snap the geometry to the target position first so the solver
  starts already satisfied. Tolerances that work in practice: ~10 px hover
  radius for point/edge snap (SolveSpace `draw.cpp`); H/V at 2° angular
  (FreeCAD `DrawSketchHandler.cpp`) or 0.02 slope ratio (SolveSpace
  `SuggestLineConstraint`); zoom-scaled model distance (0.1 × screen scale) for
  on-edge/tangent seeking. A tangent suggestion suppresses the H/V suggestion.
  Ctrl suppresses inference for one click. Don't pre-filter redundancy —
  add-then-diagnose and back out if the solver reports redundant (SolveSpace
  `TryConstrain`); our `SolveResult.redundant` already carries this.
- **Rectangle tool** (SolveSpace `mouse.cpp` RECTANGLE): 4 line requests + 4
  corner coincidents + alternating H/V constraints, created atomically under
  one undo record, then the far corner enters drag so the shape rubber-bands.
  The H/V are unconditional — a compound tool emits its defining constraints
  regardless of the auto-constraint preference.
- **Arcs**: keep a model-layer CCW-always convention (center, radius,
  startAngle, endAngle) and convert to OCC only at shape-build time. FreeCAD
  exposes OCC's CW-arc representation (basis-circle Z flip) upward and needed a
  retrofitted `emulateCCWXY` abstraction plus a file-migration pass — avoid.
  After `BRepBuilderAPI_MakeWire`, run `ShapeFix_Wire` (FixReorder /
  FixConnected / FixClosed) before face building; greedy edge order alone is
  not enough (FreeCAD `Sketch::toShape`).

## Phase A — Angle + tangent + point-on-line constraints (small, ~1 day)

Pure extension of existing plumbing. **Detailed implementation plan:
[CONSTRAINTS_PHASE_A.md](CONSTRAINTS_PHASE_A.md)** — including two corrected
assumptions: serialization *throws* on unknown kind strings (no generic
round-trip), and the remote server's `add_constraint` has its own kind
allowlist that must gain the new kinds.

- `sketch::ConstraintKind` gains `Angle`, `Tangent`, `PointOnLine`.
- Solver mapping: `addConstraintL2LAngle` (angle is a `double*` — goes in the
  params deque), `addConstraintTangent(line, circle, ccw)` (side flag computed
  from current geometry), `addConstraintPointOnLine`.
- Serialization + remote: three new kind strings in both places.
- `SketchToolbar`: Angle button (2 lines + degrees input), Tangent
  (line + circle), PointOnLine (point + line) via `ApplySketchConstraint`.
- Tests: 45° between two lines; tangent keeps the circle on its side; point
  onto line; serialization round-trip; remote smoke.

Risk: low.

**Status: shipped** (commit "feat: angle, tangent and point-on-line sketch
constraints").

## Phase B — Stable entity IDs + deletion (~1–1.5 days) — SHIPPED

The foundation split/trim/delete all stand on. Mechanical but wide.

- `SketchFeature` gains `uint32_t nextEntityId` (serialized); every entity gets
  an immutable `id`. `PointRef.entity` becomes an entity **ID**, not an index.
  An id→index lookup lives beside the entity vector (rebuilt on load/mutation).
- Serialization bumps to v4; v3 migration assigns `id = index` (old files load
  unchanged). Remote protocol (`add_constraint` refs) switches to IDs — this is
  what keeps LLM/remote references valid across concurrent edits.
- Entity deletion (lifting the v1 ban), per the reference-repo recipe:
  transfer constraints to a coincident surviving vertex → transitive
  coincidence repair → orphan sweep (warn only for non-trivial kinds).
- Tests: delete a line that a neighbour is coincident to (neighbour keeps its
  other constraints); A~B~C delete B leaves A~C; v3 file round-trips.

Risk: low-medium — touches solver load, picking, serialization, remote, but
each change is mechanical.

Shipped as commit "feat: stable sketch entity ids and entity deletion". One
scope cut: dimensional-constraint transfer onto a coincident survivor was
deferred (we do coincidence repair + orphan sweep); Phase D's `SplitEntity`
needs its own transfer logic anyway. Detailed in
[CONSTRAINTS_PHASE_B.md](CONSTRAINTS_PHASE_B.md).

## Phase C — Selectable entities: body picking + visible lines (NEW, next, ~1 day)

Pulled ahead of everything below: **constraints are currently unusable** because
only entity *endpoints* are pickable (`PickSketchPoint`, 12 px radius) and lines
render 1 px with no AA (`SketchRenderer.cpp` — `lineWidth = 1.0`,
`VK_SAMPLE_COUNT_1_BIT`), so the endpoint targets are near-invisible. You cannot
select a line to apply Horizontal/Parallel/etc. This is the picking half of the
drag phase (Phase F), brought forward; the drag-*solver* rework stays there.

Four parts: body picking, visible lines, point visualization, hover highlight.
Onshape/SolidWorks (the reference images) draw a dot at every endpoint and
circle center and highlight what's under the cursor — that legibility is what
makes a sketch clickable.

- **Body picking**: extend selection so a click that misses every endpoint
  tolerance falls back to the nearest **line segment** (point-to-segment
  distance) or **circle circumference** within a pixel tolerance, selecting the
  whole entity (ref `{id, 0}`). Endpoint-within-tolerance still wins first, so
  coincident/distance/drag keep working.
- **Visible lines**: thicken + anti-alias sketch lines. **Decision: quad
  expansion (two triangles), not `wideLines`.** Expand each segment to a
  screen-space quad in the **vertex shader** (project both endpoints, offset
  along the screen normal by a pixel half-width) and feather the alpha at the
  quad edge in the fragment shader for AA. Rationale: `wideLines` is ~universal
  on desktop (this laptop has it) but renders *aliased* fat lines with butt-cap
  notches at polyline corners, needs `VK_EXT_line_rasterization` for real AA,
  and is absent on MoltenVK/mobile; quad expansion gives constant pixel width,
  free AA, round-cap-capable corners, and stays portable (no `wideLines`/GS
  dependency).
- **Point visualization**: draw a constant-screen-size dot at every line
  endpoint, circle center, and standalone point (Onshape-style), using the same
  screen-space-expanded primitive path as the lines (a point = a small quad,
  SDF-circle fragment for AA). These dots are the endpoint pick/selection
  targets; the existing selection cross in `AppActions.cpp` becomes a filled
  bright dot.
- **Hover highlight**: run the pick each frame (not just on click) against the
  current cursor; the entity or vertex under the cursor renders brighter /
  slightly enlarged so it's clear what a click will grab. Selected entities stay
  emphasized. State lives on `SketchEditState` (`hoveredRef`).
- **Renderer**: unify the above under one screen-space-expanded pipeline — add
  viewport size to the UBO, a per-vertex pixel-offset attribute, and an SDF
  fragment for AA; it serves thick lines, point dots, and hover/selection
  emphasis (color + size). Reuses the existing dynamic-buffer upload and
  descriptor infra in `SketchRenderer`.
- Tests: GUI `pick/select_line_body` (click mid-segment → that line selected,
  Horizontal enabled and applies); `pick/prefers_endpoint` (click near a shared
  vertex → endpoint, not body); `pick/hover_tracks_cursor` (hoveredRef updates
  as the mouse moves over an entity).

Risk: low-medium — the renderer gains a screen-space-expanded pipeline and a new
vertex attribute, but it's self-contained and reuses the existing upload path.

## Phase D — Rectangle decomposition + split + line trim (~1.5–2 days)

**Detailed implementation plan: [CONSTRAINTS_PHASE_D.md](CONSTRAINTS_PHASE_D.md)**
(reuses Phase B `DeleteEntity` for trim cleanup and Phase C body picking; drops
the rigid `Rectangle` variant with a v4→v5 load migration; lines-only trim,
circle trim deferred to Phase J).

- **Rectangle tool** commits 4 lines + 4 corner Coincidents + 2 H + 2 V as one
  undo record (SolveSpace-style), then rubber-bands the far corner. The rigid
  `sketch::Rectangle` entity is removed; v4 migration decomposes any stored
  rectangles the same way. (Today rectangles are invisible to the solver —
  `SketchSolver.cpp` skips them — so this is strictly an upgrade.)
- **`SplitEntity(id, t)`** core op implementing the FreeCAD transfer policy
  above: piece 1 keeps the original id's slot, piece 2 is a new entity,
  Coincident at the cut, constraints transferred per kind. No collinear.
- **Trim tool**: click a segment → find bracketing intersections/constraint
  points → split there → delete the clicked piece (Phase B machinery cleans up
  its constraints). No bracketing cuts → delete whole entity. Circle trim
  produces arcs, so it waits for Phase J; line-only trim covers the rectangle
  workflows.
- Draw-onto-a-line behavior: for now emits `PointOnLine` (Phase A kind) rather
  than eagerly splitting — matches Onshape/SolveSpace/FreeCAD; the trim/split
  tool cuts there later. Revisit eager splitting as a policy toggle if
  trim-time splitting feels wrong in practice.
- Tests: split a dimensioned line (length re-anchors across both pieces); H
  survives on both pieces; trim the middle of a line crossed by two others;
  rectangle drag stays rectangular.

Risk: medium — the transfer policy has many small cases; the FreeCAD reference
enumerates them all.

## Phase E — Snapping / auto-constraint inference (~1.5–2 days)

**Detailed implementation plan: [CONSTRAINTS_PHASE_E.md](CONSTRAINTS_PHASE_E.md)**
(pure testable `core/SketchInference` taking a `pixelsPerUV` scale; add-then-
diagnose via the DOF delta; generalizes Phase D's `CommitLine`; ghost glyphs via
an ImGui 2D overlay).

The Onshape-feel milestone: snap + auto-constrain while drawing.

- New `SketchInference.{h,cpp}` pass, run each frame during the draw FSM:
  - cursor near an existing point (~10 px) → snap + **Coincident** on click,
  - segment within 2° of axis → snap angle + **Horizontal/Vertical**,
  - endpoint near an edge (zoom-scaled tolerance) → snap onto it +
    **PointOnLine**,
  - near-parallel/perpendicular to a nearby line (2°, nearest line wins) →
    **Parallel/Perpendicular**.
- Constraint created on click only; geometry snapped to target *before* the
  constraint is added so the solve starts satisfied. Add-then-diagnose: if
  `SolveResult.redundant` flags the new constraint, remove it silently.
- Ghost glyphs (H, V, ●, ⟂) near the cursor; tangent-class suggestions
  suppress alignment ones. `state.autoConstrain` toggle (default on); holding
  Ctrl suppresses inference for one click.
- Tests: GUI `infer/horizontal_snaps`, `infer/endpoint_coincident`,
  `infer/point_on_line`; redundant inference is dropped (draw over an existing
  H line → no second H).

Risk: medium — tolerance tuning; the numbers above are proven starting points.

## Phase F — Whole-entity dragging + drag-solver rework (~1–2 days)

**Detailed implementation plan: [CONSTRAINTS_PHASE_F.md](CONSTRAINTS_PHASE_F.md)**
(persistent `Solver` drag session `BeginDrag`/`SetDragTargets`/`SolveDrag`; weak
`rescaleConstraint(0.01)` pins; pick→mode classification; solver-level tests so
the GUI path stays thin; arcs slot into the generic point set later).

Adopt the FreeCAD drag recipe wholesale (it's PlaneGCS-native):

- `Solver` gains `BeginDrag(refs) / SetDragTarget(uv) / EndDrag`: system built
  once on mouse-down, temporary coincidents against helper params
  down-weighted with `rescaleConstraint(id, 0.01)`, per-frame just overwrite
  helpers + `solve(DogLeg)`; re-init when the cursor strays far. Refuse drag
  while conflicts exist. This also replaces today's per-frame `Load()` rebuild
  for plain endpoint drags.
- Drag modes: body picking already exists from Phase C; this phase maps a body
  hit to `DragEntityBody` (line: both endpoints, rigid delta; circle: center +
  radius-holding PointOnCircle) and a rim hit to `DragCircleRim` (drive the
  radius param), alongside the existing `DragEndpoint`.
- Mouse-up commits one `ModifyFeatureCommand` from the mouse-down baseline,
  as today.
- Tests: GUI `drag/move_whole_line` (constrained neighbour follows),
  `drag/resize_circle`; endpoint drag of a heavily constrained sketch doesn't
  oscillate.

Risk: low-medium now that the reference implementation is mapped.

## Phase G — Dimension display + editing (NEW, ~2 days)

**Detailed plan for the whole constraint-layer block (G+H+I):
[CONSTRAINTS_PHASE_GHI.md](CONSTRAINTS_PHASE_GHI.md).**

The dimensional *constraints* (Distance, Radius, Angle with a `value`) already
solve — but they're **invisible in the canvas** and only settable via the
toolbar value box. This phase adds the annotation graphics and in-canvas editing
that make a sketch feel dimensioned (the "Ø2 with a leader" in every CAD
sketcher). Independent of drag/arcs/splines; can run before or after Phase F.

- **Model**: dimensional constraints gain a **label position/offset** (extend
  `sketch::Constraint`, or a parallel map keyed by constraint) so annotations
  have a placement; serialized. Add `DistanceX` / `DistanceY` and `Diameter`
  kinds (small solver additions, same shape as Phase A).
- **Rendering** (`DrawDimensions`, a 2D ImGui overlay like `DrawSnapGlyphs`):
  project the constrained geometry to screen and draw extension lines, a
  dimension line with arrowheads, and the value text — `2.00` for a distance,
  `Ø2`/`R2` for a circle, `45°` for an angle. Auto-place initially at the stored
  offset.
- **Editing**: double-click a dimension label → inline `InputText` → parse →
  re-apply the constraint `value` → re-solve (one `ModifyFeatureCommand`).
  Drag the label to reposition (updates the offset).
- **Dimension tool**: one tool that infers the kind from the selection (2 points
  → distance, circle → radius/diameter, 2 lines → angle), places the annotation,
  and prompts for the value — the Onshape flow, alongside the existing toolbar
  buttons.
- Tests: a dimension's screen annotation hit-tests and its edited value re-drives
  the geometry; DistanceX/Diameter solve; serialization round-trips the label
  offset. Rendering itself is eyeballed via `/run` (2D overlay, like the glyphs).

Risk: medium — annotation layout (arrow placement, text avoidance) is fiddly UX,
but the constraint/solver side is done.

## Phase H — Construction geometry + extra constraints (~1.5 days)

Completes the constraint layer. Independent of arcs/splines; pairs with the
dimension work in G. **Detailed plan:
[CONSTRAINTS_PHASE_GHI.md](CONSTRAINTS_PHASE_GHI.md).**

- **Construction geometry**: a `bool construction` flag on entities (serialized).
  Construction lines/circles drive constraints and snapping but are **excluded
  from `BuildLoops`** (no profile contribution) and render **dashed / dimmed**.
  A toolbar/context toggle flips the selected entity's flag. Tiny change,
  everyday feature (centerlines, reference circles). Tests: a construction line
  doesn't close a profile but still solves; round-trips.
- **Extra constraint kinds** (Phase-A-shaped — solver map + toolbar + the Phase M
  shortcuts): **Symmetric** (two points/entities about a line —
  `addConstraintSymmetric` / `PointOnPerpBisector`), **Midpoint** (point at a
  line's middle — `addConstraintMidpointOnLine`), **Fixed/Lock** (ground a point
  by dropping its params from the unknowns), **Concentric** (coincident centres),
  **Collinear** (two lines on one line). PlaneGCS already exposes the primitives.
- Tests: symmetric reflects a point across a line; midpoint pins to 0.5; a fixed
  point doesn't move under drag; concentric/collinear solve.

Risk: low — same shape as Phase A, plus a one-field flag.

## Phase I — Constraint status & management (~2 days)

**Detailed plan: [CONSTRAINTS_PHASE_GHI.md](CONSTRAINTS_PHASE_GHI.md).**

The feedback + management layer — as important as dimensions for the sketcher
feel, and currently only half-present (the solver reports dof/conflicting but
nothing renders or lets you act on it).

- **Fully-constrained coloring**: colour entities by constrained-ness —
  under-constrained (blue) vs fully constrained (black/white), from
  `SolveResult.dof` and a per-entity DOF attribution. The classic "is my sketch
  solved" signal (Onshape's Show-overdefined).
- **Persistent constraint glyphs**: draw the little H / V / coincident / ⟂ / =
  badges *on the geometry* (reusing the Phase E glyph renderer, but for the
  committed constraints, not just live inference), with a show/hide toggle.
- **Delete an individual constraint**: today only *entities* delete (cleaning up
  their constraints); add selecting a constraint (via its glyph or a list) and
  removing just it, then re-solve. Closes the Deferred "constraint-list editor"
  gap.
- **Conflict / over-defined highlighting**: surface `conflicting`/`redundant`
  constraints in red so an over-defined sketch is diagnosable, not just stuck.
- Tests: dof attribution marks a free line under-constrained and a fully-pinned
  one solved; deleting a constraint frees the relevant DOF; a conflicting set is
  flagged.

Risk: medium — per-entity DOF attribution and constraint hit-testing are the
fiddly bits; the solver already provides the raw dof/conflicting data.

## Phase J — Arcs (~3–4 days) — SHIPPED

Shipped as `feat: sketch arcs + curve trim`: `sketch::Arc` (CCW-always) through
model / serialization (v7) / solver (`addConstraintArcRules` + arc
radius/diameter/equal/tangent/concentric) / profile (the `Loop` edge-list
refactor + `ShapeFix_Wire`) / rendering / picking; both arc tools
(center→start→end and start→end→bulge); and curve trim (circle→arc, arc→arcs).
Detailed change-list in CONSTRAINTS_PHASE_J.md. Original outline below.


As previously planned, with the repo findings applied:

- `sketch::Arc { center, radius, startAngle, endAngle }`, **CCW-always** at the
  model layer; `PointRef::which` addresses start/end/center.
- Solver: `GCS::Arc` + `addConstraintArcRules`; endpoints are solver points so
  coincident/tangent/radius work for free; enables line–arc Tangent and
  `addConstraintPointOnArc`.
- Rendering: tessellate like circles.
- Profile: real OCC circular edge (`gp_Circ` + trim range) in the wire
  builder; run `ShapeFix_Wire` (FixReorder/FixConnected/FixClosed) on every
  wire before face building.
- Unlocks circle trim (split circle → arc; split arc → two arcs + center
  Coincident per the FreeCAD policy) and the 3-click Arc tool.
- Tests: arc serialization round-trip; filleted-corner profile extrudes; arc
  radius constraint; trim a circle crossed by two lines.

Risk: medium — wire orientation is the fiddly part; the ShapeFix pass is the
known mitigation.

## Phase K — Generative operations: mirror, pattern, fillet, offset (~3–4 days)

Detailed plan (with reference-repo findings) in **CONSTRAINTS_PHASE_K.md**. Key
revision from that research: FreeCAD does these as constraint-linked bakes (no
dependency graph), so mirror + fillet need **no** derived-record machinery — the
derived record is only needed for editable pattern count. Split into K1 (mirror
+ fillet, constraint-linked), K2 (pattern, derived-record), K3 (offset, OCC).

**K1 SHIPPED** (branch feat/mirror-fillet, commit `feat: sketch mirror and
fillet`): `SketchFeature::MirrorEntities` (reflect + optional Symmetric/Equal
links, arc CCW-preserving) and `FilletCorner` (bisector/normal-offset → trim +
tangent arc + 2 Tangent/2 Coincident); Mirror/Fillet tools + Linked toggle +
radius; `mirror`/`fillet` RPCs. Verified: filleted rectangle extrudes (vol
matches analytic), linked mirror tracks a source edit through the solver.

**K2 SHIPPED** (branch feat/mirror-fillet, commit `feat: sketch linear and
circular pattern`): `sketch::DerivedOp` + `SketchFeature::RegenerateDerived()`
(id-stable replay, SolveSpace remap discipline); `AddLinearPattern`/
`AddCircularPattern` (bake or linked); serialization v7→v8 `derived_ops` +
post-load regen; `RegenerateDerived` hooked into every edit-commit path so
patterns follow their source; Pattern tool + live-editable count list;
`pattern` RPC. Verified: plate with a linked row of 5 patterned holes extrudes
(vol exact), count edit 4→6 regenerates id-stably through save/load. Note: a
pre-existing `BuildPrism` limit (disjoint loops → outer+holes, not multi-body)
means a bare row of circles won't extrude as separate solids.

**K3 SHIPPED** (branch feat/mirror-fillet, commit `feat: sketch offset`):
`SketchFeature::OffsetChain` + `sketch::OffsetLoop` (OCC
`BRepOffsetAPI_MakeOffset`, Arc/Intersection join) + `EdgeToSketchEntity`
reverse conversion (line/circle edge → sketch Line/Arc, CCW-correct); Offset
tool + distance/round-corners; `offset` RPC; needs `TKOffset`. Verified: a
rectangle offset outward with rounded corners extrudes to the analytic area.
Offset is bake-only (not yet a regenerating DerivedOp). **Phase K COMPLETE.**

Operations that *produce geometry from other geometry* — the one place a sketch
genuinely wants "history." Placed after arcs because fillet and offset produce
arcs. Each op supports **both modes, chosen by the user per invocation**:

- **Bake** — generate the geometry once as plain entities, no link. Simplest;
  matches how split/trim/rectangle already mutate the entity list. Edit the
  source → copies don't follow; re-run to update. Good for "just duplicate it."
- **Parametric ("linked")** — the generated entities carry a *derived-from* tag
  and regenerate when the source changes. The real-CAD feel: move the original,
  the mirror/pattern follows.

Shared machinery (only the parametric mode needs it):

- **Derived record** on `SketchFeature`: an ordered list of generative ops
  `{kind, sourceIds, params, generatedIds}` — a small in-sketch dependency
  graph, *not* a linear feature tree. Bake mode stores nothing.
- **Regeneration pass** (`SketchFeature::RegenerateDerived`): after the
  constraint solve, replay the parametric ops to rebuild their generated
  entities, then re-solve. Regeneration must be **id-stable** — a regenerated
  entity keeps its id so constraints and downstream refs survive (same
  discipline as split keeping piece-1's id; leans entirely on Phase B ids).
- Serialization round-trips the derived records; on load, geometry is stored and
  re-verified by a regenerate.

The ops:

- **Mirror** across a line (or a sketch axis): reflect the source entities;
  parametric mode links each copy to its source (optionally adds symmetry
  constraints).
- **Pattern** linear (count + direction + spacing) and circular (count + centre
  + angle): instances derive from the seed.
- **Fillet**: replace a corner where two lines meet with a tangent arc of radius
  r, trimming the lines — generative + reuses split/trim; **needs Phase J arcs**.
- **Offset**: offset a chain by a distance — parallel lines + arcs at convex
  corners; **needs arcs**.
- UI: mirror/pattern/fillet/offset tools with a bake-vs-linked toggle; editing a
  parametric op's params (mirror line, pattern count, fillet radius) regenerates.

- Tests: mirror reflects geometry (bake: no link; parametric: moving the source
  updates the copy after regenerate); linear/circular pattern counts; fillet on
  a corner closes and extrudes; regeneration preserves generated-entity ids so a
  constraint on a mirrored entity survives a source edit.

Risk: medium-large — the id-stable parametric regeneration is the fiddly part (a
sketch-local analogue of the toponaming problem); bake mode is easy and can ship
first within the phase.

## Phase L — B-splines (large, defer)

Unchanged: own milestone after A–K.

## Phase M — Interaction & keyboard: select-default, Escape, shortcuts, box-select (~1.5 days) — SHIPPED

**SHIPPED** (branch feat/interaction-keyboard, commit `feat: sketch
select-default, escape cascade, box-select + Onshape shortcuts`): Select is the
default tool; Escape cascade (cancel → Select → clear selection → leave);
`core/SketchSelection::BoxSelect` (pure window/crossing hit-test with
segment-vs-rect) + rubber-band drag + overlay; **context-sensitive Onshape
keymap** — sketch keys (`l c g a m o`, `Shift+f`; `h v n b t e d`; `q`,
`Shift+s`) via `HandleSketchKeys`, model keys (`f` fit, `Shift+1..7` views,
`Shift+p` planes) in ViewportInput. Detailed plan: CONSTRAINTS_PHASE_M.md.
Deferred within M: model-context `n` normal-to and `Shift+s/e` new-sketch/extrude
(need selected-face / feature plumbing) → fold into Phase N.

Sketcher UX polish, placed last so shortcuts can cover *every* tool and
constraint. The core parts (select-default, Escape, box-select) don't depend on
later phases and can be pulled forward; only the exhaustive shortcut list wants
all tools present.

- **Select is the default mode.** Change the default `SketchTool` from `Line` to
  `Select`; a new sketch lands in Select. Drawing tools stay active for repeated
  placement; Escape returns to Select — so Select is the resting state and its
  toolbar button becomes almost vestigial (kept, but rarely needed).
- **Escape cascade** (replaces today's two-step): back out one level per press —
  (1) cancel an in-progress draw (pending point); (2) a non-Select tool active →
  switch to Select; (3) selection non-empty → clear it; (4) else leave sketch
  edit. Predictable "undo my mode."
- **Keyboard shortcuts** (guarded by `!ImGui::GetIO().WantTextInput` so they
  never fire while editing a dimension/name field):
  - Tools: `L` Line, `C` Circle, `R` Rectangle, `P` Point, `T` Trim, `Esc`/`S`
    Select; `A` Arc and `D` Dimension once H/G land.
  - Constraints on the current selection: `H` Horizontal, `V` Vertical, `O`
    cOincident, `E` Equal, `//` (or `L2`) parallel, etc. — a compact mnemonic
    map (tunable; the toolbar stays for the rest). Routes to the existing
    `ApplySketchConstraint`.
- **Box (rubber-band) selection.** In Select mode, a mouse-down that misses all
  geometry starts a selection box instead of just clearing; drag → a 2D overlay
  rectangle; release → select the entities inside. Left→right = window (fully
  enclosed), right→left = crossing (touching) — the CAD convention; Ctrl adds to
  the selection. A tiny box (a plain click) still just clears. `SketchEditState`
  gains the box screen-rect; the hit-test is a **pure `BoxSelect(entities, rect,
  mode, projector)`** so it unit-tests deterministically (like `SketchInference`).
- Tests: `BoxSelect` unit cases (window vs crossing, partial overlap); GUI
  `keys/tool_switch` (`L` → Line tool), `keys/constraint_on_selection` (select
  two lines, `H` → both level), `select/box_selects_enclosed` (drag a box over
  two entities → both selected), and the Escape cascade steps.

Risk: low — mostly wiring; the box hit-test (pure) and the WantTextInput key
guard are the only fiddly bits.

## Phase N — Sketch UI redesign + generative-op editing (~3–4 days)

The sketch toolbar grew organically into a tall **floating** ImGui window with
every tool, constraint, status readout, and generative control stacked in it.
It needs a real redesign, and the multi-step tools (mirror/pattern/offset) need
staged guidance instead of terse hint text. Deferred here (from Phase K) because
it's a UI concern, not a modelling one, and wants doing once across all tools.

- **Dock the sketch UI, don't float it.** Fold the sketch tools into the app's
  docking layout (a proper docked panel / contextual toolbar that appears on
  sketch-edit and shares the frame), matching the Model/Properties panels rather
  than a stray `Begin("Sketch Tools")` window. Group: draw tools · constraints ·
  generative · status. Consider a compact tool strip + a context panel for the
  active tool's options.
- **Staged, guided multi-step tools.** Tools that take more than one kind of
  input should show explicit steps and progress, not one line of hint text:
  - *Mirror*: "Step 1 — select geometry to mirror" → "Step 2 — click the mirror
    axis line", with the current selection highlighted distinctly from the axis,
    a live preview of the reflected result, and a cancel/confirm. (Today it
    silently relies on a prior selection + a hint string.)
    - Also let a mirror axis be a **construction line or the plane X/Y axes**,
      not only a drawn line (the K1 rough edge).
  - *Pattern* / *Offset*: a small inline form for the params with a live ghost
    preview before commit, and clearer "select the loop / geometry first"
    staging.
  - A shared "tool prompt / step" area (supersedes the ad-hoc
    `SketchEditState::actionMsg`), so every tool reports what it wants next and
    why a click did nothing.
- **Generative-op parameter editing (the deferred K editing).** The Patterns
  panel currently edits only `count`. Expose the full transform: linear
  spacing/direction, circular centre/angle, and link/unlink — with an
  `EditPatternParams`-style commit alongside `SetPatternCount`. Add a
  first-class **edit-fillet-radius** affordance (today you must add a Radius
  dimension to the fillet arc). Optionally, make **offset a linked DerivedOp** so
  its distance is editable too (K3 shipped it bake-only).
- **Selected-op inspector.** Clicking a generated/derived entity (or the op in a
  list) surfaces its op's params for editing — the natural home for the above.
- Tests: GUI coverage for the staged mirror flow (select → axis → preview →
  commit) and pattern param edits regenerating; the pure param-edit commit
  (`EditPatternParams`) unit-tested like `SetPatternCount`.

Risk: medium — mostly UI/UX work and wiring; the docking integration and a
clean tool-state machine (per-tool staged input) are the substance. No new
modelling core beyond exposing params the DerivedOp already stores.

## Deferred / explicitly out of scope

Things intentionally NOT done in a given phase, with where they get picked up.
Keep this list honest — a deferred item is a known gap, not a silent one.

- **B-splines** → Phase L (own milestone; poles/knots/weights UX + OCC B-spline
  edge are each substantial).
- **Circle & arc trim** → SHIPPED in Phase J (`TrimCurveAt`): a trimmed circle
  becomes the complementary arc, a trimmed arc splits into sub-arcs.
- **Whole-entity body dragging** (drag a line/circle by its interior; rim-drag
  a circle's radius) → Phase F. Phase C added body *picking* only; the Select
  drag still moves single endpoints.
- **Dimensional-constraint transfer onto a coincident survivor on delete** →
  deferred in Phase B (we do coincidence repair + orphan sweep only). Phase D's
  `SplitEntity` implements its own per-piece transfer; a general delete-time
  transfer is still unbuilt.
- **Eager split-at-draw** (drawing onto a line splits it immediately) →
  Phase D uses trim-time split + a `PointOnLine` constraint instead (Onshape
  model). Eager splitting remains a possible policy toggle, unplanned.
- **Full auto-constraint inference** (ghost H/V/⟂/● glyphs, parallel/perp/
  tangent snapping, tolerance tuning, the Ctrl-suppress modifier) → Phase E.
  Phase D ships only a minimal `PointOnLine`-on-commit taste of it.
- **Deleting individual constraints / constraint status** → **Phase I** (was
  unplanned). Today only *entities* delete (cleaning up their constraints).
- **Extend tool** (the complement of Trim — stretch a line to meet another) →
  nice-to-have; pairs with Trim/arcs, unscheduled.
- **Transform tools** (move/rotate/scale/copy a *selection* numerically) →
  nice-to-have. Interactive move is covered by drag (F); rotate/scale/array of a
  selection overlaps Phase K generative patterns, unscheduled.
- **Convenience primitives**: slot (2 arcs + 2 lines, needs arcs), polygon
  (N-gon), ellipse (PlaneGCS supports ellipses) → nice-to-have, unscheduled.
- **Persistent face references (toponaming)** → out of scope for this plan. A
  sketch bakes its plane at creation; editing an upstream feature does NOT move
  dependent sketches. This also blocks **projected/external geometry** ("use
  edges" — projecting model edges into the sketch). Tracked separately (see the
  FreeCAD/Blender reference notes).
- **Sketch text** → niche, out of scope.
- **Rendering polish**: round joins/caps on thick lines (Phase C uses butt
  caps) and the faint sawtooth on near-edge-on lines under an oblique camera —
  cosmetic, unplanned; invisible in the normal face-on editing view.

## Cross-cutting

- **DOF / constraint status**: the raw remaining-DOF / conflicting / redundant
  data is already produced by the solver each solve (Phase E's add-then-diagnose
  relies on it); **Phase I** turns it into the fully-constrained coloring +
  conflict highlighting UX.
- **Remote parity**: constraint kinds ride the existing `{kind, refs, value}`
  schema; Phase B switches refs to stable IDs; Phase J adds arcs to
  `add_feature`. Extend `remote_smoke.py` per phase.
- **Undo**: compound tools (rectangle, split, trim) must land as a single
  `ModifyFeatureCommand`.
- **Build hygiene** (independent of the constraints work): third-party headers
  (OpenCASCADE, ImGui) come in via plain `-I`, so OCC's `std::iterator`
  deprecation warnings flood every build — switch to SYSTEM includes
  (`-isystem`) to silence them. Builds are slow because `Feature.h` →
  `CADKernel.h` pulls the whole OCC header tree into every core TU; a
  precompiled header for the heavy common set (OCC/Vulkan/glm/json), the Ninja
  generator, and ccache would each cut rebuild time. Already on C++20 (GCC
  13.3) — the standard version is not the bottleneck.

## Recommended order

A ✓ → B ✓ → C ✓ → D ✓ → E ✓ → F ✓ → **G (dimensions)** → H (construction +
extra constraints) → I (constraint status & management) → J (arcs) → K
(generative: mirror/pattern/fillet/offset) → L (splines, later) → M
(interaction & keyboard).

A–F shipped. G/H/I are the **constraint-layer** block — dimensions display,
construction geometry + the remaining standard constraints, and the
fully-constrained/conflict feedback + per-constraint delete — together the
biggest "feels-like-a-sketcher" gap after geometry. They're independent of
arcs/splines and can be reordered freely. J completes the geometry set; K
(generative) is the "history where it matters" layer and needs arcs first; L is
its own deferred milestone; M is UX polish placed last so its shortcuts cover
every tool/constraint (though select-default/Escape/box-select are independent
and can be pulled forward). Roughly 12–17 days remaining.
