# Designer — working notes for Claude

Parametric B-Rep CAD (C++23, Vulkan, OpenCASCADE 7.9, PlaneGCS, ImGui). Built
and run inside `nix-shell`. Keep this file short — it loads into every session.

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

**Grep build output for `FAILED`, not just `error:`.** A nix GC can collect the
ccache derivation the CMake cache points at; every compile then dies with
`ccache: No such file or directory`, which an `error:`-only filter reads as
success. Fix: `nix-shell shell.nix --run "cd build && cmake -UCCACHE_PROGRAM
-UCMAKE_CXX_COMPILER_LAUNCHER -UCMAKE_C_COMPILER_LAUNCHER ."`. The same GC
leaves binaries linked against collected store paths (`libnglib.so` missing) —
relink to fix.

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
| headless | `nix-shell shell.nix --run "cd build && ctest -LE gui -j$(nproc)"` | ~85 s |
| GUI | `./tools/gui-tests-xvfb.sh` | ~90 s |
| the python `gui` ones | `ctest -L gui -E gui_smoke -j1` | ~110 s |

`gui_smoke` is the SAME tests `gui-tests-xvfb.sh` runs, only serial and
unsharded — ~365 s against ~90 s. Exclude it (`-E gui_smoke`) whenever the
sharder has already run, which is every time. Full verification is then
~5 min, not ~10.

The python `gui` three went from ~46 s to ~110 s on 2026-08-25, and the cause
is worth knowing rather than treating as drift: `remote_smoke` had silently
become HEADLESS (`--headless` overrides the `--hidden` it passes), so it had
stopped testing the windowed path it exists for. Restoring it costs 51 s and
buys back the coverage.

- Headless (no GPU/display): `nix-shell shell.nix --run "cd build && ctest -LE gui -j$(nproc)"`
  — ~85 s for 836 tests, of which the SIMP gate alone is ~90 s of CPU and the
  next four topology/FEM gates 12-14 s each. **Inner loop: `tools/test-fast.sh`
  (11 s)**; full suite before committing. Each `nix-shell` costs
  ~3 s to enter, so batch commands into one invocation rather than one each.
- GUI (real GPU on Xvfb): `./tools/gui-tests-xvfb.sh` — 215 tests in **~90 s**
  across 4 parallel shards (335 s serial). `--fast` drops the 5 slowest
  (69 s), `-j 8` more shards, `--fast -j 8` = 48 s, `-f <pattern>` one case
  (11 s), `--serial` for debugging. Still **flock-serialized** machine-wide,
  so concurrent runs (parallel agents) are safe — the shards are one suite.
  The runner asserts every listed test ran, so a bad partition fails loudly
  instead of skipping tests. `--record-times` refreshes the balance data.
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

## Driving / screenshotting the running app

The app must own a real renderer to screenshot; true `--headless` has none.
- **The human's instance is `dev-watch.sh` on port 8799** — a windowed build in
  their display session that relaunches on rebuild. It is the build under review.
  Read from it (get_state) freely; do NOT mutate their open document — or their
  preferences (`set_settings` writes `~/.config/designer/settings.json`).
  `headless-preview.sh` gives its instance a scratch config dir for exactly
  that reason, seeded from the real one so it still looks like their app.
- **Agents/CI get their OWN instance**: `./tools/headless-preview.sh <scratch-port>`
  runs the real renderer offscreen under Xvfb (full materials/shadows/SSAO). Use
  a scratch port (NOT 8799). One-shot: `headless-preview.sh --render PROJECT OUT.png <port>`.
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
(ccache-warm). Each worktree drives its own scratch port. Integrate SERIALLY:
merge one branch to main, run suites, then the next.

## Crash debugging

`main()` installs a `std::stacktrace` handler (CrashHandler.cpp) — SIGSEGV/ABRT
print a symbolized stack to stderr → the dev-watch log. `designer --crash-test`
exercises it. `DEV_WATCH_VALIDATE=1 tools/dev-watch.sh` turns on Vulkan
sync/GPU-assisted validation (slow; for device-lost/OOB bugs).

## Architecture pointers (edit the domain module, not the monolith)

- RPC handlers: `src/remote/Remote{Modeling,Assembly,Inspection,…}.cpp`
  (dispatched by RemoteServer.cpp). Add new RPCs to the domain module.
- Serialization: `src/core/io/Serialize{Features,Assembly,Studies,Configurations}.cpp`.
- Core is split by domain: `src/core/{features,sketch,drawing,fem,io}/`; the
  document model (Document/Feature/Command/Expression) stays flat in `src/core/`.
- `main.cpp` + the renderers are the untested Vulkan layer — verified via
  gui_smoke + screenshots, not unit tests.

Deeper project state, decisions, and gotchas live in Claude's memory
(`~/.claude/projects/.../memory/`).
