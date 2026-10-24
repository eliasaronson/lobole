#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# profile — where the CPU time actually goes.
#
# An occasional audit tool, like tools/coverage.sh and ci-local.sh --asan: NOT
# part of CI, and not something to run after every edit.
#
# Why it exists. Every slow case found before the harness
# (docs/performance/README.md) was found by hand-placing
# std::chrono::steady_clock around a phase somebody already
# suspected — the 259 s open, the 5.9 s assembly resync, the STEP file parsed
# twice, the 7 s adaptive pocket. That works, and where the suspicion was
# right it left behind a permanent breakdown worth keeping (SyncAssemblyRenderers
# logs its phases so the next regression is READ rather than re-profiled).
#
# What it cannot do is find the cost you did not think to time. A sampling
# profiler starts from the answer instead of from a hypothesis, and nothing in
# this repo had one (docs/architecture.md#tests).
#
# perf is deliberately NOT in shell.nix: it is coupled to the running kernel,
# and a dev shell that fails to evaluate on someone else's machine costs more
# than a tool they can fetch in one line. This script fetches it itself.
#
# Usage:
#   tools/profile.sh --test '[cam]'            # a Catch2 selection
#   tools/profile.sh --app scratch/build_x.py  # the app, driven by a script
#   tools/profile.sh -- build/lobole --headless --remote 8901
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
#
# `--user-callchains`: at perf_event_paranoid 2 a call graph that asks for
# the KERNEL's chain too is refused outright ("Failure to open any events
# for recording"), even with a plain `perf record -g`. User space is all an
# unprivileged profile can see anyway.
#
# perf is resolved ONCE to its store path, so the recorder is a plain child of
# this script rather than of a nix-shell wrapper: --app has to own the
# processes it starts (below).
PERF="$(nix-shell -p perf --run 'command -v perf')"
[ -x "$PERF" ] || { echo "profile: could not fetch perf" >&2; exit 1; }
PERF_RECORD=("$PERF" record -e cpu-clock -F "$FREQ" --call-graph dwarf
             --user-callchains -o "$OUT/perf.data")

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
    BIN="$ROOT/build/lobole"
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
  #
  # It is stopped by its PID, never by a pattern: `pkill -f "designer ...
  # $PORT"` matches anything with that text on its command line, the shell
  # running it included (CLAUDE.md, "kill by a pid"). The pid is the one the
  # instance reports in `ping`, accepted only as perf's own child: a port
  # somebody else bound between scratch_port.py and our launch must not hand
  # us THEIR pid.
  #
  # And nothing is left behind, whichever process goes first:
  #   - perf: the app runs --exit-with-parent and perf is its parent, so the
  #     kernel sends it SIGTERM (src/app/ParentWatch.h);
  #   - this script, SIGKILL included: perf runs under `setpriv --pdeathsig
  #     TERM`, so it stops, writes its data and takes the app with it; on any
  #     other exit the EXIT trap stops the app by pid;
  #   - the driver: reported, and the app stopped as on success.
  PDEATH=()
  if command -v setpriv >/dev/null; then
    PDEATH=(setpriv --pdeathsig TERM --)
  else
    echo "profile: no setpriv — a SIGKILLed profile.sh would leave perf running"
  fi
  #
  # Its config and state are private to the profile, and it never
  # autosaves: an instance on the user's state directory leaves its unsaved
  # document's recovery copy beside theirs (unsaved-<session>.autosave.json),
  # and its opens land in their recently-opened list. Its port is still claimed
  # machine-wide (/tmp/lobole-ports-<uid>), whatever its state directory.
  mkdir -p "$OUT/app-config" "$OUT/app-state"
  XDG_CONFIG_HOME="$OUT/app-config" XDG_STATE_HOME="$OUT/app-state" \
    LOBOLE_NO_DIALOG=1 \
    "${PDEATH[@]}" "${PERF_RECORD[@]}" -- \
    "$BIN" --headless --remote "$PORT" --exit-with-parent --autosave 0 &
  RECORD_PID=$!
  APP_PID=""
  stop_app() {
    if [ -n "$APP_PID" ]; then
      kill -TERM "$APP_PID" 2>/dev/null || true
    else # it never answered: perf (and with it the app) is what to stop
      kill -TERM "$RECORD_PID" 2>/dev/null || true
    fi
    wait "$RECORD_PID" 2>/dev/null || true
  }
  trap stop_app EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
  # Wait for the instance to answer rather than sleeping: a fixed sleep is
  # either a race or wasted profile. Gives up when perf does.
  ping_pid() {
    PYTHONPATH="$ROOT/python" python3 -m lobole.client "$PORT" ping 2>/dev/null |
      python3 -c 'import json, sys; print(json.load(sys.stdin).get("pid", ""))' \
        2>/dev/null || true
  }
  for _ in $(seq 1 150); do
    kill -0 "$RECORD_PID" 2>/dev/null || break
    CAND="$(ping_pid)"
    if [ -n "$CAND" ] && [ "$(sed -n 's/^PPid:[[:space:]]*//p' \
        "/proc/$CAND/status" 2>/dev/null)" = "$RECORD_PID" ]; then
      APP_PID="$CAND"
      break
    fi
    sleep 0.2
  done
  if [ -z "$APP_PID" ]; then
    echo "profile: no instance of ours answered on port $PORT" >&2
    exit 1
  fi
  echo "profile: app pid $APP_PID, under perf $RECORD_PID"
  set +e
  nix-shell "$ROOT/shell.nix" --run \
    "LOBOLE_PORT=$PORT python3 '$ARG'"
  SCRIPT_STATUS=$?
  set -e
  # SIGTERM the APP, not perf: perf would stop recording and leave the app
  # to --exit-with-parent, and the profile would end before the app did.
  stop_app
  trap - EXIT INT TERM
  [ "$SCRIPT_STATUS" = 0 ] || echo "profile: driver script exited $SCRIPT_STATUS"
else
  "${PERF_RECORD[@]}" -- "${WORKLOAD[@]}"
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
# awk, not head: head exits after 25 lines, grep dies of SIGPIPE writing
# the rest, and under pipefail the script ended there with 141 on a
# profile that had worked, before printing where the reports are.
grep -v '^#' "$OUT/flat.txt" | grep -v '^$' | awk 'NR <= 25' | cut -c1-140
echo
echo "full reports:"
echo "  $OUT/flat.txt      what burns cycles"
echo "  $OUT/callers.txt   who asked for it (call graph)"
echo "  $OUT/stacks.txt    raw stacks — fold these for a flamegraph"
