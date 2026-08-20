#!/usr/bin/env bash
# gui-tests-xvfb — run the GUI test suite on a virtual X display, in parallel.
#
# Why Xvfb: on the real display, the hidden test window is throttled to ~1 fps
# by the X Present extension whenever the session is locked (or the window is
# fully occluded), so `ctest -R gui_smoke` times out. Xvfb has no compositor,
# no occlusion and no lock: the suite runs at full speed regardless of the
# desktop's state. The NVIDIA GPU renders and presents to Xvfb fine.
#
# Why shards: the suite is CPU-bound (real OCC geometry, real frames) in ONE
# process — 335 s at 95% of a single core, on a machine with 22 of them.
# Measured 2026-08-20: 148 tests, median 1.7 s, and a floor of 0.09 s, so
# there is no per-test overhead to trim; the fix is to run several at once.
#
# Usage:
#   ./tools/gui-tests-xvfb.sh              # whole suite, 4 shards  (~90 s)
#   ./tools/gui-tests-xvfb.sh --fast       # ...minus the slowest   (~70 s)
#   ./tools/gui-tests-xvfb.sh -j 8         # more shards
#   ./tools/gui-tests-xvfb.sh --serial     # one process (~335 s), for debugging
#   ./tools/gui-tests-xvfb.sh -f fillet    # only matching cases, one process
#   ./tools/gui-tests-xvfb.sh -L gui       # all GUI-labeled ctest tests
#   ./tools/gui-tests-xvfb.sh --record-times   # refresh gui-test-times.txt
#
# ALWAYS run the whole suite before committing: --fast means fewer tests, not
# weaker ones.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/build"

TIMES="$ROOT/tools/gui-test-times.txt"
# --fast skips tests recorded at or above this. Chosen from the measured
# distribution: it drops 5 of 148 (82 s, over half of it the pattern stress
# test) and keeps everything under a few seconds. A test with no recorded
# time is NEVER skipped — a new test must run in fast mode too.
FAST_LIMIT=4.0

# One GPU suite at a time, machine-wide: concurrent runs (parallel agents /
# worktrees) contend for the GPU and flake. A solo run acquires the lock in
# microseconds; a contended one waits its turn instead of corrupting both.
# The shards below run under this one lock — they are one suite, not several.
LOCK=/tmp/designer-gui-gpu.lock
if [ "${DESIGNER_GUI_LOCKED:-}" != "1" ]; then
  export DESIGNER_GUI_LOCKED=1
  exec flock "$LOCK" "$ROOT/tools/gui-tests-xvfb.sh" "$@"
fi

if command -v xvfb-run >/dev/null; then
  XVFB_RUN=xvfb-run
else
  XVFB_RUN="$(nix-shell -p xvfb-run --run 'which xvfb-run' | tail -1)"
fi
SCREEN='-screen 0 1600x1000x24'

# One test process. $1 = X display number, $2 = --gui-test-filter argument,
# $3 = a private state directory.
#
# XDG_STATE_HOME and XDG_CONFIG_HOME are per shard because parts of the app
# keep MACHINE-global state there: the recent-projects list and the user's
# preferences. Sharing state made file/open_recent_lists_what_was_opened fail
# — it clears the list, saves one project and expects to find exactly that
# one, which is not true while three other processes are saving projects of
# their own. Config is isolated for the same reason, and so that no test run
# can write the developer's real preferences.
run_shard() {
  mkdir -p "$3"
  "$XVFB_RUN" -n "$1" -s "$SCREEN" \
    nix-shell "$ROOT/shell.nix" --run \
    "cd '$ROOT/build' && XDG_STATE_HOME='$3' XDG_CONFIG_HOME='$3/config' \
     ./designer --gui-tests --hidden --gui-test-filter '$2'"
}

SHARDS=4
SLOWER_THAN=""
RECORD=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --fast) SLOWER_THAN="$FAST_LIMIT"; shift ;;
    --serial) SHARDS=1; shift ;;
    -j) SHARDS="$2"; shift 2 ;;
    --record-times) RECORD=1; SHARDS=1; shift ;;
    -f)
      # Single named case: no sharding, no display juggling.
      exec "$XVFB_RUN" -a -s "$SCREEN" \
        nix-shell "$ROOT/shell.nix" --run \
        "cd '$ROOT/build' && ./designer --gui-tests --hidden \
         --gui-test-filter '$2'"
      ;;
    *)
      # Anything else is ctest's (e.g. -L gui). This script already provides
      # the display and holds the GPU lock, so the per-test xvfb-exec.sh
      # wrapper must not start a second Xvfb inside this one.
      exec "$XVFB_RUN" -a -s "$SCREEN" \
        nix-shell "$ROOT/shell.nix" --run \
        "cd '$ROOT/build' && DESIGNER_NO_XVFB=1 ctest $* --output-on-failure"
      ;;
  esac
done

WORK="$(mktemp -d -t designer-gui-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# --record-times: one serial run, timing each case, into the file the shard
# balancer reads. Run it after adding slow tests; balance drifts, coverage
# does not (an unrecorded test still runs, at the default weight).
if [ "$RECORD" = "1" ]; then
  echo "Recording per-test times (serial, ~6 min)..."
  "$XVFB_RUN" -a -s "$SCREEN" nix-shell "$ROOT/shell.nix" --run \
    "cd '$ROOT/build' && ./designer --gui-tests --hidden" 2>&1 |
    python3 -u "$ROOT/tools/gui_shard_record.py" "$TIMES"
  exit $?
fi

# The suite's OWN inventory decides who runs — scraping IM_REGISTER_TEST out
# of the source would drift, and a test missing from the partition would be
# silently skipped. Listing needs no display and no GPU.
nix-shell "$ROOT/shell.nix" --run \
  "cd '$ROOT/build' && ./designer --gui-test-list" > "$WORK/list" 2>/dev/null
python3 "$ROOT/tools/gui_shard.py" --list "$WORK/list" --times "$TIMES" \
  --shards "$SHARDS" ${SLOWER_THAN:+--slower-than "$SLOWER_THAN"} \
  > "$WORK/filters" 2> "$WORK/info"
EXPECTED="$(sed -n 's/^# tests: //p' "$WORK/info")"
sed -n 's/^# estimated/estimated/p' "$WORK/info"

# Distinct display numbers, not `xvfb-run -a`: concurrent -a runs race for the
# same number, and the first to exit tears the server down under the others
# ("X connection to :125 broken"). The PID keeps two runs apart if the lock
# is ever bypassed.
BASE=$(( 100 + ($$ % 300) ))
i=0
while read -r filter; do
  i=$((i + 1))
  ( run_shard "$((BASE + i))" "$filter" "$WORK/state$i" \
      > "$WORK/shard$i.log" 2>&1
    echo "$?" > "$WORK/shard$i.code" ) &
done < "$WORK/filters"
START=$(date +%s)
wait
ELAPSED=$(( $(date +%s) - START ))

# Every shard's result, and the total: a filter that quietly matched nothing
# would otherwise read as a pass.
#
# The app's own "N/M passed" line is the verdict, NOT the process exit code:
# xvfb-run's wrapper returns non-zero when its server is already gone
# ("kill: No such process"), which reported a clean 35/35 shard as a failure.
# A shard that died before printing has no line at all, which is a failure.
ran=0
failed=0
for c in $(seq 1 "$i"); do
  line="$(grep -o 'GUI tests: [0-9]*/[0-9]* passed' "$WORK/shard$c.log" |
          tail -1 || true)"
  ok="$(echo "$line" | sed -n 's#GUI tests: \([0-9]*\)/[0-9]* passed#\1#p')"
  n="$(echo "$line" | sed -n 's#GUI tests: [0-9]*/\([0-9]*\) passed#\1#p')"
  ran=$(( ran + ${n:-0} ))
  if [ -z "$line" ] || [ "$ok" != "$n" ]; then
    failed=1
    echo "--- shard $c FAILED ${line:+($line)} ---"
    grep -E "test failed|Error '|Error GuiTests" "$WORK/shard$c.log" |
      tail -20 || true
    cp "$WORK/shard$c.log" "/tmp/designer-gui-shard$c.log"
    echo "    full log: /tmp/designer-gui-shard$c.log"
  fi
done

echo "GUI tests: $ran/$EXPECTED run in ${ELAPSED}s across $i shard(s)"
if [ "$ran" != "$EXPECTED" ]; then
  echo "MISMATCH: the shards did not run every test — the partition is wrong."
  exit 1
fi
exit "$failed"
