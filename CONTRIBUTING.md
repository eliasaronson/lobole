# Contributing to Loböle

Thanks for looking. This is a parametric B-Rep CAD application (C++23, Vulkan,
OpenCASCADE, PlaneGCS, Dear ImGui) with an unusual design goal: **an AI
assistant drives it through the same command layer a human does**. That shapes
most of the conventions below.

Everyone taking part follows the [code of conduct](CODE_OF_CONDUCT.md).

Start with [`docs/README.md`](docs/README.md): each area of the program has a
`README.md` under `docs/` describing how it works now, and
[`docs/roadmap.md`](docs/roadmap.md) lists what is not built yet.

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
- **Precompiled headers are off** (`-DENABLE_PCH=ON` turns them on): a
  `.gch` is not byte-reproducible, so with one no two build trees share a
  ccache object
  ([docs/architecture.md](docs/architecture.md#precompiled-headers)).
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
tools/ctest -LE gui -j$(nproc)    # ctest under build/'s lock, in nix

# GUI: the real app on a virtual display, driving its own widgets.
./tools/gui-tests-xvfb.sh            # 4 shards, ~130 s
./tools/gui-tests-xvfb.sh --fast     # ...minus the slowest few
./tools/gui-tests-xvfb.sh -f section # ...one case
```

CI runs the headless suite on every push (a PR must pass it), builds with
the test engine off and with CAM off, and runs the GUI suite serially on
software Vulkan (llvmpipe) with a byte-exact pixel gate. Run the GUI suite
locally before opening a PR if you touched anything in `src/ui`, `src/app` or
`src/render`: it is faster on a real GPU, and the runner prints the seed of a
failing order.

### The inner loop

```
tools/test-fast.sh        # the headless suite minus the `slow` label, ~65 s
```

The tests tagged `[slow]` (a python smoke: `LABELS slow`) are real gates on
real numerics and toolpaths; they stay in `ci-local` and CI, and not in the
check you run after every edit. Run the full suite before you commit.

The GUI suite (`tools/gui-tests-xvfb.sh`) runs as four parallel shards, about
three times faster than one process. It is CPU-bound (real OCC geometry,
real frames) with no per-test overhead to trim, so running several at once
is the only lever. `--fast` skips the slowest few, `-j 8` shards further,
and `-f <pattern>` runs one case.

The whole suite is still flock-serialized machine-wide: the shards are one
suite, not several, and a second run waits its turn rather than fighting for
the GPU. Balance comes from `tools/gui-test-times.txt`, refreshed with
`--record-times`; a test missing from it still runs (at a default weight), so
a stale file costs speed, never coverage. The runner asserts that the shards
ran every test the binary lists, so a mis-partition fails loudly.

### Running CI on your own machine

```
tools/ci-local.sh              # everything CI does on a push, here
tools/ci-local.sh --nix        # just the headless suite, ~2 min
tools/ci-local.sh --image ubuntu:26.04
tools/ci-local.sh --asan       # the ASan + UBSan job, ~6 min warm
```

It reproduces both CI jobs, including the stock-Ubuntu containers that check
the README's package list is complete — and it **derives** the package list,
the container images and the excluded tests from
`.github/workflows/ci.yml`, so the two cannot drift. Build trees live in
`.ci-local/` and are reused, so a second run is a rebuild of what changed
(~1 minute) rather than a fresh compile. The containers run as you
(`--user`), so `.ci-local/` is yours to delete; a tree that an older run left
owned by root is handed back once, through the same image, at the next run.

`--asan` is separate because the sanitized build is its own ~4-minute compile.
On GitHub that job runs weekly and on demand rather than per push, so if you
are touching lifetimes — caches, the regen prefix cache, anything holding a
`Feature*` — run it locally rather than waiting for Monday.

Worth having whether or not you are watching a minutes budget: a red build
found here costs a coffee, and found on the runner costs a round trip.

(`act` runs the workflow YAML itself, which is more faithful but pulls
multi-GB runner images. Reach for it when the workflow *file* is what you are
debugging.)

No test is a known flake. A test that fails only under load is worth one
look for a mechanism before it earns that label: every one that looked like
it here was a real bug.

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
  [`docs/ui/tool-sessions.md#defaults-and-refusals`](docs/ui/tool-sessions.md#defaults-and-refusals).
- **A feature an assistant cannot drive is half-shipped.** A new feature type
  needs its row in the `describe` table (`remote_smoke` fails otherwise) and
  in `FEATURE_FORMATS` in `tools/designer_mcp.py` (`mcp_formats_gate`
  cross-checks it against `describe`).
- **A new tool must be reachable by clicking.** Give the button a stable
  `###tb_*` id and add a GUI test that finishes the tool and checks the
  resulting geometry — not just that a feature appeared. Hidden labels
  (`##Name`) and `ImGui::PushID` scopes make a widget unaddressable to tests
  and scripts; use `###Name`. See
  [`docs/ui/testing.md`](docs/ui/testing.md).
- **A serialization change bumps the format version, written `vNEXT`**:
  it raises `kVersionPending` (`src/core/io/SerializationInternal.h`),
  **adds a golden** `vNEXT-<name>.json` under `tests/golden/` (or its
  `assembly/`, `job/` or `drawing/` directory) **and a row in
  [Pending changes](docs/versioning/format.md#pending-changes)**. The merge
  gives it its number
  ([how](docs/versioning/format.md#a-bump-takes-its-number-when-it-lands));
  `test_golden.cpp` and `format_version_gate` check it.
- **Edit the domain module, not the monolith.** RPCs live in
  `src/remote/Remote{Modeling,Assembly,Inspection,…}.cpp`; serialization in
  `src/core/io/Serialize*.cpp`.

## Driving the app (how most debugging happens here)

```
build/lobole --remote 8437              # windowed
build/lobole --headless --remote 8437   # no GPU, no display: full modeling
```

Then drive it from Python:

```python
import sys; sys.path.insert(0, "tools")
from designer_remote import Designer
d = Designer(8437)
d.call("add_feature", feature={"type": "Box", "name": "B", "on": "Top",
                               "width": 10, "depth": 6, "height": 4})
print(d.call("get_state")["bodies"][0]["volume"])
```

The same surface is exposed to MCP clients — see
[`docs/scripting/mcp.md`](docs/scripting/mcp.md). Instances announce
themselves, so a client finds a running app without being told a port.

For a screenshot-capable instance without a desktop session:
`./tools/headless-preview.sh <port>`.

## Reporting a bug

Open an issue with the bug template
([`.github/ISSUE_TEMPLATE/`](.github/ISSUE_TEMPLATE/bug_report.yml)); ideas
use the feature template. A security problem is reported privately instead,
as [`SECURITY.md`](SECURITY.md) describes, and never in a public issue.

Include `lobole --version`, your OS and GPU, and what you did. If it
crashed, the symbolized stack trace is on stderr (a `std::stacktrace` handler
is installed for SIGSEGV/SIGABRT) — paste it. A `.dsgn` project that
reproduces the problem is worth more than a description; the format is text,
so it diffs and reviews cleanly.

## Pull requests

Small and focused beats large and complete. Say what you changed and how you
verified it — this codebase's habit is to state the actual numbers ("volume
31.5, matching the exact frustum", "580 headless, 134 GUI") rather than "tests
pass".

## Licensing a contribution

By contributing you agree that your work is licensed under
**AGPL-3.0-or-later**, the license of the project.

Sign off each commit (`git commit -s`). That appends a `Signed-off-by:` line
certifying you wrote the patch, or otherwise have the right to submit it under
that licence — the [Developer Certificate of
Origin](https://developercertificate.org/), as the Linux kernel and the CNCF
projects use it. If your employer holds rights in work you do, have their
permission before you send it.

**A new dependency is a licensing change, not a build change, and is reviewed
as one.** The LGPL libraries here are linked as *separate shared objects*, and
that is the only reason this application can be licensed on its own terms:
LGPL-2.1 section 6 permits the combination while — and only while — a recipient
can relink the library. Turning one into a static link removes that, silently,
in builds already shipped. So:

- a new dependency ships its [`THIRD_PARTY.md`](THIRD_PARTY.md) row (component,
  purpose, licence, linkage) in the same pull request; and
- `tests/linkage_gate.py` fails if an LGPL-family library stops being linked
  dynamically, or if anything is linked that no row covers.

Nothing GPL-licensed is copied into `src/`. That one links nothing, so no
build-level check can see it — it is the only rule here that rests entirely on
review.
