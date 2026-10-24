# Tests that time things

## The problem

Three suites failed tonight on a loaded machine (load 24 to 61, other builds
running) and passed alone: a reader-fuzz case past its budget (fixed: the
budget is CPU time now), and `refused_calls_smoke` twice past its 180 s
timeout. About fifteen other checks compare a wall-clock time against a fixed
limit, and the tightest are as exposed:

- **C++:** `test_direct_edit.cpp` ("Delete Face fills all 100 holes",
  `s < 90`), `test_meshcollide.cpp` (mesh vs exact ratio),
  `test_outline_edges.cpp` (hidden `[.outline-perf]`), `test_actions.cpp`'s
  `PumpUntil` hang guard.
- **Python smokes:** `contact_smoke` (`slowest < 1.0`), `insertion_smoke`
  (`< 0.5`), `cam_parked_smoke` (ping lag `< 2.0`), `drawing_parked_smoke`
  (`< 1.0`, `< 2.0`), `requirements_smoke` (`< 2.0`, `< 1.0`), `remote_smoke`
  (idle pings under 30 ms, two of eight allowed slower), `topo_smoke`,
  `fem_async_smoke`, `motion_smoke` (reuse `< 1.0`), `study_reopen_smoke`
  (`< 5.0`), `study_jobs_smoke` (`< 1.0`), `drawing_quality_smoke`
  (`< 60`).

Separately, `refused_calls_smoke` grew from 34-50 s to 108 s over the night's
merges, and it sits in the inner loop (`tools/test-fast.sh`).

## The shape of the fix

Each timing check is one of two kinds, and each kind gets its own fix:

- **Cost:** "this must not go quadratic", "the cached path is cheaper than
  the full one". These move to CPU time (the process's, with its children's,
  as the reader-fuzz budget does). A ratio of two times measured in the same
  run stays wall-clock, since load cancels out of it.
- **Responsiveness:** "a ping answers within 2 s while a job runs", "a reuse
  answers within 1 s". Wall time is the point, so they keep it, but they read
  the host first: when `tools/bench.py`'s busy probe says other work holds the
  CPUs, the check is skipped with a printed reason ("skipped: 18 of 22 CPUs
  busy with other work"), and the test still runs everything else. The probe
  moves to a module the smokes and `bench.py` share (`tests/host_load.py`).
  A hang guard (`PumpUntil`, a 30 s wait) stays as it is: it is there to end a
  hang, not to judge speed.

Each check is classified in the commit that changes it.

`refused_calls_smoke` is tagged `slow` now, so `test-fast.sh` drops it while
the full suite, `tools/merge-branch.sh` and CI keep it. Then it is measured on
a quiet machine and profiled (`tools/profile.sh --app`). If the doubling is
direct edit's try-on-a-copy check running for calls already refused on their
parameters, the check moves after the parameter checks. Whatever the profile
shows is fixed, and the tag is lifted if the test returns under a minute.

## Files

- `tests/host_load.py` (new), `tools/bench.py`: the shared busy probe.
- The smokes and C++ tests listed above.
- `tests/CMakeLists.txt`: `refused_calls_smoke` labelled `slow`.
- `src/remote/RemoteModeling.cpp` (or wherever the profile points).
- CLAUDE.md's Tests section: one line on the two kinds of timing check.

## Verification

- Each changed check passes under `tools/with-cpu-load.sh` at every CPU, and
  still fails on a planted slowdown (`LOBOLE_TEST_REGEN_DELAY_MS`, or a sleep
  in the measured path) on a quiet machine.
- A responsiveness check run under load prints its skip reason and the test
  passes; run quiet, it measures.
- `refused_calls_smoke`: its time before and after on a quiet machine, in the
  commit; `tools/test-fast.sh` no longer runs it; the full suite does.
- The headless suite, `tools/ctest -L pygui`, and both run once under
  `tools/with-cpu-load.sh` with no failures.
