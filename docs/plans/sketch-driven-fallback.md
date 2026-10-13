# Driven dimensions, and constraints that must constrain

## The problem

A sketch takes any constraint it can represent, whether or not it adds
anything. Only an unsupported one is refused
([refusals](../sketching/README.md#refusals)); a redundant or conflicting one
is stored and left for the user to notice.

- **Over RPC nothing is said at all.** On a fully defined triangle (base
  fixed and horizontal, three lengths), `add_constraint` with a `distance_x`
  that repeats the base length answers `added: 1`, `regen.ok`, no warnings.
  So does a `distance` of 10 between two corners that are 72 apart: the solve
  fails, the geometry stays where it was, and the sketch now holds a
  dimension it does not meet. No RPC reports a sketch's DOF or its
  conflicts, so a caller cannot find out afterwards either.
- **In the GUI, dimensioning defined geometry makes a broken sketch.** The
  Dimension tool commits the measured value as a driving dimension
  (`CommitInferredDimension` → `ApplySketchConstraint`). On geometry that is
  already determined it is redundant: `UpdateSketchStatus` paints it red, and
  the inline editor opens on it as if it could be edited. Typing any other
  value then conflicts. Users dimension finished geometry to read it, and
  every CAD tool they know adds a reference dimension for that.
- **The relation commands** (H, V, parallel, equal, …) are added the same way
  when they repeat what the sketch already says. Only auto-constraints are
  checked: `ApplyInferred` (`SketchTools.cpp`) keeps an inferred constraint
  only when the solve after it succeeds, reports no conflict, and has fewer
  DOF. That is the right rule; it is just not applied to what the user or an
  agent adds.

Loböle has no driven (reference) dimension: `IsDimension` means "driving",
and `sketch::Constraint` has no flag to say otherwise.

## The shape of it

### 1. A driven dimension

`sketch::Constraint` gains `bool driven`, legal on the dimension kinds only.
A driven dimension:

- **adds no equation.** `Solver::LoadFrom` skips it (its `constraintFirstTag`
  slot spans no tags, so the tag→index mapping holds) and it never appears
  in the conflict lists or the DOF;
- **has no input.** Its value is what the geometry measures,
  `sketch::MeasureDimension` (`SketchOrientation.h`, from
  [sketch-flip-refusal](sketch-flip-refusal.md); whichever plan lands first
  adds it), computed where it is read: the label, the serializer and
  `get_state`. It is never stored as a separate truth, so it cannot go stale.
  A `valueExpr` on a driven dimension is refused;
- **is drawn in parentheses in a muted colour**, from a theme token next to
  the dimension colour, and is never painted as conflicting;
- **opens no editor.** Double-clicking its label says "A driven dimension
  measures the sketch; make it driving to edit it".

Format: `"driven": true` in the constraint JSON, omitted when false, and
`value` written as the measure. The loader refuses `driven` on a
non-dimension kind, naming the dimension kinds. A `vNEXT` bump with golden
`tests/golden/vNEXT-driven-dimension.json` and its row in
`docs/versioning/format.md`'s Pending changes.

### 2. One test for "does this constrain anything"

`ApplyInferred`'s rule becomes a function every add goes through,
`sketch::AddVerdict JudgeAddition(const SketchFeature& before, const
SketchFeature& after)` in `SketchSolver.{h,cpp}`: one solve of each, and the
addition is accepted when the solve after succeeds, its DOF is strictly
lower, and no constraint is conflicting or redundant after that was not
before. A refused verdict carries the constraints PlaneGCS names (the set the
new constraint repeats or contradicts), so the message can point at them. A
sketch that was already over-constrained can still take a constraint that
touches none of the broken ones. `ApplyInferred` calls it too.

### 3. What each entry point does with a refusal

- **GUI Dimension tool** (`CommitInferredDimension`, `SketchDimensions.cpp`):
  a dimension that does not constrain is added **driven**, with the status
  "That dimension would over-constrain the sketch, so it was added as a
  driven (reference) dimension", and no inline editor opens. Its value is the
  measure, so it can never conflict.
- **Dimension typed right after drawing** (`DrawDimensionOnCreate`,
  `SketchDimensionDraw.cpp`): the typed value is an input, so a driven
  dimension cannot carry it. It is refused: "Not added: the sketch already
  fixes that (constraints 3, 5)".
- **Relation commands** (`ApplySketchConstraint`, `SketchOps.cpp`): refused
  with the same message, nothing added, as unsupported ones are now.
- **`add_constraint`**: refused, naming the constraints involved; for a
  dimension kind the message adds "pass driven: true to add it as a
  reference dimension". `driven: true` is a new parameter, accepted on a
  dimension kind with neither `value` nor `expr` (either one with it is
  refused), and the answer gives `measured` in the kind's unit. Every answer
  also returns the sketch's `dof` after the add.
- **`add_constraints`**: judged once over the whole list; a refusal leaves
  the sketch as it was and names the index of the first refused spec, as an
  unsupported one does now.

### 4. Switching a dimension between driving and driven

The Model panel's Constraints section and the dimension label's context menu
get **Driven** as a toggle. Making a driven dimension driving takes its
measured value as the value and goes through `JudgeAddition`, so a dimension
that would over-constrain the sketch stays driven, with the reason. Making a
driving one driven cannot over-constrain anything. Both are one undo step.
Over RPC, `modify_feature` with the flag changed takes the same check.

Sketches already saved with redundant constraints load and solve as they do
now, painted red; nothing is converted for them.

## Files

- `src/core/features/SketchFeature.h`: `Constraint::driven`.
- `src/core/io/SerializeFeatures.cpp`: write and read `driven`, the measured
  `value`, the refusals; `src/core/io/SerializationInternal.h`:
  `kVersionPending`.
- `src/core/sketch/SketchSolver.{h,cpp}`: skip driven constraints;
  `JudgeAddition`.
- `src/core/sketch/SketchOrientation.{h,cpp}`: `MeasureDimension`, if
  [sketch-flip-refusal](sketch-flip-refusal.md) has not added it.
- `src/ui/SketchOps.cpp` (`ApplySketchConstraint`, the toggle),
  `src/ui/SketchTools.cpp` (`ApplyInferred`), `src/ui/SketchDimensions.cpp`
  (driven fallback), `src/ui/SketchDimensionDraw.cpp` (label, no editor,
  on-create refusal, context menu), `src/ui/SketchToolbar.cpp` (the Model
  panel toggle), the theme for the muted colour.
- `src/remote/RemoteModeling.cpp`: `add_constraint(s)` judgement, `driven`,
  `measured`, `dof`, and the `MethodDoc` rows.
- Docs: `docs/sketching/README.md` (`#constraints` for the flag, `#refusals`
  for the new refusal, `#gui` for the fallback and the toggle, the RPC
  table); `docs/versioning/format.md`.

## Verification

- **`tests/test_sketch_solver.cpp` `[solver]`**: a driven distance on a
  fully defined triangle leaves DOF 0 with no conflicts, and reads the
  measured length after another dimension changes the triangle;
  `JudgeAddition` refuses a repeated horizontal and a contradicting
  distance, naming the constraints, and accepts an addition to an
  over-constrained sketch that touches none of its broken constraints.
- **`tests/test_sketch_feature.cpp` `[io]`**: a driven dimension round-trips;
  `driven` on a `horizontal` is refused at load; the golden `vNEXT` pair.
- **An RPC smoke** (`tests/sketch_constraint_refusal_smoke.py`): the two
  constraints reproduced above are refused and the sketch is unchanged; the
  same distance with `driven: true` is added and answers `measured`; `driven`
  with a `value` is refused; `add_constraints` with one bad spec in three
  adds nothing and names index 1.
- **GUI tests** `dims/over_constraining_becomes_driven`: dimension a side of
  a fully defined rectangle; the label reads in parentheses, the status line
  says why, nothing is red, and double-clicking it opens no editor;
  `constraints/repeat_is_refused`: H on a horizontal line adds nothing and
  says so; `dims/toggle_driven`: a driven dimension on an under-defined
  sketch made driving holds its value and lowers the DOF.
- `tools/ctest -C generators -L generators` and
  `benchmarks/runners/reference.py` pass: any generator or reference task
  that adds a redundant constraint is fixed at the source, since the GUI
  already shows such a sketch red. The headless, fast and GUI suites pass.
