# Assembly animation: a sequence played, and filmed

## The problem

An assembly sequence (docs/assemblies/README.md#assembly-sequences) says
the order an assembly goes together in and how each part moves home, and
the check proves it does. Nothing shows it: the order lives as text on a
generator's card, and a person building the clock or the Strandbeest has
to picture each step. The data an animation needs — the steps, each
step's parts, its path, its distance from clear, its turn, the poses
between steps — is already in the sequence and already resolved by its
check.

## The shape of the fix

1. **Sequence poses.** One function turns a sequence into a timeline of
   poses: at its step, each step's occurrences (one or a group) travel
   from their start (seat + path × distance from clear, turned by the
   step's turn; a thread step turns one turn per pitch) to their seat;
   before its step a part is hidden; after it, it stays home. A pose step
   moves the placed parts through the mate values it sets (solved at the
   ends, interpolated between). Times come from the distances at one
   speed derived from the assembly's size (its bbox), with a hold between
   steps — never literal millimetres per second. The path and distances
   are the ones the check resolved, so the film and the check cannot
   disagree; a sequence that does not check is refused naming its first
   failing step.
2. **Play it in the app.** The assembly's sequence gets Play / scrub on
   the same playback the Motion window uses; the display poses are
   display-only (as an exploded view's offsets are) and never move the
   solved occurrences.
3. **Film it.** `export_assembly_video` — the same request and job as
   `export_motion_video` (path, format, width, height, fps, quality,
   async; the current camera), with the sequence's timeline as the pose
   source. `MotionVideo.cpp`'s capture, encoder thread and job are reused,
   not copied: the pose source becomes the thing that differs.
4. **Generators.** The clock (both stages) and the Strandbeest write a
   draft video of their sequence beside their card when asked (a flag,
   off in their tests' default runs so the suites stay fast).

## Files

- A sequence-timeline module beside the sequence check (`src/core/`),
  `src/ui/MotionVideo.cpp` and `MotionActions.cpp` (the pose source
  split out), the assembly panel's sequence section for Play,
  `src/remote/RemoteMotion.cpp` or `RemoteInspection.cpp` with its
  `describe` row.
- `generators/clock/build.py`, `generators/strandbeest/build.py`.
- `docs/assemblies/README.md` (#assembly-sequences, #motion-video).

## Verified by

- Unit tests on the timeline: a part is hidden before its step and home
  after it; at the step's midpoint it is half way along its path; a group
  moves rigidly; a thread step's turn matches its pitch; a pose step
  reaches its mate value; the speed scales with the assembly's size.
- A smoke that films a two-step sequence to PNG frames and reads the
  part's screen position move between frames; and a refused sequence
  that does not check.
- A GUI test: Play on the sequence moves the display, Stop restores it,
  and the solved placements are unchanged.
- The clock's stage-1 film made and looked at.
- The headless, GUI and pygui suites.
