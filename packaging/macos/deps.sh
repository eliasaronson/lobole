#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# What a macOS build needs (docs/packaging/README.md#macos): Homebrew's
# formulas and a python venv, then OpenCASCADE with our patches, Netgen,
# Spectra, libvterm and glslc from source into $LOBOLE_MACOS_DEPS, by
# packaging/appimage/deps.sh.
#
#   packaging/macos/deps.sh               # everything
#   packaging/macos/deps.sh --brew-only   # the formulas and the venv, for a
#                                         # prefix already built (a CI cache)
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
case "${1:-}" in
  '') brew_only=0 ;;
  --brew-only) brew_only=1 ;;
  *) echo "deps.sh: unknown argument '$1' (accepted: --brew-only)" >&2; exit 2 ;;
esac

# cmake, libtool (libvterm's Makefile runs glibtool on Darwin) and python
# (the venv's mcp needs 3.10; the system's is 3.9) were on the machine this
# was first written on and are not on every Mac.
brew install \
  cmake ninja pkgconf ccache coreutils flock clang-format libtool python \
  shaderc molten-vk vulkan-loader vulkan-headers vulkan-tools vulkan-validationlayers \
  glfw glm spdlog nlohmann-json eigen boost spectra libvterm catch2 \
  rapidjson freetype ffmpeg

# shellcheck source=../../tools/macos-env.sh
. "$repo/tools/macos-env.sh"
mkdir -p "$LOBOLE_MACOS_DEPS"

python3 -m venv "$LOBOLE_MACOS_DEPS/venv"
"$LOBOLE_MACOS_DEPS/venv/bin/pip" install --quiet mcp pillow mypy setuptools

[ "$brew_only" = 1 ] && exit 0

export CFLAGS="-ffp-contract=off -fno-strict-aliasing"
export CXXFLAGS="-ffp-contract=off -fno-strict-aliasing"
export OCCT_EXTRA_CMAKE_ARGS="-DBUILD_CPP_STANDARD=C++20"
JOBS="$("$repo/tools/build-jobs")" "$repo/packaging/appimage/deps.sh" "$LOBOLE_MACOS_DEPS"
