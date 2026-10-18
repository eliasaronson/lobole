#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# Record the demo scenes (src/app/guitests/GuiTestsDemo.cpp) as video clips:
# each scene is a GUI test in the `demo` category, run here at Cinematic
# speed in a 3840x2160 window at interface scale 2, recording itself through the test engine
# (docs/ui/testing.md#recording-a-scene). The suite runs the same scenes
# Fast and records nothing.
#
#   tools/demo/record.sh                   # every scene -> scratch/demo/
#   tools/demo/record.sh parametric cam    # just these
#   tools/demo/record.sh --out DIR ...
#   tools/demo/record.sh --draft cam       # 1920x1080 at scale 1: a quick look
#
# Its own Xvfb display and its own config and state directories: a scene
# saves projects and changes settings, and none of it may reach the
# developer's. Needs the built app (build/lobole) and ffmpeg (shell.nix).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/scratch/demo"
# 3840x2160 at an interface scale of 2: a 1920x1080 layout drawn at the
# density of a HiDPI display (Xvfb reports a scale of 1).
SIZE=3840x2160
UI_SCALE=2
SCENES=()
while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$(realpath -m "$2")"; shift 2 ;;
    --draft) SIZE=1920x1080; UI_SCALE=1; shift ;; # quick: half the pixels
    -h|--help) sed -n '5,17p' "$0"; exit 0 ;;
    -*) echo "record: unknown option $1" >&2; exit 2 ;;
    *) SCENES+=("$1"); shift ;;
  esac
done

ALL=(parametric history fem cam om615)
# om615 plays the generated OM615 (generators/om615/), opened from a COPY: the
# scene edits a part of it. Generate it with the app as built, so its mates
# name faces the way this build does (tools/demo/README.md):
#   python3 generators/om615/om615.py --port PORT --stage preview --no-export \
#       --out scratch/demo/om615-gen
OM615="${LOBOLE_DEMO_OM615_SOURCE:-$ROOT/scratch/demo/om615-gen/om615-unchecked.dsgn}"
if [ ${#SCENES[@]} -eq 0 ]; then
  SCENES=("${ALL[@]}")
  # Without the generated model, the rest still record.
  [ -d "$OM615" ] || SCENES=("${SCENES[@]/om615}")
  read -r -a SCENES <<< "${SCENES[*]}"
fi
for s in "${SCENES[@]}"; do
  case " ${ALL[*]} " in
    *" $s "*) ;;
    *) echo "record: no scene '$s'; the scenes are: ${ALL[*]}" >&2; exit 2 ;;
  esac
done
SEQUENCE="$(printf 'demo/%s,' "${SCENES[@]}")"
SEQUENCE="${SEQUENCE%,}"

[ -x "$ROOT/build/lobole" ] || { echo "record: build/lobole is not built (tools/build.sh)" >&2; exit 1; }
if command -v xvfb-run >/dev/null; then
  XVFB_RUN=xvfb-run
else
  XVFB_RUN="$(nix-shell -p xvfb-run --run 'command -v xvfb-run' | tail -1)"
fi

mkdir -p "$OUT"
for s in "${SCENES[@]}"; do rm -f "$OUT/$s.mp4"; done
STATE="$(mktemp -d)"
trap 'rm -rf "$STATE"' EXIT
DEMO_ENV=""
case " ${SCENES[*]} " in
  *" om615 "*)
    [ -d "$OM615" ] || { echo "record: no OM615 project at $OM615 (generators/om615/om615.py builds it)" >&2; exit 1; }
    cp -r "$OM615" "$STATE/om615.dsgn"
    DEMO_ENV="LOBOLE_DEMO_OM615='$STATE/om615.dsgn'" ;;
esac
LOG="$OUT/record.log"

echo "record: ${SCENES[*]} at $SIZE -> $OUT"
"$XVFB_RUN" -a -s "-screen 0 ${SIZE}x24" \
  "$ROOT/tools/nix-run" \
  "cd '$ROOT/build' && $DEMO_ENV XDG_STATE_HOME='$STATE' XDG_CONFIG_HOME='$STATE/config' \
   ./lobole --gui-tests --hidden --window-size $SIZE --ui-scale $UI_SCALE \
   --gui-test-speed cinematic --capture-dir '$OUT' \
   --gui-test-sequence '$SEQUENCE'" > "$LOG" 2>&1 || true

# The app's exit status does not say whether a scene failed; its verdict does.
verdict="$(grep -o 'GUI tests: [0-9]*/[0-9]* passed' "$LOG" | tail -1 || true)"
echo "record: ${verdict:-no verdict} (log: $LOG)"
missing=0
for s in "${SCENES[@]}"; do
  if [ -s "$OUT/$s.mp4" ]; then
    echo "  $OUT/$s.mp4"
  else
    echo "  $s: NO CLIP" >&2
    missing=1
  fi
done
case "$verdict" in
  "GUI tests: ${#SCENES[@]}/${#SCENES[@]} passed") exit $missing ;;
  *) grep -E 'Error |test failed' "$LOG" | head -20 >&2; exit 1 ;;
esac
