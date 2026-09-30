#!/usr/bin/env bash
# Wait for something to finish: a PID to exit, or a LINE to appear in a file.
# Never a `pgrep -f PATTERN` loop -- the pattern is in the loop's own command
# line, so it matches itself and waits forever (33 minutes, once), and the
# same pattern given to `pkill -f` kills the shell that ran it (exit 144,
# seven times in one round).
#
#   tools/wait-for.sh --pid 12345
#   tools/wait-for.sh --file scratch/chess/cam_all.log --line '^done$'
#   tools/wait-for.sh ... --timeout 540      # seconds; default 540
#
# The default deadline sits under a tool call's 600 s cap, so a waiter never
# gets moved to the background and left running. Exit 0 when it happened,
# 124 when the deadline passed (the caller decides what next), 2 on usage.
set -euo pipefail
pid="" file="" line="" timeout=540
while [ $# -gt 0 ]; do
  case "$1" in
    --pid) pid="$2"; shift 2 ;;
    --file) file="$2"; shift 2 ;;
    --line) line="$2"; shift 2 ;;
    --timeout) timeout="$2"; shift 2 ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    *) echo "wait-for: unknown argument $1" >&2; exit 2 ;;
  esac
done
if [ -z "$pid" ] && { [ -z "$file" ] || [ -z "$line" ]; }; then
  echo "wait-for: give --pid N, or --file F --line REGEX" >&2
  exit 2
fi
deadline=$(( $(date +%s) + timeout ))
while [ "$(date +%s)" -lt "$deadline" ]; do
  if [ -n "$pid" ]; then
    kill -0 "$pid" 2>/dev/null || exit 0
  elif [ -f "$file" ] && grep -qE -- "$line" "$file"; then
    exit 0
  fi
  sleep 2
done
echo "wait-for: still waiting after ${timeout} s" >&2
exit 124
