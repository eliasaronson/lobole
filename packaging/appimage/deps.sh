#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# Build what the GUI AppImage needs and Ubuntu 22.04 lacks, into PREFIX:
# OpenCASCADE 7.9 (22.04 has 7.5) and Netgen, which must link that OCC,
# Spectra, libvterm 0.3 (22.04 has 0.1) and glslc
# (docs/packaging/README.md#the-gui-appimage). Flags as in the Flatpak
# manifest, the OCC patches as in nix/occt-overlay.nix.
#
#   packaging/appimage/deps.sh /opt/lobole-deps
#
# Runs in the ubuntu:22.04 build image, after build.sh's apt list. Each
# checkout is checked against its commit, so a moved tag fails here.
set -euo pipefail

prefix="$1"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
jobs="${JOBS:-$(nproc)}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

fetch() {  # url tag commit dir
  git -c advice.detachedHead=false clone -q --depth 1 --branch "$2" "$1" "$4"
  local got
  got="$(git -C "$4" rev-parse HEAD)"
  [ "$got" = "$3" ] || { echo "$1 $2 is $got, not $3" >&2; exit 1; }
}

fetch https://github.com/Open-Cascade-SAS/OCCT.git V7_9_3 \
  a016080bf6738d6aeae020badee4e888ad1540a5 "$work/occt"
patches="occt-shell-cap-order occt-fillet-reorder-faces-loop
         occt-draft-null-curve-and-parallel
         occt-topopebrep-unclosed-wire-iterator
         occt-offset-arc-bind-order occt-offset-inter3d-progress
         occt-error-handler-thread-local
         occt-inttools-fclass2d-leak"
for patch in $patches; do
  git -C "$work/occt" apply "$repo/nix/patches/$patch.patch"
done
# Exceptions stay on: OCC turns them off in Release, which breaks the
# error paths a refused feature takes.
cmake -S "$work/occt" -B "$work/occt-build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DINSTALL_DIR_LAYOUT=Unix -DBUILD_LIBRARY_TYPE=Shared \
  -DBUILD_MODULE_Draw=OFF -DBUILD_DOC_Overview=OFF \
  -DUSE_VTK=OFF -DUSE_FREEIMAGE=OFF -DUSE_TBB=OFF \
  -DUSE_RAPIDJSON=ON -DUSE_FREETYPE=ON \
  -DBUILD_RELEASE_DISABLE_EXCEPTIONS=FALSE
cmake --build "$work/occt-build" -j"$jobs"
cmake --install "$work/occt-build"
# The patches it carries, as the nix overlay lists them.
mkdir -p "$prefix/share/lobole"
for patch in $patches; do echo "$patch.patch"; done \
  > "$prefix/share/lobole/occt-patches"

fetch https://github.com/NGSolve/netgen.git v6.2.2605 \
  6070897b4a3df45244cb8230c2e8354a4d49c1ab "$work/netgen"
cmake -S "$work/netgen" -B "$work/netgen-build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DCMAKE_PREFIX_PATH="$prefix" \
  -DUSE_OCC=ON -DUSE_GUI=OFF -DUSE_PYTHON=OFF -DUSE_MPI=OFF \
  -DUSE_SUPERBUILD=OFF -DUSE_NATIVE_ARCH=OFF -DBUILD_STUB_FILES=OFF
cmake --build "$work/netgen-build" -j"$jobs"
cmake --install "$work/netgen-build"

# Header-only; CMake finds its package file.
fetch https://github.com/yixuan/spectra.git v1.0.1 \
  bdd707b9a872bb17622696ed3cca2a27b743a5b7 "$work/spectra"
cmake -S "$work/spectra" -B "$work/spectra-build" -G Ninja \
  -DCMAKE_INSTALL_PREFIX="$prefix"
cmake --install "$work/spectra-build"

# glslc, the shader compiler, which only the build runs. Its own
# dependencies come at the commits its DEPS file names.
fetch https://github.com/google/shaderc.git v2026.4 \
  b64fd42c4a2a9d501c47aca5d3571f7673a5e9cc "$work/shaderc"
python3 "$work/shaderc/utils/git-sync-deps"
cmake -S "$work/shaderc" -B "$work/shaderc-build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON \
  -DSPIRV_SKIP_TESTS=ON -DENABLE_GLSLANG_BINARIES=OFF
cmake --build "$work/shaderc-build" -j"$jobs" --target glslc_exe
install -m755 "$work/shaderc-build/glslc/glslc" "$prefix/bin/glslc"

# The terminal panel's emulator, from neovim's mirror of the upstream tree.
fetch https://github.com/neovim/libvterm.git v0.3.3 \
  9d6d2112335080312ef8c36667fa717ded4f7daf "$work/libvterm"
make -C "$work/libvterm" -j"$jobs" PREFIX="$prefix" install
