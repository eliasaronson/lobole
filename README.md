# Designer

[![CI](https://github.com/eliasaronson/Designer/actions/workflows/ci.yml/badge.svg)](https://github.com/eliasaronson/Designer/actions/workflows/ci.yml)

Project to build an open-source parametric 3D CAD application using Vulkan, ImGui and
OpenCASCADE — working similarly to professionally used CAD software (Onshape, SolidWorks),
with transformation checkpoints, global variables, a Python REPL/scripting API, git-based
versioning, and an interface designed so that LLM assistants can use the same commands as
human users.

See [docs/roadmap.md](docs/roadmap.md) for the current state and the long-term plan.

## Requirements

System packages (Ubuntu 24.04):

```
sudo apt install cmake build-essential libvulkan-dev glslc libglfw3-dev libglm-dev \
    libspdlog-dev libgtk-3-dev nlohmann-json3-dev libeigen3-dev libboost-dev \
    libocct-data-exchange-dev libocct-modeling-algorithms-dev libocct-modeling-data-dev \
    libocct-foundation-dev libocct-ocaf-dev libocct-visualization-dev
```

On NixOS, `shell.nix` provides everything instead: run `nix-shell` in the repo root.
(The Vulkan ICD comes from the system config — `hardware.graphics.enable = true` plus
your GPU driver module — not from the shell.)

Vendored dependencies (ImGui, nativefiledialog) are git submodules:

```
git submodule update --init --recursive
```

## Building

```
mkdir -p build
cd build
cmake ..
cmake --build . -j$(nproc)
```

Works with OpenCASCADE 7.6 (Ubuntu packages) and 7.8+ (the STEP/STL library renaming is
handled in CMakeLists.txt).

## Running

```
cd build
./designer [-f path/to/file.step]
```

## Testing

```
cd build
ctest              # everything (unit + GUI)
ctest -LE gui      # unit tests only (safe on headless machines)
./designer --gui-tests   # GUI suite directly, with verbose log
```

Unit tests are Catch2 against the GUI-free `designer_core` library. GUI tests
drive the real application through
[ImGui Test Engine](https://github.com/ocornut/imgui_test_engine) (simulated
mouse/keyboard on the actual UI) and need a display.

## Remote control

Scripts and AI assistants can drive a live session over JSON-RPC
(localhost-only, opt-in):

```
./designer --remote 8437
python3 tools/designer_remote.py 8437 get_state
python3 tools/designer_remote.py 8437 add_feature \
    '{"feature": {"type": "Box", "name": "Box 1", "position": [0,0,0],
                  "width": 2, "height": 2, "depth": 2}}'
python3 tools/designer_remote.py 8437 menu_click '{"path": "Edit/Undo"}'
python3 tools/designer_remote.py 8437 screenshot '{"path": "/tmp/view.png"}'
```

Two tiers share the same underlying command layer as the GUI: semantic
methods (`get_state`, `add/modify/delete_feature` using the project-file JSON
format, `undo`/`redo`, project I/O, `import_step`, `select_face`, `fit_view`)
and human-parity UI actions executed through the embedded test engine
(`menu_click`, `item_click`, `item_input`, `viewport_click`, `viewport_drag`,
`key_press`, `scroll`), plus `screenshot`. See `tools/designer_remote.py` and
`tests/remote_smoke.py` for reference usage.

## License

Copyright © Elias Högbom Aronsson

Designer is free software licensed under the **GNU Affero General Public License,
version 3 or (at your option) any later version** (AGPL-3.0-or-later). See the
[`LICENSE`](LICENSE) file for the full text.

In short: you may use, study, modify, and redistribute it, but any distributed
or network-hosted derivative must also be released under the AGPL with its
source available. This copyleft governs the *software only* — files you author
with Designer (CAD models, drawings, exports) are yours, with no obligation to
publish them.

### Additional permission (GPL/AGPL section 7): imgui_test_engine linking

As an exception, and as an additional permission under section 7 of the AGPL,
Elias Högbom Aronsson grants permission to link or combine this program with
**Dear ImGui Test Engine** (`deps/imgui_test_engine`, © Omar Cornut / DISCO
HELLO, distributed under its own license), and to convey the resulting work.
You may extend this exception to a modified version of the program, but you are
not obligated to do so. This exception does not modify any other term of the
AGPL, and does not grant any rights in Dear ImGui Test Engine itself beyond
those in its own license.

### Third-party components

Designer links LGPL libraries (OpenCASCADE, Netgen, and the vendored, modified
PlaneGCS built as a shared library) and bundles other open-source components;
see [`THIRD_PARTY.md`](THIRD_PARTY.md) for the full inventory and their
licenses.
