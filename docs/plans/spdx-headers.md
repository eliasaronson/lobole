# SPDX headers on authored source

## The problem

Two of 1122 authored source files say what they are licensed as. The top-level
`LICENSE` makes the licence valid, so this is not a correctness gap — it is
two practical ones. Distribution packagers (Debian, Fedora, Flathub, and
Flatpak is already in the packaging plan) check for per-file SPDX identifiers
and file bugs when they are missing. And a file carrying no notice is one
whose licence a person copying a function out of it has to guess, which for
AGPL is exactly the guess you do not want them making.

## The header

```
// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
```

`#` instead of `//` for Python, shell and CMake, after any shebang.

A project-level notice, not a per-file author list: a list is never maintained,
reviewers nitpick it, and git already records authorship per line, accurately.
"and Loböle contributors" is true whether or not a contributor agreement is
ever signed, so this choice does not depend on the open `CLA.md` decision.

No year. Copyright duration does not depend on the notice, and a year is what
generates the annual "update the copyright" churn.

## Scope

1122 files: `src` (702), `tests` (264), `generators` (100), `tools` (43),
`benchmarks` (9), `packaging` (2), `CMakeLists.txt` and `cmake/`. Extensions
`.cpp .h .hpp .py .sh .cmake` and the shader stages.

Not, and each for its own reason:

- `deps/` and `src/thirdparty/` — other people's code. Fifteen of those files
  already carry their upstream SPDX headers, which are correct and stay.
  Putting our copyright on vendored LGPL source would be false.
- `benchmarks/results/` — committed records of runs, never rewritten.
- `assets/parts/` (FreeCAD Fasteners, bd_warehouse), `assets/hdri/` (CC0 from
  Poly Haven) — third-party data, covered by `THIRD_PARTY.md` and
  `assets/hdri/README.md`.
- Documentation, goldens and data files — a reader of a `.md` or a `.nc` is
  not about to copy a function out of it.

## Why not `reuse lint`

The obvious gate would be the [REUSE](https://reuse.software/) tool, and it
does not fit: REUSE requires `LICENSES/` to hold files named by SPDX
identifier, and ours is a curated distribution bundle named by component
(`GLFW.txt`, `Dear-ImGui-MIT.txt`, `OpenCASCADE-exception.txt`). It is shipped
by CMake, cross-referenced by exact filename from `THIRD_PARTY.md` in four
places, and carries the LGPL written offer. Renaming it to satisfy a linter
would mean re-verifying a compliance story to gain a tool, which is the wrong
way round. Full REUSE compliance stays available later as its own piece of
work.

So the gate is `tests/spdx_gate.py`, in the same shape as `linkage_gate.py`:
it fails on an authored source file with no header, so the rule holds for
files added later rather than decaying from the day it is written.

## How it is verified

- The gate is written and run BEFORE the sweep, and must fail on ~1120 files.
  A check nothing has ever failed is not a check.
- After the sweep it passes, and `tools/test-fast.sh`, the full headless suite
  and the GUI suite are unchanged — a comment cannot alter behaviour, so any
  movement here means a file was corrupted rather than annotated.
- Spot-check the forms that are not plain line comments: a shebang file keeps
  its shebang on line 1, and the shaders keep `#version` first.
- `git diff --stat` shows exactly two inserted lines per file and no deletions.

Expect one full rebuild: ccache keys on file content, so touching every
translation unit invalidates every entry once.
