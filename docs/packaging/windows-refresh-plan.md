# Refreshing the Windows cross build

## The problem

`packaging/windows/` was built and Wine-verified on 2026-07-26/27 and has not
been exercised since — **594 commits to `src/` and `CMakeLists.txt` later**. It
is the only Windows path there is: there is no native MSVC/vcpkg recipe, so an
`.exe` exists only if this Linux tree cross-compiles one. Nothing in CI builds
it, so any of those 594 commits could have broken it silently, and the first
person to find out would be a tester holding a zip that does not run.

A tester needs a build of *today's* app: CAM, the tolerance/fit work, the
freewheel relations, the section-view picking, the present-stall fix — none of
which existed when that exe was last produced.

## The shape of the fix

Repair the cross derivation until it builds current `main`, prove the result
runs, then produce the two distributables that already have a recipe.

1. **Build** `nix-build packaging/windows/cross-app.nix` and fix what the 594
   commits broke. Two classes, and they are handled differently:
   - a **dependency** the cross set does not have (new `find_package`, new
     pkg-config module) — either cross it, or switch the feature off in
     `cmakeFlags` and say so in the release note. `ENABLE_TERMINAL` is already
     `NOT WIN32`-guarded, so libvterm is not one of these.
   - a **platform assumption** in `src/` (a POSIX call added without a
     `_WIN32` branch) — shim it the way the existing six shims are written:
     `#ifdef`-guarded, Linux path byte-identical, no core restructuring.
2. **Verify under Wine**, which is what makes this more than "it compiled":
   - headless: `--version`, then a `--remote` instance driven from the host
     over 127.0.0.1 — `new_project`, a feature, a sketch constraint that
     actually *solves* (the v1 check: circle r=1→2, extruded volume 25.13).
   - windowed GPU: `DISPLAY=:0`, `wine designer.exe --gui-tests
     --gui-test-filter boots_and_renders --hidden` — winevulkan hands the real
     RTX 4060 through, ray-tracing extensions included.
3. **Distributables**: `nix-build packaging/windows/installer.nix` →
   `designer-<ver>-win64-portable.zip` + `-setup.exe`.
4. **Regression check on Linux**: any shim added in step 1 is a source edit, so
   the native suite runs before this is called done — a Windows fix that costs a
   Linux test is not a fix.

## Files it touches

- `packaging/windows/cross-app.nix` — the dependency set and `cmakeFlags`.
- `packaging/windows/occ-cross.nix` — only if OCC 7.9's derivation moved under
  it in nixpkgs.
- `src/**` — only `#ifdef _WIN32` shims, if step 1 turns any up.
- `docs/packaging/plan.md` — Deliverable 3's status, the feature-parity list,
  and the gap list, against what this round actually produced.

## How it is verified

- `nix-build packaging/windows/cross-app.nix` exits 0 and `bin/` holds
  `designer.exe` plus its DLLs (OCC's 48 among them — they live in
  `win64/gcc/bin` and are gathered by the `postInstall`, not by nix).
- The two Wine runs above pass.
- `tools/test-fast.sh` stays green on Linux if `src/` was touched.
- The known gap is stated, not discovered by the tester: **FEM studies that
  need meshing do not work on Windows** (Netgen's source is MSVC-only —
  `netgen-cross.nix` holds the blocker notes). Anything else this round has to
  switch off gets added to that sentence.

## What it found (2026-09-14)

Six breakages, five of them in code written after the port last built, plus one
that had never worked. They are listed in docs/packaging/plan.md's Deliverable
3 rather than duplicated here. Two are worth carrying as rules:

- **`near`/`far` are empty macros**, so the platform that breaks is the one
  nothing builds. `test_conventions.cpp` now scans `src/` for the names — a
  central `#undef` is not a fix, because the next `#include <windows.h>`
  puts them back.
- **`designer_adaptive` had no install rule**, which is a LINUX packaging bug
  the Windows bundle happened to expose: every suite runs from the build tree,
  where the library sits beside the binary, so nothing here had ever exercised
  an installed layout. `cmake --install` to a scratch prefix and `ldd` the
  result — it takes a minute and no test covers it.

The nix cross build is memory-hungry at full parallelism: `-j22` on a 30 GB
machine was OOM-killed twice mid-link. `--cores 8` is the safe setting.
