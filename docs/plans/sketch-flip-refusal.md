# A dimension edit never flips the sketch

## The problem

Every change to a dimension's value is solved in one jump: `Solver::Load`
seeds PlaneGCS with the current geometry, DogLeg runs once toward the new
value, and whatever it converges to is written back. A sketch has more than
one solution for most values (a triangle and its mirror image, an arc and its
complement), and one jump lands on whichever basin it falls into. Driven
headless over RPC, on a sketch where every value is reachable:

- **An arc turns inside out.** Arc ends fixed at (0,0) and (100,0), radius
  58.31: a 118° arc, centre (50, −30). Radius 5000 by `modify_feature`
  gives centre (50, +4999.75) and a 358.9° sweep, the complement; set back to
  60 it stays a 247° major arc. 2000 and 10000 keep the minor arc while 5000
  and 20000 flip it, so the result of an edit is effectively random. The
  `set_variable` path (an expression-driven radius) flips it the same way.
- **A triangle mirrors.** Base fixed, 100 long, the other two sides driven by
  variables: from 10000 down to 51 puts the apex at y = −10.05; from 500,
  1000, 2000 or 20000 it stays at +10.05.
- **A value that is reachable fails.** Radius 1e7 on the same arc (any radius
  over 50 is valid) does not converge in one jump: `modify_feature` refuses it
  as "does not solve", and through `set_variable` the old geometry stays.

The edit paths also differ in what happens when the solve fails:

| path | on a failed solve |
|---|---|
| `modify_feature` (literal value, primitives) | refused, document unchanged |
| GUI inline edit (`ApplyDimensionEdit`) | value stored, geometry not moved, undo record pushed |
| regeneration (`set_variable`, a variable the sketch reads) | old geometry kept, `LOG_WARN` only: `regen` says `ok`, no warning. Radius 49 on the arc above leaves it at 58.31 with a dimension that reads 49 |

None of them checks the result for a flip. The guards already in the solver
hold sides the constraint itself can see: line–circle tangency and
`DistancePL` pin the side at load, a tangent at a shared end keeps 0 or π,
circle–circle tangency keeps inside/outside, and `DistanceX/Y` and `Angle`
are signed. A triangle of unsigned distances and an arc's sweep have no
constraint that knows their orientation, which is why those two cases flip.

## The shape of it

### 1. Orientations

A new `src/core/sketch/SketchOrientation.{h,cpp}` (pure geometry, no
PlaneGCS) reads a sketch and gives the orientations its dimensions leave
open, each with the constraints that set it:

- **Triangle turn.** Union the points that `Coincident` joins; join two
  groups by an edge for every `Distance` between them; every triangle of
  edges with at least one solver-free corner is an orientation, its sign the
  cross product of the corners. Its constraints are the three distances.
  Capped (a few hundred triangles, a work limit on the search) so a large
  sketch costs nothing noticeable.
- **Arc sweep.** For each arc with a free parameter: the sign of
  `sweep − π`, which is the side of its chord the centre is on. Its
  constraints are the dimensions on the arc and its ends.

An orientation counts only where it is clear: a triangle's height over
1/1000 of its longest side, an arc's sweep more than 1e-3 rad from π. Nearer
than that, solver noise decides the sign and nothing is refused for it.

The same file gets `MeasureDimension(sketch, constraint)`: the value a
dimension has in the current geometry (`Distance`, `DistanceX/Y`,
`DistancePL`, `Radius`, `Diameter`, `Angle`, with the sign conventions the
solver uses). Regeneration needs it below (the old value is what the stored
geometry measures), and
[sketch-driven-fallback](sketch-driven-fallback.md) needs it to display a
driven dimension.

### 2. A value edit is solved by continuation when one jump will not do

`Solver` records, at load, which solver parameter holds each dimension's
value (the `im.params` slot each dimension case already pushes; a diameter's
is the half). A new entry point solves a value edit:

```cpp
struct EditOutcome {
  SolveResult solve;
  std::vector<size_t> flipped; // constraints whose orientation turned over
  bool reached = false;        // the new values were solved
};
EditOutcome Solver::SolveValueEdit(const std::vector<ValueChange>& changes);
// ValueChange {size_t constraint; double from, to;}
```

It runs after `Load` on the sketch carrying the new values, from the
pre-edit geometry:

1. Read the orientations of the pre-edit geometry.
2. Solve in one jump. If it solves and no clear orientation turned, that is
   the answer, so an ordinary edit costs one solve plus the check.
3. Otherwise **continue**: snapshot every parameter value, set the changed
   dimensions to their old values, and step them toward the new ones in
   sixty-fourths, starting with a half-way step. A step is accepted when it
   solves and keeps every orientation that was clear in the state it started
   from; the next step then doubles. A rejected step restores the snapshot
   and halves. It stops at a sixty-fourth that will not go, or at a budget of
   solves (sized on the largest sketch in `scratch/`, see Verification).
4. `reached` with no flip is accepted. Anything else restores the pre-edit
   geometry and reports `flipped` (the constraints of each orientation that
   turned at the step that was refused) or, with none turned, the failure.

Steps compare with the state they start from, not with the pre-edit
geometry, so a sweep that passes 180° continuously (an arc whose chord and
radius both change) is accepted: near π the orientation is not clear, and
halving finds a step that crosses there. A triangle can only fold through a
flat one, and the side lengths that make a triangle are a convex set (three
linear inequalities), so stepping between two valid sets of lengths never
passes a flat triangle: continuation never carries a triangle across. On the
cases above, continuation keeps the 1.15° minor arc for radius 5000,
reaches 1e7, and keeps the apex at +10.05.

The angle of an `Angle` steps linearly in its stored (signed) value. The
ellipse pre-pass in `Solver::Load` is unchanged; the orientation check runs
on the final result, so the pre-pass is covered too.

### 3. Every value edit goes through it, and a refusal says why

The refusal names the dimension and what would happen, in the same words on
every path: `distance (constraint 7) = 51 would mirror the triangle of
constraints 5, 6, 7` / `radius (constraint 4) = 5000 would turn arc 3 inside
out` / `radius (constraint 4) = 49 cannot be met`.

- **GUI inline edit** (`ApplyDimensionEdit`, `SketchDimensionDraw.cpp`): a
  refused edit restores the old value and expression, pushes no undo record,
  and shows the message as `actionMsg` ("Not changed: …"). A negative value
  is a flip the user asked for: the orientations are read from the seed the
  function hands the solver, after `ReflectPLPoint` and the ref swap, so the
  requested side is the one that is kept.
- **`modify_feature`** (`RemoteModeling.cpp`): the sketch branch that already
  re-solves a changed value calls `SolveValueEdit` with each changed
  dimension's old and new value, and throws the refusal text; its existing
  "does not solve" refusal becomes the same text.
- **Regeneration** (`SketchFeature::Execute`): the re-solve for
  expression-driven dimensions takes each dimension's old value from
  `MeasureDimension` on the stored geometry (what it was solved for) and its
  new one from the refreshed `value`. A refused edit keeps the previous
  geometry, as now, but **says so with `SetWarning`**, the refusal text plus
  "keeping the previous geometry", so `set_variable`'s `regen` names the
  sketch and the dimension instead of answering `ok` with no warning.
  Expression-driven entity coordinates are written into the entities before
  `Execute`, so their old values are gone; they keep the one-jump solve, and
  the orientation check on its result still turns a flip into the same
  warning.

`add_constraint(s)` is not a value edit (the geometry has not been solved
for the new constraint yet) and keeps its single solve.

### 4. Drags step the same way

`UpdateDragSession` (`SketchTools.cpp`) solves each frame from the last one.
Each frame's result is checked against the previous frame's orientations;
on a flip, the frame retries with the target stepped from the previous
frame's target, halving as in step 2, and keeps the furthest step that holds.
The geometry stops at the fold and follows again when the cursor comes back.
A drag flip was not reproduced here (a frame moves little), so this is the
same check run where it is cheap, with its own test.

## Files

- `src/core/sketch/SketchOrientation.{h,cpp}` (new): orientations,
  `MeasureDimension`; `CMakeLists.txt` lists it beside `SketchInference.cpp`.
- `src/core/sketch/SketchSolver.{h,cpp}`: the value-parameter map,
  `SolveValueEdit`, parameter snapshot and restore, the drag retry.
- `src/ui/SketchDimensionDraw.cpp`: `ApplyDimensionEdit` refuses.
- `src/remote/RemoteModeling.cpp`: `modify_feature`'s sketch re-solve.
- `src/core/features/SketchFeature.cpp`: `Execute`'s re-solve and its warning.
- `src/ui/SketchTools.cpp`: `UpdateDragSession`.
- Docs: `docs/sketching/README.md#solving` describes continuation and the
  refusal, `#refusals` lists the new one, `#constraints` drops nothing (the
  existing side guards stay); the `modify_feature` row in its RPC table says a
  flip is refused.

## Verification

- **`tests/test_sketch_solver.cpp` `[solver][flip]`**, each case first
  asserting that a plain `Solve()` from the same seed flips or fails, so the
  test keeps proving the guard does something if PlaneGCS changes:
  - the fixed-ends arc, radius 58.31 → 5000: sweep under 180°, centre below
    the chord; → 1e7 reached; → 49 refused naming the radius;
  - the triangle, sides 10000 → 51: apex above the base;
  - an arc whose chord and radius change together so its sweep crosses 180°
    continuously: accepted;
  - a sketch with no orientation at stake: exactly one solve (count solves
    through a test hook).
- **`[solver][drag]`**: a triangle hinged at one corner, the free corner
  dragged across the far side in one frame: it stops at the fold.
- **`tests/regen_honesty_smoke.py`** gains: a radius driven by a variable set
  to an unreachable value returns `regen.warnings` naming the sketch and the
  dimension; `modify_feature` to 5000 keeps the minor arc; to 49 it is
  refused and the document is unchanged.
- **A GUI test** `dims/edit_refuses_flip`: type 49 into the arc's radius
  label; the label still reads 58.31, the action message says "Not
  changed", and undo has nothing new.
- **Cost**: time `SolveValueEdit` on the largest sketch under `scratch/`
  with a continuation forced; that sets the solve budget in step 2. The
  headless suite, `tools/test-fast.sh`, the GUI suite and
  `benchmarks/runners/reference.py` (its parts edit dimensions) pass.
