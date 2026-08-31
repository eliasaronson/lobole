#!/usr/bin/env bash
# profile — where the CPU time actually goes.
#
# An occasional audit tool, like tools/coverage.sh and ci-local.sh --asan: NOT
# part of CI, and not something to run after every edit.
#
# Why it exists. Every performance result in docs/performance/ was found by
# hand-placing std::chrono::steady_clock around a phase somebody already
# suspected — the 259 s open, the 5.9 s assembly resync, the STEP file parsed
# twice, the 7 s adaptive pocket. That works, and where the suspicion was
# right it left behind a permanent breakdown worth keeping (SyncAssemblyRenderers
# logs its phases so the next regression is READ rather than re-profiled).
#
# What it cannot do is find the cost you did not think to time. A sampling
# profiler starts from the answer instead of from a hypothesis, and nothing in
# this repo had one (docs/capability-review-2026-08-31.md, P1).
#
# perf is deliberately NOT in shell.nix: it is coupled to the running kernel,
# and a dev shell that fails to evaluate on someone else's machine costs more
# than a tool they can fetch in one line. This script fetches it itself.
#
# Usage:
#   tools/profile.sh --test '[cam]'            # a Catch2 selection
#   tools/profile.sh --app scratch/build_x.py  # the app, driven by a script
#   tools/profile.sh -- build/designer --headless --remote 8901
#
# Options:
#   --freq N     sampling frequency, Hz (default 999 — coprime with common
#                timer rates, so a periodic workload is not sampled in lockstep)
#   --out DIR    where perf.data and the report go (default build/profile)
#   --keep       do not delete a previous run's data first
#
# Reading the output: the report is inclusive-by-caller (`perf report -g`), so
# start at the top of the flat list for "what burns cycles" and follow the call
# graph for "who asked for it". A symbol from OpenCASCADE or the driver at the
# top is normal and usually still OUR fault one frame up — that is what the
# call graph is for.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FREQ=999
OUT="$ROOT/build/profile"
KEEP=0
MODE=""
ARG=""

while [ $# -gt 0 ]; do
  case "$1" in
    --test) MODE=test; ARG="${2:?--test needs a Catch2 filter}"; shift 2 ;;
    --app)  MODE=app;  ARG="${2:?--app needs a python script}";  shift 2 ;;
    --freq) FREQ="${2:?}"; shift 2 ;;
    --out)  OUT="${2:?}";  shift 2 ;;
    --keep) KEEP=1; shift ;;
    --) MODE=cmd; shift; break ;;
    -h|--help) sed -n '2,36p' "$0"; exit 0 ;;
    *) echo "profile: unknown option $1 (try --help)" >&2; exit 2 ;;
  esac
done
[ -n "$MODE" ] || { echo "profile: pick --test, --app, or -- <cmd>" >&2; exit 2; }

# perf_event_paranoid >= 2 blocks KERNEL samples for an unprivileged user but
# still allows profiling your own processes, which is all we want. Say so
# rather than letting the user read an empty kernel column as a broken run.
PARANOID="$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo '?')"
if [ "$PARANOID" != "?" ] && [ "$PARANOID" -ge 2 ]; then
  echo "profile: perf_event_paranoid=$PARANOID — user-space samples only."
  echo "         (kernel time shows as unresolved; that is the setting, not a bug)"
fi

[ "$KEEP" = 1 ] || rm -rf "$OUT"
mkdir -p "$OUT"

# Two choices worth stating, because the defaults are wrong here in ways that
# look like a working profile rather than a broken one.
#
# `-e cpu-clock`, not the default hardware cycles event. This is a HYBRID Intel
# machine (P-cores + E-cores are separate PMUs, cpu_core and cpu_atom), and
# plain `perf record` picks ONE of them: a 27 s run came back with 42 samples,
# because it only counted the slices that happened to land on an E-core. That
# reads as a thin but usable profile, not as an error. cpu-clock is a software
# event, samples every core the same way, and for "where does the time go" it
# answers the same question.
#
# `--call-graph dwarf`, not the default frame pointers: this tree is built -O2
# without -fno-omit-frame-pointer, so fp unwinding loses most of the stack.
# dwarf makes a bigger perf.data and is the only mode that produces a usable
# call graph here.
PERF_RECORD=(perf record -e cpu-clock -F "$FREQ" --call-graph dwarf
             -o "$OUT/perf.data")

case "$MODE" in
  test)
    BIN="$ROOT/build/tests/designer_tests"
    [ -x "$BIN" ] || { echo "profile: build designer_tests first" >&2; exit 1; }
    echo "profile: recording $BIN '$ARG'"
    WORKLOAD=("$BIN" "$ARG")
    ;;
  cmd)
    [ $# -gt 0 ] || { echo "profile: -- needs a command" >&2; exit 2; }
    echo "profile: recording $*"
    WORKLOAD=("$@")
    ;;
  app)
    [ -f "$ARG" ] || { echo "profile: no such script: $ARG" >&2; exit 1; }
    BIN="$ROOT/build/designer"
    [ -x "$BIN" ] || { echo "profile: build designer first" >&2; exit 1; }
    PORT="$("$ROOT/tools/scratch_port.py")" || {
      echo "profile: no scratch port free" >&2; exit 1; }
    echo "profile: recording $BIN --headless on port $PORT, driven by $ARG"
    ;;
esac

if [ "$MODE" = app ]; then
  # The app is the thing being profiled, so IT runs under perf and the driver
  # script runs beside it. The app is stopped with SIGTERM once the script
  # finishes: perf writes perf.data when its child exits, so a killed app is a
  # lost profile — never SIGKILL here.
  nix-shell -p perf --run \
    "${PERF_RECORD[*]} -- '$BIN' --headless --remote $PORT" &
  RECORD_PID=$!
  # Wait for the port rather than sleeping: a fixed sleep is either a race or
  # wasted profile.
  for _ in $(seq 1 100); do
    if python3 -c "import socket,sys; s=socket.socket()
s.settimeout(0.2)
sys.exit(0 if s.connect_ex(('127.0.0.1',$PORT))==0 else 1)"; then break; fi
    sleep 0.2
  done
  set +e
  nix-shell "$ROOT/shell.nix" --run \
    "DESIGNER_PORT=$PORT python3 '$ARG'"
  SCRIPT_STATUS=$?
  set -e
  # SIGTERM the APP, not the perf wrapper — killing the wrapper orphans the
  # child, the trap this repo has hit twice with xvfb-run.
  pkill -TERM -f "designer --headless --remote $PORT" || true
  wait "$RECORD_PID" 2>/dev/null || true
  [ "$SCRIPT_STATUS" = 0 ] || echo "profile: driver script exited $SCRIPT_STATUS"
else
  nix-shell -p perf --run "${PERF_RECORD[*]} -- ${WORKLOAD[*]@Q}"
fi

[ -s "$OUT/perf.data" ] || { echo "profile: no samples recorded" >&2; exit 1; }

echo "profile: writing reports to $OUT"
nix-shell -p perf --run "
  perf report -i '$OUT/perf.data' --stdio -g none --no-children \
    --sort=symbol --percent-limit 0.5 > '$OUT/flat.txt' 2>/dev/null
  perf report -i '$OUT/perf.data' --stdio -g graph,0.5,caller \
    > '$OUT/callers.txt' 2>/dev/null
  perf script -i '$OUT/perf.data' > '$OUT/stacks.txt' 2>/dev/null
"

echo
echo "=== hottest symbols, SELF time (>=0.5%) ===================="
# Truncated: a C++ template symbol runs to hundreds of characters and wraps the
# preview into unreadability. The untruncated names are in flat.txt.
grep -v '^#' "$OUT/flat.txt" | grep -v '^$' | head -25 | cut -c1-140
echo
echo "full reports:"
echo "  $OUT/flat.txt      what burns cycles"
echo "  $OUT/callers.txt   who asked for it (call graph)"
echo "  $OUT/stacks.txt    raw stacks — fold these for a flamegraph"
