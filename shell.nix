# Dev shell for NixOS / nix users. Enter with `nix-shell`, then the usual
# `cmake -B build && cmake --build build`.
#
# Note: only build-time deps live here. The Vulkan ICD (GPU driver) comes from
# the system configuration (`hardware.graphics.enable = true` plus your GPU's
# driver module) — without it the app builds but finds no Vulkan device.
{ pkgs ? import <nixpkgs> { } }:

pkgs.mkShell {
  packages = with pkgs; [
    # Toolchain
    cmake
    ninja # optional generator; plain make works too
    gcc
    pkg-config
    ccache # auto-detected by CMakeLists, no-op if absent
    python3 # tools/designer_remote.py + remote_smoke ctest
    git # submodules

    # Vulkan
    vulkan-headers
    vulkan-loader
    vulkan-validation-layers
    shaderc # glslc (shader compiler; hard requirement)
    shaderc.bin

    # Libraries
    glfw
    glm
    spdlog
    nlohmann_json
    opencascade-occt # CAD kernel, found via find_package(OpenCASCADE)
    eigen # PlaneGCS constraint solver
    boost # PlaneGCS (headers only)
    gtk3 # nativefiledialog
    libx11 # non-blocking XMapWindow path (see main.cpp)
    catch2_3 # unit tests (FetchContent fallback needs network otherwise)
  ];

  # Let the Vulkan loader find the validation layers from this shell.
  VK_LAYER_PATH = "${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d";
}
