#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Joins the recorded demo scenes into the release video: the scenes in order,
# a short crossfade between each, a caption over the start of each, and an
# end card with the lockup and the repository's address. Writes two files:
#
#   lobole-demo.mp4        the master, at the clips' own size (4K from record.sh)
#   lobole-demo-1080p.mp4  1920x1080 and under the size GitHub accepts for a
#                          video attached to a README (--max-mb)
#
# Usage:
#   tools/demo/assemble.sh                       # scratch/demo/*.mp4 -> scratch/demo/
#   tools/demo/assemble.sh --in DIR --out DIR
#   tools/demo/assemble.sh --scenes "om615 cam"  # these, in this order
#   tools/demo/assemble.sh --url example.org/x   # the address on the end card
#   tools/demo/assemble.sh --speed 1             # the scenes as recorded
#
# The captions are the CAPTION table below. The clips come from
# tools/demo/record.sh and tools/demo/film.sh (tools/demo/README.md).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IN="$ROOT/scratch/demo"
OUT=""
SCENES="om615 assembly strandbeest parametric history fem cam"
URL=""
MAX_MB=9.5     # GitHub's limit for a video attached on a free plan is 10 MB
SPEED=1.5      # the scenes play this much faster than recorded
FADE=0.4       # seconds of crossfade between two scenes
CARD=4.0       # seconds the end card holds

declare -A CAPTION=(
  [om615]="Assemblies — turned, cut open and run"
  [assembly]="Assembly sequences — checked, then put together"
  [strandbeest]="Filmed from the sequence — a walking linkage, piece by piece"
  [parametric]="Parametric — sketches, dimensions and variables"
  [history]="Versioned — every change kept in git"
  [fem]="Simulation — a static study, in the part"
  [cam]="Manufacturing — toolpaths, simulated"
)

while [[ $# -gt 0 ]]; do
  case "$1" in
    --in) IN="$(realpath -m "$2")"; shift 2 ;;
    --out) OUT="$(realpath -m "$2")"; shift 2 ;;
    --scenes) SCENES="$2"; shift 2 ;;
    --url) URL="$2"; shift 2 ;;
    --max-mb) MAX_MB="$2"; shift 2 ;;
    --speed) SPEED="$2"; shift 2 ;;
    -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
    *) echo "assemble: unknown argument '$1' (see --help)" >&2; exit 2 ;;
  esac
done
OUT="${OUT:-$IN}"
mkdir -p "$OUT"

if [[ -z "$URL" ]]; then
  # git@github.com:owner/repo.git or https://github.com/owner/repo(.git)
  URL="$(git -C "$ROOT" remote get-url origin 2>/dev/null || true)"
  URL="${URL#git@}"; URL="${URL#https://}"; URL="${URL%.git}"; URL="${URL/://}"
fi

FONT="$ROOT/deps/imgui/misc/fonts/Roboto-Medium.ttf" # the app's own face
[[ -f "$FONT" ]] || { echo "assemble: missing $FONT (submodules?)" >&2; exit 1; }

NIXRUN="$ROOT/tools/nix-run"
probe() { # probe FILE ENTRY -> one value
  "$NIXRUN" "ffprobe -v error -select_streams v:0 -show_entries $2 \
    -of default=nw=1:nk=1 '$1'" | head -1
}

read -ra LIST <<<"$SCENES"
[[ ${#LIST[@]} -gt 0 ]] || { echo "assemble: no scenes" >&2; exit 2; }
for s in "${LIST[@]}"; do
  [[ -f "$IN/$s.mp4" ]] || {
    echo "assemble: no $IN/$s.mp4 -- record it: tools/demo/record.sh $s" >&2
    exit 1
  }
  [[ -n "${CAPTION[$s]+x}" ]] || {
    echo "assemble: no caption for '$s'; known: ${!CAPTION[*]}" >&2
    exit 2
  }
done

W="$(probe "$IN/${LIST[0]}.mp4" stream=width)"
H="$(probe "$IN/${LIST[0]}.mp4" stream=height)"
FPS=60
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# The end card: the lockup on the viewport's colour, the address beneath.
nix-shell -p librsvg --run \
  "rsvg-convert -w $((W * 45 / 100)) '$ROOT/assets/brand/lobole-lockup-dark.svg' \
   -o '$TMP/lockup.png'" >/dev/null
printf '%s' "$URL" >"$TMP/url.txt"

# Sizes scale with the frame, so the 1080p cut is the master made smaller.
FS=$((H * 34 / 1000))         # caption text
PAD=$((H * 14 / 1000))        # caption box padding
MARGIN=$((H * 45 / 1000))     # caption distance from the frame's edge
URLFS=$((H * 30 / 1000))

inputs=() filters="" offset=0 i=0
for s in "${LIST[@]}"; do
  printf '%s' "${CAPTION[$s]}" >"$TMP/cap$i.txt"
  d="$(probe "$IN/$s.mp4" format=duration)"
  d="$(awk -v d="$d" -v s="$SPEED" 'BEGIN{printf "%.3f", d/s}')"
  inputs+=(-i "$IN/$s.mp4")
  # Caption in from 0.4 s, out by 3.6 s, fading 0.3 s each way.
  a="if(lt(t,0.4),0,if(lt(t,0.7),(t-0.4)/0.3,if(lt(t,3.3),1,if(lt(t,3.6),(3.6-t)/0.3,0))))"
  # Sped up before the caption, so the caption keeps its own pace.
  filters+="[$i:v]setpts=PTS/$SPEED,fps=$FPS,scale=$W:$H,format=yuv420p,setsar=1,settb=AVTB,"
  filters+="drawtext=fontfile='$FONT':textfile='$TMP/cap$i.txt':fontsize=$FS"
  filters+=":fontcolor=white:alpha='$a':box=1:boxcolor=0x1b1d23@0.78"
  filters+=":boxborderw=$PAD:x=$MARGIN:y=h-th-$MARGIN-$PAD[v$i];"
  durs[$i]="$d"
  i=$((i + 1))
done
n=$i
inputs+=(-loop 1 -t "$CARD" -framerate $FPS -i "$TMP/lockup.png")
filters+="color=c=0x23262e:s=${W}x${H}:r=$FPS:d=$CARD[bg];"
filters+="[bg][$n:v]overlay=(W-w)/2:(H-h)/2-$((H * 4 / 100)):shortest=1,"
filters+="drawtext=fontfile='$FONT':textfile='$TMP/url.txt':fontsize=$URLFS"
filters+=":fontcolor=0x8cd9ff:x=(w-tw)/2:y=h*0.62,"
filters+="format=yuv420p,setsar=1,settb=AVTB[v$n];"
durs[$n]="$CARD"

# Crossfades: each joins at the running length less one fade.
prev="v0" total="${durs[0]}"
for ((k = 1; k <= n; k++)); do
  offset="$(awk -v t="$total" -v f="$FADE" 'BEGIN{printf "%.3f", t-f}')"
  filters+="[$prev][v$k]xfade=transition=fade:duration=$FADE:offset=$offset[x$k];"
  prev="x$k"
  total="$(awk -v t="$total" -v d="${durs[$k]}" -v f="$FADE" \
    'BEGIN{printf "%.3f", t+d-f}')"
done
filters+="[$prev]null[out]"

MASTER="$OUT/lobole-demo.mp4"
SMALL="$OUT/lobole-demo-1080p.mp4"
echo "assemble: ${LIST[*]} + end card, ${total} s at ${W}x${H} -> $MASTER"
q() { printf '%q ' "$@"; }
"$NIXRUN" "ffmpeg -nostdin -hide_banner -loglevel error -y $(q "${inputs[@]}") \
  -filter_complex $(q "$filters") -map '[out]' -an \
  -c:v libx264 -preset slow -crf 16 -pix_fmt yuv420p -movflags +faststart \
  $(q "$MASTER")"

# The README cut: a bitrate that lands under --max-mb for this length.
kbps="$(awk -v mb="$MAX_MB" -v t="$total" 'BEGIN{printf "%d", mb*8000*0.95/t}')"
echo "assemble: 1920x1080 at ${kbps} kb/s -> $SMALL"
"$NIXRUN" "cd '$TMP' && for pass in 1 2; do \
  out=/dev/null; [ \$pass = 2 ] && out=$(q "$SMALL"); \
  ffmpeg -nostdin -hide_banner -loglevel error -y -i $(q "$MASTER") \
    -vf scale=1920:1080:flags=lanczos -an -c:v libx264 -preset slow \
    -b:v ${kbps}k -pass \$pass -pix_fmt yuv420p -movflags +faststart \
    -f mp4 \$out || exit 1; done"
mb="$(awk -v b="$(stat -c %s "$SMALL")" 'BEGIN{printf "%.1f", b/1e6}')"
echo "assemble: $SMALL is $mb MB (limit $MAX_MB)"
awk -v a="$mb" -v b="$MAX_MB" 'BEGIN{exit !(a <= b)}' || {
  echo "assemble: the README cut is over the limit" >&2
  exit 1
}
