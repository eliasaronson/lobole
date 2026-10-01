#!/usr/bin/env bash
# The pan/tilt head, from nothing: every part, then the assembly with its
# checks, studies and renders. What it makes (projects, renders, reports,
# these logs) goes to scratch/pantilt/.
#
#   generators/pantilt/run_all.sh PORT                  # everything
#   generators/pantilt/run_all.sh PORT --from assembly  # the saved parts,
#                                                       # assembly only
#
# Stages, in order: base worms tilt yoke assembly. Each opens the project the
# one before saved; `--from` starts at one and reuses what the earlier ones
# left in scratch/pantilt/. Run it against a scratch instance
# (tools/headless-preview.sh --start PORT), never somebody's open document.
#
# run_all.log ends in `done` or `failed`, a marker to wait on
# (tools/wait-for.sh --file scratch/pantilt/run_all.log --line '^(done|failed)$').
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../../scratch/pantilt"
mkdir -p "$OUT"
P=${1:?usage: run_all.sh PORT [--from STAGE]}
shift
FROM=base
while [ $# -gt 0 ]; do
  case "$1" in
    --from) FROM="$2"; shift 2 ;;
    *) echo "run_all: unknown argument $1 (stages: base worms tilt yoke assembly)" >&2; exit 2 ;;
  esac
done
STAGES=(base worms tilt yoke assembly)
start=-1
for i in "${!STAGES[@]}"; do [ "${STAGES[$i]}" = "$FROM" ] && start=$i; done
if [ "$start" -lt 0 ]; then
  echo "run_all: no stage '$FROM'; the stages are ${STAGES[*]}" >&2
  exit 2
fi
LOG="$OUT/run_all.log"
: > "$LOG"
for i in "${!STAGES[@]}"; do
  [ "$i" -lt "$start" ] && continue
  stage=${STAGES[$i]}
  echo "== $stage" >> "$LOG"
  if ! python3 -u "$HERE/build_$stage.py" --port "$P" > "$OUT/build_$stage.log" 2>&1; then
    tail -n 30 "$OUT/build_$stage.log" >> "$LOG"
    echo failed >> "$LOG"
    exit 1
  fi
  tail -n 3 "$OUT/build_$stage.log" >> "$LOG"
done
echo done >> "$LOG"
