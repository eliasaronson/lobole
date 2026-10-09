#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# The whole chess set, from nothing: design, then every CAM job. What it makes
# (projects, programs, renders, reports, these logs) goes to scratch/chess/.
#
#   generators/chess/run_all.sh PORT                      # everything (~9 min)
#   generators/chess/run_all.sh PORT --from cam           # the saved set, CAM only
#   generators/chess/run_all.sh PORT --from cam --only board blank_pawns
#
# Stages, in order: piece, blanks, board, set, cam. `--from` starts at one and
# reuses what the earlier ones left in scratch/chess/; `--only` names the CAM
# jobs to run (cam.py's own flag). DESIGNER_SCRATCH moves scratch/
# (tests/generator_runs.py). The full run is the acceptance check after
# a change; one stage, one job, is the loop (docs/architecture.md#the-development-loop).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${DESIGNER_SCRATCH:-$HERE/../../scratch}/chess"
mkdir -p "$OUT/nc"
P=$1
shift
FROM=piece
ONLY=()
while [ $# -gt 0 ]; do
  case "$1" in
    --from) FROM="$2"; shift 2 ;;
    --only) shift; while [ $# -gt 0 ] && [ "${1#--}" = "$1" ]; do ONLY+=("$1"); shift; done ;;
    *) echo "run_all: unknown argument $1 (stages: piece blanks board set cam)" >&2; exit 2 ;;
  esac
done
STAGES=(piece blanks board set cam)
start=-1
for i in "${!STAGES[@]}"; do [ "${STAGES[$i]}" = "$FROM" ] && start=$i; done
if [ "$start" -lt 0 ]; then
  echo "run_all: no stage '$FROM'; the stages are ${STAGES[*]}" >&2
  exit 2
fi
run() { # stage index, then the command; a failure stops the run (set -e)
  local i=$1; shift
  if [ "$i" -ge "$start" ]; then "$@"; fi
}
run 0 sh -c "python3 '$HERE/build_piece.py' --port '$P' > '$OUT/build_piece.log' 2>&1"
run 1 sh -c "python3 '$HERE/build_blanks.py' --port '$P' > '$OUT/build_blanks.log' 2>&1"
run 2 sh -c "python3 '$HERE/build_board.py' --port '$P' > '$OUT/build_board.log' 2>&1"
run 3 sh -c "python3 '$HERE/build_set.py' --port '$P' > '$OUT/build_set.log' 2>&1"
# The log ends in `done` or `failed`, a marker to wait on
# (tools/wait-for.sh --file scratch/chess/cam_all.log --line '^(done|failed)$').
[ ${#ONLY[@]} -eq 0 ] && rm -f "$OUT"/nc/*.nc
rm -f "$OUT"/cam_all.log
# A partial run saves beside the full set, never over it: chess.dsgn is the
# whole set's CAM, chess_only.dsgn the last `--only` run's.
only=() stem=chess
if [ ${#ONLY[@]} -gt 0 ]; then only=(--only "${ONLY[@]}"); stem=chess_only; fi
if python3 -u "$HERE/cam.py" --port "$P" --out "$stem" "${only[@]}" > "$OUT/cam_all.log" 2>&1; then
  echo done >> "$OUT/cam_all.log"
else
  echo failed >> "$OUT/cam_all.log"
  exit 1
fi
