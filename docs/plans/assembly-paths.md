# Assembly sequences: groups, poses, and the engine and Strandbeest

## The problem

Assembly sequences are built ([sequences](../assemblies/README.md#assembly-sequences))
and the clock states both stages'. A step moves ONE occurrence along one
straight line with every other part where it was left, and that cannot
state how three of our assemblies actually go together:

- **Parts that go in together.** Clock stage 2's escape wheel and third
  wheel each block the other's straight path (lifted 3.1 mm the escape
  piece overlaps the third wheel's teeth by 1.88 mm), so the generator
  requires that failure (`TOGETHER`) and stage 2 has no
  `assembly_sequence` requirement. On the bench both arbors go into the
  back plate together, meshed; moved as one, they never meet each other.
  The engine's piston, rod and wrist pin are the same shape of problem.
- **A pose between steps.** The Strandbeest's caps go on with the crank
  turned; a step has no way to say "the crank at 90° while this goes on".
- **Slow on big assemblies.** A step re-slices every placed part: the
  engine's 78 steps take about 460 s against its test's 420 s budget,
  clock stage 2 about 400 s.

And the first tries found real errors that wait on the above:

- **The engine**, at allowance 0:
  - the cap pins, pressed along X from the nearer end, are forced 52-74 mm
    through the bedplate and 18.5 mm (0.45 deep) through the crank webs —
    with a one-piece crank no order gets a pin past a web
    (`engine_spec.solve`: `y_web` reaches past the pin's inner edge, `y_cp`);
  - the block lowered over the pistons is forced 98.6 mm, 3.4 mm into a
    piston: something of the piston stands proud of the bore.
  - The subagent's sequence is in the scratchpad as `engine-sequence.patch`.
- **The Strandbeest.** The stage-1 assembly places both frame plates as
  one occurrence and both discs, the crank pin and the handle as another,
  so the card's order cannot be stated on it. On a put-together assembly
  of the printed pieces the card was found wrong twice: pin FC cannot go
  up from below (the plate is under its hole: 6.2 mm forced), and the last
  plate cannot go down over the last disc's flange (7.6 mm). The draft is
  `strandbeest-sequence.patch`, never run after its last change.
- **The jar** screwed into its lid by its modelled thread is forced 7.7 mm,
  1.34 mm into the gasket. Not yet looked at.

## The shape of the fix

1. **Group steps.** A step names one occurrence or several; a group moves
   rigidly along one path, is not checked against itself, and is checked
   against everything placed before it. The default path of a group is its
   members' common seat axis, refused by name when they have none (axes
   not parallel) and no direction is given.
2. **Pose steps.** A step that places nothing and sets one or more mate
   values (a revolute's angle, a slider's distance); the assembly is
   solved at that pose with the existing mate solver, every placed part
   moves to it, and later steps are checked there. A value outside the
   mate's limits, or a pose that does not solve, is refused naming the
   mate.
3. **Sections reused between steps.** A placed part's bands, per path
   axis and orientation, are cached for the run and moved with the pose
   rather than re-sliced; the cache keeps to the memory ceilings
   `MeasureSections` already enforces. Timings before and after on the
   engine, clock stage 2 and the Strandbeest, in the docs.
4. **Clock stage 2** states its arbors-into-the-back-plate step as one
   group, drops `TOGETHER`, and gains its `assembly_sequence` requirement.
   No geometry change (stage 1's reprint is being printed).
5. **The engine.** Move `y_cp` out (or `y_web` in) so a cap pin passes the
   webs; find what stands proud of the piston and fix it; state the
   sequence (piston, rod and wrist pin as one group) with its requirement;
   re-export and slice its jobs, the old ones moved to
   `scratch/engine/old-<date>/`.
6. **The Strandbeest.** Build the put-together assembly (`assembled()`
   records each piece's placement), state the card's order on it with the
   two corrections and the crank-turned caps as pose steps, replace
   `check_order`, and correct the card.
7. **The jar's thread:** find whether the 7.7 mm is the gasket's size or
   the thread step's reading, and fix whichever is wrong.

## Files

- `src/core/Occurrence.h` (`SequenceStep`: occurrences, pose values),
  `src/core/MeasureSections.{h,cpp}` and the sequence check,
  `src/core/io/SerializeAssembly.cpp` (`vNEXT`, its golden),
  `src/remote/RemoteInspection.cpp` / `RemoteRequirements.cpp` and their
  `describe` rows.
- `generators/{clock,engine,strandbeest,jar}/` and their READMEs and
  `test.json`.
- `docs/assemblies/README.md#assembly-sequences`,
  `docs/inspect/README.md#insertion`, `docs/versioning/format.md`.

## Verified by

- `assembly_sequence_smoke`, new cases each failing before its change:
  two meshed gears that block each other one at a time pass as a group; a
  group with no common axis is refused; a cap that fails at the rest pose
  passes at a stated crank angle; a pose past a mate's limit is refused; a
  sequence with groups and poses survives save and reopen.
- Sequence timings before and after on the engine, clock stage 2 and the
  Strandbeest.
- The clock, engine, Strandbeest and jar generator tests with their
  markers; the engine's (and the jar's, if it changes) jobs sliced clean
  in the Bambu CLI.
- The headless, GUI and generators suites.
