#!/usr/bin/env bash
# The whole chess set, from nothing: design, then every CAM job. What it makes
# (projects, programs, renders, reports, these logs) goes to scratch/chess/.
#   generators/chess/run_all.sh PORT
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../../scratch/chess"
mkdir -p "$OUT/nc"
P=$1
rm -f "$OUT"/nc/*.nc "$OUT"/cam_all.log
python3 "$HERE/build_piece.py" --port "$P" > "$OUT/build_piece.log" 2>&1
python3 "$HERE/build_blanks.py" --port "$P" > "$OUT/build_blanks.log" 2>&1
python3 "$HERE/build_board.py" --port "$P" > "$OUT/build_board.log" 2>&1
python3 "$HERE/build_set.py" --port "$P" > "$OUT/build_set.log" 2>&1
# The log ends in `done` or `failed`, a marker to wait on
# (tools/wait-for.sh --file scratch/chess/cam_all.log --line '^(done|failed)$').
if python3 -u "$HERE/cam.py" --port "$P" --out chess > "$OUT/cam_all.log" 2>&1; then
  echo done >> "$OUT/cam_all.log"
else
  echo failed >> "$OUT/cam_all.log"
  exit 1
fi
