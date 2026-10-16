# Loböle — working notes for Claude

Parametric B-Rep CAD (C++23, Vulkan, OpenCASCADE 7.9, PlaneGCS, ImGui). Built
and run inside `nix-shell`. Keep this file short — it loads into every session.

## How to work here

- **Anything multi-step and not trivial starts with a PLAN, written before the
  first edit** — `docs/plans/<topic>.md`. Agree it, then build it. Plan mode
  writes one; move it into the repo when the work lands there.
- **A plan is what to build and why. Nothing else.** The problem, the shape of
  the fix, the files it touches, and how it is verified. No dates, no decision
  log, no status chatter, no alternatives already rejected — a reader wants the
  argument, not the meeting.
- **When the work lands, the docs describe it and the plan goes.** Fold what
  is still true into the area's `docs/<area>/README.md` (docs/README.md has
  the rules), delete the plan, and check every other document that describes
  the behaviour you changed, the RPC `describe` rows, and
  `docs/versioning/format.md` if the format moved. For anything wider than a
  file or two, delegate that audit (see "Reading the code"): a round that
  documented as it went still left stale statements behind. The
  `[docs]` convention test fails on a doc path or `#anchor` that does not
  resolve, so a moved page cannot leave its citations dangling.
- **A merged worktree is deleted the same day** — see "Parallel work in git
  worktrees". A checkout that outlives its branch drifts, and the next session
  cannot tell which tree is the real one.

## Build & run — always through nix

Bare `ninja`/`cmake` fail outside the nix env (missing glm/vulkan/OCC headers).
Always wrap:

```
tools/build.sh            # = ninja -j$(tools/build-jobs) designer designer_tests, in nix
```

That pair is the inner loop. **`-j` comes from free memory, not cores**
(`tools/build-jobs`, ~1.5 GB a compiler): at one job per core, parallel
builds got the OOM killer to take the agent host itself. `tools/build.sh`
also holds `build/`'s lock exclusively, as `tools/merge-branch.sh` does before
it changes the tree: a merge that lands mid-build links objects that saw two
versions of a header. A suite holds it SHARED — **run ctest only through
`tools/ctest`** (every argument is ctest's), or a merge can relink
`designer_tests` mid-run and a test fails once and passes on the rerun. A
*fresh* build dir needs plain `ninja` — the pair skips `femgpu_tests`, and
ctest then fails on a missing executable instead of running the suite.

ccache is shared machine-wide, so rebuilds are fast. Do NOT nest a `nix-shell`
inside another (temp-dir failures) — the tool scripts already self-manage nix.

**A .cpp costs 10-17 s to rebuild; a real edit to `core/Feature.h` ~3.5 min, a
narrower header ~2 min** (a touch, revert or branch switch ~12 s, from ccache; precompiled headers
are off because they stop trees sharing ccache:
docs/architecture.md#precompiled-headers). So
edit headers in BATCHES — one rebuild for five header edits, not five — and
keep iterating in the .cpp where you can. (Editing a header mid-`ninja` also
fails with a bogus `redefinition of class`.)

**Grep build output for `FAILED`, not just `error:`.** A nix GC can collect the
ccache derivation the CMake cache points at; every compile then dies with
`ccache: No such file or directory`, which an `error:`-only filter reads as
success. It takes headers and libraries the same way, and each looks like a
different bug (`Vulkan::Vulkan includes non-existent path`, `undefined
reference to vterm_new`). One fix for all of them, across every build tree:
`tools/fix-stale-nix-cache.sh`. The same GC leaves binaries linked against
collected store paths (`libnglib.so` missing) — relink to fix.

**Formatting:** `.clang-format` (LLVM, 2-space, 80-col) governs C/C++. Run
`tools/setup-hooks.sh` once per clone to enable the `.githooks/pre-commit` hook,
which clang-format-checks only the staged hunks — keeps new code formatted
without forcing whole-file reflows.

## Defaults

A default carrying a unit — mm, degrees, a scale, a position — must be derived
from what it acts on (the bore, the body's bbox, the sheet), never a literal.
Hard-coded ones are correct in the abstract and wrong against any real part;
the `defaults/*` GUI suite enforces this for creation tools (every tool must
either change the document with its defaults or refuse visibly). See
docs/ui/tool-sessions.md#defaults-and-refusals.

## Refuse, don't substitute

An input we don't recognise — an enum string, a feature type, a mate type — is
REFUSED, naming what IS accepted. Never quietly replaced by a default: the
substitute is always plausible, so the document loads, looks right and is
wrong — an A2 sheet loading as A4, `add_mate(type:"revolut")` accepted as a
RIGID joint that solves with the wrong degrees of freedom, a modal study
running as static. Bool parsers are `[[nodiscard]]`, and
`tests/test_conventions.cpp` scans the source so a new one cannot arrive
silent.

Same rule for reporting: a mutating RPC that leaves a feature broken says so in
ITS OWN response (`regen`), not in a `get_state` the caller has to know to poll
— `tests/regen_honesty_smoke.py`.

## Tests

**Three suites, and they do NOT overlap — running all of ctest's `gui` label
after the sharder re-runs the whole GUI suite serially for 6 minutes.** Costs
are upper bounds on a loaded machine; the suites print their own counts.

| what | command | cost |
|---|---|---|
| headless | `tools/ctest -LE gui -j$(nproc)` | ~110 s |
| **inner loop** | `tools/test-fast.sh` | **~65 s** |
| GUI | `./tools/gui-tests-xvfb.sh` | ~130 s |
| the python `gui` ones | `tools/ctest -L pygui -j1` | ~110 s |
| the big generators | `tools/ctest -C generators -L generators -j2` (generators/README.md#the-big-generators-as-tests) | ~6 min |
| random designs | `tools/fuzz.py run --count 200` (docs/modeling/README.md#random-designs; background past 600 s) | ~1 min |

`gui_smoke` is the SAME tests `gui-tests-xvfb.sh` runs, only serial and
unsharded — ~365 s against ~130 s. It keeps the `gui` label so the headless
run excludes it; the python ones also carry **`pygui`**, which is why the row
above asks for a label instead of excluding `gui_smoke` by name. Full
verification is ~5 min. `remote_smoke` is most of the `pygui` time because it
drives the WINDOWED path (`--hidden`); passing `--headless` would override
that and stop testing what it exists for.

**Tags are ctest labels** (`ADD_TAGS_AS_LABELS`): `tools/ctest -L cam`, `-L sketch`,
`-L solver` all work — and that is the ONLY way to run a tag. The same filter
handed to the binary (`./tests/designer_tests '[cam]'`) runs those tests
SERIALLY in one process, about three times slower. The binary is for ONE case,
by name, while you iterate on it.

`[slow]` is the one tag with a rule attached, and `test-fast.sh` is exactly
`-LE 'gui|slow'`. Tag a new heavy test `[slow]` where you write it — a by-name
list kept elsewhere drifts.

- Entering `nix-shell` costs ~5 s; `tools/nix-run 'cmd'` runs a command in
  the same environment from a cache (`nix print-dev-env`, keyed on shell.nix
  and nixpkgs) in ~0.1 s, and `tools/build.sh`, `tools/ctest` and
  `tools/test-fast.sh` use it. Where `nix-shell` is still used, batch
  commands into one invocation rather than one each. Run the full headless suite before committing.
- GUI (real GPU on Xvfb): `./tools/gui-tests-xvfb.sh` — 4 parallel shards.
  `--fast` drops the slowest few, `-j 8` more shards, `-f <pattern>` one case,
  `--serial` for debugging, `--in-order` to switch the shuffle off.
  **Shuffled by default**, printing one base seed that replays the whole run
  (`--shuffle <seed>`) — an order-dependent failure needs that seed to be
  diagnosed. **flock-serialized** machine-wide, so concurrent runs (parallel
  agents) are safe — the shards are one suite. The runner asserts every listed
  test ran, so a bad partition fails loudly instead of skipping tests.
  `--record-times` refreshes the balance data.
- **`-DENABLE_CAM=OFF` is a supported configuration and CI builds AND tests
  it** on every push; a configuration nothing builds stops linking unnoticed.
  `ping` reports which optional subsystems a build has (`features`), and the
  smokes gate on that rather than on catching "unknown method", which would
  equally swallow a real typo.
- **A solver's test checks the outcome, not the formula.** Apply the answer
  through the real pipeline (`BodiesInSetupFrame`, the post, the verifier) and
  assert what it was for. `SolveIndexA`'s test compared its formula with its
  own number, and the toolpath framing turned the part the opposite way to the
  post, the rotary check and playback. Two fixes each made one side agree
  before a test through the pipeline showed which was right, and an indexed
  cut would have landed on the far side of the part.
- No test is a known flake. Anything "flaky under load" is worth one look for
  a mechanism before it earns that label: the ones that looked like it here
  were real bugs (a version number used as a document identity, a port race,
  a heap corruption, and femgpu "TopologyOptimize…": an unconverged fp32
  solve). A GUI test failing only while others build: replay it with the
  runner's `--slow-frames 160` / `--slow-jobs 3000`, which make it fail on a
  quiet machine (docs/ui/testing.md#wait-on-the-condition-not-a-frame-count).
  `tests/smoke_app.py` is the one launcher for smokes, keeps the app's log,
  and makes the app die with the test (`LOBOLE_EXIT_WITH_PARENT`).
- **An OpenCASCADE boolean MODIFIES its arguments** (it writes the p-curves it
  computes onto their edges). Two threads running booleans on pairs that share
  a body corrupt the heap — `malloc(): unaligned tcache chunk` frames away from
  anything of ours. Every query boolean runs non-destructive
  (`SetNonDestructive`; every boolean in `Measure.cpp` goes through
  `BuildQuery`, and `test_conventions.cpp` says so;
  docs/inspect/README.md#query-booleans-are-non-destructive). **Warming caches single-threaded — the
  rule core/Parallel.h states — does NOT make OCC safe to share**, because a
  boolean WRITES pair-specific data. `--tsan` names this class of bug.
- **Generating CPU load for a test: `tools/with-cpu-load.sh N -- cmd` — NEVER
  bare `while :; do :; done &` burners.** Backgrounded pids noted in one shell
  command are invisible to the next (each is a new shell), the burners orphan
  to init, and `kill $BURNERS` reaps nothing — they pin the machine for hours.
  Same class: don't SIGKILL `xvfb-run` wrappers — killing the wrapper (or
  `.terminate()` on it) orphans the Xvfb child AND the app; kill the announced
  app pid (`headless-preview.sh --stop`), and the GUI runner reaps orphaned
  Xvfb on its next run.
- **Quote globs in shell commands** (`grep --include='*.cpp'`): zsh fails an
  unquoted one with "no matches found" before the command runs.
- **Wait on a pid or a marker, kill by a pid — never by a pattern.** A
  `pgrep -f PATTERN` loop matches its own command line and waits forever; the
  same pattern given to `pkill -f` kills the shell that ran it (exit 144). A
  long run goes through `run_in_background` and its completion notice, or
  `tools/wait-for.sh --pid N` / `--file F --line REGEX`, whose deadline sits
  under the 600 s tool cap so a waiter is never left running in the
  background.
- `tools/ci-local.sh` runs both CI jobs here (headless suite + the stock-Ubuntu
  containers), deriving the package list and test exclusions from ci.yml.
  **`--pixel` is the third**: the gui-suite job's byte-exact render gate, in a
  container carrying the runner's llvmpipe — the only place its baseline can
  be checked or (`--pixel-record`) written, since a developer GPU records a
  different key.
- **`--asan` is the fourth, and it runs NOWHERE else.** The sanitizers CI job
  is `workflow_dispatch` only: a cold compile plus ~98 min on runner hardware
  against ~6 here, and the weekly cron it replaced sat red for three weeks
  unread. So run `tools/ci-local.sh --asan` when it earns its minutes —
  before a release, after touching ownership or threading, when chasing a
  crash — not on a cadence. The one thing it cannot see is what only Ubuntu's
  apt OpenCASCADE shows (nix's OCCT does not leak the STEP controller the way
  it does); dispatch the CI job by hand for that.
- **A format change never picks its number: write `vNEXT`.** Raise
  `kVersionPending` (src/core/io/SerializationInternal.h), name the golden
  `vNEXT-<name>.json` + `.expect.json` (`tests/golden/`, or `assembly/`,
  `job/`, `drawing/`), add its row to format.md's *Pending changes* table, and
  say `vNEXT` (`kVersionNEXT` in a migration) wherever the number would go.
  `tools/merge-branch.sh` numbers it as it lands, so parallel bumps never
  collide (docs/versioning/format.md#a-bump-takes-its-number-when-it-lands).
  `test_golden.cpp` and `format_version_gate` check it; CI fails a `vNEXT`
  left on main. No back-compat/migration is required (no users yet); fix any
  test that assumed old behavior.

## Benchmark — unfinished

`benchmarks/` grades an agent that has ONLY the MCP surface (`--tools ""`, no
shell, no repo) on described parts, with a C++/OCCT oracle that reads the
exported STEP back. **It is unfinished and does not yet measure what it is
for**, so it neither gates a change to a tool, a description or `help()` nor
backs a claim about one. Results that are run are committed, never
overwritten.

- Free self-test, run it first: `python3 benchmarks/runners/reference.py`
  builds every task by hand and must score every check. It PRINTS the totals;
  do not restate them anywhere, because a count written down goes stale.
  If it fails, the app or a task is broken — not the agent. Its counterpart
  `negative_controls.py` builds the construction- and fit-sensitive parts the
  WRONG way and must see every one REJECTED — a check nothing fails is not a
  check. `villain.py` is the floor from the other side: a block of each task's
  own bbox, which must not pass one.
- A sweep costs money. **`--dry-run` prints the plan, the run count and the
  ceiling for free — read it rather than a number written here.** ASK before
  spending it.

## Driving / screenshotting the running app

The app must own a real renderer to screenshot; true `--headless` has none.
- **The human's instance is `dev-watch.sh`, and the REGISTRY says which port** —
  `~/.local/state/lobole/instances/*.json`, or `Client.instances()`; a port
  number written in a doc goes stale while the registry cannot. It is a
  windowed build in their display session that relaunches on rebuild, and it is
  the build under review. Read from it (get_state) freely; do NOT mutate their
  open document — or their preferences (`set_settings` writes
  `~/.config/lobole/settings.json`). `headless-preview.sh` gives its instance
  a scratch config dir for exactly that reason, seeded from the real one so it
  still looks like their app.
- **Agents/CI get their OWN instance**: `./tools/headless-preview.sh --start
  <scratch-port>` backgrounds it, waits until it answers, and prints the pid;
  `--stop <port>` kills it by the pid it announced. **Never sleep for it** — an
  offscreen instance is drivable in about a SECOND, renderers and all.
  Without `--start` the plain form serves in the foreground.
  It runs `--offscreen` — the real renderer (full materials/shadows/SSAO) with
  no window, no Xvfb, no display server. **It has NO PANELS, so the 3D
  viewport is the whole framebuffer** — anything that draws full-window looks
  perfect here while being scaled and offset in the real app, so that bug is
  invisible offscreen and needs a windowed GUI test
  (`cam/simulation_plays_and_gives_it_back` asserts it). It projects the
  document into the renderers only when something LOOKS (a capture, or an RPC
  that resolves an element to a mesh), so `get_state` omits `bodies[].mesh`
  while the projection is behind rather than reporting the previous edit's id
  (docs/rendering/README.md#offscreen-mode-and-lazy-projection). A new scene renderer
  goes in `RegisterSceneRenderers()`/`ShutdownSceneRenderers()` in main.cpp and
  nowhere else; `test_conventions.cpp` fails if it is missing from either
  (docs/rendering/README.md#scene-renderers).
  It also does NOT isolate `XDG_STATE_HOME`: its autosave lands in the user's
  real `~/.local/state/lobole/` and will overwrite an unsaved document's
  recovery copy. Export a scratch one when driving a job.
  Use a scratch port (`tools/scratch_port.py` picks one that is free and
  unclaimed, and never the human's). One-shot:
  `headless-preview.sh --render PROJECT OUT.png <port>`. Only `ui_action`
  needs a GUI: `headless-preview.sh --gui <port>` (windowed under Xvfb).
- **Non-visual checks need no GPU**: `build/lobole --headless --remote <port>`.
- **Don't guess ports**: every instance announces itself in
  `~/.local/state/lobole/instances/<port>.json` (`Client.instances()` in
  the python client). `ping` reports the port, the open project, whether it is
  dirty, and whether ui_action is available in that build.
- Drive any of them with `python3` and `from lobole import Client` (or the
  SDK, `lobole.connect()`), or the `lobole` MCP tools. Screenshots come back
  as images — actually look at them; verify volumes with real numbers.

## Parallel work in git worktrees

Worth it only for genuinely **file-disjoint** work (different subsystems);
overlapping edits just move the cost to merge. **Start worktree agents two at
a time**: memory runs out before cores do. A docs-only change needs no
worktree; it goes in main after the code has merged. Setup:
```
git worktree add ../lobole-wtX -b wtX   &&   (cd ../lobole-wtX && tools/worktree-build.sh)
```
`worktree-build.sh` handles the gotcha: a fresh worktree has EMPTY submodules
(cmake fails without `git submodule update --init`; it fills them from main's
checkout, not the network), builds main's build type
at a `-j` sized by free memory, and hits main's ccache: ~2 min for `designer`
plus `designer_tests` in a fresh tree (docs/architecture.md#precompiled-headers).
If it compiles everything, main's `build/` is on an old toolchain:
`tools/fix-stale-nix-cache.sh`. It then **picks and prints a scratch port** for that worktree
(`tools/scratch_port.py` — free per the kernel, unclaimed per the announced
instances, and never the human's). Integrate SERIALLY, in the order branches finish:
`tools/merge-branch.sh BRANCH [--full]` cherry-picks under `build/`'s lock,
builds, runs the suites and removes the worktree once every commit is on main
and it holds nothing uncommitted. By hand, REMOVE the worktree as soon as its
branch is in. `git worktree remove` REFUSES here ("working trees containing
submodules cannot be moved or removed"), so it is:
```
git merge-base --is-ancestor wtX main && [ -z "$(git -C ../lobole-wtX status --porcelain)" ] \
  && rm -rf ../lobole-wtX && git worktree prune && git branch -d wtX
```
Check both first — merged, and nothing uncommitted — because the `rm` takes
the build directory with it (several GB each). A merged worktree left lying
about is a second checkout of the same project that no longer matches it, with
its own build dir and its own scratch port.

## Profiling

`tools/profile.sh` — the sampling profiler, an occasional audit like
`coverage.sh`, not part of CI. Three modes: `--test '<catch2 filter>'`,
`--app <driver.py>` (launches a headless instance on a scratch port and
profiles it while the script drives it), and `-- <any command>`. `perf` is
fetched by the script rather than living in shell.nix, which is kernel-coupled.

Two defaults it overrides, both of which fail in ways that look like a working
profile: this machine is **hybrid** (P-cores and E-cores are separate PMUs), so
the default hardware `cycles` event samples only one of them and returns a
handful of samples — hence `-e cpu-clock`; and the tree is built without frame
pointers, so `--call-graph dwarf` is the only mode with a usable stack.

## Crash debugging

`main()` installs a `std::stacktrace` handler (CrashHandler.cpp) — SIGSEGV/ABRT
print a symbolized stack to stderr → the dev-watch log, and write raw frames
to a crash file that the next start turns into a report folder under
`<state>/crashes/` (`crash_reports` RPC; `tools/symbolize-crash.py` names
the frames). `lobole --crash-test` exercises it; `debug_crash` does after
edits (needs `LOBOLE_ALLOW_DEBUG_CRASH=1`). **A crash fix lands with a
regression test built from its report**: the autosave copy is the starting
document, `session.log`'s `edit:`/`rpc:` lines are the steps, and the test
crashes before the fix. A HANG is reported by the frame watchdog: `main thread stalled
N s (in <RPC or dialog>)` plus the main thread's stack, then `resumed after`
(docs/architecture.md#crashes-and-stalls); `LOBOLE_WATCHDOG_S=0` turns it off
under a debugger. `DEV_WATCH_VALIDATE=1 tools/dev-watch.sh` turns on Vulkan
sync/GPU-assisted validation (slow; for device-lost/OOB bugs).

**GPU:** two different questions — "did we use Vulkan correctly" (validation
layers, above) and "what did this frame actually draw" (a RenderDoc capture:
`nix-shell -p renderdoc --run qrenderdoc`, launch `build/lobole` from inside
nix-shell). docs/rendering/gpu-debugging.md says which answers which and what
to look at; neither reports device memory, which nothing does yet.

## Reading the code

For anything that spans more than about three files, **delegate the reading**
to one Explore/general-purpose agent and act on its answer: many small greps
cost far more model time than one agent answering the multi-file question.
Grep directly when you know the file, or the symbol, or are checking one fact.

## Architecture pointers (edit the domain module, not the monolith)

docs/architecture.md#the-ui-actions-layer describes how the code is organised.
- RPC handlers: `src/remote/Remote{Modeling,Assembly,Inspection,…}.cpp`,
  dispatched by `RemoteDispatch.cpp` (the socket is `RemoteSession.cpp`, the
  GUI-action queue `RemoteUiActions.cpp`; `RemoteServer.cpp` is just the
  class). Add new RPCs to the domain module — and a row in its `MethodDoc`
  table, whose `flags` column is what gates it (NeedsPart / NotInBatch /
  Deferrable / UiAction). There is no separate gate list to remember.
  **A refused call changes nothing**: read and check every parameter before
  the first file, tab or job, and flag what reaches outside the document
  `NotInBatch` (docs/scripting/rpc.md#a-refused-call-changes-nothing,
  `refused_calls_smoke`). A parameter naming a physical quantity must carry a unit suffix (`angle_deg`,
  `tolerance_mm`) or have its unit named in the row — `tests/rpc_units_gate.py`
  fails otherwise, because a wrong unit is invisible in the answer
  (docs/conventions.md#units-and-angles).
- **The model is double; derived bulk data is float.** Positions, planes,
  placements, mate values, parameters: `double`/`glm::dvec3`/`dmat4`. Meshes,
  height maps, GPU buffers: float, narrowed once where a renderer is filled,
  never fed back. `test_conventions.cpp` scans the model headers
  (docs/conventions.md#the-model-is-double).
- **A CAM job edit calls `ui::MarkJobEdited`.** The job's undo guard records
  on that counter, not on the job's content (serializing it every frame was
  8% of an idle frame), so an edit that skips it does not undo; the GUI suite
  audits every guard against the content and fails the test that forgot
  (docs/cam/gui.md#undo).
- Serialization: `src/core/io/Serialize{Features,Assembly,Studies,Configurations}.cpp`.
- Core is split by domain: `src/core/{features,sketch,drawing,fem,io}/`; the
  document model (Document/Feature/Command/Expression) stays flat in `src/core/`.
- `main.cpp` + the renderers are the untested Vulkan layer — verified via
  gui_smoke + screenshots, not unit tests. `tests/render_pixel_smoke.py` is the
  one automated check on them: it re-renders a fixed scene and diffs the PNG
  byte-for-byte against a per-GPU baseline, so run it after ANYTHING that
  touches `RendererSync.cpp` or the renderers, which no unit test covers.
- The `ui/` actions layer is one file per subject (`ProjectActions`,
  `DocumentSlots`, `ViewActions`, `DrawingActions`, `MeasureActions`,
  `RendererSync`, `CreateActions`, `SceneOverlays`, `SketchOverlay`,
  `VariableActions`, `ImportActions`, …), each with its own header; helpers
  shared between them live in `AppActionsInternal.h`. Same shape in
  `core/cam/` (`CamGeometry`, `CamSetup`, `CamPicks`, `CamDerive`,
  `CamStrategies`, `CamTravel`) and for the sketch session (`SketchTools`,
  `SketchDimensions`, `SketchPick`, `SketchOps`, …).

Deeper project state, decisions, and gotchas live in Claude's memory
(`~/.claude/projects/.../memory/`).
