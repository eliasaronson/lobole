# Designer — working notes for Claude

Parametric B-Rep CAD (C++23, Vulkan, OpenCASCADE 7.9, PlaneGCS, ImGui). Built
and run inside `nix-shell`. Keep this file short — it loads into every session.

## Build & run — always through nix

Bare `ninja`/`cmake` fail outside the nix env (missing glm/vulkan/OCC headers).
Always wrap:

```
nix-shell shell.nix --run "cd build && ninja designer designer_tests"
```

ccache is shared machine-wide, so rebuilds are fast. Do NOT nest a `nix-shell`
inside another (temp-dir failures) — the tool scripts already self-manage nix.

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
  (~60s; the SIMP topology-optimization gate dominates the wall-clock).
- GUI (real GPU on Xvfb): `./tools/gui-tests-xvfb.sh` (`-f <pattern>` filters a
  case). Runs `designer --gui-tests --hidden` under Xvfb; **flock-serialized**,
  so concurrent runs (parallel agents) are safe.
- Known flakes — rerun, don't chase: femgpu "TopologyOptimize…", and
  `headless_smoke` occasionally under load. Both pass on rerun.
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
