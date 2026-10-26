# Performance

How fast Loböle is, measured the same way every time, so a change that makes
a known case slow again is found by a command rather than by a user waiting.
The suites cannot find it: they assert outcomes, and a regeneration-cache
miss still produces the right bodies, only tens of seconds later.

`tools/bench.py` is the harness; `tools/bench_workloads.py` is its table of
workloads and fixtures. `tools/profile.sh` answers the next question, where
the time goes, once a workload is known to be slow
([handing a slow case to the profiler](#handing-a-slow-case-to-the-profiler)).

`benchmarks/` is a different thing — it grades an LLM agent on the MCP
surface ([scripting/benchmark.md](../scripting/benchmark.md)) — and nothing
here reads or writes it.

## Running it

```
tools/bench.py list                          # the workloads, their inputs and fixtures
tools/bench.py run --dry-run                 # the plan, the fixtures to build, the expected time
tools/bench.py run                           # the default set, recorded
tools/bench.py run 'open/*' history/om615-head
tools/bench.py compare                       # the latest record against the previous one
tools/bench.py compare --against main        # ... against the last record of a revision
tools/bench.py compare --ab A/lobole B/lobole history/om615-head
tools/bench.py run --profile history/om615-head
tools/bench.py fixtures                      # build the fixtures, time nothing
```

`run` takes workload names or globs; an unknown one is refused, naming
`list`. `--set big` adds the workloads on the clock and planetary fixtures,
which take ten minutes or more to build. `--binary` measures another build,
`--repeats N` overrides a row's repeats, `--history PATH` writes and reads
another history file. `--scale small` is the cut-down form the harness's own
test runs ([its own test](#its-own-test)); a small record is never compared
with a full one.

It is not a CI gate. A shared runner is too noisy, and a gate that fails on
noise gets ignored.

## The instance

Every workload runs in an instance of its own, launched by
`tests/smoke_app.py` — the launcher every smoke uses — as
`build/lobole --headless` (or `--offscreen` for a render workload)
`--remote 0 --autosave 0`, wrapped in `systemd-run --user --scope -p
MemoryMax=8G`:

- **A scope per instance.** The cap keeps one workload from taking the
  machine, and the scope's `memory.peak` is that workload's peak, recorded
  as `memory_peak_bytes`. Where no systemd user manager runs (a container)
  `run` refuses, and `--no-scope` runs without the cap or the peak and says
  so in the record (`scope: null`). The startup workloads launch without
  one, since creating the scope is a round trip to the user manager that
  would be timed as the app's startup.
- **Pinned to the P-cores.** On a hybrid CPU a run that wanders onto
  E-cores is a different measurement, so the instance's affinity is set to
  `/sys/devices/cpu_core/cpus` (every CPU on a machine that is not hybrid;
  `--cpus LIST` overrides). It is set with `sched_setaffinity` before the
  exec, and the record keeps the set each instance actually ran on. A
  scope's `AllowedCPUs=` is not used: the user manager is not delegated
  the cpuset controller here, and the property is accepted and ignored.
- **A private config and state directory**, as every smoke's. The user's
  preferences (tessellation quality, render options) change no number; and
  an instance on the user's state directory would put the fixtures in their
  recently-opened list and leave its own recovery copies there. The port is still claimed
  machine-wide in `/tmp/lobole-ports-<uid>`, which `tools/scratch_port.py`
  reads whatever an instance's state directory, so no other agent is handed
  it; and an instance missing from the registry is one no agent finds and
  drives in the middle of a measurement.
- **The kernel picks the port** (`--remote 0`, bound in the process that
  keeps it), for the reason smoke_app's docstring gives: a port picked first
  and bound later is a race.
- **It dies with the harness**, SIGKILL included (`LOBOLE_EXIT_WITH_PARENT`):
  the scope execs the app in place, so its parent is the harness. The app
  takes the signal between requests, so one killed in the middle of a long
  call (an `open/rebuild` of om615) exits when that call returns. Its
  working directory, `/tmp/lobole-bench-<pid>-…`, is removed by the next
  run once that pid is gone. A failed workload's evidence is kept in the
  record, never archived under the user's state.

The harness holds `build/`'s lock shared for the run, as `tools/ctest` does,
so a build or a merge cannot relink the binary under it.

## The workloads

Each is a row in `tools/bench_workloads.py`: its instance kind, fixture,
untimed setup, timed body, the reset between repeats, its repeats, and the
assertions that prove it measured what its name says. They are chosen from
what has regressed before or what a user waits on.

| workload | timed body | asserts |
|---|---|---|
| `startup/headless`, `startup/offscreen` | launch to the first `ping` | the instance kind |
| `open/baked/<fixture>` | `open_project` of a copy baked by this build | `rebuilt_parts` is 0 |
| `open/rebuild/<fixture>` | `open_project` of a copy with every part's `geometry/` removed | every part that had geometry rebuilt, reason "no saved geometry" |
| `history/om615-head` | on the cylinder head, evaluated once untimed: rollback to the start and back, the second Extrude's distance edited (a value no repeat used), undo, redo | the edit executes; `regen.executed == 0` after the return, the undo and the redo |
| `boolean/plate-holes` | one `batch`: a plate with a 20 × 20 Hole pattern (one point sketch) beside a plate of 400 separate Cylinder cuts | the hole count of each plate, from `list_elements` |
| `step/import/<file>`, `step/export/<file>` | `import_step`; `export_step` of what came in | a body count, the same every repeat; the export reads back to as many bodies |
| `render/first-capture/<fixture>` | in a fresh `--offscreen` instance each repeat: open, then the first `screenshot`, which projects the scene lazily | a "… synced" line was logged |
| `render/passes/<fixture>` | `frame_stats {renders: N}`, after an untimed `{reset}` | GPU pass times present |
| `assembly/engine` | `solve_assembly`, then the no-pair `interference` sweep with no deadline | no errored mate, no clash, the sweep not truncated |
| `motion/hinge-swing` | `run_motion_study {recompute}` of the hinge box's 0-180° swing | clean, as the saved result is |
| `cam/chess-board` | `cam_toolpath` of every setup of the board's job | per-operation move counts equal to the fixture's |
| `fem/hook` | `run_study {recompute}` at the saved mesh | the factor of safety within 1 % of the saved result's |

The open, render and history workloads take their fixture by name:
`open/*` runs on every fixture of the set, `render/*` on om615 and the
chassis. The STEP files are named by path — the OCCT sample
`linkrods.step` and the KiCad mirror's `USB3stacked.STEP`, both outside the
repository — and recorded with their content hash, so a compare refuses
across a changed file. A workload whose input is missing (a STEP file not on
this machine, a renderer `--offscreen` cannot start) is reported as skipped,
with the reason; it is never swapped for another input.

A workload whose assertion fails still records its times, and `compare`
prints the failure beside them: a regeneration-cache miss is both slower and
a failed `executed == 0`, and the second says why.

Three rows are shaped by what the app does:

- **A baked open is not free of execution.** Datums execute on a baked open
  ([baked geometry](../versioning/README.md#baked-geometry)); the OM615
  cylinder head alone carries dozens of Frames. So `open/baked/om615` moves with the
  cost of a feature's execution, and `LOBOLE_TEST_REGEN_DELAY_MS` slows it.
- **The history workload evaluates the head before it times anything.** A
  baked open seeds the regeneration cache with the tip only, so the first
  return to the opened state rebuilds from the start by design; timed, that
  would measure the load, not the cache. The edit is a feature's distance,
  not a variable: no feature of the generated head reads one.
- **The interference sweep stops at a deadline** (20 s by default) and says
  what it left unchecked, so a slower build would check fewer pairs in about
  the same time. `assembly/engine` passes no deadline and fails a truncated
  sweep.

## Fixtures

A fixture is a project its generator builds, once, into
`~/.cache/lobole/bench/fixtures/<generator>-<hash>/` (`LOBOLE_SCRATCH`
pointed there), against an instance of its own. The hash covers the
generator's own files, every generators module they import, the scripts its
steps name, `generators/_lib.py` and the recipe, so editing any of them
builds the fixture again. A build that fails or is killed leaves no
`meta.json` and is built again rather than used.

Most recipes are the generator's own `test.json`
([generators/README.md](../../generators/README.md#the-big-generators-as-tests)):
om615 (`--stage preview`), engine, hook, hinge_box and chess (`--only
board`). The chassis has no test and is declared in the table
(`chassis.py --save`). Clock and planetary are in `--set big`. A fixture
can carry probes taken when it is built: the chess board's per-operation
move counts are the ones `cam/chess-board` holds a run to.

Before timing anything, each workload copies its fixture to a working
directory and opens and saves it there, untimed. That bakes it for the
binary under test, so `open/baked` measures a load, not a "from an older
build" rebuild.

## Timing

Wall time is measured in the client, around each timed call; a workload's
sample is the sum of its timed calls, and each call is also kept as a phase.
Every workload runs one warm-up that is thrown away, then its repeats (five,
three for `boolean/plate-holes`), with the row's reset (`new_project` or a
re-open) between them. The figure is the median; the spread is 1.4826 × the
median absolute deviation, which one outlier cannot move.

The phases are what the app already reports, beside the times:
`regen.executed`, `rebuilt_parts`, a study's own counts, the "Document /
Job / Assembly synced … [tessellate, instance, picking]" log lines and
`frame_stats`' pass split. The harness adds no timer to the app. A phase the
app stopped reporting is recorded as missing (`null`), never as zero.

## The machine is part of the result

Each record stores a host key — CPU model, P- and E-CPU counts, RAM, and
the GPU an `--offscreen` instance names in `ping` (probed once a boot and
cached) — with the commit and whether the tree is dirty, `CMAKE_BUILD_TYPE`
from the binary's build directory, the binary's mtime, the CPU governors, AC
power, the CPUs it was pinned to, the other lobole instances running, the
load average and a one-second sample of busy CPUs before the run, and per
workload the load average before and after and its `foreign_load` (below).

**A busy machine is refused.** Agents and builds run in parallel here, and
a number taken beside a compiler measures the compiler. `run` refuses to
start, naming the cause, when the 1-minute load average or the CPUs busy
over a one-second sample are above a quarter of the pinned CPUs, or a
compiler is running (`cc1plus`, `ld`, `ninja`, …). `--busy-ok` runs anyway
and marks the record `busy` with the reasons. A busy record is printed and
kept, and never used as a baseline.

During each workload the harness also measures what the REST of the
machine did: the CPU time `/proc/stat` counts, less its own and its
instances' (`/proc/<pid>/stat`), over the wall time, recorded as
`foreign_load`. The load average cannot answer that, since the workload's
own threads raise it. A workload whose `foreign_load` passes the limit
marks itself and the record busy.

### Is the machine busy

The probe is `tests/host_load.py`, shared with the smokes' wall-clock
checks: `Foreign` is the measure above, `compilers()` the compiler scan,
`load1()` the load average. A smoke measures a responsiveness check inside
a `host_load.Window(app_pid)` (or `Timed`, which also times its body), and
`host_load.judge` lets a check that holds pass; one that fails is a failure
when the rest of the machine held under a quarter of the CPUs and no
compiler ran, and otherwise a printed skip naming the cause ("18.3 of 22
CPUs busy with other work"). A window under a second is judged on a fresh
one-second sample, because `/proc/stat` counts in 10 ms ticks and a
millisecond window reads one tick of anything as several CPUs. Which
checks are which: [testing.md](../ui/testing.md#tests-that-time-things).

## The history

Records are appended to `~/.local/state/lobole/bench/<host-key>.jsonl`, one
JSON object per run (`--history PATH` overrides). They stay out of the
repository: a number is only comparable on the machine and load that
produced it, a committed one reads as a fact and goes stale, and a shared
file would collide in every merge of parallel worktrees. Each record carries
its commit, so "what did main measure before this" is a lookup, and every
worktree on the machine shares the one history.

A record: `schema`, `host_key`, `host`, `build`, `machine`, `pinned_cpus`,
`scope`, `injected`, `options` (`scale`, `set`, `repeats`), `busy`,
`busy_reasons`, `started`, `finished`, and `workloads`, each with its
`status` (`ok`, `failed` — an assertion — `error` or `skipped`), `reason`,
`failures`, `fixture` {name, hash}, `inputs` {path: sha256}, `samples_s`,
`median_s`, `spread_s`, `phases` (medians), `phase_samples`,
`memory_peak_bytes`, `cpus`, `load_before`, `load_after`, `foreign_load`
and `busy`.

## Reading a compare

`compare` checks the latest record against a baseline: `--against last`
(the record before it), a git revision (the last record of that commit) or
a file (its last record). It refuses, rather than compares, a baseline that
is busy, or from another host, build type or scale; a workload whose fixture
hash or input file differs is refused on its own line.

A workload regresses when its new median exceeds the baseline's by more
than max(3 × √(s_new² + s_base²), 5 % of the baseline, 5 ms), with s the
spreads: the noise floor comes from the repeats, not from a constant. An
improvement is reported by the same rule. The exit status is 1 on a
regression, 2 when something was refused, 0 otherwise.

```
compare: 3f2a91c0e4 against the previous record, 8b1d07a2f3
  history/om615-head   <base> ->  <new>   +x%  (threshold t)  SLOWER  ASSERTIONS FAILED: undo executed N feature(s), not 0: the regen cache missed
```

`compare --ab BIN_A BIN_B [WORKLOAD…]` is the form for judging one change,
and the one that holds on a busy machine. Each round launches a fresh
instance of each binary, on its own working copy of the same fixture, runs
its setup and a warm-up, and times one body; the order alternates ABBA ABBA…,
so drift in load or clock speed, and whatever falls on the first or the
second of a pair, lands on both sides. A fresh instance per round matters:
on a loaded machine one instance can run a workload steadily slower than
another instance of the same binary, so a single instance per side judges
two processes, not two binaries. The verdict is read from the paired
differences b_i − a_i: their median must pass the threshold above, computed
with their own spread, and every pair must agree in sign, so a load spike
that moves one pair moves no verdict. It prints each workload's two medians
with their ranges and the verdict, exits 1 when B is slower, and records
nothing in the history (`--json PATH` keeps the result). A wrapper script
that sets an environment variable and execs the binary is a binary here,
which is how a planted slowdown is A/B'd against the plain build.

## Injected slowdowns

Two environment variables plant a slowdown, for verifying the harness and
nothing else; both are read in `src/core/Document.cpp`, and the app logs a
warning when either is on:

- `LOBOLE_TEST_REGEN_DELAY_MS=N` sleeps N ms per executed feature.
- `LOBOLE_TEST_NO_REGEN_CACHE=1` caps the regeneration cache at zero, so
  only the current chain survives an eviction: an undo or a redo
  re-executes.

A run that sees either records it under `injected`, and `compare` says so
beside the numbers.

## Handing a slow case to the profiler

`run --profile WORKLOAD` writes the workload's setup and one timed body as
a driver script that reads `LOBOLE_PORT`, then runs `tools/profile.sh --app
<driver>`, so a regression `compare` found is one command from a call graph.
`profile.sh --app` launches `--headless` only, so for an `--offscreen` or a
startup workload it prints the `tools/profile.sh -- …` command for an
instance instead. The profiled instance, like the harness's, has a private
config and state directory and `--autosave 0`.

## The kernel's error-handler lock

The strandbeest generator's exact clash studies ran their pair checks on
every core and spent more time in the kernel than computing; `perf` put it
in OpenCASCADE's `Standard_ErrorHandler`.

Every `OCC_CATCH_SIGNALS` scope constructs a `Standard_ErrorHandler` and
links it into a stack; OCCT defines `OCC_CONVERT_SIGNALS` on every
non-MSVC build, so the scopes are live on Linux and on our mingw Windows
build. OCCT 7.6 to 7.9 keep ONE stack for all threads behind ONE mutex,
taken in the constructor and again in `Unlink`. A boolean opens thousands
of scopes: `GeomLib_CheckCurveOnSurface_TargetFunc::Value` opens one per
function evaluation, and every face/face intersection runs it
(`IntTools_FaceFace::ComputeTolReached3d`). Past a dozen threads the
workers queue on that mutex in futex calls, and the queue is bistable: the
same run on 22 threads took 94 s or 139 s.

The leg-pair study (stage 1, `recompute`, 6516 exact pair checks), wall
and system seconds:

| threads | shared stack (stock 7.9.3) | per-thread stack (patched) |
|---|---|---|
| 22 | 94-139 s, 457-1537 s sys; capped to 11, 64 s | 60-62 s, 8-9 s sys |
| 16 | 109 s, 589 s sys | |
| 11 | 68 s, 21 s sys | 62 s, 2 s sys |
| 8 | 73-75 s, 9 s sys | 72-76 s, 2 s sys |
| 4 | 113 s, 3 s sys | |

Ubuntu 24.04's OCCT 7.6.3 (in `tools/ci-local.sh`'s container) queues the
same way: 150-155 s and 1184-1464 s of system time on 22 threads, 93 s and
16 s capped to 11.

Two halves to the fix:

- **Every kernel we build keeps the stack per thread**
  (`nix/patches/occt-error-handler-thread-local.patch`, in the overlay, the
  Flatpak, the AppImage and the Windows cross build). It is the `.cxx` half
  of upstream's OCCT#980 (in 8.0): a `thread_local` top of stack and no
  mutex, the header and the ABI unchanged. A handler belongs to the thread
  that made it, and a converted signal is delivered to the faulting
  thread, so no other thread ever had business with it. Its test measures
  a scope's CPU on every thread against one: 13 us against 37 ns on the
  shared stack, 17 ns against 11 ns patched (`test_occ_patches`).
- **A kernel that still shares it runs booleans on half the machine**
  (`cad::BooleanWorkers`, `src/cad/KernelThreads.h`), since CI's stock
  Ubuntu builds, the Debian package and anyone building against a
  distribution's OCCT older than 8.0 do not get the patch. The loops whose
  items are booleans -- the motion study's exact checks, the contact and
  interference sweeps' exact pairs (`RunSweep`), the design sweep's
  regenerations -- pass it to `parallel::For` as a cap. Where the queue
  starts depends on the geometry: two crossed cylinders (a pin in a hole)
  queue from two threads, 10 booleans a second on 22 threads against 42 on
  2, where a per-thread stack gives 262; the leg pair from about sixteen.
  So no count suits every loop, and half the hardware threads is what the
  measurements chose: on 22 it took the leg pair from 94-139 s to 64 s and
  the walker study (39,100 pairs) from 478 s to 344 s; on a machine of 8
  (four cores, emulated by affinity), where nothing queued, it cost the leg
  pair 112 s against 99 s, the second thread of a core buying little.
  (The walker does not scale past 11 threads on either stack: 349 s
  patched on 11, 343 s on 22.) Whether the stack is per thread is asked of
  the running
  library (`cad::PerThreadErrorHandlers`: a handler made on one thread is
  unlinked from another, which only a shared stack lets it find), so a
  build run against another kernel than it was configured with still
  answers right; a pinned `LOBOLE_SWEEP_THREADS` is left alone. Only the
  thread count moves: every item is done once, and every result is what a
  one-thread run gives.

## Its own test

`bench_harness_smoke` (headless suite, label `bench`) runs
`startup/headless` and `boolean/plate-holes` at `--scale small` twice with
`--repeats 2 --busy-ok` into a private history, checks each record against
the schema, compares the second with the first, and has `compare` refuse a
busy baseline and one of another build type (whichever this binary is not:
a fixed "Debug" was build-asan's own type), `run` refuse an unknown
workload, and a missing input skip naming its path. It never asserts a
duration, and it writes nothing of the user's: its history, and the GPU
cache it seeds so that it launches no `--offscreen` instance, are in a
private directory. Where there is no systemd user manager it passes
`--no-scope`.

## What it does not do yet

- `profile.sh --app` cannot profile an `--offscreen` instance.
- The fixture's hash does not cover the Python SDK (`python/lobole`) a
  generator uses; a change there that alters what a generator builds needs
  the fixture removed by hand (`rm -r ~/.cache/lobole/bench/fixtures/<name>-*`).
