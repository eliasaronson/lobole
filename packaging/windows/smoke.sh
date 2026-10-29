#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# Smoke the portable zip under Wine, from a folder named the way the
# installer names it (docs/packaging/README.md#windows):
#
#   packaging/windows/smoke.sh ZIP VERSION
#
# Needs wine, Xvfb, python3 and Mesa's lavapipe on the host (the release job
# runs it in a `nix shell`, with Nix's Mesa at /run/opengl-driver as on
# NixOS, which Nix's Xvfb and Wine need for GLX and Vulkan; $LAVAPIPE_ICD
# names lavapipe's ICD file):
#   - tools/release-smoke.py: it starts, models with OpenCASCADE, solves a
#     sketch, and finds the ccx.exe beside lobole.exe, which then solves a
#     deck under Wine (--calculix; this build has no Netgen to mesh a study);
#   - tools/release-smoke.py --system-faults: a fault the system catches
#     itself leaves the app running, and one it does not, on a worker thread,
#     leaves a crash report;
#   - a GUI test (app/boots_and_renders) in a window on Xvfb, through
#     winevulkan to lavapipe. Headless never loads the fonts, so it passed
#     from C:\Program Files\Loböle while the window died there on the first
#     non-ASCII path; the folder below is what catches that again;
#   - with no Vulkan driver, exit 3 and last-startup-failure.txt under
#     %LOCALAPPDATA%\Lobole\state.
set -euo pipefail

zip="$(realpath "$1")"
version="$2"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
dir="$(mktemp -d)/Loböle"
mkdir -p "$dir"
(cd "$dir" && unzip -q "$zip")
exe="$dir/app/bin/lobole.exe"
# winedbg off, so a crash fails the smoke instead of waiting in Wine's
# debugger; mscoree and mshtml off, so the new prefix does not offer to
# install Wine Mono and Gecko in a dialog nobody will click (a cached
# installer hides that on a developer's machine). The app needs neither.
export WINEDEBUG=-all WINEDLLOVERRIDES="winedbg.exe=d;mscoree=;mshtml="

# The display comes first: the first wine command starts the wineserver and
# its desktop process, which keep the display they were started with, so a
# headless run without one left the window below unable to open
# ("Failed to create helper window").
Xvfb :99 -screen 0 1600x1000x24 &
xvfb=$!
trap 'kill $xvfb' EXIT
for _ in $(seq 50); do [ -e /tmp/.X11-unix/X99 ] && break; sleep 0.2; done
export DISPLAY=:99
unset WAYLAND_DISPLAY
wineboot --init
wineserver --wait

python3 "$repo/tools/release-smoke.py" --port 8437 --version "$version" \
  --calculix -- wine "$exe"
python3 "$repo/tools/release-smoke.py" --port 8437 --system-faults \
  -- wine "$exe"

[ -f "$LAVAPIPE_ICD" ] || { echo "LAVAPIPE_ICD is not a file: '$LAVAPIPE_ICD'" >&2; exit 1; }
export VK_ICD_FILENAMES="$LAVAPIPE_ICD"
if ! wine "$exe" --gui-tests --gui-test-filter boots_and_renders; then
  # Say why before failing: which Vulkan drivers the Windows loader (the
  # zip's vulkan-1.dll) was given, and what it and winevulkan made of them.
  echo "windows-smoke: the window test failed; Vulkan in Wine, for when it is that:" >&2
  wine reg query 'HKLM\Software\Khronos\Vulkan\Drivers' >&2 || true
  VK_LOADER_DEBUG=error,warn,driver WINEDEBUG=err+all,warn+vulkan \
    wine "$exe" --gui-tests --gui-test-filter boots_and_renders 2>&1 \
    | grep -v -E '^ ?>|xkbcomp' | tail -60 >&2 || true
  exit 1
fi
echo "windows-smoke: the window opened and rendered from $dir"

# No usable renderer is exit 3 and a failure file, not a crash or a hang
# (docs/packaging/README.md#when-it-will-not-start). The loader is the zip's
# vulkan-1.dll, given no driver at all; LOBOLE_NO_DIALOG=1 keeps the
# MessageBoxW nobody would click off the virtual display.
code=0
VK_ICD_FILENAMES=/nonexistent VK_DRIVER_FILES=/nonexistent LOBOLE_NO_DIALOG=1 \
  timeout 180 wine "$exe" --hidden --autosave 0 || code=$?
failure="$(find "${WINEPREFIX:-$HOME/.wine}/drive_c/users" -path '*Lobole/state/last-startup-failure.txt' 2>/dev/null | head -1)"
if [ "$code" != 3 ] || [ -z "$failure" ]; then
  echo "windows-smoke: with no Vulkan driver, expected exit 3 and" \
    "%LOCALAPPDATA%\\Lobole\\state\\last-startup-failure.txt; got exit $code" >&2
  exit 1
fi
echo "windows-smoke: no driver says why (exit 3, $failure)"
