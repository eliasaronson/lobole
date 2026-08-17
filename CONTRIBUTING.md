# Contributing to Designer

Thanks for looking. This is a parametric B-Rep CAD application (C++23, Vulkan,
OpenCASCADE, PlaneGCS, Dear ImGui) with an unusual design goal: **an AI
assistant drives it through the same command layer a human does**. That shapes
most of the conventions below.

Start with [`docs/roadmap.md`](docs/roadmap.md) for what exists and what is
planned; each workstream has its own plan document under `docs/`.

## Getting a build

**Clone with submodules.** This is the single most common way to waste an
afternoon here — CMake fails in confusing ways against empty `deps/`
directories:

```
git clone --recurse-submodules https://github.com/eliasaronson/Designer.git
# already cloned?
git submodule update --init --recursive
```

**With Nix** (the environment the project is developed in — every dependency
pinned, nothing to install):

```
nix-shell shell.nix --run "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release"
nix-shell shell.nix --run "cd build && ninja"
```

Build **all** targets the first time. `ninja designer designer_tests` is the
fast inner-loop command, but it skips `femgpu_tests`, and `ctest` then reports
a missing executable rather than a passing suite.

Do not nest `nix-shell` inside another one; the tool scripts manage it
themselves.

### Faster builds

- **ccache** is picked up automatically when installed (`apt install ccache`,
  or it is already in `shell.nix`) and is shared machine-wide, so a rebuild
  after `git checkout` of a branch you have built before is close to free.
  CMake prints `ccache enabled: …` at configure time when it found it.
- **Ninja** over Make: `-G Ninja`. Every command here assumes it.
- **Build one target while iterating**: `ninja designer` for the app,
  `ninja designer_tests` for the unit tests. The full `ninja` is only needed
  the first time (and after touching CMakeLists).
- A **precompiled header** already covers the heavy common set
  (`CADKernel.h`, glm, nlohmann/json) for `designer_core`; nothing to enable.
- A cold full build is ~2 minutes on 24 threads, ~15 on 4. If it is slower
  than that, check that ccache is actually being used — see the `FAILED`
  caveat in [CLAUDE.md](CLAUDE.md#build--run--always-through-nix).

**Without Nix**: the apt list in the [README](README.md#requirements) is what
CI installs, so it is kept honest by every push.

**Without Dear ImGui Test Engine**: `-DENABLE_TEST_ENGINE=OFF`. That is the
one dependency here that is not open source (free for individuals,
non-profits, open-source derivative works and companies under 2M USD turnover;
larger companies pay). The flag costs you `--gui-tests` and the remote
`ui_action` tier and nothing else — see the README section.

## Running the tests

Two suites, and both matter:

```
# Headless: unit tests + python integration smokes. No GPU, no display.
nix-shell shell.nix --run "cd build && ctest -LE gui -j$(nproc)"

# GUI: the real app on a virtual display, driving its own widgets.
./tools/gui-tests-xvfb.sh            # -f <pattern> filters to one case
```

CI runs the headless suite on every push (a PR must pass it) plus a build with
the test engine off. The GUI suite needs a GPU, so it is local-only — please
run it before opening a PR if you touched anything in `src/ui`, `src/app` or
`src/render`.

Known flakes, worth a rerun rather than an investigation: the femgpu
`TopologyOptimize` gate, and `headless_smoke` under heavy parallel load —
specifically its autosave-sidecar wait, which prints the app's log and state
when it trips, so please paste that if you hit it.

## Formatting and commits

```
tools/setup-hooks.sh    # once per clone
```

That installs a pre-commit hook which clang-formats **only your staged hunks**
(`.clang-format` is LLVM, 2-space, 80 columns) — new code stays formatted
without whole-file reflows landing in your diff.

Commit messages follow [Conventional
Commits](https://www.conventionalcommits.org/): `fix:`, `feat:`, `refactor:`,
`chore:`, `docs:`, `test:`, `ci:`. Write the body for someone reading it in a
year: what was wrong, and why this is the fix.

## Conventions that are enforced, and why

These are the ones a newcomer trips over. Most have a test behind them.

- **Defaults carry units, so derive them from what they act on.** A
  countersink sized from the bore, a view scale from the sheet, a force
  direction from the picked face — never a literal. Every creation tool must
  either change the document with its defaults or refuse visibly; the
  `defaults/*` GUI suite is the gate. See
  [`docs/ui/u10-defaults-and-refusals.md`](docs/ui/u10-defaults-and-refusals.md).
- **A feature an assistant cannot drive is half-shipped.** A new feature type
  needs its row in the `describe` table (`remote_smoke` fails otherwise) and
  in `FEATURE_FORMATS` in `tools/designer_mcp.py` (`mcp_smoke` cross-checks
  it against `describe`).
- **A new tool must be reachable by clicking.** Give the button a stable
  `###tb_*` id and add a GUI test that finishes the tool and checks the
  resulting geometry — not just that a feature appeared. Hidden labels
  (`##Name`) and `ImGui::PushID` scopes make a widget unaddressable to tests
  and scripts; use `###Name`. See
  [`docs/ui/gui-coverage.md`](docs/ui/gui-coverage.md).
- **A serialization change bumps `kVersion`** (`src/core/io/SerializationInternal.h`)
  **and adds a golden pair** under `tests/golden/`. One bump, one golden.
- **Edit the domain module, not the monolith.** RPCs live in
  `src/remote/Remote{Modeling,Assembly,Inspection,…}.cpp`; serialization in
  `src/core/io/Serialize*.cpp`.

## Driving the app (how most debugging happens here)

```
build/designer --remote 8437              # windowed
build/designer --headless --remote 8437   # no GPU, no display: full modeling
```

Then drive it from Python:

```python
import sys; sys.path.insert(0, "tools")
from designer_remote import Designer
d = Designer(8437)
d.call("add_feature", feature={"type": "Box", "name": "B", "position": [0,0,0],
                               "width": 10, "height": 4, "depth": 6})
print(d.call("get_state")["bodies"][0]["volume"])
```

The same surface is exposed to MCP clients — see
[`docs/scripting/mcp.md`](docs/scripting/mcp.md). Instances announce
themselves, so a client finds a running app without being told a port.

For a screenshot-capable instance without a desktop session:
`./tools/headless-preview.sh <port>`.

## Reporting a bug

Include `designer --version`, your OS and GPU, and what you did. If it
crashed, the symbolized stack trace is on stderr (a `std::stacktrace` handler
is installed for SIGSEGV/SIGABRT) — paste it. A `.dsgn` project that
reproduces the problem is worth more than a description; the format is text,
so it diffs and reviews cleanly.

## Pull requests

Small and focused beats large and complete. Say what you changed and how you
verified it — this codebase's habit is to state the actual numbers ("volume
31.5, matching the exact frustum", "580 headless, 134 GUI") rather than "tests
pass".

By contributing you agree that your work is licensed under
**AGPL-3.0-or-later**, the license of the project.
