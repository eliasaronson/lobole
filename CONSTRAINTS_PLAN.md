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
   when building Phase D's cursor behavior, nothing more).

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
  this — replace it in Phase E.
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

## Phase B — Stable entity IDs + deletion (new, ~1–1.5 days)

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

## Phase C — Rectangle decomposition + split + line trim (~1.5–2 days)

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
  produces arcs, so it waits for Phase F; line-only trim covers the rectangle
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

## Phase D — Snapping / auto-constraint inference (~1.5–2 days)

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

## Phase E — Whole-entity dragging + drag-solver rework (~1–2 days)

Adopt the FreeCAD drag recipe wholesale (it's PlaneGCS-native):

- `Solver` gains `BeginDrag(refs) / SetDragTarget(uv) / EndDrag`: system built
  once on mouse-down, temporary coincidents against helper params
  down-weighted with `rescaleConstraint(id, 0.01)`, per-frame just overwrite
  helpers + `solve(DogLeg)`; re-init when the cursor strays far. Refuse drag
  while conflicts exist. This also replaces today's per-frame `Load()` rebuild
  for plain endpoint drags.
- Picking: `PickSketchPoint` grows body hits → `DragEndpoint` |
  `DragEntityBody` (line: both endpoints, rigid delta; circle: center +
  radius-holding PointOnCircle) | `DragCircleRim` (drive the radius param).
- Mouse-up commits one `ModifyFeatureCommand` from the mouse-down baseline,
  as today.
- Tests: GUI `drag/move_whole_line` (constrained neighbour follows),
  `drag/resize_circle`; endpoint drag of a heavily constrained sketch doesn't
  oscillate.

Risk: low-medium now that the reference implementation is mapped.

## Phase F — Arcs (~3–4 days)

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

## Phase G — B-splines (large, defer)

Unchanged: own milestone after A–F.

## Cross-cutting

- **DOF badge**: surface remaining DOF / fully-constrained / conflicting set as
  each phase adds constraints; Phase D's add-then-diagnose depends on the
  redundant list being reliable.
- **Remote parity**: constraint kinds ride the existing `{kind, refs, value}`
  schema; Phase B switches refs to stable IDs; Phase F adds arcs to
  `add_feature`. Extend `remote_smoke.py` per phase.
- **Undo**: compound tools (rectangle, split, trim) must land as a single
  `ModifyFeatureCommand`.

## Recommended order

A → B → C → D → E → F (→ G later). A is a quick win; B is the foundation
everything with topology change needs; C delivers cuttable rectangles; D is
the requested snapping; E and F complete the Onshape feel. Roughly 9–12 days
end to end at the original plan's calibration.
