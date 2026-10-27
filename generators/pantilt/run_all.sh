#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# The pan/tilt head, from nothing: every part, then the assembly with its
# checks, studies and renders, the base plate's CAM job on the 6090, the
# drawings and the final renders. What it makes goes to scratch/pantilt/:
# the head's one project, pantilt.lob (parts, assembly, requirements, the
# CAM job and the drawings), the renders, drawings, reports and these logs;
# each stage's own project, the one `--from` resumes from, under stages/.
#
#   generators/pantilt/run_all.sh PORT                  # everything
#   generators/pantilt/run_all.sh PORT --from assembly  # the saved parts,
#                                                       # assembly only
#   generators/pantilt/run_all.sh PORT --from drawings  # the sheets, renders
#   generators/pantilt/run_all.sh PORT --skip yoke_topo renders
#
# Stages, in order: base worms tilt yoke assembly yoke_topo cam drawings
# renders. Each opens the project the one before saved (stages/<stage>.lob;
# the drawings stage saves pantilt.lob, which renders opens); `--from`
# starts at one and reuses what the earlier ones left there; `--skip`
# leaves stages out (no later stage opens yoke_topo's or renders' output).
# LOBOLE_SCRATCH moves scratch/ (tests/generator_runs.py). Run it
# against a scratch instance (tools/headless-preview.sh --start PORT), never
# somebody's open document.
#
# run_all.log ends in `done` or `failed`, a marker to wait on
# (tools/wait-for.sh --file scratch/pantilt/run_all.log --line '^(done|failed)$').
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${LOBOLE_SCRATCH:-$HERE/../../scratch}/pantilt"
mkdir -p "$OUT"
P=${1:?usage: run_all.sh PORT [--from STAGE] [--skip STAGE...]}
shift
FROM=base
SKIP=" "
while [ $# -gt 0 ]; do
  case "$1" in
    --from) FROM="$2"; shift 2 ;;
    --skip) shift; while [ $# -gt 0 ] && [ "${1#--}" = "$1" ]; do SKIP="$SKIP$1 "; shift; done ;;
    *) echo "run_all: unknown argument $1 (stages: base worms tilt yoke assembly yoke_topo cam drawings renders)" >&2; exit 2 ;;
  esac
done
STAGES=(base worms tilt yoke assembly yoke_topo cam drawings renders)
start=-1
for i in "${!STAGES[@]}"; do [ "${STAGES[$i]}" = "$FROM" ] && start=$i; done
if [ "$start" -lt 0 ]; then
  echo "run_all: no stage '$FROM'; the stages are ${STAGES[*]}" >&2
  exit 2
fi
for s in $SKIP; do
  case " ${STAGES[*]} " in
    *" $s "*) ;;
    *) echo "run_all: cannot skip '$s'; the stages are ${STAGES[*]}" >&2; exit 2 ;;
  esac
done
LOG="$OUT/run_all.log"
: > "$LOG"
for i in "${!STAGES[@]}"; do
  [ "$i" -lt "$start" ] && continue
  stage=${STAGES[$i]}
  case "$SKIP" in *" $stage "*) echo "== $stage (skipped)" >> "$LOG"; continue ;; esac
  echo "== $stage" >> "$LOG"
  script="build_$stage.py"
  [ "$stage" = cam ] && script=cam.py   # not a part: the job that cuts one
  if ! python3 -u "$HERE/$script" --port "$P" > "$OUT/build_$stage.log" 2>&1; then
    tail -n 30 "$OUT/build_$stage.log" >> "$LOG"
    echo failed >> "$LOG"
    exit 1
  fi
  tail -n 3 "$OUT/build_$stage.log" >> "$LOG"
done
echo done >> "$LOG"
