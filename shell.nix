# Dev shell for NixOS / nix users. Enter with `nix-shell`, then the usual
# `cmake -B build && cmake --build build`.
#
# Note: only build-time deps live here. The Vulkan ICD (GPU driver) comes from
# the system configuration (`hardware.graphics.enable = true` plus your GPU's
# driver module) — without it the app builds but finds no Vulkan device.
{ pkgs ? import <nixpkgs> { } }:

let
  # The same trimmed netgen the flake builds against, so a dev build and a
  # packaged build cannot diverge (nix/netgen-lean.nix explains the trim).
  netgenLean = import ./nix/netgen-lean.nix pkgs;
in
pkgs.mkShell {
  packages = with pkgs; [
    # Toolchain
    cmake
    ninja # optional generator; plain make works too
    gcc
    pkg-config
    ccache # auto-detected by CMakeLists, no-op if absent
    # tools/*.py + the remote/mcp ctests; pillow for render_ao_smoke, which
    # compares pixels and would otherwise skip itself into uselessness.
    (python3.withPackages (ps: [ ps.mcp ps.pillow ps.mypy ]))
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
    eigen # PlaneGCS constraint solver + FEM solver
    boost # PlaneGCS (headers only)
    netgenLean # FEM tet mesher (nglib + OCC interface, LGPL); HAVE_NETGEN
    spectra # sparse eigensolver for FEM modal analysis (header-only); HAVE_SPECTRA
    dbus # nativefiledialog-extended portal backend (xdg-desktop-portal)
    systemd # libsystemd.pc — dbus-1.pc's Requires.private needs it resolvable
    libx11 # non-blocking XMapWindow path (see main.cpp)
    libvterm-neovim # VT emulation for the terminal panel (docs/ui/terminal-panel.md)
    catch2_3 # unit tests (FetchContent fallback needs network otherwise)
  ];

  # Let the Vulkan loader find the validation layers from this shell.
  VK_LAYER_PATH = "${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d";
}
