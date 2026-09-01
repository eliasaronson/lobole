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
#   ./tools/gui-tests-xvfb.sh              # whole suite, 4 shards, SHUFFLED
#   ./tools/gui-tests-xvfb.sh --fast       # ...minus the slowest   (~70 s)
#   ./tools/gui-tests-xvfb.sh -j 8         # more shards
#   ./tools/gui-tests-xvfb.sh --serial     # one process (~335 s), for debugging
#   ./tools/gui-tests-xvfb.sh -f fillet    # only matching cases, one process
#   ./tools/gui-tests-xvfb.sh -L gui       # all GUI-labeled ctest tests
#   ./tools/gui-tests-xvfb.sh --shuffle 12345  # replay a recorded seed
#   ./tools/gui-tests-xvfb.sh --in-order   # the declared order, no shuffle
#   ./tools/gui-tests-xvfb.sh --record-times   # refresh gui-test-times.txt
#
# Shuffled BY DEFAULT since 2026-09-01, with the seed printed. The suite has a
# history of order-dependent failures — three different tests in one day, each
# failing in a sharded run and passing on the next — and every hunt for them
# has stalled at the same place: the failure is reproducible only from its
# seed, and nobody ran shuffled locally, so no seed was ever generated. CI has
# shuffled since d1396b5; this makes every developer run generate one too.
# The cost is that a bare re-run is a DIFFERENT order — replay with
# `--shuffle <seed>`, which the run prints, or use `--in-order`.
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

# Reap orphaned Xvfb servers before starting new ones. When a run (or a
# headless-preview / ad-hoc xvfb-run) is killed hard, xvfb-run dies without
# reaping its Xvfb child, which reparents to init and idles forever — 43 of
# them had accumulated by 2026-08-26, one with 8 CPU-hours. Under this lock
# no other suite is running, and a LIVE run's Xvfb has a living xvfb-run
# parent — so PPID 1 alone identifies the corpses. The user's real session
# is Xorg/Wayland, never an "Xvfb" comm.
ps -eo pid,ppid,comm --no-headers | awk '$2==1 && $3=="Xvfb" {print $1}' |
  xargs -r kill 2>/dev/null || true

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
  # $4: extra args (the shuffle seed for this shard, when --shuffle is on).
  "$XVFB_RUN" -n "$1" -s "$SCREEN" \
    nix-shell "$ROOT/shell.nix" --run \
    "cd '$ROOT/build' && XDG_STATE_HOME='$3' XDG_CONFIG_HOME='$3/config' \
     ./designer --gui-tests --hidden --gui-test-filter '$2' ${4:-}"
}

SHARDS=4
SLOWER_THAN=""
RECORD=0
SHUFFLE=1  # on by default; --in-order turns it off
SEED=0     # 0 = pick one below, so a whole run replays from ONE number
while [ "$#" -gt 0 ]; do
  case "$1" in
    --shuffle)
      # Run each shard in a random order. The point is not randomness for its
      # own sake: it is that an order dependency then fails on some run rather
      # than lying dormant until the time-balanced partition reshuffles and
      # takes an unrelated test down with it.
      #
      # Now the default, so the flag's remaining job is REPLAY: `--shuffle
      # <seed>` reproduces a run exactly.
      SHUFFLE=1
      if [ -n "${2:-}" ] && [ "${2#-}" = "$2" ]; then SEED="$2"; shift; fi
      shift ;;
    --in-order)
      # The declared order. For bisecting a failure down to a pair, where a
      # changing order is noise rather than signal.
      SHUFFLE=0; shift ;;
    --fast) SLOWER_THAN="$FAST_LIMIT"; shift ;;
    --serial) SHARDS=1; shift ;;
    -j) SHARDS="$2"; shift 2 ;;
    --record-times) RECORD=1; SHARDS=1; shift ;;
    -f)
      # Single named case: no sharding, no display juggling — but the SAME
      # verdict as the shard loop below. This used to exec straight through,
      # so the exit code was xvfb-run's; the app exits 0 whether or not a case
      # failed, and xvfb-run's own status is unreliable. `-f whatever` on a
      # failing test therefore reported success, which is worse than no check
      # at all because it looks like one.
      FLOG="$(mktemp -t designer-gui-f-XXXXXX.log)"
      "$XVFB_RUN" -a -s "$SCREEN" \
        nix-shell "$ROOT/shell.nix" --run \
        "cd '$ROOT/build' && ./designer --gui-tests --hidden \
         --gui-test-filter '$2'" > "$FLOG" 2>&1 || true
      cat "$FLOG"
      fline="$(grep -o 'GUI tests: [0-9]*/[0-9]* passed' "$FLOG" | tail -1 ||
               true)"
      fok="$(echo "$fline" | sed -n 's#GUI tests: \([0-9]*\)/[0-9]* passed#\1#p')"
      fn="$(echo "$fline" | sed -n 's#GUI tests: [0-9]*/\([0-9]*\) passed#\1#p')"
      rm -f "$FLOG"
      if [ -z "$fline" ]; then
        echo "--- the run produced no verdict line ---" >&2
        exit 1
      fi
      # A filter that matched nothing is not a pass, for the same reason the
      # shard partition is checked: silence is not success.
      if [ "${fn:-0}" = 0 ]; then
        echo "--- no test matched '$2' ---" >&2
        exit 1
      fi
      [ "$fok" = "$fn" ] || exit 1
      exit 0
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

# One base seed for the whole run, chosen here rather than letting each shard
# pick its own: four independent seeds mean four numbers to keep, and the
# thing that has to survive a failure is ONE number a person can paste back.
# Shard N runs with SEED+N, so `--shuffle <SEED>` reproduces every shard.
if [ "$SHUFFLE" = "1" ] && [ "$SEED" = "0" ]; then
  SEED=$(od -An -N4 -tu4 < /dev/urandom | tr -d ' ')
  SEED=$(( SEED % 1000000 ))
fi
i=0
while read -r filter; do
  i=$((i + 1))
  SHUFFLE_ARG=""
  if [ "$SHUFFLE" = "1" ]; then
    # A distinct seed per shard, derived from ONE base so a whole run replays
    # from a single number: --shuffle <SEED> reproduces every shard exactly.
    SHUFFLE_ARG="--gui-test-shuffle $((SEED + i))"
  fi
  ( run_shard "$((BASE + i))" "$filter" "$WORK/state$i" "$SHUFFLE_ARG" \
      > "$WORK/shard$i.log" 2>&1
    echo "$?" > "$WORK/shard$i.code" ) &
done < "$WORK/filters"
START=$(date +%s)
wait
ELAPSED=$(( $(date +%s) - START ))

if [ "$SHUFFLE" = "1" ]; then
  grep -h "shuffled order, seed" "$WORK"/shard*.log 2>/dev/null || true
  # The one number that reproduces this whole run. Printed on every run, pass
  # or fail, because the failure you want to replay is the one you did not
  # know was coming — and a seed recovered after the fact is the thing three
  # separate order-dependency hunts did not have.
  echo "  shuffled: base seed $SEED   (replay: --shuffle $SEED)"
fi

# Every shard's result, and the total: a filter that quietly matched nothing
# would otherwise read as a pass.
#
# The app's own "N/M passed" line is the verdict, NOT the process exit code:
# xvfb-run's wrapper returns non-zero when its server is already gone
# ("kill: No such process"), which reported a clean 35/35 shard as a failure.
# A shard that died before printing has no line at all, which is a failure.
# Yesterday's failure logs are not today's. They are only written when a
# shard fails, and nothing ever removed them — so a clean run left four stale
# ones lying about, which is exactly long enough to be read as current.
#
# But deleting them created the opposite problem, and it is the one that
# actually cost time: the natural response to an order-dependent failure is
# to RE-RUN it, and the re-run wiped the evidence. Three separate flakes on
# 2026-08-25 were each read from a log the following run destroyed, so none
# of them could be diagnosed afterwards.
#
# So: the fixed path is still cleared (nothing stale can be mistaken for
# current), and a failing shard is ALSO archived under a timestamped
# directory that no later run touches, alongside the one thing that makes it
# reproducible — the exact filter, in order, and the seed.
rm -f /tmp/designer-gui-shard*.log
FAILDIR="${XDG_STATE_HOME:-$HOME/.local/state}/designer/gui-failures"
STAMP="$(date +%Y%m%d-%H%M%S)"
# Keep the last 20 archived failures; past that the oldest are noise.
if [ -d "$FAILDIR" ]; then
  ls -1dt "$FAILDIR"/* 2>/dev/null | tail -n +21 | xargs -r rm -rf
fi

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
    # ...and an archive the next run will not delete, with the two things a
    # later reader needs and never had: which tests shared this process, in
    # which order, and the seed that produced that order.
    ARCH="$FAILDIR/$STAMP-shard$c"
    mkdir -p "$ARCH"
    cp "$WORK/shard$c.log" "$ARCH/shard.log"
    sed -n "${c}p" "$WORK/filters" > "$ARCH/filter"
    SEEDLINE="$(grep -m1 'shuffled order, seed' "$WORK/shard$c.log" || true)"
    {
      echo "# $(date -Is)  shard $c of $i${line:+  ($line)}"
      echo "# ${SEEDLINE:-not shuffled: the order is the filter order}"
      echo "#"
      echo "# Reproduce THIS process — same tests, same order:"
      echo "cd '$ROOT/build' && ./designer --gui-tests --hidden \\"
      printf "  --gui-test-filter '%s'" "$(sed -n "${c}p" "$WORK/filters")"
      if [ -n "$SEEDLINE" ]; then
        printf " \\\n  --gui-test-shuffle %s" \
          "$(echo "$SEEDLINE" | sed -n 's/.*seed[ =]*\([0-9]*\).*/\1/p')"
      fi
      echo
    } > "$ARCH/reproduce.sh"
    chmod +x "$ARCH/reproduce.sh"
    echo "    archived: $ARCH  (reproduce.sh has the filter and seed)"
  fi
done

# "run" is not "passed", and the last line is the one people read. Say which
# it is: a suite that reports "171/171 run" after four failures is telling the
# truth and being misunderstood, which is the same cost as being wrong.
if [ "$failed" = 0 ]; then
  echo "GUI tests: $ran/$EXPECTED passed in ${ELAPSED}s across $i shard(s)"
else
  echo "GUI tests: $ran/$EXPECTED run in ${ELAPSED}s across $i shard(s) — SOME FAILED"
fi
if [ "$ran" != "$EXPECTED" ]; then
  echo "MISMATCH: the shards did not run every test — the partition is wrong."
  exit 1
fi
exit "$failed"
