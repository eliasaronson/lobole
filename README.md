# Designer

Project to build an open-source parametric 3D CAD application using Vulkan, ImGui and
OpenCASCADE — working similarly to professionally used CAD software (Onshape, SolidWorks),
with transformation checkpoints, global variables, a Python REPL/scripting API, git-based
versioning, and an interface designed so that LLM assistants can use the same commands as
human users.

See [ROADMAP.md](ROADMAP.md) for the current state and the long-term plan.

## Requirements

System packages (Ubuntu 24.04):

```
sudo apt install cmake build-essential libvulkan-dev glslc libglfw3-dev libglm-dev \
    libspdlog-dev libgtk-3-dev \
    libocct-data-exchange-dev libocct-modeling-algorithms-dev libocct-modeling-data-dev \
    libocct-foundation-dev libocct-ocaf-dev libocct-visualization-dev
```

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
