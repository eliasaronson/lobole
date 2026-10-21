# ci-local.sh in parallel

## The problem

`tools/ci-local.sh` runs its jobs one after another: the nix chain (Release
build, headless suite, GUI suite, the no-test-engine and no-CAM builds and
suites), then each stock-Ubuntu container (build, then suite). A full run
took 70 minutes. Each step alone already uses the machine — compiles at
`tools/build-jobs` (free memory / ~1.5 GB), suites at `-j$(nproc)` — so the
time lost is in the tails: a container's compile waits for the nix suite's
last serial tests (the worm gears, ~280 s on one core), the GUI suite runs
four shards on a 22-thread machine, and every link step is single-threaded.

Starting everything at once is not the fix. Two builds at the full
`build-jobs` each doubles the memory a compile budget was sized for (the
out-of-memory killer once took the agent host), and two suites at
`-j$(nproc)` each fail the deadline-based tests (`hostile_params`,
`fuzz_seeds_smoke`, the drawing-tab stall) that a loaded machine already
fails.

## The shape

**Lanes, run concurrently.** One lane per independent chain: `nix` (the
existing order), and one per matrix image. Each lane is a background
function writing only to its own logs, as every step already does; the
script waits on the lanes' pids and prints each lane's results in a fixed
order at the end, so the output reads as it does today.

**Builds share the memory budget.** The `build-jobs` figure is divided
among the lanes building at that moment, so the sum never exceeds what
one build was allowed. Ninja 1.13's jobserver client would share it
exactly; until the toolchain has it, a fixed split (`jobs / lanes`) is
the conservative version.

**Suites take turns.** A machine-wide lock (`flock` on
`.ci-local/suite.lock`, the pattern `gui-tests-xvfb.sh` already uses for
the GPU) lets one suite run at a time at full `-j$(nproc)`, so no
deadline-based test sees more load than it does today. The gain is that
one lane's compile overlaps another lane's suite.

**Timing per step first.** Each `step` records its seconds and the end of
the run prints them, so the split is measured rather than assumed — and
the first run of this change says whether the overlap pays.

**`--serial`** keeps today's order, for a run that is being debugged.

## Files

- `tools/ci-local.sh` — lanes, the split budget, the suite lock, step
  timings, `--serial`.
- `CLAUDE.md` / `docs/architecture.md` — wherever ci-local's cost is
  quoted.

## Verified by

- A full run, timed per step, before and after; the after must be
  shorter and must fail exactly the same tests.
- A run with a deliberately broken container build: the failure is
  reported, the other lanes finish, and the exit status is non-zero.
- Peak memory during the run (`/proc/meminfo` sampled) stays under what
  a serial run reaches.
