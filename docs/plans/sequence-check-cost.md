# The assembly sequence check's cost

## Problem

The clock generator spends 245 s of its 725 s in assembly sequence checks,
and two causes account for nearly all of it.

1. **The same check runs twice per stage.** `generators/clock` calls
   `check_assembly_sequence`, then adds an `assembly_sequence` requirement
   and calls `check_requirements`. The requirement's measure
   (`MeasureSequence`, src/remote/RemoteRequirements.cpp) calls
   `remote::RunSequenceCheck` from scratch: the same inputs and the same
   answer, computed again (80 s + 84 s at stage 1, 165 s + 165 s at
   stage 2). Play (src/ui/SequencePlayback.cpp) runs it a third time.
2. **One step costs 132 s of the 245 s:** the rating nut screwed onto the
   pendulum rod (stage 2: sections 93 s, 3058 fixed bands). A screwed or
   turned path does not use the band cache, and its uncached branch in
   `Measure::Insertion` (src/core/MeasureInsertion.cpp) cuts every fixed
   solid within reach of the axis over its WHOLE length. The rod's line
   runs up through the movement, so plates, wheels and arbors far up the
   axis from where the nut travels were cut into bands, each an exact
   section of the whole solid (a profile puts 51 % of the time in
   `TopLoc` location chains under `BOPAlgo` section booleans). The cached
   branch already cuts only the window the moving side can reach.

## Fix

1. **Cut the fixed side only over the reachable window on the uncached
   branch too.** The span the moving side can come near along the axis:
   its own span, extended by the longest travel on each side it may come
   from, two slices to spare (a helix's last step ends up to a step past
   its travel). A fixed band outside that window overlaps no moving band
   at any slide a try reads, so every reading is unchanged; only bands
   that are never read go.
2. **Cache the sequence check's answer, keyed on everything it reads.**
   `ReadSequenceCheck` stamps the check with an input key; `RunSequenceCheck`
   answers from a small session store when the key matches, and stores what
   it computed when it ran to the end. The key is the one the
   `assembly_sequence` requirement already uses for staleness, moved into one
   function both call: the assembly's content (occurrences, mates,
   relations, configurations), every sub-assembly's, every placed part's
   bodies and tolerances and print process, and the sequence as saved
   (steps, allowance). The bodies' hash gains the thread marks
   (`Body::threads`), which the screw check reads and which a thread's
   designation can change without changing the shape. A hit says so
   (`reused: true`). The check_assembly_sequence RPC, the requirement and
   Play all go through `RunSequenceCheck`, so all three share it.

## Files

- src/core/MeasureInsertion.cpp: the window on the uncached branch.
- src/core/InputHash.cpp: thread marks in `Hasher::Bodies`.
- src/remote/RemoteRequirements.{h,cpp}: `SequenceInputKey`, used by the
  requirement's key.
- src/remote/RemoteInspection.{h,cpp}: the key on `SequenceCheck`, the
  store, `reused`; the describe row.
- docs/assemblies/README.md, docs/inspect/README.md: what is reused, and
  the window.

## Verification

- tests/assembly_sequence_smoke.py: a second identical check is reused and
  answers the same; an edit to a placed part, to the assembly (an
  occurrence moved) and to the sequence (its allowance) each makes the next
  check run again; the requirement after a check is reused.
- tests/test_study_inputs.cpp (or the inputhash tests): two bodies that
  differ only in a thread mark hash differently.
- A unit test for the window: a screw check whose fixed side also has a
  solid far along the axis line gives the same report as without it, and
  cuts no bands from it (a band count, the cost check).
- Before/after on the clock's saved stage-2 project: every step's answer
  identical, the rating nut's sections time and band count down.
- `tools/ctest -C generators -R generator_clock` once, alone: the wall
  time against 725 s.
