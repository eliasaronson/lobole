# <img src="assets/icons/org.lobole.Lobole.svg" width="48" height="48" alt="" align="top"> Loböle

[![CI](https://github.com/eliasaronson/lobole/actions/workflows/ci.yml/badge.svg)](https://github.com/eliasaronson/lobole/actions/workflows/ci.yml)

Open-source parametric CAD: B-Rep modeling on OpenCASCADE, a Vulkan
viewport, and one command layer that the GUI, Python scripts and AI agents
all drive. Linux first; Windows builds exist, macOS is untested.

## What it does

- **Parametric modeling:** a constraint sketcher, features that survive
  upstream edits (stable topological naming), variables and expressions,
  configurations, sheet metal, bent tube.
- **Assemblies:** mates, sub-assemblies, motion studies with clash checks,
  exploded views, a BOM, standard fasteners.
- **Drawings:** sheets with projected views, dimensions and tables, out to
  SVG and PDF.
- **Simulation:** static and modal FEM, and topology optimization, on the
  part itself.
- **Manufacturing:** 2.5D, 3-axis and 4th-axis milling posted for Mach3,
  LinuxCNC or GRBL, each file verified by simulating it; checks for 3D
  printing.
- **Versioning:** history, named versions, branches and merges, backed by
  git.
- **Scripting and agents:** JSON-RPC, a Python SDK and an MCP server reach
  every command the GUI has; a terminal panel runs an agent inside the
  window.
- **Interchange:** STEP, STL and BREP in; those plus glTF, OBJ, PLY, DXF,
  SVG and PDF out.

> [!WARNING]
> **CAM has cut real parts on one machine only**: a 6090 CNC router on
> Mach3. Simulate and dry-run every file before cutting.

https://github.com/user-attachments/assets/ac232b74-7f4d-4371-a852-b47f5414cd35

## Gallery

Every model here is built by a generator, an ordinary Python program driving
the API; [generators/README.md](generators/README.md) lists them all.

| | |
|:---:|:---:|
| [![OM615 diesel with its injection pump](docs/gallery/om615-engine.jpg)](generators/om615) | [![Automatic watch](docs/gallery/automatic-watch.jpg)](generators/watch) |
| **OM615 diesel** (Opus 5.5): every part down to the bearing shells, with its injection pump chained to the crank and timed | **Automatic watch** (Opus 5, one shot): a 3 Hz self-winding movement behind the dial, with lever escapement, rotor winding and keyless works |
| [![Bicycle](docs/gallery/bicycle.jpg)](generators/bike) | [![FPV quadcopter](docs/gallery/fpv-drone.jpg)](generators/fpv_drone) |
| **Bicycle** (Opus 5.5, one shot): modelled from photographs | **5-inch FPV quadcopter** (Opus 5, one shot): 91 occurrences, fitted fasteners, FEM on the arms |

## Installing

Each release on the GitHub releases page carries:

- **Nix / NixOS:** `nix run github:eliasaronson/lobole` runs the full app,
  GUI included.
- **Windows:** a portable zip and an installer. They are built on Linux,
  tested under Wine only and not signed, so SmartScreen warns. Netgen
  meshing, and with it FEM, is missing.
- **Linux, headless:** an AppImage that runs the document engine and its
  JSON-RPC interface without a window (`--headless --remote PORT`). It needs
  FUSE and unprivileged user namespaces, which Ubuntu 23.10+ restricts
  ([the headless AppImage](docs/packaging/README.md#the-headless-appimage)).
  For the GUI on a non-Nix distribution, build from source (below).
- **Container:** `ghcr.io/eliasaronson/lobole-headless`, the same headless
  engine ([Container](#container-headless)).
- **Python:** `pip install lobole` (or `uv tool install lobole`) installs the
  client, the SDK and the MCP server (`lobole-mcp`), which drive a running
  app ([docs/scripting/README.md](docs/scripting/README.md)).

[docs/packaging/README.md](docs/packaging/README.md) describes how each is
built and what a release checks before publishing.

## Requirements

Loböle builds on any Linux with the libraries below.

**Ubuntu 24.04:** CI installs exactly this list on every push, so it stays
complete:

```
sudo apt install git cmake ninja-build build-essential python3 \
    libvulkan-dev glslc libglfw3-dev libglm-dev \
    libspdlog-dev libgtk-3-dev nlohmann-json3-dev libeigen3-dev libboost-dev \
    libocct-data-exchange-dev libocct-modeling-algorithms-dev libocct-modeling-data-dev \
    libocct-foundation-dev libocct-ocaf-dev libocct-visualization-dev \
    libvterm-dev
```

**Nix / NixOS:** `shell.nix` pins every dependency, the optional ones too.
Run `nix-shell` in the repo root; there is nothing to install. (The Vulkan
driver comes from your system config: `hardware.graphics.enable = true` plus
your GPU's driver module.)

**Flatpak:** the GPU-accelerated build for any distribution, without
matching system libraries: `tools/flatpak-build.sh` (manifest in
`packaging/flatpak/`).

**Other distributions:** a C++23 compiler, CMake 3.14+, Vulkan headers,
loader and `glslc`, GLFW, GLM, spdlog, nlohmann-json, Eigen 3.3+, Boost
headers, GTK 3 (file dialogs) and OpenCASCADE 7.6+ (foundation, modeling
data, modeling algorithms, data exchange, OCAF, visualization). The Ubuntu
list above says *what*; your package manager decides the names.

**Windows** cross-compiles from Linux (`packaging/windows/`). **macOS:**
nobody has built it there yet.

Vendored dependencies (Dear ImGui, Dear ImGui Test Engine, nativefiledialog,
Clipper2) are git submodules: clone with `--recurse-submodules`, or run
`git submodule update --init --recursive`.

### Optional features

`cmake` prints what it compiled in:

```
Loböle 0.1.0 optional features
  [x] CAM (milling, G-code)        deps/clipper2 (vendored, Boost)  -DENABLE_CAM=OFF
  [x] Terminal panel               libvterm (system)  -DENABLE_TERMINAL=OFF
  [x] FEM solver                   Eigen (system)
  ...
```

A feature with a switch can be turned off at configure time, and then it does
not exist: no sources, no dependency, no tests, no RPCs, no preference. Those
builds pass their own suite, and CI builds the one without CAM on every push.

Two dependencies are found if present, and the app builds without them:

| Missing | Effect |
| --- | --- |
| **Netgen** | No FEM meshing: the simulation workspace opens and `run_study` refuses. Not in Ubuntu's repos; `shell.nix` and the Flatpak have it. |
| **Spectra** | No modal analysis (natural frequencies). Static stress still works. |

So an apt build gives you the whole CAD application except simulation. For
FEM, use Nix or the Flatpak, or build Netgen and Spectra yourself and re-run
CMake.

### OpenCASCADE versions

7.6 (Ubuntu 24.04) and 7.7+ build; development is against 7.9.3. On 7.6, PLY
export is unavailable, two tests fail on kernel behaviour that changed by
7.9, and some booleans come out wrong. CI checks all of it on every push;
[OpenCASCADE 7.6](docs/packaging/README.md#opencascade-76) has the details.

## Building

```
cmake -S . -B build -G Ninja
cmake --build build
cmake --install build --prefix "$HOME/.local"   # optional: app, assets, desktop entry
```

### Without Dear ImGui Test Engine

```
cmake -S . -B build -DENABLE_TEST_ENGINE=OFF
```

Every other dependency is open source; the test engine is not. It is free
for individuals, non-profits, education, open-source derivative works and
companies under 2M USD turnover, but a larger company that builds or ships
Loböle needs a paid licence from DISCO HELLO. This flag removes it entirely.
You lose the GUI test suite (`--gui-tests`) and the remote `ui_action` tier.

## Running

```
build/lobole [-f path/to/file.step]
```

### Container (headless)

The document engine and its JSON-RPC interface with no GPU and no display.
It is built from the flake, so it has the full feature set (OpenCASCADE 7.9,
Netgen, Spectra) that a distribution build cannot:

```
nix build .#dockerHeadless && docker load < result
docker run --rm -p 8437:8437 lobole-headless:latest
PYTHONPATH=python python3 -m lobole.client 8437 get_state
```

It models, measures, meshes, solves and exports; `screenshot` and the
UI-action tier need a renderer and are unavailable. The image is about
1.1 GB (169 MB compressed), nearly all of it the CAD kernel.

> [!CAUTION]
> The JSON-RPC interface is **unauthenticated and can read and write
> files**. The server binds to localhost unless started with
> `--remote-bind-all`, which the image does so a published port is
> reachable. Never publish that port on a network you do not trust.

## Testing

```
tools/ctest -LE gui -j$(nproc)    # headless: no GPU, no display
./tools/gui-tests-xvfb.sh         # the GUI suite on a virtual display
```

[CONTRIBUTING.md](CONTRIBUTING.md#running-the-tests) covers both suites,
running CI on your own machine, and the sanitizer builds.

## Remote control

Scripts and AI agents can drive a live session over JSON-RPC (localhost
only, opt-in):

```
./lobole --remote 8437
export PYTHONPATH=$PWD/python   # the lobole package; nix-shell sets this
python3 -m lobole.client 8437 get_state
python3 -m lobole.client 8437 add_feature \
    '{"feature": {"type": "Box", "name": "Box 1", "on": "Top",
                  "width": 2, "depth": 2, "height": 2}}'
python3 -m lobole.client 8437 screenshot '{"path": "/tmp/view.png"}'
```

Every GUI command is reachable this way, plus UI actions that click and type
in the real window (`menu_click`, `item_click`, `viewport_drag`, …).
`--headless` runs everything except the UI actions and screenshots with no
GPU and no display. Running instances announce themselves, so a client finds
the app without being told a port. The Python SDK is in
[docs/scripting/sdk.md](docs/scripting/sdk.md), and MCP clients (Claude and
others) get the same surface through `lobole.mcp`
([docs/scripting/mcp.md](docs/scripting/mcp.md)).

## License

Copyright © Elias Högbom Aronsson

Loböle is free software licensed under the **GNU Affero General Public
License, version 3 or any later version** (AGPL-3.0-or-later). See
[`LICENSE`](LICENSE) for the full text.

In short: you may use, study, modify and redistribute it, but a distributed
or network-hosted derivative must also be released under the AGPL with its
source. The copyleft covers the *software only*: files you make with Loböle
(models, drawings, exports) are yours, with no obligation to publish them.

It comes **without any warranty** (sections 15 and 16 of the licence). Check
simulation results by hand or by test before relying on them for anything
that carries load, and dry-run every G-code file. What you build and cut with
Loböle is your responsibility.

### Additional permission (GPL/AGPL section 7): imgui_test_engine linking

As an exception, and as an additional permission under section 7 of the AGPL,
Elias Högbom Aronsson grants permission to link or combine this program with
**Dear ImGui Test Engine** (`deps/imgui_test_engine`, © Omar Cornut / DISCO
HELLO, distributed under its own license), and to convey the resulting work.
You may extend this exception to a modified version of the program, but you are
not obligated to do so. This exception does not modify any other term of the
AGPL, and does not grant any rights in Dear ImGui Test Engine itself beyond
those in its own license. The distributable notice is also available as
[`ADDITIONAL_PERMISSION.md`](ADDITIONAL_PERMISSION.md).

### Trademarks

Other products named in this repository are trademarks of their owners, and
Loböle is **not affiliated with, endorsed by, or sponsored by** any of them;
[`TRADEMARKS.md`](TRADEMARKS.md) says where those names are used and why.

The project's own name and logo are **not licensed under the AGPL**: a fork
or modified build takes its own name, so anyone can tell whose build they
are running ([`TRADEMARKS.md`](TRADEMARKS.md#this-projects-own-name)).

### Third-party components

Third-party components and their licences, including the LGPL libraries
Loböle links dynamically, are listed in [`THIRD_PARTY.md`](THIRD_PARTY.md).
