# Assembly animation: group and pose steps, and the Strandbeest's film

What is built is in docs/assemblies/README.md#playing-and-filming-a-sequence:
a sequence's timeline laid out from its check, Play / scrub / Stop in the
Motion window, `export_assembly_video`, and the clock's stage-1 film
(`--video`). What is left waits on group and pose steps
(docs/plans/assembly-paths.md).

## The problem

A sequence step that places several occurrences together (the clock's
stage-2 escape and third wheels) or that sets mate values between steps (a
Strandbeest cap put on with the crank turned) cannot be played yet: the
timeline reads one occurrence a step and refuses a step that sets mate
values. So stage 2 of the clock and the Strandbeest have no film.

## The shape of the fix

1. **Group steps.** `StepOccurrences` in `src/ui/SequencePlayback.cpp`
   returns the step's list instead of its one `occurrenceId`. The timeline
   already moves a step's occurrences rigidly together
   (`sequence::TimelineStep::occurrences`); the thread lookup there reads
   the first occurrence's bodies, which stays right for a group going on by
   one thread. If the check's answer for a group step moves its rows
   (one row per step, `steps[i]` against `seq.steps[i]`), the row-to-step
   mapping in `ResolveSequence`'s worker follows it.
2. **Pose steps.** `StepMateValues` returns the step's mate values, and
   `ResolveSequence` stops refusing them. A pose step becomes a
   `TimelineStep` with no path and, instead, the placements of every placed
   occurrence before and after it: solved on the frame with the mate solver
   at both ends (a copy of the assembly, as the motion run does), stored on
   the step, and blended between them over the step's time (position lerped,
   orientation slerped, the motion playback's `Blend`); after it, the parts
   it moved stand at the solved "after" placement in place of their seats.
   Its duration comes from the largest angle or distance it moves through,
   at the timeline's one speed. Unit test: a pose step reaches its mate
   value at its end.
3. **Films.** The clock films stage 2 as well with `--video`, and the
   Strandbeest gets the same flag (`generators/strandbeest/build.py`,
   off in its test's run).

## Files

- `src/ui/SequencePlayback.cpp` (the two accessors, the pose solve),
  `src/core/SequenceTimeline.{h,cpp}` (a pose step's placements, `PoseAt`),
  `tests/test_sequence_timeline.cpp`.
- `generators/clock/build.py`, `generators/strandbeest/build.py`.
- docs/assemblies/README.md#playing-and-filming-a-sequence.

## Verified by

- The unit test above, and the timeline's existing ones.
- `assembly_video_smoke.py` extended with a group step (two parts that
  move together keep their relative placement on screen).
- The clock's stage-2 and the Strandbeest's films made and looked at.
