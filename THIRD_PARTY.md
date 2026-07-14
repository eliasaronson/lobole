# Vendored third-party code

## deps/planegcs — PlaneGCS constraint solver

Copied from FreeCAD (`src/Mod/Sketcher/App/planegcs`), licensed under the **GNU
Lesser General Public License v2.1 or later** (LGPL-2.1-or-later). The original
copyright headers are preserved in each file.

Local changes to build it standalone (without the rest of FreeCAD):
- `#include <FCConfig.h>` and `#include <Base/Console.h>` in `GCS.cpp` replaced
  with `FreeCADCompat.h` (a no-op `Base::Console` shim).
- `#include "../../SketcherGlobal.h"` replaced with `SketcherExport.h`
  (defines the `SketcherExport` visibility macro as empty).
- `boost_graph_adjacency_list.hpp` wrapper replaced with the real
  `<boost/graph/adjacency_list.hpp>`.

Depends on Eigen (MPL2) and Boost.Graph. Built as the `planegcs` static library,
gated by `HAVE_PLANEGCS` (the app builds without it, constraints disabled).

## Netgen — tetrahedral mesher (linked, not vendored)

No Netgen source lives in this tree; it is an **external shared library** linked
at build time (nglib + ngcore), provided by the `netgen` package in `shell.nix`.
Licensed under the **GNU Lesser General Public License v2.1 or later**
(LGPL-2.1-or-later) — the same standing as PlaneGCS, and satisfied here by
dynamic linking against the unmodified upstream library.

Used only by `src/core/FemMesher.cpp` for FEM tetrahedral meshing, gated by
`HAVE_NETGEN` (the app builds and runs without it; FEM studies then report
"built without FEM support"). Netgen meshes an OpenCASCADE `TopoDS_Shape`
directly through its native OCC interface; the nixpkgs `netgen` is built against
the same `opencascade-occt` this project uses, so there is a single OCC in the
process. The wrapper is compiled as its own static library to keep Netgen's
build requirements (the `PARALLEL`/MPI defines and `-fabi-version=17` the
package was built with) out of the rest of the core.

## deps/fonts/fa-solid-900.ttf — Font Awesome 6 Free (Solid)

SIL Open Font License 1.1. The `IconsFontAwesome6.h` codepoint header is MIT
(juliettef/IconFontCppHeaders).

## deps/imgui/misc/fonts/Roboto-Medium.ttf — Roboto

Apache License 2.0 (bundled with the Dear ImGui submodule).
