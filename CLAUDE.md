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

## Tests

- Headless (no GPU/display): `nix-shell shell.nix --run "cd build && ctest -LE gui -j$(nproc)"`
  — 81 s for 622 tests, of which the SIMP gate alone is 89 s of CPU and the
  next four topology/FEM gates 12-14 s each. **Inner loop: `tools/test-fast.sh`
  (8 s, 603 tests)**; full suite before committing.
- GUI (real GPU on Xvfb): `./tools/gui-tests-xvfb.sh` — 148 tests in **80 s**
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
- `tools/ci-local.sh` runs both CI jobs here (headless suite + the stock-Ubuntu
  containers), deriving the package list and test exclusions from ci.yml.
- Format changes bump `kVersion` (src/core/io/SerializationInternal.h) AND add
  a golden `tests/golden/v<N>-<name>.json/.expect.json` — one per
  bump. No back-compat/migration is required (no users yet); fix any test that
  assumed old behavior. (Check the header for the current version — it moves.)

## Driving / screenshotting the running app

The app must own a real renderer to screenshot; true `--headless` has none.
- **The human's instance is `dev-watch.sh` on port 8799** — a windowed build in
  their display session that relaunches on rebuild. It is the build under review.
  Read from it (get_state) freely; do NOT mutate their open document.
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
