# The insertion band window, without its margin

## The problem

The kept (cached) branch of `Measure::Insertion`
(`src/core/MeasureInsertion.cpp`) cuts each solid over the window the
moving side can reach -- its own span plus the travel the way it comes,
a slice to spare -- and then "as much again either side". That margin
costs the clock's bob step 14.5 s of its 16 (1.6 s without it; the stage-2
sequence 95 s to 72 s), and dropping it changed one answer: the stage-2
"centre spacer" step went from `travel_mm` 5.701 with a seat `confirm` to
`travel_mm` 0 and no `confirm`. Its playback (`SequencePlayback.cpp`
reads `travel_mm`) then puts the spacer in place instead of sliding it on.

## The mechanism

The window is right; the margin is not hiding a reach. With the margin
dropped, every other step of the stage-2 sequence (23 kept checks, 21 of
their solids cut along the reversed direction) answers exactly as before.
What changed is that the centre spacer's step went from having bands that
never meet anything to having none at all, and the two take different
paths:

- The travel comes from `ReachSpan` (`src/core/MeasureSections.cpp`), the
  fixed side's span over the faces whose `rMin` -- a box measure -- lies
  within reach. The third wheel's faces' boxes reach within the spacer's
  6.8 mm, so the fixed side's span reaches 2.4 mm above the spacer's
  seat and `travelOf` gives 5.701 mm. Its matter does not: its teeth are
  14.3 mm from the centre axis, 10.25 mm from the spacer.
- The back plate is 0.40 mm under the seat. A spacer coming from above
  never reaches it, so the window (from 0.08 mm under the seat) leaves it
  out, correctly.
- So without the margin no fixed band survives (`Moved` clips the third
  wheel's to the reach disk and finds nothing), and
  `if (bandsA.empty() || bandsB.empty()) return clear();` reports a clear
  try: travel 0, the confirm skipped. With the margin the plate was cut
  whole, its 21 bands met nothing (`pairs_read` 0 either way), and the
  ordinary try reported the travel and ran the seat's boolean.

The answer therefore depends on whether an irrelevant solid happens to be
cut, which no window can make right: it differs today, margin and all,
between a fixed side with and without a plate under the seat. The margined
answer is the one the check states (`travel_mm` from the reach span that
also set the step and slices; "one exact boolean ... the seat when nothing
interferes on the way confirms it"); the early `clear()` uses a second,
tighter idea of "near" after the first already said something was.

## The fix

1. Once both sides have a span within reach (`nearA && nearB`), empty
   bands are an ordinary check: drop the early `return clear()` after the
   bands and let the tries run with no pairs (they pass at `travelOf`),
   and the confirm run at the seat. The earlier `clear()` for `!nearA ||
   !nearB` stays. This covers the uncached branch too.
2. Drop the margin: cut the window itself, or the whole solid when the
   window covers it (`wl` and the `whole` test in the kept branch).
3. Docs: `docs/inspect/README.md` -- "cuts that stretch, three times what
   it sweeps" becomes the stretch itself; the sequence timing rows that
   moved. Delete the known-bugs entry.

## Files

- `src/core/MeasureInsertion.cpp` -- the two changes above.
- `tests/test_measure_insertion.cpp` -- the regression test.
- `docs/inspect/README.md`, `docs/known-bugs.md`.

## Verification

- A test, failing today: a spacer coming down the axis onto nothing, a
  plate 0.4 mm under its seat, and a ring off the axis whose faces' boxes
  reach the path but whose matter does not. Checked with and without the
  plate, kept (with a cache) and uncached, every answer is the same --
  `travelMm`, `pass`, `confirm.ran` and its readings -- and the travel is
  the ring's box span's. Today the kept check with the plate runs the
  tries (travel, confirm) and without it returns clear (0, none).
- The clock's stage-2 sequence (`check_assembly_sequence`), before and
  after: every step's answer identical apart from `bands`, `solids` and
  timings; the bob step about 1.6 s. A trial of both changes did exactly
  that (stage 95 s to 72 s, bob 14.5 s to 1.6 s, the centre spacer 5.701
  mm with its confirm).
- `tools/test-fast.sh`, then the headless suite.
