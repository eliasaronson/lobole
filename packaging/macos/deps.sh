#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

brew install \
  ninja pkgconf ccache coreutils flock clang-format \
  shaderc molten-vk vulkan-loader vulkan-headers vulkan-tools vulkan-validationlayers \
  glfw glm spdlog nlohmann-json eigen boost spectra libvterm catch2 \
  rapidjson freetype ffmpeg

# shellcheck source=../../tools/macos-env.sh
. "$repo/tools/macos-env.sh"
mkdir -p "$LOBOLE_MACOS_DEPS"

python3 -m venv "$LOBOLE_MACOS_DEPS/venv"
"$LOBOLE_MACOS_DEPS/venv/bin/pip" install --quiet mcp pillow mypy setuptools

export CFLAGS="-ffp-contract=off -fno-strict-aliasing"
export CXXFLAGS="-ffp-contract=off -fno-strict-aliasing"
export OCCT_EXTRA_CMAKE_ARGS="-DBUILD_CPP_STANDARD=C++20"
JOBS="$("$repo/tools/build-jobs")" "$repo/packaging/appimage/deps.sh" "$LOBOLE_MACOS_DEPS"
