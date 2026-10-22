#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# Smoke the portable zip under Wine, from a folder named the way the
# installer names it (docs/packaging/README.md#windows):
#
#   packaging/windows/smoke.sh ZIP VERSION
#
# Needs wine, Xvfb, python3 and Mesa's lavapipe on the host (the release job
# runs it in a `nix shell`; $LAVAPIPE_ICD names lavapipe's ICD file):
#   - tools/release-smoke.py: it starts, models with OpenCASCADE, solves a
#     sketch;
#   - a GUI test (app/boots_and_renders) in a window on Xvfb, through
#     winevulkan to lavapipe. Headless never loads the fonts, so it passed
#     from C:\Program Files\Loböle while the window died there on the first
#     non-ASCII path; the folder below is what catches that again.
set -euo pipefail

zip="$(realpath "$1")"
version="$2"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
dir="$(mktemp -d)/Loböle"
mkdir -p "$dir"
(cd "$dir" && unzip -q "$zip")
exe="$dir/app/bin/lobole.exe"
export WINEDEBUG=-all WINEDLLOVERRIDES=winedbg.exe=d

python3 "$repo/tools/release-smoke.py" --port 8437 --version "$version" \
  -- wine "$exe"

Xvfb :99 -screen 0 1600x1000x24 &
xvfb=$!
trap 'kill $xvfb' EXIT
for _ in $(seq 50); do [ -e /tmp/.X11-unix/X99 ] && break; sleep 0.2; done
DISPLAY=:99 VK_ICD_FILENAMES="$LAVAPIPE_ICD" \
  wine "$exe" --gui-tests --gui-test-filter boots_and_renders
echo "windows-smoke: the window opened and rendered from $dir"
