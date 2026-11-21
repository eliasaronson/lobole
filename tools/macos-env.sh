# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

lobole_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
lobole_brew="${HOMEBREW_PREFIX:-/opt/homebrew}"

export LOBOLE_MACOS_DEPS="${LOBOLE_MACOS_DEPS:-$HOME/.local/share/lobole/macos-deps}"
export PATH="$LOBOLE_MACOS_DEPS/venv/bin:$LOBOLE_MACOS_DEPS/bin:$lobole_brew/opt/coreutils/libexec/gnubin:$lobole_brew/bin:$PATH"
export CMAKE_PREFIX_PATH="$LOBOLE_MACOS_DEPS:$LOBOLE_MACOS_DEPS/Contents/Resources:$lobole_brew"
export SDKROOT="$(xcrun --sdk macosx --show-sdk-path 2>/dev/null)"
export MACOSX_DEPLOYMENT_TARGET=26.0
export VK_DRIVER_FILES="$lobole_brew/etc/vulkan/icd.d/MoltenVK_icd.json"
export VK_LAYER_PATH="$lobole_brew/share/vulkan/explicit_layer.d"
export PYTHONPATH="$lobole_root/python${PYTHONPATH:+:$PYTHONPATH}"

unset lobole_root lobole_brew
