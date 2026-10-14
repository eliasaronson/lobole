# Dev shell for NixOS / nix users. Enter with `nix-shell`, then the usual
# `cmake -B build && cmake --build build`.
#
# Note: only build-time deps live here. The Vulkan ICD (GPU driver) comes from
# the system configuration (`hardware.graphics.enable = true` plus your GPU's
# driver module) — without it the app builds but finds no Vulkan device.
# The OpenCASCADE overlay carries our kernel patches (nix/occt-overlay.nix).
{ pkgs ? import <nixpkgs> { overlays = [ (import ./nix/occt-overlay.nix) ]; } }:

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
    # compares pixels and would otherwise skip itself into uselessness;
    # setuptools for packaging_smoke, which builds the lobole wheel.
    (python3.withPackages (ps: [ ps.mcp ps.pillow ps.mypy ps.setuptools ]))
    git # submodules
    # export_motion_video encodes mp4/webm/gif by running ffmpeg (an external
    # program, never linked); here so the smoke always has it. The headless
    # build carries libx264, libvpx and the gif encoder without X or SDL.
    ffmpeg-headless

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
    libvterm-neovim # VT emulation for the terminal panel (docs/ui/terminal.md)
    catch2_3 # unit tests (FetchContent fallback needs network otherwise)
  ];

  # Let the Vulkan loader find the validation layers from this shell.
  VK_LAYER_PATH = "${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d";

  # The lobole Python package (python/lobole/), importable from anywhere in
  # this shell: the tests, generators and benchmarks import it without
  # editing sys.path. `toString` keeps it the checkout's own path (a
  # worktree's, in a worktree) rather than a copy in the store.
  shellHook = ''
    export PYTHONPATH="${toString ./python}''${PYTHONPATH:+:$PYTHONPATH}"
  '';
}
