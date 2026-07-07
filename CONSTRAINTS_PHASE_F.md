# Phase F — Whole-entity dragging + drag-solver rework

Detailed plan for Phase F of [CONSTRAINTS_PLAN.md](CONSTRAINTS_PLAN.md). Adopt
the FreeCAD PlaneGCS-native drag recipe: build the solver once on mouse-down,
pin the dragged points with *weak* temporary constraints, and per frame just
overwrite the targets and re-solve — replacing today's naive per-frame rebuild,
and extending single-endpoint drag to whole lines and circle radius.

## Current state (verified)

- The Select-tool drag rebuilds the whole system every frame:
  `Solver::Load()` + `SetDragPoint()` + `Solve()` + `WriteBack()`
  (`SketchInteraction.cpp`). `SetDragPoint` adds a **hard** `P2PCoincident` to a
  fixed target (`SketchSolver.cpp`) — no down-weighting, so a heavily
  constrained sketch can oscillate.
- The `Solver` is created fresh each frame and is **not** stored anywhere — the
  rework needs it to persist across the drag.
- Phase C body picking (`PickSketchEntity`) already returns a `body` flag + the
  entity ref; a circle body hit is its **rim**, the circle **centre** is an
  endpoint hit. Resolve `ref.entity`'s type to tell a line body from a circle
  rim.
- The vendored PlaneGCS exposes everything the recipe needs:
  `rescaleConstraint(id, coeff)` (`GCS.h:605`), `initSolution(DogLeg)` (`:609`),
  `DefaultTemporaryConstraint = -1` (`:103`). The Impl already uses `std::deque`
  for params (stable addresses — safe to append helper params mid-drag).

## Solver API rework (`core/SketchSolver`)

Replace `SetDragPoint` with a persistent drag session (FreeCAD `initMove` /
`moveGeometries`):

```
// After Load(). Pins each point in `points` with a weak temporary coincident
// against a free helper param (rescaleConstraint(id, 0.01) — the anti-
// oscillation soft pull). Declares unknowns + initSolution(DogLeg). Returns
// false (refuses) if the current system has conflicts.
bool BeginDrag(const std::vector<PointRef>& points);

// A circle-radius drag: a weak PointOnCircle between the rim and a free helper
// point, so driving the helper to the cursor changes the radius (centre held).
bool BeginRadiusDrag(uint32_t circleId);

// Overwrite the per-point drag targets (one per BeginDrag point; for a rigid
// body translation each is initialPos + cursorDelta). For a radius drag, the
// single target is the cursor. No rebuild.
void SetDragTargets(const std::vector<glm::vec2>& targets);

// solve(true, DogLeg), single subsystem, no fallback chain (isInitMove) — no
// reload. Re-initSolution() internally if the cursor strayed far from the
// linearization point (FreeCAD's >20*step guard).
SolveResult SolveDrag();

// WriteBack() unchanged. EndDrag() clears the temporary constraints.
```

The whole session lives as long as the `Solver` object does (pImpl deques keep
pointer stability), so **store the `Solver` in state** for the drag's lifetime.

## Picking → drag mode

On mouse-down in the Select tool, classify the pick:

- **Endpoint hit** (Point, line endpoint, or circle centre) → `DragEndpoint`:
  `BeginDrag({ref})`, target = cursor. (Dragging a circle centre translates it —
  it's the circle's only point.)
- **Line body hit** → `DragEntityBody`: `BeginDrag({both endpoints})`; each
  frame target[i] = initialPos[i] + (cursor − initialCursor) — a rigid
  translation. Constrained neighbours follow.
- **Circle rim body hit** → `DragCircleRim`: `BeginRadiusDrag(id)`; target =
  cursor, so the radius tracks `|cursor − centre|` while the centre stays put.

`BeginDrag`/`BeginRadiusDrag` returning false (conflicting sketch) → no drag
(leave geometry untouched), matching FreeCAD's refuse-on-conflict.

## State + interaction (`AppState`, `SketchInteraction`)

- A `SketchDragSession` in `AppState` (not `SketchEditState`, to keep that
  cheaply-copyable): `std::unique_ptr<sketch::Solver> solver;` + the mode, the
  dragged `PointRef`s, their `initialPos`, `initialCursor`, and (rim) the
  circle id. `active` flag.
- Mouse-down (Select tool): pick, classify, capture the mouse-down JSON baseline
  (as today), `BeginDrag`. Mouse-move: `SetDragTargets` from the cursor delta,
  `SolveDrag`, `WriteBack`, `Regenerate`. Mouse-up: `EndDrag`, one
  `ModifyFeatureCommand` from the baseline (unchanged commit pattern), release
  the session.
- This *replaces* the current per-frame `Load()` path for plain endpoint drags
  too — they go through the same session with a single point.

## Tests

Most coverage is at the solver level (deterministic, no GUI/rendering — the
GPU-heavy GUI path stays thin):

- `test_sketch_solver.cpp`:
  - **move_whole_line**: two lines joined by a coincident; body-drag one line by
    a delta → both its endpoints translate by that delta and the neighbour's
    coincident endpoint follows.
  - **resize_circle**: rim-drag a circle → radius ≈ `|cursor − centre|`, centre
    unchanged.
  - **endpoint_drag_no_oscillation**: a heavily constrained sketch (e.g. a
    constrained rectangle), repeated `SetDragTargets`+`SolveDrag` steps converge
    to stable, finite positions (no blow-up) — the rescale-0.01 guard.
  - **refuse_on_conflict**: a conflicting sketch → `BeginDrag` returns false.
- GUI (`GuiTests.cpp`, minimal — viewport-drag wiring): `drag/move_whole_line`
  (mouse-down on a segment, drag, mouse-up → the line moved, one undo step).
  Keep it small; the mechanism is covered above.

## Build order (each step green)

1. Solver session API (`BeginDrag`/`SetDragTargets`/`SolveDrag`/`EndDrag` +
   `BeginRadiusDrag`) with the weak rescaled constraints; solver tests. 2. Wire
   the Select-tool mouse-down/move/up to the session (replace the per-frame
   `Load`), with the pick→mode classification. 3. `BeginRadiusDrag` + rim mode.
   4. GUI wiring test + `/run` visual.

## Verification

- `cmake --build build && ctest` green; `remote_smoke.py` passes.
- `/run`: draw two lines sharing a coincident corner; drag one line by its
  interior and watch the shared corner follow; drag a heavily constrained
  rectangle corner and confirm it doesn't jitter; grab a circle's rim and resize
  it with the centre fixed.

Risk: low-medium — the reference implementation (FreeCAD `Sketch::initMove` /
`moveGeometries`) is mapped in CONSTRAINTS_PLAN.md; the rescale-0.01 weak pull is
the known anti-oscillation fix, and refuse-on-conflict avoids garbage solves.

Note for later: built off the generic dragged-point set, so Phase J arcs slot in
by exposing start/end/centre points (endpoint/body drag free) plus an arc-rim
mode reusing `BeginRadiusDrag`'s scalar-DOF pattern.

Commit as `feat: whole-entity dragging with the FreeCAD drag-solver recipe`.
