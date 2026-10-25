#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# Build what the GUI AppImage needs and Ubuntu 24.04 lacks, into PREFIX:
# OpenCASCADE 7.9 (24.04 has 7.6) and Netgen, which must link that OCC
# (docs/packaging/README.md#the-gui-appimage). Flags as in the Flatpak
# manifest, the OCC patches as in nix/occt-overlay.nix.
#
#   packaging/appimage/deps.sh /opt/lobole-deps
#
# Runs in the ubuntu:24.04 build image, after build.sh's apt list. Each
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
         occt-offset-arc-bind-order"
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
