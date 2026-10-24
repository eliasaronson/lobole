# Third-party components

Loböle is AGPL-3.0-or-later software. Binary distributions also include or
link the components below. Full license texts shipped with Loböle are under
[`LICENSES/`](LICENSES/README.md); exact dependency revisions for reproducible
Nix builds are pinned by `flake.lock` and the submodule commits.

## LGPL-family libraries

### PlaneGCS — constraint solver

- **Location:** `deps/planegcs`
- **License:** LGPL-2.1-or-later
- **Linkage:** separate shared library, `libplanegcs.so`
- **Source:** copied from FreeCAD's `src/Mod/Sketcher/App/planegcs`

Loböle carries local standalone-build modifications while preserving the
original per-file copyright headers:

- replaced FreeCAD configuration/console includes with `FreeCADCompat.h`;
- replaced `SketcherGlobal.h` with the local `SketcherExport.h` visibility shim;
- replaced FreeCAD's Boost adjacency-list wrapper with
  `<boost/graph/adjacency_list.hpp>`;
- set Eigen's thread count once per process instead of in every
  `System` constructor (a data race when sketches solve side by side);
- ordered a subsystem's unknowns, and the two-priority solve's, as they
  were declared instead of by address, so a solve's result does not depend
  on where the heap put its parameters.

It is deliberately built as a shared library so users can relink or replace it.
The modified source is included in every Loböle source release.

### libarea Adaptive — adaptive clearing for CAM

- **Location:** `deps/libarea`
- **License:** `Adaptive.{cpp,hpp}` LGPL-2.1-or-later (Kresimir Tusek);
  `clipper.{cpp,hpp}`, the Clipper 1 it uses internally, BSL-1.0 (Angus
  Johnson)
- **Linkage:** separate shared library, `libdesigner_adaptive.so`, installed by
  every package; only `src/core/cam/CamAdaptive.cpp` calls it
- **Source:** copied from FreeCAD commit
  `b2da06bfe0521773f302c8be977b31dc27f60203`, `src/Mod/CAM/libarea/`

Loböle's changes are the patches in `deps/libarea/patches/`, each also marked
and dated where it lands in the source; `deps/libarea/README.md` lists them.
`-DENABLE_CAM_ADAPTIVE=OFF` builds without it.

### Netgen — tetrahedral mesher

- **Version used by the current Nix build:** 6.2.2605
- **License:** LGPL-2.1-or-later
- **Linkage:** external shared libraries (`nglib`, `ngcore`), unmodified
  source; the Nix build configures it **without MPI, Python bindings or the
  Tk GUI** (`nix/netgen-lean.nix`) — features Loböle does not use, whose
  dependencies otherwise account for two thirds of the runtime closure
- **Source:** <https://github.com/NGSolve/netgen>

Netgen is used by `src/core/fem/FemMesher.cpp`. Builds without Netgen remain
functional but cannot generate FEM meshes.

### OpenCASCADE Technology — B-Rep kernel and interchange

- **Version used by the current Nix build:** 7.9.3
- **License:** LGPL-2.1 with the Open CASCADE exception
- **Linkage:** external shared libraries, built with the patches in
  `nix/patches/` (one: `occt-shell-cap-order.patch`)
- **Source:** <https://github.com/Open-Cascade-SAS/OCCT>

OpenCASCADE supplies B-Rep modeling, topology, tessellation, mass properties,
and STEP/IGES/STL/OBJ/BREP interchange. The OCCT exception is reproduced in
`LICENSES/OpenCASCADE-exception.txt`.

See [`LICENSES/SOURCE-OFFER.md`](LICENSES/SOURCE-OFFER.md) for exact source
locations and LGPL library replacement information.

## Header-only and linked dependencies

| Component | Purpose | License |
| --- | --- | --- |
| Eigen 3.4 | linear algebra and FEM | MPL-2.0 |
| Spectra 1.2 | sparse modal eigensolver | MPL-2.0 |
| Boost | PlaneGCS graph support | Boost Software License 1.0 |
| GLFW 3.4 | windowing and Vulkan surface creation | zlib license |
| GLM 1.0 | vector/matrix math | MIT or Happy Bunny License |
| spdlog 1.17 | logging | MIT |
| fmt | formatting used by spdlog | MIT |
| JSON for Modern C++ 3.12 | document/RPC JSON | MIT |
| Vulkan Loader and headers | Vulkan API loading | Apache-2.0 and bundled notices |
| libvterm 0.3 | VT emulation for the terminal panel (optional, `-DENABLE_TERMINAL=OFF`) | MIT |

These are supplied by the build/distribution environment rather than vendored
in the Loböle source tree. The Nix package pins their exact revisions.

## Vendored application components

### Dear ImGui

- **Location:** `deps/imgui` submodule
- **Revision:** v1.91.9b-docking pin
- **License:** MIT

### Dear ImGui Test Engine

- **Location:** `deps/imgui_test_engine` submodule
- **Revision:** v1.91.9 pin
- **License:** Dear ImGui Test Engine License v1.04

The engine is included at runtime because Loböle's remote `ui_action`
automation uses it, not only the GUI test suite. Loböle's AGPL grant includes
a section-7 additional permission for this linkage; see
`ADDITIONAL_PERMISSION.md` and the README.

**Optional at build time.** `cmake -DENABLE_TEST_ENGINE=OFF` removes it
entirely — no sources compiled, no headers included, no
`IMGUI_ENABLE_TEST_ENGINE` hooks in imgui — producing a binary with no
component under a non-open-source license. That build loses `--gui-tests` and
the remote `ui_action` tier and nothing else; the semantic RPC surface,
screenshots included, is unchanged. It is the configuration for anyone whose
use falls outside the engine's free-license criteria. CI compiles it on every
push so it cannot rot.

### Clipper2

- **Location:** `deps/clipper2` submodule
- **Revision:** Clipper2 1.5.4 pin
- **License:** Boost Software License 1.0
- **Linkage:** built as a static library into Loböle (CAM polygon offsetting;
  absent with `-DENABLE_CAM=OFF`)

### nativefiledialog-extended

- **Location:** `deps/nativefiledialog-extended` submodule
- **Revision:** v1.3.0 pin
- **License:** zlib license

### stb_image and stb_image_write

- **Location:** `src/thirdparty/`
- **License:** MIT or public domain, at the recipient's choice

## Fonts and generated icon declarations

| Component | Location | License |
| --- | --- | --- |
| Font Awesome 6 Free Solid | `deps/fonts/fa-solid-900.ttf` | SIL Open Font License 1.1 |
| IconFontCppHeaders declarations | `deps/fonts/IconsFontAwesome6.h` | zlib license |
| Roboto Medium | `deps/imgui/misc/fonts/Roboto-Medium.ttf` | Apache-2.0 |
| JetBrains Mono Regular | `deps/fonts/JetBrainsMono-Regular.ttf` | SIL Open Font License 1.1 |
| Liberation Sans Regular 2.1.5 (sketch text, font `sans`) | `deps/fonts/LiberationSans-Regular.ttf` | SIL Open Font License 1.1 |
| Hershey Roman Simplex (sketch text, font `hershey`) | `src/core/sketch/HersheyRomanSimplex.inc` | public domain with required attribution (`LICENSES/Hershey.txt`) |
| Loböle CAD icon glyphs | `deps/fonts/designer-icons.ttf`, `deps/fonts/IconsDesigner.h` | first-party: generated by `tools/icons/build_icon_font.py` (project license) |

## Build- and test-only dependencies

Catch2, CMake, Ninja, shaderc/glslc, ccache, and static-analysis tools are not
part of the installed runtime payload. Their licenses remain in their upstream
packages and source distributions.

## FreeCAD Fasteners Workbench — standard-parts dimension tables

`assets/parts/iso4762def.csv`, `iso4032def.csv`, `iso7089def.csv`,
`iso888length.csv`, `iso4757def.csv`, `iso10664def.csv`, `iso14579def.csv`,
`iso4017head.csv`, `iso10642def.csv`, `iso7380def.csv`

Vendored from https://github.com/shaise/FreeCAD_FastenersWB (`FsData/`),
**GPL-2.0-or-later**, unmodified below a provenance header. Data only — no code
is taken. The licence text is `LICENSES/GPL-2.0.txt`, with attribution in
`LICENSES/NOTICE-data-tables.txt`. These are the dimension tables a generated fastener is built from and
checked against (docs/parts/README.md).

## bd_warehouse — clearance-hole table

`assets/parts/clearance_hole_sizes.csv`, `tap_hole_sizes.csv`

Vendored from https://github.com/gumyr/bd_warehouse
(`src/bd_warehouse/data/`), **Apache-2.0**, metric rows
only — upstream also carries imperial sizes whose columns are fractional drill
designations ("17/64") rather than numbers. Data only; no code is taken. It is
what lets a plain 6.6 mm hole be read back as "an M6 normal clearance"
(docs/parts/README.md, phase 3). The licence text is `LICENSES/Apache-2.0.txt`;
the attribution and modification notice Apache-2.0 §4 asks for is
`LICENSES/NOTICE-data-tables.txt`, installed with every binary.

