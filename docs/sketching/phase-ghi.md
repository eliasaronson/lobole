# Constraint-layer block — Phases G, H, I (detailed plan)

The three phases that complete the constraint layer: **G** dimension display +
editing, **H** construction geometry + the remaining standard constraints, **I**
constraint status & management. Grouped because they interlock — G's dimension
annotations and I's constraint glyphs share a 2D overlay renderer, and H's
construction flag and I's DOF coloring share the entity-styling path. Independent
of arcs/splines; buildable in any order (suggested H → G → I below).

## Current state (verified)

- `sketch::Constraint { kind; PointRef a, b; double value; }` — dimensional
  kinds (Distance, Radius, Angle) carry `value` but no label placement.
- Entities (`Point`/`Line`/`Circle`) have no `construction` flag; `BuildLoops`
  consumes every line/circle.
- The solver already returns `dof`, `conflicting`, `redundant` per solve, but
  `conflicting`/`redundant` are PlaneGCS **tags** (`getConflicting` → `VEC_I`),
  and `Load` assigns `tag = 1..N` per constraint *in order* (skipping ones whose
  refs don't resolve) — there is **no tag→constraint-index map** yet.
- PlaneGCS exposes `addConstraintP2PSymmetric`, `addConstraintMidpointOnLine`,
  `addConstraintDifference`, `getDependentParams` (free-param query, for
  per-entity DOF), `rescaleConstraint`. No `Fix` primitive — pin by *not*
  declaring a point's params as unknowns.
- `DrawSnapGlyphs` (Phase E) is the 2D ImGui foreground-overlay pattern both G
  and I reuse; `BuildActiveSketchPrimitives` (`AppActions.cpp`) is where entity
  colour/style is decided.

---

## Phase G — Dimension display + editing (~2 days)

- **Model**: add `glm::vec2 labelOffset{0}` to `Constraint` (only meaningful for
  dimensional kinds), serialized. New kinds: `DistanceX` / `DistanceY` (drive a
  single coordinate difference via `addConstraintDifference`), `Diameter` (drives
  `radius = value/2`, renders `Ø`). Toolbar/`ApplySketchConstraint` gain them
  (Phase-A shape).
- **Rendering** — `DrawDimensions(state)`, a 2D overlay beside `DrawSnapGlyphs`,
  called from `UI.cpp`. For each dimensional constraint, project its geometry to
  screen and draw:
  - Distance/X/Y: extension lines from the two points + a dimension line with
    arrowheads + the value (`2.00`), offset by `labelOffset`.
  - Radius/Diameter: a leader from centre/rim + `R2` / `Ø2`.
  - Angle: an arc between the two lines + `45°`.
- **Editing**: hit-test the label's screen rect; double-click → an inline
  `InputText` seeded with the value → parse → write `constraint.value` →
  re-solve, as one `ModifyFeatureCommand`. Drag the label → update `labelOffset`.
- **Dimension tool** (`SketchTool::Dimension`): infer the kind from the current
  selection (2 points → distance, +Shift → X/Y; circle → radius/diameter; 2
  lines → angle), place the annotation, prompt for the value. Coexists with the
  toolbar buttons.
- Tests: a placed dimension's label hit-tests; editing its value re-drives the
  geometry; DistanceX / Diameter solve; `labelOffset` round-trips. Rendering
  eyeballed via `/run`.

## Phase H — Construction geometry + extra constraints (~1.5 days)

- **Construction flag**: `bool construction = false` on `Point`/`Line`/`Circle`
  (a `IsConstruction(Entity)` / `SetConstruction` visitor, like `EntityId`),
  serialized (v5→v6; default false on old files). `BuildLoops` **skips**
  construction lines/circles (no profile contribution). `BuildActiveSketch-
  Primitives` renders them **dashed + dimmed** (a dash pattern in the prim
  fragment shader, or short segment stipple in the builder). A toolbar/context
  toggle flips the selected entities' flag (one `ModifyFeatureCommand`).
- **New constraint kinds** — solver map + toolbar + `ApplySketchConstraint`
  validation, mirroring Phase A:
  - **Symmetric**: two points about a line — `addConstraintP2PSymmetric(p1, p2,
    lineP1, lineP2)`.
  - **Midpoint**: a point at a line's middle — `addConstraintMidpointOnLine`.
  - **Fixed/Lock**: ground a point — in `Solver::Load`/`DeclareUnknowns`, omit a
    fixed point's params from the unknowns (track a `fixed` set from the
    constraints). Purely a solver-side exclusion; renders a lock glyph (Phase I).
  - **Concentric**: coincident of two circle centres (reuse `P2PCoincident` on
    centre points).
  - **Collinear**: two lines on one line — `Parallel` + a `PointOnLine` of one
    line's endpoint on the other (or a dedicated equal-angle+offset). Document
    the composition.
- Serialization: new kind strings + the construction flag; remote allowlist
  gains the kinds.
- Tests: a construction line doesn't close a profile but still solves + can be
  snapped to; symmetric reflects a point across a line; midpoint pins to 0.5; a
  fixed point doesn't move under a drag; concentric/collinear solve; v5 file
  loads with `construction=false`.

## Phase I — Constraint status & management (~2 days)

Turns the solver's already-produced dof/conflict data into feedback + control.

- **Solver: expose the mapping** — `Load` records a `std::vector<int>
  tagOfConstraint` (sketch-constraint index → GCS tag, 0 if skipped) and its
  inverse, so `conflicting`/`redundant` tags map back to *our* constraints. Add
  a `std::vector<uint32_t> FreeParamEntities()` (or reuse `getDependentParams`)
  that reports which entity ids still have a free param — the per-entity DOF
  attribution.
- **Fully-constrained coloring**: in `BuildActiveSketchPrimitives`, colour each
  entity by constrained-ness — under-constrained (blue) vs fully constrained
  (a neutral "solved" colour) from the free-param set; the whole-sketch case
  (`dof == 0`) is the fast path. A status line shows the DOF count /
  "fully constrained".
- **Persistent constraint glyphs**: a `DrawConstraintGlyphs(state)` overlay
  (reusing the Phase E glyph primitives) that draws H / V / ● / ⟂ / = / ∥ / lock
  badges *on the committed constraints* near their geometry, with a show/hide
  toggle (default on). Distinct from the transient inference glyphs.
- **Delete an individual constraint**: hit-test a constraint glyph (or a
  constraint list panel) → select → Delete removes just that constraint and
  re-solves (one `ModifyFeatureCommand`). Closes the deferred gap.
- **Conflict / over-defined highlighting**: draw the constraints whose tags are
  in `conflicting`/`redundant` in red (via the tag→constraint map) so an
  over-defined sketch is diagnosable.
- Tests: DOF attribution marks a free line under-constrained and a fully-pinned
  one solved; the tag→constraint map round-trips a known conflicting set to the
  right constraint index; deleting a constraint frees the expected DOF and
  re-solves; a redundant constraint is flagged.

---

## Shared infrastructure (build once, both G & I use it)

- **2D overlay renderer**: G's `DrawDimensions` and I's `DrawConstraintGlyphs`
  are siblings of `DrawSnapGlyphs` — same `ImGui::GetForegroundDrawList()` +
  `SketchUVToScreen` projection. Factor the arrow/leader/label helpers once.
- **Entity styling**: H's dashed construction and I's DOF colour both feed
  `BuildActiveSketchPrimitives`'s per-entity colour/width/dash — extend that one
  function, don't fork it.
- **Hit-testing screen annotations**: G's label edit and I's constraint-glyph
  select both need "is the cursor on this 2D overlay element" — a shared
  `screen-rect / leader hit-test` helper.

## Build order (each step green)

Suggested **H → G → I** (H adds constraints for I to visualise; G is the big
user-facing win; I lights everything up), but any order works:

1. **H**: construction flag (model + BuildLoops skip + dashed render) → its
   tests; then the extra constraint kinds → their tests.
2. **G**: `labelOffset` + DistanceX/Y/Diameter + `DrawDimensions` +
   inline-edit + the Dimension tool.
3. **I**: solver tag-map + free-param query → coloring → persistent glyphs →
   delete-a-constraint → conflict highlighting.

## Verification

- `cmake --build build && ctest` green; `remote_smoke.py` passes.
- `/run`: draw a rectangle, dimension a side and a diagonal, double-click to edit
  the value and watch it re-drive; add a construction centreline and confirm it
  doesn't extrude but snaps; mark two holes symmetric about it; watch the sketch
  turn "fully constrained" colour as you pin it; delete one constraint and see it
  go blue again; over-constrain it and see the culprit flagged red.

Commit as three: `feat: sketch construction geometry and extra constraints`,
`feat: sketch dimension display and editing`, `feat: sketch constraint status
and management`.
