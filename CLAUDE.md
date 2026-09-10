# Designer — working notes for Claude

Parametric B-Rep CAD (C++23, Vulkan, OpenCASCADE 7.9, PlaneGCS, ImGui). Built
and run inside `nix-shell`. Keep this file short — it loads into every session.

## How to work here

- **Anything multi-step and not trivial starts with a PLAN, written before the
  first edit** — `docs/<area>/plan.md` for a subsystem, `docs/<topic>-plan.md`
  for a round of work. Agree it, then build it. Plan mode writes one; move it
  into the repo when the work lands there.
- **A plan is what to build and why. Nothing else.** The problem, the shape of
  the fix, the files it touches, and how it is verified. No dates, no decision
  log, no status chatter, no alternatives already rejected — a reader wants the
  argument, not the meeting.
- **When the work is done, check the docs against it** — every document that
  describes the behaviour you changed, the RPC `describe` rows, and
  `docs/versioning/format.md` if the format moved. For anything wider than a
  file or two, delegate the audit (see "Reading the code"). One round that
  thought it was documented as it went still had nine stale statements,
  including a format-history row naming a field that had been renamed.
- **A merged worktree is deleted the same day** — see "Parallel work in git
  worktrees". A checkout that outlives its branch drifts, and the next session
  cannot tell which tree is the real one.

## Build & run — always through nix

Bare `ninja`/`cmake` fail outside the nix env (missing glm/vulkan/OCC headers).
Always wrap:

```
nix-shell shell.nix --run "cd build && ninja designer designer_tests"
```

That pair is the inner loop. A *fresh* build dir needs plain `ninja` — the
pair skips `femgpu_tests`, and ctest then fails on a missing executable
instead of running the suite.

ccache is shared machine-wide, so rebuilds are fast. Do NOT nest a `nix-shell`
inside another (temp-dir failures) — the tool scripts already self-manage nix.

**A .cpp costs 6-13 s to rebuild; a widely-included header costs ~90 s.** So
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
which clang-formats-checks only the staged hunks — keeps new code formatted
without forcing whole-file reflows.

## Defaults

A default carrying a unit — mm, degrees, a scale, a position — must be derived
from what it acts on (the bore, the body's bbox, the sheet), never a literal.
Hard-coded ones are correct in the abstract and wrong against any real part;
the `defaults/*` GUI suite enforces this for creation tools (every tool must
either change the document with its defaults or refuse visibly). See
docs/ui/u10-defaults-and-refusals.md.

## Refuse, don't substitute

An input we don't recognise — an enum string, a feature type, a mate type — is
REFUSED, naming what IS accepted. Never quietly replaced by a default: the
substitute is always plausible, so the document loads, looks right and is
wrong. Real cases: an A2 sheet loading as A4 (every view then placed against
different paper), `add_mate(type:"revolut")` accepted as a RIGID joint that
solves perfectly well with the wrong degrees of freedom, a modal study running
as static. Bool parsers are `[[nodiscard]]`, and `tests/test_conventions.cpp`
scans the source so a new one cannot arrive silent.

Same rule for reporting: a mutating RPC that leaves a feature broken says so in
ITS OWN response (`regen`), not in a `get_state` the caller has to know to poll
— `tests/regen_honesty_smoke.py`.

## Tests

**Three suites, and they do NOT overlap — running all of ctest's `gui` label
after the sharder re-runs the whole GUI suite serially for 6 minutes:**

| what | command | cost |
|---|---|---|
| headless | `nix-shell shell.nix --run "cd build && ctest -LE gui -j$(nproc)"` | ~83 s (1113) |
| **inner loop** | `tools/test-fast.sh` | **~19 s** (1094 of 1113) |
| GUI | `./tools/gui-tests-xvfb.sh` | ~130 s |
| the python `gui` ones | `ctest -L pygui -j1` | ~110 s |

`gui_smoke` is the SAME tests `gui-tests-xvfb.sh` runs, only serial and
unsharded — ~365 s against ~130 s. It keeps the `gui` label so the headless
run excludes it; the four python ones also carry **`pygui`**, which is why the
row above asks for a label instead of excluding `gui_smoke` by name. Full
verification is ~5 min, not ~10.

**Tags are ctest labels** (`ADD_TAGS_AS_LABELS`): `ctest -L cam`, `-L sketch`,
`-L solver` all work — and that is the ONLY way to run a tag. The same filter
handed to the binary (`./tests/designer_tests '[cam]'`) runs those tests
SERIALLY in one process: 144 s against ~50 s for `ctest -L cam -j$(nproc)`.
The binary is for ONE case, by name, while you iterate on it.

`[slow]` is the one tag with a rule attached — 18 tests, ~310 s of CPU
between them, and `test-fast.sh` is exactly `-LE 'gui|slow'`. Tag
a new heavy test `[slow]` where you write it; a by-name list in `tools/` used
to do this and drifted until the "8 s" loop took 49 s.

The python `gui` three went from ~46 s to ~110 s on 2026-08-25, and the cause
is worth knowing rather than treating as drift: `remote_smoke` had silently
become HEADLESS (`--headless` overrides the `--hidden` it passes), so it had
stopped testing the windowed path it exists for. Restoring it costs 51 s and
buys back the coverage.

- Headless (no GPU/display): `nix-shell shell.nix --run "cd build && ctest -LE gui -j$(nproc)"`
  — ~83 s for 1113 tests, of which the SIMP gate alone is 77 s of CPU and four
  CAM clearing cases 138 s between them. **Inner loop: `tools/test-fast.sh`
  (~19 s)**; full suite before committing. Each `nix-shell` costs
  ~3 s to enter, so batch commands into one invocation rather than one each.
- GUI (real GPU on Xvfb): `./tools/gui-tests-xvfb.sh` — 282 tests in **~130 s**
  across 4 parallel shards (335 s serial). `--fast` drops the 5 slowest
  (69 s), `-j 8` more shards, `--fast -j 8` = 48 s, `-f <pattern>` one case
  (11 s), `--serial` for debugging, `--in-order` to switch the shuffle off.
  **Shuffled by default** since 2026-09-01, printing one base seed that
  replays the whole run (`--shuffle <seed>`) — three order-dependent failures
  went undiagnosed because nobody ran shuffled locally, so no seed existed.
  Still **flock-serialized** machine-wide,
  so concurrent runs (parallel agents) are safe — the shards are one suite.
  The runner asserts every listed test ran, so a bad partition fails loudly
  instead of skipping tests. `--record-times` refreshes the balance data.
- **`-DENABLE_CAM=OFF` is a supported configuration and CI builds AND tests it**
  on every push (968 tests). It had silently not linked for weeks before
  2026-09-07, because nothing built it. `ping` reports which optional
  subsystems a build has (`features`), and the smokes gate on that rather than
  on catching "unknown method", which would equally swallow a real typo.
- Known flakes — rerun, don't chase: femgpu "TopologyOptimize…". The
  `headless_smoke` autosave flake was a real bug, fixed 2026-08-19 (a document
  version number used as a document identity, ee247f9) — a "flaky under load"
  test is worth one look for a mechanism before it earns that label.
  `fem_smoke` failed twice under `-j$(nproc)` on 2026-08-25; the suspected
  `free_port()` race was FIXED the same day (L8: the app binds `--remote 0`
  itself; `tests/smoke_app.py` is the one launcher and keeps the app's log).
- **Generating CPU load for a test: `tools/with-cpu-load.sh N -- cmd` — NEVER
  bare `while :; do :; done &` burners.** Backgrounded pids noted in one shell
  command are invisible to the next (each is a new shell), the burners orphan
  to init, and `kill $BURNERS` reaps nothing. This has pinned the machine for
  hours twice. Same class: don't SIGKILL `xvfb-run` wrappers — killing the
  wrapper (or `.terminate()` on it) orphans the Xvfb child AND the app; kill
  the announced app pid (`headless-preview.sh --stop`), and the GUI runner now
  reaps orphaned Xvfb on its next run.
- `tools/ci-local.sh` runs both CI jobs here (headless suite + the stock-Ubuntu
  containers), deriving the package list and test exclusions from ci.yml.
- Format changes bump `kVersion` (src/core/io/SerializationInternal.h), add
  a golden AT that version, AND a row in docs/versioning/format.md's version
  history. **Both are gated** (`test_golden.cpp`): the newest golden must
  equal `kVersion`, and the history table must reach it — it had rotted five
  versions behind. The golden goes in `tests/golden/v<N>-<name>.json` +
  `.expect.json` for a document, or `tests/golden/assembly/v<N>-<name>.json`
  for assembly schema; **the gate counts both directories**, and forgetting
  the second is how an audit once reported a present golden as missing.
  The corpus is NOT one-per-bump historically — 11 of 35 bumps have none,
  almost all v2-v16 — so do not read it as exhaustive. No back-compat/
  migration is required (no users yet); fix any test that assumed old
  behavior. (Check the header for the current version — it moves.)

## Benchmark — is the API still usable by an agent?

`benchmarks/` grades an agent that has ONLY the MCP surface (`--tools ""`, no
shell, no repo) on described parts — static ones, T4/T5 mechanisms, and a Fit
task graded against a host part the agent never sees — with a C++/OCCT oracle
that reads the exported STEP back and links no designer_core. Changing a tool, a description,
a refusal message or `help()` changes what this measures — **no benchmark, no
claim**, and results are committed, never overwritten.

- Free self-test, run it first: `python3 benchmarks/runners/reference.py`
  builds every task by hand and must score every check. It PRINTS the totals;
  do not restate them here, because this line has already gone stale twice
  (8 tasks then 13, 48/48 then 68/68) — the same rule as ports, one file up.
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
  `~/.local/state/designer/instances/*.json`, or `Designer.instances()`. It was
  8799 when this line was written and has been 9090 since; a number repeated in
  a doc goes stale while the registry cannot. It is a windowed build in
  their display session that relaunches on rebuild. It is the build under review.
  Read from it (get_state) freely; do NOT mutate their open document — or their
  preferences (`set_settings` writes `~/.config/designer/settings.json`).
  `headless-preview.sh` gives its instance a scratch config dir for exactly
  that reason, seeded from the real one so it still looks like their app.
- **Agents/CI get their OWN instance**: `./tools/headless-preview.sh --start
  <scratch-port>` backgrounds it, waits until it answers, and prints the pid;
  `--stop <port>` kills it by the pid it announced. **Never sleep for it** — an
  offscreen instance is drivable in about a SECOND, renderers and all, and a
  guessed `sleep 16` after `nohup` cost one dogfooding round six minutes of
  doing nothing. Without `--start` the plain form serves in the foreground.
  It runs `--offscreen` — the real renderer (full materials/shadows/SSAO) with
  no window, no Xvfb, no display server. **It has NO PANELS, so the 3D
  viewport is the whole framebuffer** — anything that draws full-window looks
  perfect here while being scaled and offset in the real app, so that bug is
  invisible offscreen and needs a windowed GUI test
  (`cam/simulation_plays_and_gives_it_back` asserts it). It projects the
  document into the renderers only when something LOOKS (a capture, or an RPC
  that resolves an element to a mesh) — on a 200-body part that took an edit
  from 1778 ms back to 141 ms — so `get_state` omits `bodies[].mesh` while the
  projection is behind rather than reporting the previous edit's id
  (docs/rendering/offscreen-lazy-projection.md). A new scene renderer
  goes in `RegisterSceneRenderers()`/`ShutdownSceneRenderers()` in main.cpp and
  nowhere else; `test_conventions.cpp` fails if it is missing from either
  (docs/rendering/renderer-wiring.md).
  It also does NOT isolate `XDG_STATE_HOME`: its autosave lands in the user's
  real `~/.local/state/designer/` and will overwrite an unsaved document's
  recovery copy. Export a scratch one when driving a job.
  Use a scratch port (`tools/scratch_port.py` picks one that is free and
  unclaimed, and never the human's). One-shot:
  `headless-preview.sh --render PROJECT OUT.png <port>`. Only `ui_action`
  needs a GUI: `headless-preview.sh --gui <port>` (windowed under Xvfb).
- **Non-visual checks need no GPU**: `build/designer --headless --remote <port>`.
- **Don't guess ports**: every instance announces itself in
  `~/.local/state/designer/instances/<port>.json` (`Designer.instances()` in
  the python client). `ping` reports the port, the open project, whether it is
  dirty, and whether ui_action is available in that build.
- Drive any of them with `python3` importing `Designer` from
  `tools/designer_remote.py`, or the `designer` MCP tools. Screenshots come back
  as images — actually look at them; verify volumes with real numbers.

## Parallel work in git worktrees

Worth it only for genuinely **file-disjoint** work (different subsystems);
overlapping edits just move the cost to merge. Setup:
```
git worktree add ../designer-wtX -b wtX   &&   (cd ../designer-wtX && tools/worktree-build.sh)
```
`worktree-build.sh` handles the gotcha: a fresh worktree has EMPTY submodules
(cmake fails without `git submodule update --init`). ~75s to a working binary
(ccache-warm). It then **picks and prints a scratch port** for that worktree
(`tools/scratch_port.py` — free per the kernel, unclaimed per the announced
instances, and never the human's), so you no longer hold one in your head. Integrate
SERIALLY: merge one branch to main, run suites, then the next — and REMOVE the
worktree as soon as its branch is in. `git worktree remove` REFUSES here
("working trees containing submodules cannot be moved or removed"), so it is:
```
git merge-base --is-ancestor wtX main && [ -z "$(git -C ../designer-wtX status --porcelain)" ] \
  && rm -rf ../designer-wtX && git worktree prune && git branch -d wtX
```
Check both first — merged, and nothing uncommitted — because the `rm` takes
the build directory with it. Five merged worktrees from one week held 16 GB.
A merged worktree left lying about is a second checkout of the same project
that no longer matches it, with its own build dir and its own scratch port.

## Profiling

`tools/profile.sh` — the sampling profiler, an occasional audit like
`coverage.sh`, not part of CI. Three modes: `--test '<catch2 filter>'`,
`--app <driver.py>` (launches a headless instance on a scratch port and
profiles it while the script drives it), and `-- <any command>`. `perf` is
fetched by the script rather than living in shell.nix, which is kernel-coupled.

Two defaults it overrides, both of which fail in ways that look like a working
profile: this machine is **hybrid** (P-cores and E-cores are separate PMUs), so
the default hardware `cycles` event samples only one of them — a 27 s run came
back with 42 samples — hence `-e cpu-clock`; and the tree is built without
frame pointers, so `--call-graph dwarf` is the only mode with a usable stack.

## Crash debugging

`main()` installs a `std::stacktrace` handler (CrashHandler.cpp) — SIGSEGV/ABRT
print a symbolized stack to stderr → the dev-watch log. `designer --crash-test`
exercises it. `DEV_WATCH_VALIDATE=1 tools/dev-watch.sh` turns on Vulkan
sync/GPU-assisted validation (slow; for device-lost/OOB bugs).

**GPU:** two different questions — "did we use Vulkan correctly" (validation
layers, above) and "what did this frame actually draw" (a RenderDoc capture:
`nix-shell -p renderdoc --run qrenderdoc`, launch `build/designer` from inside
nix-shell). docs/rendering/gpu-debugging.md says which answers which and what
to look at; neither reports device memory, which nothing does yet.

## Reading the code

For anything that spans more than about three files, **delegate the reading**
to one Explore/general-purpose agent and act on its answer. Measured over one
round: 200 small greps cost 4.7 minutes of tool time and **46 minutes of model
time** — the largest single line in the round — while one agent answered two
multi-file questions (where tabs live in a toolpath; what a face pick does to
a profile) in 4.5 minutes and replaced a dozen of them. Grep directly when you
know the file, or the symbol, or are checking one fact.

## Architecture pointers (edit the domain module, not the monolith)

- RPC handlers: `src/remote/Remote{Modeling,Assembly,Inspection,…}.cpp`,
  dispatched by `RemoteDispatch.cpp` (the socket is `RemoteSession.cpp`, the
  GUI-action queue `RemoteUiActions.cpp`; `RemoteServer.cpp` is now just the
  class). Add new RPCs to the domain module — and a row in its `MethodDoc`
  table, whose `flags` column is what gates it (NeedsPart / NotInBatch /
  Deferrable / UiAction). There is no separate gate list to remember.
  A parameter naming a physical quantity must carry a unit suffix (`angle_deg`,
  `tolerance_mm`) or have its unit named in the row — `tests/rpc_units_gate.py`
  fails otherwise. Forty-two did not, and a wrong unit is invisible in the
  answer (docs/units-plan.md).
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
  `VariableActions`, `ImportActions`, …), each with its own header. There is no
  `AppActions` any more; helpers shared between them live in
  `AppActionsInternal.h`. Same shape in `core/cam/` (`CamGeometry`, `CamSetup`,
  `CamPicks`, `CamDerive`, `CamStrategies`, `CamTravel`) and for the sketch
  session (`SketchTools`, `SketchDimensions`, `SketchPick`, `SketchOps`, …).
  docs/architecture-review-2026-09-06.md is why.

Deeper project state, decisions, and gotchas live in Claude's memory
(`~/.claude/projects/.../memory/`).
