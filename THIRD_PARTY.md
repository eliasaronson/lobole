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

## deps/fonts/fa-solid-900.ttf — Font Awesome 6 Free (Solid)

SIL Open Font License 1.1. The `IconsFontAwesome6.h` codepoint header is MIT
(juliettef/IconFontCppHeaders).

## deps/imgui/misc/fonts/Roboto-Medium.ttf — Roboto

Apache License 2.0 (bundled with the Dear ImGui submodule).
