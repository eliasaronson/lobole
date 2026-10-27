#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# Film the demo's export scenes: an assembly sequence filmed by
# export_assembly_video from a saved project, not the app's window as
# record.sh's scenes are. The clip holds still a moment at each end, as every
# scene does, so assemble.sh's crossfades join it like the others.
#
#   tools/demo/film.sh                  # every film scene -> scratch/demo/
#   tools/demo/film.sh strandbeest      # just these
#   tools/demo/film.sh --draft ...      # 1920x1080: a quick look
#   tools/demo/film.sh --out DIR ...
#
# strandbeest: the leg pair put together piece by piece, each piece lit while
# it goes home and the crank turned before the cap that goes on with it.
# It films the saved stage 1 of a generator run (tools/demo/README.md):
#   python3 generators/strandbeest/build.py --port PORT --stage 1 \
#       --no-controls --out scratch/strandbeest-film
# Its own offscreen instance on a scratch port, under a memory cap, stopped
# when the script exits; the project is opened from a copy.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MAIN="$(cd "$(git -C "$ROOT" rev-parse --git-common-dir)/.." && pwd)"
SCRATCH="$MAIN/scratch"
OUT="$SCRATCH/demo"
SIZE=3840x2160
HOLD_START=1.0 # seconds the first frame holds
HOLD_END=2.0   # and the last, assembled
SCENES=()
while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$(realpath -m "$2")"; shift 2 ;;
    --draft) SIZE=1920x1080; shift ;;
    -h|--help) sed -n '5,20p' "$0"; exit 0 ;;
    -*) echo "film: unknown option $1" >&2; exit 2 ;;
    *) SCENES+=("$1"); shift ;;
  esac
done

ALL=(strandbeest)
STRANDBEEST="${LOBOLE_DEMO_STRANDBEEST_SOURCE:-$SCRATCH/strandbeest-film/stage1/strandbeest_stage1.lob}"
[ ${#SCENES[@]} -gt 0 ] || SCENES=("${ALL[@]}")
for s in "${SCENES[@]}"; do
  case " ${ALL[*]} " in
    *" $s "*) ;;
    *) echo "film: no scene '$s'; the film scenes are: ${ALL[*]}" >&2; exit 2 ;;
  esac
done
[ -e "$STRANDBEEST" ] || {
  echo "film: no Strandbeest project at $STRANDBEEST (generators/strandbeest/build.py --stage 1 builds it)" >&2
  exit 1
}
[ -x "$ROOT/build/lobole" ] || { echo "film: build/lobole is not built (tools/build.sh)" >&2; exit 1; }

TMP="$(mktemp -d)"
PORT="$(python3 "$ROOT/tools/scratch_port.py")"
cleanup() {
  "$ROOT/tools/headless-preview.sh" --stop "$PORT" >/dev/null 2>&1 || true
  rm -rf "$TMP"
}
trap cleanup EXIT
cp -r "$STRANDBEEST" "$TMP/strandbeest.lob"
# A film of a big assembly is the costliest thing an instance does; a cap
# keeps a runaway from taking the desktop with it.
systemd-run --user --scope -p MemoryMax=8G -q \
  "$ROOT/tools/headless-preview.sh" --start "$PORT" >/dev/null

mkdir -p "$OUT"
W="${SIZE%x*}"
H="${SIZE#*x}"
for s in "${SCENES[@]}"; do
  echo "film: $s at $SIZE -> $OUT/$s.mp4"
  PYTHONPATH="$ROOT/python" python3 - "$PORT" "$TMP" "$W" "$H" "$ROOT" <<'EOF'
import sys
from pathlib import Path
port, tmp = int(sys.argv[1]), Path(sys.argv[2])
w, h, root = int(sys.argv[3]), int(sys.argv[4]), Path(sys.argv[5])
from lobole import Client
d = Client(port=port, timeout=3600)
d.call("open_project", path=str(tmp / "strandbeest.lob"))
d.call("set_active_part", name="leg pair, put together")
sys.path.insert(0, str(root / "generators" / "strandbeest"))
from build import film_sequence  # noqa: E402
film_sequence(d, tmp, size=(w, h), name="raw.mp4")
EOF
  "$ROOT/tools/nix-run" "ffmpeg -loglevel error -y -i '$TMP/raw.mp4' \
    -vf 'tpad=start_mode=clone:start_duration=$HOLD_START:stop_mode=clone:stop_duration=$HOLD_END' \
    -c:v libx264 -preset slow -crf 16 -pix_fmt yuv420p -an '$OUT/$s.mp4'"
  rm -f "$TMP/raw.mp4"
  echo "  $OUT/$s.mp4"
done
