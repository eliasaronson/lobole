# The planar check on every core, and each setup inspected once

The planar check (`Inspect`, CamVerify.cpp) is serial. The chess board's
check costs 0.2–0.35 s at the panel's 0.4 mm and 0.4–0.9 s at 0.25 mm for its
own setup. Every check ALSO inspects each of the job's other setups again
(`OtherSetupsOf`, CamOtherSetups.cpp) to know what stock they leave behind. On
the board that doubles the cost, and on a job with more setups it multiplies
it: the chess set has 17. Measured on the board (docs/plans/gpu-tool-sweep.md,
step 1b):

| part of one `Inspect` | Top 0.25 mm | Face 0.25 mm |
|---|---|---|
| carve + rapids, one thread | 225 ms | 290 ms |
| `SampleModel` | 122 ms | 108 ms |
| `Compare` | 21 ms | 395 ms |
| the other setup's `Inspect` | 593 ms | 407 ms |

## The fix

1. **The carve in bands of rows.** The rotary check's banded walk
   (docs/cam/rotary.md#on-every-core, `WalkRotaryProgram`) is the pattern.
   Each band walks every move in file order over its own rows, so a rapid is
   still checked against the stock as it stands at that line. Per-move
   results merge in band order: a rapid's deepest hit, the swept cells, and
   the bite-volume, plunge-bite and shank sums. The result must be identical
   to one thread's, to the bit, as the rotary one is. The prototype measured
   225 → 48 ms (Top) and 290 → 62 ms (Face) at 0.25 mm on 22 cores.
2. **`SampleModel` in parallel.** It rasterizes triangles into a max-height
   grid. Split it by rows (each band rasterizes the triangles that overlap
   it) so the result is the same `max`, order-free.
3. **`Compare` where it is heavy.** Face's 395 ms is out of line with Top's
   21 ms. Find out why before parallelizing it (region labelling? other
   setups' `Removes` per cell?) and fix the cause.
4. **Each other setup inspected once.** `OtherSetupsOf`'s per-setup result
   (the machined height or radius map) depends only on that setup's posted
   program, its framing and stock, the part's geometry and the resolution.
   Memoize it process-wide under a key built from exactly those, the way
   `JobPassesKey` / `FramingKey` key the toolpath caches. An edit to any
   input misses the cache. The independent setups can also be inspected
   concurrently when the cache misses.

## Files

`src/core/cam/CamVerify.{h,cpp}` (carve bands, `SampleModel`, `Compare`),
`src/core/cam/CamOtherSetups.{h,cpp}` (the memo), and the callers in
`src/ui/CamTool.cpp`, `src/ui/CamPlayback.cpp` and `src/remote/RemoteCam.cpp`
only if the memo needs a key they hold. Tests go in `tests/test_cam_verify.cpp`.
Docs: `docs/cam/simulation.md`.

## Verification

- The banded check is the serial check to the bit (map, swept cells, rapids,
  bite and shank results), 1 thread against N, on a program with rapids
  through stock. This matches the rotary check's test.
- The memo returns the same maps as a fresh inspect, and misses after an edit
  to the other setup's program, its stock, or the part.
- The CAM suites; the chess set regenerated with every verdict unchanged
  against `scratch/chess/chess_report.json`; and the board's check timed
  before and after, at 0.25 and 0.4 mm.
