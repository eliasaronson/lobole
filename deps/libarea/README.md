# libarea — Adaptive clearing (vendored)

`Adaptive.cpp` / `Adaptive.hpp` are Kresimir Tusek's adaptive-clearing
algorithm, and `clipper.cpp` / `clipper.hpp` are the Clipper 1 it uses
internally. Copied from FreeCAD, with the one patch listed below:

    https://github.com/FreeCAD/FreeCAD
    commit b2da06bfe0521773f302c8be977b31dc27f60203
    src/Mod/CAM/libarea/

Copied at 2026-08-21. FreeCAD's copy is mid-migration from Clipper 1 to
Clipper 2 (the header includes both), so "we took it from FreeCAD" would not
be enough to reproduce this — hence the pinned commit.

## Licences, and why this is a separate shared library

| file | licence |
|---|---|
| `Adaptive.{cpp,hpp}` | **LGPL-2.1-or-later** (Kresimir Tusek, 2018) |
| `clipper.{cpp,hpp}` | BSL-1.0 (Angus Johnson) |

Adaptive is the only copyleft dependency in Designer. It is therefore built
as its own shared library — `designer_adaptive`, with an SONAME — and linked
dynamically; it is never absorbed into `designer_core`. The LGPL's condition
for use by a differently-licensed work is that the user can replace the
library with a modified one, and a versioned `.so` behind a stable interface
is exactly that. Keeping the boundary here keeps Designer's own licence a
decision that can still be made later.

Two rules follow, and both matter more than build tidiness:

1. **Do not link it statically** or move these sources into `core_sources`.
2. **Only `src/core/cam/CamAdaptive.cpp` may include `Adaptive.hpp`.** That
   file is the whole boundary; its header exposes nothing from libarea.

These files are kept identical to upstream **plus the patches in `patches/`**,
and nothing else. That is what makes "what did we change" checkable: apply
the patches to the pinned commit and the result must equal these files.

| patch | why |
|---|---|
| `0001-deterministic-link-path.patch` | `ResolveLinkPath` gave up after a `clock()` budget. `clock()` is the whole PROCESS's CPU time, and adaptive passes run on parallel workers, so the other passes drained each one's budget: the same part produced a different toolpath depending on thread scheduling. Two CAM tests failed only under a parallel `ctest`. The search's own step limit (10000) still bounds it. |
 The library narrates itself to `std::cout`;
that is swallowed by the bridge at the call site rather than patched out
here. `-DENABLE_CAM_ADAPTIVE=OFF` removes the dependency entirely, and the
concentric pocket strategy covers for it.
