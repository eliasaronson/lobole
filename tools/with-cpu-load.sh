#!/usr/bin/env bash
# with-cpu-load — run a command while N CPU burners load the machine, without
# ever leaking them.
#
# Twice now a session has pinned this machine for hours with orphaned burner
# loops (`while :; do :; done &` ... `kill $BURNERS`). The cleanup-by-variable
# pattern is structurally broken here: each agent shell invocation is a NEW
# shell, so pids noted in one command are invisible to the next, and a cleanup
# living in the same compound command dies with it on any error. Discipline
# does not fix that shape; ownership does:
#   - the burners are children of THIS script, which outlives the command and
#     kills them in an EXIT trap (which also runs on error and Ctrl-C);
#   - every burner additionally carries its own `timeout` TTL, so even SIGKILL
#     of the whole tree cannot leave one spinning past the deadline.
#
# Usage: tools/with-cpu-load.sh [-n N] [-t TTL_SECONDS] -- command args...
#   -n  burner count (default: nproc)
#   -t  hard per-burner lifetime in seconds (default 900) — the backstop, not
#       the mechanism; the EXIT trap ends them with the command.
# Exit code is the command's.
set -eu

N="$(nproc)"
TTL=900
while [ $# -gt 0 ]; do
  case "$1" in
    -n) N="$2"; shift 2 ;;
    -t) TTL="$2"; shift 2 ;;
    --) shift; break ;;
    *) echo "usage: $0 [-n N] [-t TTL_SECONDS] -- command args..." >&2; exit 2 ;;
  esac
done
[ $# -gt 0 ] || { echo "usage: $0 [-n N] [-t TTL_SECONDS] -- command args..." >&2; exit 2; }

PIDS=""
for _ in $(seq 1 "$N"); do
  # nice: the point is load, not starving the subject under test.
  nice timeout "$TTL" sh -c 'while :; do :; done' &
  PIDS="$PIDS $!"
done
# shellcheck disable=SC2086  # $PIDS is a deliberate word-split list
cleanup() { kill $PIDS 2>/dev/null || true; }
trap cleanup EXIT INT TERM

"$@"
