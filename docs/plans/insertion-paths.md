# Insertion paths: a part must be able to reach where it is mated

## The problem

The clock's stage-1 escape wheel could not be assembled. Its bore is a press
D (0.05 mm a side over the arbor, printing under), and the escape arbor's
D-flat stops short of both ends, so the round back pivot has to be forced
through the whole hub to reach the D: it bound halfway. The anchor arbor
has the same shape, and so do stage 2's escape, third and centre arbors.
Nothing caught it. The motion studies start with every part already seated,
and the Strandbeest's `check_order` checks the order of steps, not whether a
piece can travel to its seat. The short D parts (the pendulum rod on the
anchor arbor) went together because the press there is only as long as the
part.

## The shape of the fix

1. **An insertion check.** For a mated pair that slides together along an
   axis (a cylindrical or revolute mate, or a press fit between a bore and a
   shaft), sweep the moving part along the axis from clear of the other to
   its seat, from the end the mate or the assembly order says it goes on
   from, and measure the interference at each step (the exact boolean the
   clash check already runs, non-destructive). It reports the length of
   travel under interference and the worst depth, and fails a pair whose
   travel under interference is longer than its press length (the overlap of
   the bore and the shaft's press band at the seat) plus a small lead-in.
   - An RPC (or a `measure` kind) taking the two placed bodies, the axis and
     the entry side; a requirement kind so an assembly states it once.
   - Rotation about the axis is free while sliding: the check tries the
     part's orientation that clears best (a D bore can pass a D shaft only
     aligned), and says which.
   - Tests: a D bore over a shaft whose D stops short of the end fails, the
     same with the D run out through the end passes, a plain press of its
     own length passes, a bore that is too small for its whole length fails.
2. **The clock's arbors, assemblable.** Every D runs out through the end its
   part goes on from, so nothing round passes through a D bore; where a
   section must slide through a bore freely, it takes the profile's running
   clearance, not the press allowance. Each arbor's assembly direction is
   chosen and written in the card. The insertion check is required on every
   pressed or slid pair in both stages, and the old stage-1 escape arbor is
   its negative control (fails: travel under interference over the hub's
   length).
   - The arbors still print lying (the slender item still passes), the
     pivots still run in their plates, the motion and strength studies still
     pass.
   - Stage 1's new arbors go on a small plate of their own so the user can
     reprint just those; stage 2's jobs re-exported.
3. **The other generators.** Run the insertion check over the engine,
   Strandbeest, planetary, jar and counter assemblies; fix anything it fails,
   or report it.

## Files

- `src/core/Measure*.cpp` / `src/core/Interference*` (the sweep),
  `src/remote/RemoteRequirements.cpp` and `RemoteInspection.cpp` (RPC rows),
  tests in `tests/`, docs under `docs/inspect/` and `docs/assembly/`.
- `generators/clock/` (clock_parts.py, clock_spec.py, build.py, README,
  card); `generators/_lib.py` for a shared helper.
- Other generators only where the check finds something.

## Verified by

- the check's tests, each failing before it;
- the old stage-1 escape arbor failing, the new one passing;
- the clock's and the other generators' tests, every plate passing
  `check_print`;
- the stage-1 arbor plate and stage 2's job sliced clean in the Bambu CLI;
- the headless, GUI and generators suites.
