# Phase A — Angle, Tangent, PointOnLine constraints (implementation plan)

Detailed plan for Phase A of [plan.md](plan.md). Pure
extension of the v1 constraint plumbing; no new geometry, no format bump.

Two corrections to earlier assumptions, found by reading the code:

- `ConstraintKindFromString` **throws** on unknown kinds
  (`src/core/Serialization.cpp:90`) — there is no generic round-trip for
  unknown kinds. New kind strings must be added in both directions. Files
  saved with the new kinds won't load in older builds; acceptable, no
  version bump needed (the loader only rejects versions *newer* than
  `kVersion`).
- The remote server keeps its own static kind allowlist
  (`src/remote/RemoteServer.cpp:301`) — the three new kinds must be added
  there too; they do **not** ride along for free.

## Constraint semantics (ref conventions)

| Kind | `a` | `b` | `value` |
|---|---|---|---|
| `Angle` | line entity | line entity | radians (UI shows degrees) |
| `Tangent` | line entity | circle entity | unused |
| `PointOnLine` | point (entity+which) | line entity | unused |

Store **radians** in `Constraint::value`; only `SketchToolbar` converts
degrees↔radians at the input box.

## 1. `src/core/SketchFeature.h`

Add `Angle`, `Tangent`, `PointOnLine` to `ConstraintKind` (after `Radius`,
so existing serialized enum-free code is unaffected).

## 2. `src/core/SketchSolver.cpp` (`Solver::Load`, constraint switch)

Vendored PlaneGCS signatures (`deps/planegcs/GCS.h`):

- `addConstraintL2LAngle(Line&, Line&, double* angle, tag)` — the angle is a
  **pointer**; push `con.value` into `im.params` first (same pattern as
  Distance/Radius, which keeps the storage stable in the deque).
- `addConstraintTangent(Line&, Circle&, bool ccw, tag)` — implemented as a
  *signed* point-to-line distance (`GCS.cpp:1121`), so `ccw` selects which
  side of the line the circle sits on. Compute it from the loaded geometry at
  add time so the solve keeps the circle on its current side instead of
  yanking it across the line:
  `ccw = cross(l.b - l.a, center - l.a) > 0` using the entity shapes already
  captured in `im.entityShapes`.
- `addConstraintPointOnLine(Point&, Line&, tag)` — direct; resolve the point
  with `im.Resolve(con.a)`, the line via `im.lineOf`.

All three follow the existing guard style (skip silently if the refs don't
resolve to the right entity types).

## 3. `src/core/Serialization.cpp`

`ConstraintKindToString` / `ConstraintKindFromString` gain
`"angle"`, `"tangent"`, `"point_on_line"`. Format version unchanged.

## 4. `src/remote/RemoteServer.cpp`

Add the three entries to the `kinds` map in `add_constraint` (line ~301).

## 5. `src/ui/SketchInteraction.cpp` (`ApplySketchConstraint` switch)

- `Angle`: like Parallel — two lines required, `c.a = {sel[0].entity, 0}`,
  `c.b = {sel[1].entity, 0}`, keep `value` (radians).
- `Tangent`: require one line + one circle **in either order**; normalize to
  `a = line, b = circle` before storing.
- `PointOnLine`: require ≥2 selections where `sel[1]` (or `sel[0]`,
  whichever is the line) resolves to a line and the other is the point ref;
  store the point ref verbatim in `c.a` (its `which` matters), line entity in
  `c.b`. Reject when the picked point belongs to the same line (it's trivially
  on it — would add a redundant/degenerate constraint).

## 6. `src/ui/SketchToolbar.cpp`

Reuse the `conButton` lambda row:

- **Tangent** button (`twoSel && (line+circle in either order)`).
- **PointOnLine** button (2 selections, exactly one resolving to a line,
  point not on that line).
- **Angle**: in the dimensional row, a second `static float angleDeg = 90`
  input (`DragFloat`, 0–360) + button enabled with `twoLines`, calling
  `ApplySketchConstraint(state, K::Angle, glm::radians(angleDeg))`.

## 7. Tests

`tests/test_sketch_solver.cpp` (Catch2, follow the existing fixtures):

- `Angle constraint sets the angle between two lines` — two lines at ~30°,
  constrain to 45°, assert `acos(dot(d1,d2))` ≈ π/4. Note L2LAngle is
  direction-sensitive (measured from l1's direction to l2's); assert with the
  entity directions, not `abs()`.
- `Tangent keeps circle on its side of the line` — circle above a horizontal
  line, constrain tangent, assert distance(center, line) ≈ radius **and**
  center.y still > line y (the `ccw` side computation).
- `PointOnLine pulls a point onto a line` — free `Point` entity off-axis,
  constrain onto a fixed-ish line, assert colinearity of (a, b, p).

`tests/test_serialization.cpp`: round-trip a sketch containing all three new
kinds; assert kind strings `angle`/`tangent`/`point_on_line` in the JSON.

`tests/remote_smoke.py`: one `add_constraint` call with `"angle"` on two
lines, assert solved geometry.

## Order of work

Everything compiles after each step: 1 → 2 → 3 (+ solver & serialization
tests) → 4 (+ smoke test) → 5 → 6 (manual check in the app) → GUI polish.
Done = `cmake --build build && ctest` green and the three buttons usable in a
live sketch.
