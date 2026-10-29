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
#     sketch, finds the ccx it carries and solves a study on it (--calculix);
#   - a GUI test (app/boots_and_renders) in a window on Xvfb: the host's
#     Vulkan driver loads into the bundled app and renders, which is what
#     the nix bundle could not do;
#   - with no Vulkan driver at all, exit 3 and last-startup-failure.txt,
#     with and without a way to show a box.
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
  --calculix -- "$appimage"

Xvfb :99 -screen 0 1600x1000x24 &
xvfb=$!
trap 'kill $xvfb' EXIT
for _ in $(seq 50); do [ -e /tmp/.X11-unix/X99 ] && break; sleep 0.2; done
DISPLAY=:99 "$appimage" --gui-tests --gui-test-filter boots_and_renders
echo "appimage-smoke: the window opened and rendered"

# No usable renderer is exit 3 and a failure file, never a crash or a hang
# (docs/packaging/README.md#when-it-will-not-start). Twice: once with the
# box suppressed, once without LOBOLE_NO_DIALOG, zenity, kdialog or a
# session bus (this image has none of them), where the chain must end at
# the text file.
state="$(mktemp -d)"
no_renderer() {
  local code=0
  env "$@" XDG_STATE_HOME="$state" VK_ICD_FILENAMES=/nonexistent \
    VK_DRIVER_FILES=/nonexistent DISPLAY=:99 \
    timeout 120 "$appimage" --hidden --autosave 0 > "$state/out.txt" 2>&1 || code=$?
  if [ "$code" != 3 ] || [ ! -s "$state/lobole/last-startup-failure.txt" ]; then
    echo "appimage-smoke: with no Vulkan driver, expected exit 3 and the" \
      "failure file; got exit $code:" >&2
    cat "$state/out.txt" >&2
    exit 1
  fi
  rm -f "$state/lobole/last-startup-failure.txt"
}
no_renderer LOBOLE_NO_DIALOG=1
no_renderer -u LOBOLE_NO_DIALOG DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent
echo "appimage-smoke: no driver says why (exit 3, last-startup-failure.txt)"
