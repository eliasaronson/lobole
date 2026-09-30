# The rotary check on every core

The rotary check (`InspectRotary`, src/core/cam/CamRotary.cpp) is now most of
the chess CAM run: about 350 s of 490, on one core. The playback carves with
the same function (`CarveSweepRotary`) and is just as serial.

## Why it can be split, and how

Walking the program in file order does two things to the radius map:

- **A cut** lowers each cell it passes, `r = min(r, cut)`. The final map does
  not depend on the order.
- **A rapid** is checked against the stock as it stands at that moment: every
  earlier cut applied, no later one. This part is order-dependent, and it is
  the failure a shape comparison cannot see, so it stays exact.

So the map is split into **bands along the rotary axis**, one per worker.
Each worker walks every move in file order and touches only its band's cells,
skipping the part of a move whose tip, plus the cutter's radius, lies outside
the band. Every cell then sees exactly the sequence it sees today, and no two
workers write the same cell. A rapid's depth is the deepest any band found,
and its line, position and the counts are merged in band order. The verdict is
therefore identical, not close.

The playback's seek (`RotaryStockSim`) carves through the same banded walk,
so its last frame is still the verdict's stock, cell for cell.

## Files

`src/core/cam/CamRotary.cpp` (`WalkSweepRotary` taking a band, the move loop
in `InspectRotary`), `src/core/cam/CamSim.cpp` (`RotaryStockSim`), and
`core/Parallel.h` for the workers. The thread count comes from the machine,
and the results do not depend on it.

## Verification

- A test that 1 and N threads give byte-identical radius maps and verdicts,
  including rapids through stock at the right line, on a turned part with
  several passes.
- The existing rotary tests, and `ctest -L cam`.
- Timings: the knights setup's `cam_verify` before and after, and the chess
  CAM stage (`generators/chess/run_all.sh PORT --from cam`).
