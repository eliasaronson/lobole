#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# Run the GUI AppImage the way a user's machine would, in a fresh image
# that has never seen the build (docs/packaging/README.md#the-gui-appimage):
#
#   packaging/appimage/smoke.sh APPIMAGE VERSION
#
# Installs only what any desktop already has (python3 for the smoke itself,
# the Vulkan loader with Mesa's lavapipe, X11, fontconfig, D-Bus) and Xvfb,
# with whichever package manager the image has, then:
#   - tools/release-smoke.py: it starts, models with OpenCASCADE, solves a
#     sketch;
#   - a GUI test (app/boots_and_renders) in a window on Xvfb: the host's
#     Vulkan driver loads into the bundled app and renders, which is what
#     the nix bundle could not do.
# Containers have no FUSE, so the AppImage runs extracted.
set -euo pipefail

appimage="$(realpath "$1")"
version="$2"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if command -v apt-get > /dev/null; then
  apt-get update
  DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    python3 libvulkan1 mesa-vulkan-drivers xvfb libx11-6 libx11-xcb1 \
    libxcursor1 libxrandr2 libxinerama1 libxi6 libfontconfig1 libdbus-1-3
elif command -v dnf > /dev/null; then
  dnf install -y python3 vulkan-loader mesa-vulkan-drivers \
    xorg-x11-server-Xvfb libX11 libX11-xcb libXcursor libXrandr libXinerama \
    libXi fontconfig dbus-libs
else
  echo "no apt-get or dnf in this image" >&2
  exit 1
fi

export APPIMAGE_EXTRACT_AND_RUN=1
python3 "$repo/tools/release-smoke.py" --port 8437 --version "$version" \
  -- "$appimage"

Xvfb :99 -screen 0 1600x1000x24 &
xvfb=$!
trap 'kill $xvfb' EXIT
for _ in $(seq 50); do [ -e /tmp/.X11-unix/X99 ] && break; sleep 0.2; done
DISPLAY=:99 "$appimage" --gui-tests --gui-test-filter boots_and_renders
echo "appimage-smoke: the window opened and rendered"
