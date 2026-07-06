# Sketch constraints v2 — plan for the deferred features

The constraints v1 milestone (coincident, H/V, parallel, perpendicular, equal,
distance, radius; picking + endpoint drag-solve; a constraints toolbar) shipped.
Four things were deferred: **angle constraints, arcs/splines, auto-constraint
inference, and whole-entity dragging**. This is the plan for them.

PlaneGCS already exposes every solver primitive we need (`addConstraintL2LAngle`,
`addConstraintTangent`, `addConstraintArcRules`, `addConstraintPointOnArc`,
`addConstraintPointOnLine`, `addConstraintMidpointOnLine`), so most of the work is
in our own data model, rendering, profile building, and UI — not the solver.

Sequenced cheapest-first so each phase ends green (`cmake --build build && ctest`)
and is independently useful.

## Phase A — Angle + tangent + point-on-line constraints (small, ~1 day)

Pure extension of the existing constraint plumbing; no new geometry.

- `sketch::ConstraintKind` gains `Angle`, `Tangent`, `PointOnLine`.
- `SketchSolver::addConstraint` maps them:
  - `Angle` (2 lines) → `addConstraintL2LAngle(l1, l2, radians)`.
  - `Tangent` (line+circle, later line+arc) → `addConstraintTangent`.
  - `PointOnLine` (point + line) → `addConstraintPointOnLine`.
- Serialization: add the three kind strings (format stays v3; unknown kinds
  already round-trip as generic `{kind,a,b,value}`).
- `DrawSketchToolbar`: an **Angle** button (enabled with 2 lines selected) with a
  **degrees** input that converts to radians; **Tangent** (line + circle/arc);
  **PointOnLine** (point + line). Reuse `ApplySketchConstraint(kind, value)`.
- Tests (`test_sketch_solver.cpp`): two lines → 45°, assert solved angle; a line
  made tangent to a circle; a point pulled onto a line.

Risk: low. This is the same shape as the v1 constraints.

## Phase B — Whole-entity dragging (medium, ~2–3 days)

Today the Select-tool drag moves a single endpoint (one `SetDragPoint`). Extend it
to drag a whole line or circle.

- **Picking** (`SketchInteraction`): `PickSketchPoint` today returns the nearest
  endpoint. Add a body-hit result: if the click is not within the endpoint
  tolerance but is within tolerance of a **segment** (line) or a **circle/arc
  circumference** or **center**, return an *entity* hit with a drag mode
  (`DragEndpoint` | `DragEntityBody` | `DragCircleRim`).
- **Drag-solve** each frame:
  - `DragEndpoint` — unchanged.
  - `DragEntityBody` (line/arc/circle by its interior or center) — translate every
    one of the entity's points by the frame's UV delta, pin them all
    (`SetDragPoint` per point), `Solve`, `WriteBack`. Constraints on other
    entities relax to follow.
  - `DragCircleRim` — drive the radius instead of translating (matches Onshape).
- Mouse-up commits one `ModifyFeatureCommand` (baseline snapshot on mouse-down),
  exactly like the endpoint drag.
- `SketchSolver` may need a `SetDragPoints(vector<PointRef,target>)` helper so the
  DogLeg solve sees several pinned points at once. FreeCAD's `moveGeometry` is the
  reference.
- Tests: GUI `drag/move_whole_line` (both endpoints translate by the same delta,
  a coincident-constrained neighbour follows); `drag/resize_circle`.

Risk: medium — multi-point drag can oscillate if pinned too hard; mitigate by
pinning with the solver's move mode rather than hard coincidence.

## Phase C — Arcs (medium-large, ~3–4 days)

The biggest geometry addition. Do circular arcs; **defer B-splines to Phase E.**

- **Data model**: add `sketch::Arc { glm::vec2 center; double radius; double
  startAngle; double endAngle; }` to the entity variant. Extend `PointRef::which`
  so an arc addresses `start`, `end`, and `center`.
- **Rendering** (`SketchRenderer` / `AppendSketchLines`): tessellate an arc into a
  short polyline (like circles today).
- **Profile / extrude** (`SketchProfile::BuildLoops` + the OCC wire builder): arcs
  join lines in a loop by their endpoints. Build a real OCC circular edge
  (`BRepBuilderAPI_MakeEdge` with a `gp_Circ` + parameter range) rather than a
  tessellated approximation, so extrudes stay exact.
- **Drawing FSM** (`SketchInteraction`): a new `SketchTool::Arc`, 3-click
  (start → end → point-on-arc, which fixes the bulge). Toolbar gets an Arc button.
- **Solver** (`SketchSolver`): map `Arc` to `GCS::Arc` (center/start/end/radius/
  angles) with `addConstraintArcRules`; arc endpoints become solver points, so
  coincident/tangent/radius work on them for free. Enables line–arc **Tangent**
  from Phase A and `addConstraintPointOnArc`.
- Tests: arc round-trips through serialization; a rectangle-with-a-filleted-corner
  (two lines + a tangent arc) closes and extrudes; arc radius constraint solves.

Risk: medium-large — the OCC arc edge in the profile builder is the fiddly part
(parametrization direction / orientation in the wire).

## Phase D — Auto-constraint inference (medium, ~3–4 days, UX-heavy)

Onshape-style live inference while drawing and dragging: snap the cursor and add
the implied constraint automatically, with a ghost glyph showing what will be
applied.

- An inference pass (new `SketchInference.{h,cpp}`) runs each frame during the
  draw FSM and Select drag, over the current `uv` vs existing geometry, using the
  shared `WorldToViewport` math for pixel tolerances:
  - endpoint near an existing point → **coincident** (snap uv to it),
  - segment nearly axis-aligned → **horizontal/vertical**,
  - endpoint near a line → **point-on-line**,
  - endpoint near a circle/arc → **tangent** / **point-on-arc**,
  - two segments near-parallel/-perpendicular → **parallel/perpendicular**.
- Snap `uv` to the inferred target and, on commit, append the inferred
  constraints (then solve, as usual).
- Draw ghost glyphs (H, V, ●, ⟂) near the cursor for the active inference; a small
  overlay legend.
- A toggle (`state.autoConstrain`, default on) — some users want it off; a
  modifier key (e.g. hold Ctrl) suppresses inference for one click.
- Tests: GUI `infer/horizontal_snaps` (draw a near-horizontal line → an H
  constraint appears and y's equalize); `infer/endpoint_coincident`.

Risk: medium — mostly tuning tolerances and avoiding over-constraining; keep the
inferred set minimal and let the DOF badge show the result.

## Phase E — B-splines (large, defer)

`GCS::BSpline` exists but poles/knots/weights management, the control-point
drawing UX, and the OCC B-spline profile edge are each substantial. Split into its
own milestone after A–D land. Not planned in detail here.

## Cross-cutting

- **DOF badge**: surface the solver's remaining DOF / "fully constrained" /
  conflicting set (already partly wired) as each phase adds constraints.
- **Remote parity**: `add_constraint` already takes `{kind, refs, value}`; the new
  kinds work over the wire for free. Add arc entities to the sketch `add_feature`
  schema in Phase C. Extend `remote_smoke.py` per phase.
- **Entity deletion** is still forbidden while constraints reference an entity
  (v1 limitation); revisit when the constraint list gets an editor.

## Recommended order

A (angle/tangent/point-on-line) → B (whole-entity drag) → C (arcs) → D
(auto-constrain) → E (splines, later). A and B are quick wins on the existing
foundation; C unlocks real profiles; D is the polish that makes it feel like
Onshape.
