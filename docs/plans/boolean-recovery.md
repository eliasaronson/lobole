# A refused boolean gets one fuzzy retry

## The problem

Every body boolean is now checked (`BooleanRefusal`, BodyMode.cpp), and a
result that is the wrong size or the wrong way out is refused instead of kept.
Both refusals the generators have hit were coincident geometry, which
OpenCASCADE's exact booleans handle badly:

- A slot exactly as wide as a seat, with its floor on the seat's axis: the
  cut returned the target unchanged.
- Two spokes whose axes cross 8 µm apart: the union counted the overlap
  twice.

The model was reasonable both times. A user, or an agent, who draws that
now gets a refusal and has to nudge dimensions by hand. The chassis jig and
the bike carry those nudges.

## The shape of the fix

- **One retry.** When the check refuses a result, run the same boolean once
  more with OCC's fuzzy option (`SetFuzzyValue`). The tolerance is derived
  from the operands' size, a small fraction of the smaller bounding box's
  diagonal, never a literal. The retry uses the same non-destructive,
  `BuildNonDestructive` path, and its result gets the SAME check. It is kept
  only if it passes. The feature records that it needed the fuzzy retry (a
  warning on the feature, carried in `regen`), so a caller can see it.
- **If the retry fails too**, the refusal stands, and it now says the
  retry was tried and with what tolerance.
- **Every boolean path.** This applies wherever the check runs:
  `ApplyBodyMode` add and cut, and the `Boolean` feature including
  intersect.
- **The OM615's "Shaft bores: cut removed no material" warning** is looked
  at. If it is the same class, it is fixed by this; if not, the reason is
  written down.

## Files

- `src/core/BodyMode.{h,cpp}`, `src/cad/NonDestructiveBoolean.h` or
  `CADKernel.cpp`, `src/core/features/BooleanFeature.cpp`.
- The two repros in `scratch/kernel-repros/` become tests.
- `generators/chassis` (jig slot) and `generators/bike` (spoke stagger) go
  back to their natural dimensions, if the retry makes them pass.
- `docs/modeling/README.md#a-boolean-is-checked-before-it-is-kept`.

## Verification

- `tests/test_boolean_volume.cpp`: both repros produce the right volume
  through the retry, with the warning. A case the retry cannot save is still
  refused, with the new text.
- The cost on the OM615 preview build: retries happen only on refusals, so
  it should be unchanged.
- Chassis and bike generators pass with their natural dimensions.
- Headless suite.
