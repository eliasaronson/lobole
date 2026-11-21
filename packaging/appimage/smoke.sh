#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# Run the GUI AppImage the way a user's machine would, in a fresh image
# that has never seen the build (docs/packaging/README.md#the-gui-appimage):
#
#   packaging/appimage/smoke.sh APPIMAGE VERSION [--no-vulkan]
#
# Installs only what any desktop already has (python3 for the smoke itself,
# the Vulkan loader with Mesa's lavapipe, X11, fontconfig, D-Bus) and Xvfb,
# with whichever package manager the image has, then:
#   - tools/release-smoke.py: it starts, models with OpenCASCADE, solves a
#     sketch, finds the ccx it carries and solves a study on it (--calculix);
#   - a GUI test (app/boots_and_renders) in a window on Xvfb: the host's
#     Vulkan driver loads into the bundled app and renders, which is what
#     the nix bundle could not do;
#   - with VK_ICD_FILENAMES=/nonexistent, the start restarts itself on the
#     host's lavapipe (docs/rendering/README.md#the-device-ladder): ping says
#     so, and app/boots_and_renders passes through the restart;
#   - with no Vulkan driver at all and LOBOLE_SOFTWARE_DRIVER=none, exit 3
#     and last-startup-failure.txt, with and without a way to show a box.
#
# --no-vulkan is the other host: X11, fontconfig, D-Bus and python, and
# neither the Vulkan loader nor any driver (a VM, the AppImage catalog's
# test machine). The bundled loader is what runs: release-smoke.py passes
# over --headless, and a window is exit 3 with the report naming the driver
# packages, with and without a way to show a box. It fails if a loader or a
# driver manifest is in the image, so it cannot pass as the first lane.
# Containers have no FUSE, so the AppImage runs extracted.
set -euo pipefail

appimage="$(realpath "$1")"
version="$2"
lane=mesa
[ "${3:-}" = --no-vulkan ] && lane=none
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if command -v apt-get > /dev/null; then
  vulkan="libvulkan1 mesa-vulkan-drivers"
  [ "$lane" = none ] && vulkan=
  apt-get update
  # shellcheck disable=SC2086 # $vulkan is a list, or nothing
  DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    python3 $vulkan xvfb libx11-6 libx11-xcb1 \
    libxcursor1 libxrandr2 libxinerama1 libxi6 libfontconfig1 libdbus-1-3
elif command -v dnf > /dev/null; then
  vulkan="vulkan-loader mesa-vulkan-drivers"
  [ "$lane" = none ] && vulkan=
  # shellcheck disable=SC2086
  dnf install -y python3 $vulkan \
    xorg-x11-server-Xvfb libX11 libX11-xcb libXcursor libXrandr libXinerama \
    libXi fontconfig dbus-libs
else
  echo "no apt-get or dnf in this image" >&2
  exit 1
fi

if [ "$lane" = none ]; then
  # What this lane is for: no loader and no driver anywhere the loader or
  # the dynamic linker would look.
  found="$( (ldconfig -p 2> /dev/null | grep 'libvulkan\.so\.1 ') || true)"
  for d in /usr/share/vulkan/icd.d /etc/vulkan/icd.d /usr/local/share/vulkan/icd.d; do
    [ -d "$d" ] && found="$found $(ls "$d")"
  done
  if [ -n "${found// /}" ]; then
    echo "appimage-smoke: --no-vulkan, yet the image has Vulkan: $found" >&2
    exit 1
  fi
fi

# The smoke drives the app with the SDK, which needs Python 3.11; 22.04's
# python3 is 3.10. The app itself needs no Python.
python=python3
if ! python3 -c 'import sys; sys.exit(sys.version_info < (3, 11))'; then
  DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    python3.11
  python=python3.11
fi

export APPIMAGE_EXTRACT_AND_RUN=1
"$python" "$repo/tools/release-smoke.py" --port 8437 --version "$version" \
  --calculix -- "$appimage"

Xvfb :99 -screen 0 1600x1000x24 &
xvfb=$!
trap 'kill $xvfb' EXIT
for _ in $(seq 50); do [ -e /tmp/.X11-unix/X99 ] && break; sleep 0.2; done
if [ "$lane" = mesa ]; then
  DISPLAY=:99 "$appimage" --gui-tests --gui-test-filter boots_and_renders
  echo "appimage-smoke: the window opened and rendered"

  # A driver variable pointing nowhere (a stale VK_ICD_FILENAMES): the start
  # cannot get a hardware renderer and restarts itself on lavapipe, which
  # draws the window (docs/rendering/README.md#the-device-ladder).
  DISPLAY=:99 LOBOLE_NO_DIALOG=1 VK_ICD_FILENAMES=/nonexistent \
    VK_DRIVER_FILES=/nonexistent "$python" "$repo/tools/release-smoke.py" \
    --software-restart --port 8438 -- "$appimage"
  DISPLAY=:99 VK_ICD_FILENAMES=/nonexistent VK_DRIVER_FILES=/nonexistent \
    "$appimage" --gui-tests --gui-test-filter boots_and_renders
  echo "appimage-smoke: with no hardware driver it restarted on lavapipe"
fi

# No usable renderer is exit 3 and a failure file, never a crash or a hang
# (docs/packaging/README.md#when-it-will-not-start). Twice: once with the
# box suppressed, once without LOBOLE_NO_DIALOG, zenity, kdialog or a
# session bus (this image has none of them), where the chain must end at
# the text file.
# The first lane has lavapipe, so the restart is turned off for this;
# the second has no driver to restart on.
state="$(mktemp -d)"
no_renderer() {
  local code=0
  env "$@" XDG_STATE_HOME="$state" VK_ICD_FILENAMES=/nonexistent \
    VK_DRIVER_FILES=/nonexistent LOBOLE_SOFTWARE_DRIVER=none DISPLAY=:99 \
    timeout 120 "$appimage" --hidden --autosave 0 > "$state/out.txt" 2>&1 || code=$?
  if [ "$code" != 3 ] || [ ! -s "$state/lobole/last-startup-failure.txt" ]; then
    echo "appimage-smoke: with no Vulkan driver, expected exit 3 and the" \
      "failure file; got exit $code:" >&2
    cat "$state/out.txt" >&2
    exit 1
  fi
  if [ "$lane" = none ] &&
     ! grep -q 'libvulkan1 mesa-vulkan-drivers' "$state/lobole/last-startup-failure.txt"; then
    echo "appimage-smoke: the report does not name the driver packages:" >&2
    cat "$state/lobole/last-startup-failure.txt" >&2
    exit 1
  fi
  rm -f "$state/lobole/last-startup-failure.txt"
}
no_renderer LOBOLE_NO_DIALOG=1
no_renderer -u LOBOLE_NO_DIALOG DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent
echo "appimage-smoke: no driver says why (exit 3, last-startup-failure.txt)"
