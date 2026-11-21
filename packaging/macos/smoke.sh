#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# Install the release's .dmg the way a user does and start what it holds
# (docs/packaging/README.md#the-macos-app):
#
#   packaging/macos/smoke.sh lobole-<v>-macos-arm64.dmg <v>           # the app
#   packaging/macos/smoke.sh lobole-<v>-macos-arm64.dmg <v> --window  # + a frame
#
# The app is copied out of the mounted image, then:
#   - its signature verifies, and every Mach-O in it loads only the system's
#     libraries and the bundle's own (packaging/macos/bundle.py --verify);
#   - started, the dynamic linker loads nothing from Homebrew or the build's
#     prefix (DYLD_PRINT_LIBRARIES; a hardened-runtime signature ignores
#     DYLD_ variables, so a Developer ID build relies on the check above);
#   - tools/release-smoke.py, in an environment with none of the build's
#     variables: it starts headless, models with OpenCASCADE and solves a
#     sketch;
#   - --window: where Homebrew's MoltenVK finds a device on this machine, a
#     GUI test (app/boots_and_renders) opens a window and renders on the
#     BUNDLED MoltenVK, found through the bundle's own ICD manifest. Where
#     it finds none, the log says so and nothing is claimed.
set -euo pipefail
dmg="$1"
version="$2"
window="${3:-}"
case "$window" in '' | --window) ;; *) echo "smoke: unknown argument '$window' (accepted: --window)" >&2; exit 2 ;; esac
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# The build's environment, for this script's own tools (the venv's python,
# GNU timeout); the app runs without it.
# shellcheck source=../../tools/macos-env.sh
. "$repo/tools/macos-env.sh"
python="$(command -v python3)"
brew="${HOMEBREW_PREFIX:-/opt/homebrew}"
tmp="$(mktemp -d)"
mnt="$tmp/mnt"
trap 'hdiutil detach -quiet "$mnt" 2>/dev/null || true; rm -rf "$tmp"' EXIT

hdiutil attach -nobrowse -readonly -mountpoint "$mnt" "$dmg" >/dev/null
[ -L "$mnt/Applications" ] || { echo "smoke: the image has no Applications link" >&2; exit 1; }
mkdir -p "$tmp/Applications"
ditto "$mnt/Lobole.app" "$tmp/Applications/Lobole.app"
hdiutil detach -quiet "$mnt"
app="$tmp/Applications/Lobole.app"
exe="$app/Contents/MacOS/lobole"

codesign --verify --strict --verbose=2 "$app"
"$python" "$repo/packaging/macos/bundle.py" --verify "$app"

# What a Finder launch has: no Homebrew, no build prefix, no Vulkan variables.
clean=(env -i HOME="$HOME" TMPDIR="${TMPDIR:-/tmp}" PATH=/usr/bin:/bin:/usr/sbin:/sbin
       LOBOLE_NO_DIALOG=1 LOBOLE_SMOKE_LOG_DIR="${LOBOLE_SMOKE_LOG_DIR:-}")

if codesign -dv "$app" 2>&1 | grep -q 'flags=.*runtime'; then
  echo "smoke: hardened runtime; DYLD_PRINT_LIBRARIES is ignored, the load check is bundle.py's"
else
  loaded="$("${clean[@]}" DYLD_PRINT_LIBRARIES=1 "$exe" --version 2>&1 >/dev/null || true)"
  outside="$(echo "$loaded" | grep -E "$brew/|${LOBOLE_MACOS_DEPS:-/nonexistent}/" || true)"
  if [ -n "$outside" ]; then
    echo "smoke: the app loaded libraries from outside the bundle:" >&2
    echo "$outside" >&2
    exit 1
  fi
  echo "smoke: $(echo "$loaded" | grep -c 'dyld') images loaded, none from $brew"
fi

"${clean[@]}" "$python" "$repo/tools/release-smoke.py" --port 8437 \
  --version "$version" -- "$exe"

[ "$window" = --window ] || exit 0
device="$(VK_DRIVER_FILES="$brew/etc/vulkan/icd.d/MoltenVK_icd.json" \
  timeout 60 vulkaninfo --summary 2>/dev/null |
  awk -F= '/deviceName/ {sub(/^ +/, "", $2); print $2; exit}' || true)"
if [ -z "$device" ]; then
  echo "smoke: MoltenVK finds no Vulkan device on this machine; the window was not opened"
  exit 0
fi
echo "smoke: Vulkan device: $device"
"${clean[@]}" "$exe" --gui-tests --hidden --gui-test-filter boots_and_renders \
  | tee "$tmp/gui.log"
# The app exits 0 whether or not a case failed; its summary is the verdict.
line="$(grep -o 'GUI tests: [0-9]*/[0-9]* passed' "$tmp/gui.log" | tail -1 || true)"
ok="${line#GUI tests: }"; ok="${ok%%/*}"
total="${line#*/}"; total="${total%% passed}"
if [ -z "$line" ] || [ "$ok" != "$total" ] || [ "$total" -lt 1 ]; then
  echo "smoke: app/boots_and_renders did not pass on the bundled MoltenVK" >&2
  exit 1
fi
echo "smoke: the window opened and rendered on the bundled MoltenVK"
