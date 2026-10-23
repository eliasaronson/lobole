# Performance harness

## The problem

Nothing measures how fast Loböle is. Every speed result so far was found by
hand, after somebody felt it: the project open that rebuilt every part
(fixed by baked geometry), the set-back, undo and redo that re-executed the
whole OM615 cylinder head (fixed by the regeneration cache), the STEP file
parsed twice, the assembly resync. Each fix left a phase log or a unit test
behind for its own case, and none of them says when a different change makes
the same thing slow again. `tools/profile.sh` answers "where does the time
go" once a case is known to be slow; nothing finds the case.

So a regression arrives silently and is found the way the originals were: by
a user waiting. The suites cannot catch it. They assert outcomes, and a
regen-cache miss still produces the right bodies, only tens of seconds later.

`benchmarks/` is a different thing (it grades an LLM agent on the MCP
surface) and stays separate; nothing here reads or writes it.

## The shape of the fix

1. **`tools/bench.py`**, a Python CLI on the `lobole` client:
   `run [WORKLOAD…]`, `compare`, `list`, `--dry-run` (the plan, the fixtures
   that would be built, and the expected time, for free). It drives its own
   instance and never the human's.
2. **One instance per workload**, launched by the harness:
   `systemd-run --user --scope -p MemoryMax=8G -p AllowedCPUs=<set>` around
   `build/lobole --headless` (or `--offscreen` for the render workloads)
   `--remote <port> --autosave 0 --exit-with-parent`. The port comes from
   `tools/scratch_port.py`. `XDG_STATE_HOME` is left alone so the instance
   announces itself in the real registry and no other agent is handed its
   port. `--autosave 0` keeps it from writing recovery copies there. A fresh
   `XDG_CONFIG_HOME`, so the user's preferences (tessellation quality, render
   options) do not change the numbers. The instance is stopped by the pid it
   reports, and its log is kept for the phase lines (step 5). With one
   instance per workload, the scope's `MemoryPeak` is that workload's peak.
3. **Workloads** are chosen from what has regressed or what a user waits on.
   Each is a row in `tools/bench_workloads.py` giving its instance kind,
   fixture, untimed setup, timed body, repeats, and the assertions that prove
   it measured what its name says:

   | workload | timed body | asserts |
   |---|---|---|
   | `startup/headless`, `startup/offscreen` | launch to the first `ping` | the instance kind |
   | `open/baked/<fixture>` | `open_project` of a project baked by this build | `rebuilt_parts` empty |
   | `open/rebuild/<fixture>` | `open_project` of a copy with every part's `geometry/` removed | every part in `rebuilt_parts`, reason "no saved geometry" |
   | `history/om615-head` | on the cylinder head: rollback to the start and back, a variable edit, undo, redo | `regen.executed == 0` on every return, undo and redo |
   | `boolean/plate-holes` | full regen of a plate with a 20 × 20 hole pattern and a plate of 400 separate cuts, built in the harness by one `batch` | the hole count from `list_elements` |
   | `step/import/<file>`, `step/export/<file>` | `import_step`, then `export_step` of what came in | body count; the export reads back |
   | `render/first-capture/<fixture>` | open, then the first `screenshot`, which projects the scene lazily | the "Document synced" line was logged |
   | `render/passes/<fixture>` | `frame_stats {reset, renders: N}` | GPU pass times present |
   | `assembly/engine` | `solve_assembly`, then the no-pair `interference` sweep on the engine | no clash, as the generator checks |
   | `motion/hinge-swing` | `run_motion_study` of the hinge box's 0-180° swing | the study's own pass |
   | `cam/chess-board` | `cam_toolpath` of the chess board's setup | move count unchanged from the fixture |
   | `fem/hook` | `run_study` of the hook's study at its saved mesh | the factor of safety the generator reads |

   A workload whose input is missing (a STEP file not on this machine, a
   renderer `--offscreen` cannot start) is reported as skipped with the
   reason. It is never swapped for a substitute input.
4. **Fixtures are built by their generators, once**, into
   `~/.cache/lobole/bench/fixtures/<generator>-<hash>/` (`LOBOLE_SCRATCH`
   pointed there). The hash is over the generator's own files and
   `generators/_lib.py`, so editing a generator rebuilds its fixture.
   The default set uses the generators that build in minutes: om615
   (`--stage preview`), engine, hook, hinge_box, chess (`--only board`), and
   chassis (`--save`). Clock and planetary take ten minutes or more to build
   and are in `--set big`. Before timing anything, each run copies its fixture
   to a working directory and opens and saves it there (untimed). That
   re-bakes it for the binary under test, so `open/baked` measures a load and
   not a "from an older build" rebuild. STEP inputs are named by path
   (`~/Documents/CAD/OCCT/data/step/linkrods.step`, the KiCad mirror's
   `USB3stacked.STEP`) and recorded with their content hash.
5. **Timing.** Wall time is measured in the client around each timed call.
   Every workload runs one warm-up that is thrown away, then N repeats (five
   by default, three for a workload longer than 30 s, set in its row), with
   `new_project` or a re-open between repeats. The reported figure is the
   median, with the spread given as 1.4826 × MAD. Phases come from what the
   app already reports: `regen.executed`/`cached`, `rebuilt_parts`, the
   "Document synced … [tessellate, instance, picking]" and "Job synced" log
   lines, and `frame_stats`' pass split. The harness adds no new timers.
   A phase the log stops printing shows up as missing, not as zero.
6. **The machine is part of the result.** Each record stores a host key
   (CPU model, P- and E-core counts, RAM, GPU name from `ping`), the commit
   and whether the tree is dirty, `CMAKE_BUILD_TYPE` from the build's cache,
   the binary's mtime, the CPU governor, AC power, and the 1-minute load
   average before and after each workload. The harness also checks for other
   lobole instances (`tools/stray-instances.sh`), running compilers, and
   `build/`'s lock. On this hybrid CPU the app is pinned to the P-cores
   (`/sys/devices/cpu_core/cpus`, through `AllowedCPUs`), because a run that
   wanders onto E-cores is a different measurement. The harness takes
   `build/`'s lock SHARED for the run, as `tools/ctest` does, so a merge
   cannot relink the binary mid-run.
7. **Busy machines are refused.** Agents and builds run in parallel here.
   The run refuses to start, naming the cause, when the load average is above
   a quarter of the pinned CPUs or a compiler is running. `--busy-ok`
   overrides this and marks the record `busy`. A workload whose load rose
   during the run is marked as well. A busy record is printed but never used
   as a baseline.
8. **Results are printed and appended** to
   `~/.local/state/lobole/bench/<host-key>.jsonl`, one JSON record per run
   (`--history PATH` overrides it). They stay out of the repository because a
   number is only comparable on the machine and load that produced it. A
   committed per-host file would put figures in the tree that read as facts
   and go stale, as CLAUDE.md says a written number does. It would also
   collide in every merge of parallel worktrees. Each record carries its
   commit, so "what did main measure before this" is a lookup, and the
   history is shared by every worktree on the machine.
9. **`compare`** checks the latest record against a baseline: `--against
   last|REV|FILE`, the same host key, build type and fixture hash, and not
   busy. A mismatch in any of these is refused rather than compared. A
   workload regresses when its new median exceeds the baseline's by more
   than max(3 × √(s_new² + s_base²), 5 % of the baseline, 5 ms). The noise
   floor therefore comes from the repeats, not from a constant. Improvements
   are reported by the same rule. The exit status is non-zero on a
   regression. `compare --ab BIN_A BIN_B` is the A/B form for judging one
   change: it alternates the two binaries ABAB… on the same fixture copies,
   so drift in load or clock speed falls on both, and prints each
   workload's two medians with their ranges.
10. **Handing a slow case to the profiler.** `run --profile WORKLOAD` writes
    the workload's setup and one timed body as a driver script that reads
    `LOBOLE_PORT`, then runs `tools/profile.sh --app <driver>`. A regression
    found by `compare` is then one command from a call graph. `profile.sh
    --app` launches headless only, so an offscreen workload prints the
    `tools/profile.sh -- …` command for its instance instead.
11. **Not a CI gate.** Shared runners are too noisy, and a gate that fails
    on noise gets ignored. What belongs in ctest is a check that the
    harness itself works: `bench_harness_smoke` (headless suite, label
    `bench`). It runs `startup/headless` and `boolean/plate-holes` at a small
    scale with `--repeats 2 --busy-ok --history <tmp>`, checks the record
    against the schema, and runs `compare` on two such records. Once the
    workload rows have been written, it finishes in seconds. It never
    asserts a duration, and it never writes the user's history.
12. **An injected slowdown, for verification only.** `LOBOLE_TEST_REGEN_DELAY_MS`
    sleeps per executed feature. `LOBOLE_TEST_NO_REGEN_CACHE` sets the cache
    cap to zero. Both are read in `Document.cpp` beside the existing
    `LOBOLE_TEST_*_MS` hooks, and a run that sees either says so in its
    record.

When this lands, a `docs/performance/README.md` describes the harness, the
workloads and how to read a compare. `tools/profile.sh` already cites
`docs/performance/`, which does not exist yet. The docs table and CLAUDE.md's
profiling section each get a line. No measured number goes into any of them.

## Files

- `tools/bench.py` (new): CLI, launcher, host probe, stats, history, compare,
  A/B, profile hand-off.
- `tools/bench_workloads.py` (new): the workload table and fixture recipes.
- `tests/smoke_app.py`: a launch option that keeps the real
  `XDG_STATE_HOME` and wraps the app in a `systemd-run` scope, so the
  harness shares the one launcher.
- `src/core/Document.cpp`: the two `LOBOLE_TEST_*` hooks.
- `tests/bench_harness_smoke.py` (new), `tests/CMakeLists.txt`.
- `docs/performance/README.md` (new), `docs/README.md`,
  `docs/architecture.md#tests`, `CLAUDE.md` (profiling section).

## Verified by

- **It sees an injected regression.** Run the default set on a quiet machine,
  then run again with `LOBOLE_TEST_NO_REGEN_CACHE=1`. `compare` flags
  `history/om615-head`, and that workload's `executed == 0` assertion fails
  and is reported. The other workloads pass. With
  `LOBOLE_TEST_REGEN_DELAY_MS=50`, `boolean/plate-holes` and `open/rebuild/*`
  are flagged and `open/baked/*` is not, because a baked open executes
  nothing.
- **It does not see one that isn't there.** Two runs of the same binary, back
  to back, compare clean, and `compare --ab build/lobole build/lobole` reports
  no difference.
- **It refuses a busy machine.** Under `tools/with-cpu-load.sh`, `run`
  refuses and names the load. With `--busy-ok` the record is marked busy,
  and `compare --against` refuses to use it as a baseline.
- **Its refusals name what is wrong.** An unknown workload is refused and
  `list` is named. A compare across build types or fixture hashes is refused.
  A missing STEP input is skipped with its path.
- **It hands off to the profiler.** `run --profile history/om615-head` under
  `LOBOLE_TEST_NO_REGEN_CACHE` gives a profile whose top frames are
  feature execution under `Document::Regenerate`.
- **It leaves nothing behind.** Neither `tools/stray-instances.sh` nor the
  registry shows an instance after a run, `~/.local/state/lobole/` has no
  autosave from it, and a killed `bench.py` takes its instance with it.
- `bench_harness_smoke` in the headless suite, and the headless suite as a
  whole.
