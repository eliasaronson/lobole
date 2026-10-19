# Assembly sequences for the engine and the Strandbeest

## The problem

Assembly sequences are built ([sequences](../assemblies/README.md#assembly-sequences))
and the clock states both stages'. The engine and the Strandbeest do not
yet, and what the first tries found says each needs more than a list of
steps.

- **The engine.** Its real order (bedplate, crank down into the mains,
  each rod down and its cap up, the cap pins along X, the piston, the
  wrist pin last through both; the block down over the pistons; sump,
  head over its valves, cam, cover; pulleys, belt and flywheel along X)
  fails in three places on the saved engine, at allowance 0:
  - the cap pins, pressed along X from the nearer end, are forced 52-74 mm
    through the bedplate and 18.5 mm (0.45 deep) through the crank webs --
    with a one-piece crank no order gets a pin past a web
    (`engine_spec.solve`: `y_web` reaches past the pin's inner edge,
    `y_cp`);
  - the block lowered over the pistons is forced 98.6 mm, 3.4 mm into a
    piston: something of the piston (a wrist pin's ends?) stands proud of
    the bore;
  - a step's 2D reading near its seat is more than the boolean finds on
    the rod caps and valve springs (said, not failed).
  The 78-step check takes about 460 s against the test's 420 s budget.
- **The Strandbeest.** The stage-1 assembly places both frame plates as
  one occurrence and both discs, the crank pin and the handle as another,
  so its card's order cannot be stated on it; a put-together assembly of
  the printed pieces at the solved placements can. On that, the card was
  found wrong twice: pin FC cannot go up from below (the plate is under
  its hole: 6.2 mm forced), and the last plate cannot go down over the
  last disc's flange (7.6 mm). A cap put on with the crank turned needs
  the pose changed between steps, which one sequence cannot say.

## The shape of the fix

1. Engine: move `y_cp` out (or `y_web` in) so a cap pin passes the webs,
   find what stands proud of the piston, state the sequence in
   `build.py` with its marker and an `assembly_sequence` requirement,
   re-export and slice its jobs if the geometry moved, and give the test
   the budget the check takes.
2. Strandbeest: build the put-together assembly (`assembled()` records
   each piece's placement), state the card's order on it with the two
   corrections (each new pin in first, its bar laid on; the last plate
   before the last disc), check the crank-turned caps in an assembly
   solved at that angle, and replace `check_order`.
3. App: per-step poses (a mate value to hold while a step goes on), and
   two parts going on together.

## Files

`generators/engine/{build.py,engine_spec.py}`, `generators/strandbeest/build.py`
and their READMEs and `test.json`; for 3, the sequence model in
`src/core/Occurrence.h` and `src/remote/RemoteInspection.cpp`.

## Verified by

The engine and Strandbeest generator tests with their new markers, the
engine's jobs sliced clean in the Bambu CLI if its geometry changed, and
`assembly_sequence_smoke` with a step held at a pose.
