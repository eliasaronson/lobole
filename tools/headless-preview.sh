#!/usr/bin/env bash
# headless-preview — a Designer instance that renders WITHOUT a visible window
# or your display session, for agents and CI to drive + screenshot themselves.
#
# Default is `lobole --offscreen`: a real Vulkan renderer with no window and
# no display server at all — screenshot, render_still, render_sheet, render
# options and face selection all work, no Xvfb involved. The one thing it
# cannot do is DRIVE the GUI (ui_action needs an interface to click), so
# `--gui` keeps the old shape: the windowed app, invisible, under Xvfb.
#
# vs. dev-watch.sh: dev-watch runs a WINDOWED instance in YOUR session on the
# canonical port (8799 = the build you're looking at). This runs an isolated
# instance on a SCRATCH port — so parallel agents / git worktrees each get
# their own without colliding with your main instance. Convention:
#   dev-watch 8799  = main, the binary you review
#   headless-preview = agents/CI/worktrees, scratch ports
#
# Usage:
#   tools/headless-preview.sh [port]            # serve until Ctrl-C (default 8798)
#   tools/headless-preview.sh --start [port]    # background it, WAIT until it
#                                               # answers, print its pid, exit
#   tools/headless-preview.sh --gui [port]      # windowed under Xvfb: ui_action works
#   tools/headless-preview.sh --stop [port]     # stop it, by the pid it announced
#   tools/headless-preview.sh --render PROJECT OUT.png [port]
#                                               # one-shot: open, screenshot, exit
#
# Drive it with tools/designer_remote.py <port> ... or the MCP server
# (DESIGNER_PORT=<port>). Screenshots work exactly as against dev-watch.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/lobole"
[ -x "$BIN" ] || { echo "headless-preview: no binary at $BIN — build it first." >&2; exit 1; }

# Only the --gui path needs a display server; resolve Xvfb lazily so the
# default path works on a machine that has neither X nor xvfb-run.
resolve_xvfb() {
  if command -v xvfb-run >/dev/null; then
    XVFB_RUN=xvfb-run
  else
    # Fall back to nixpkgs. gui-tests-xvfb.sh does the same; the nested
    # nix-shell can fail (e.g. a GC'd/HOME-less env) and yield an empty
    # string, so guard it instead of exec'ing nothing ("exec: : not found").
    XVFB_RUN="$(nix-shell -p xvfb-run --run 'command -v xvfb-run' 2>/dev/null | tail -1)"
    if [ -z "$XVFB_RUN" ] || ! command -v "$XVFB_RUN" >/dev/null 2>&1; then
      echo "headless-preview: xvfb-run not found (not on PATH, and the nixpkgs" >&2
      echo "  fallback failed). Install it or run inside 'nix-shell -p xvfb-run'." >&2
      exit 1
    fi
  fi
}

# Block until the remote answers ping, so callers know it's drivable. Given
# the pid just launched, the answer must come from THAT process: a port handed
# out earlier (worktree-build.sh prints one) can be taken by another instance
# before this one binds it, and then the new app fails to listen while the old
# one answers every ping -- a parallel agent once drove another's instance for
# minutes that way. So: the launched process must still be alive, and the pid
# `ping` reports must be it.
wait_ready() {
  local port="$1" want="${2:-}" i out got
  for i in $(seq 1 100); do
    if [ -n "$want" ] && ! kill -0 "$want" 2>/dev/null; then
      echo "headless-preview: the instance for port $port exited before" \
        "answering (is the port taken?)" >&2
      return 1
    fi
    if out="$(python3 "$ROOT/tools/designer_remote.py" "$port" ping 2>/dev/null)"; then
      [ -z "$want" ] && return 0
      got="$(printf '%s' "$out" | python3 -c \
        'import json, sys; print(json.load(sys.stdin).get("pid", ""))')"
      [ "$got" = "$want" ] && return 0
      echo "headless-preview: port $port is answered by pid ${got:-unknown}," \
        "not the $want just started: another instance holds it" >&2
      kill "$want" 2>/dev/null || true
      return 1
    fi
    sleep 0.2
  done
  echo "headless-preview: instance on $port never became ready" >&2
  return 1
}

# A scratch instance must not write the HUMAN's preferences. Settings live in
# $XDG_CONFIG_HOME/lobole (docs/ui/settings.md), and an agent calling
# set_settings — or just toggling shadows to compare a render — would
# otherwise change the config of the windowed instance on 8799. Point it at a
# per-port scratch dir, seeded from the real one so the preview still LOOKS
# like the user's app. Same reasoning as the per-shard isolation in
# gui-tests-xvfb.sh. Override by exporting XDG_CONFIG_HOME yourself.
if [ -z "${DESIGNER_PREVIEW_CONFIG:-}" ]; then
  DESIGNER_PREVIEW_CONFIG="${TMPDIR:-/tmp}/designer-preview-config-$$"
  mkdir -p "$DESIGNER_PREVIEW_CONFIG/lobole"
  REAL_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/lobole"
  [ -f "$REAL_DIR/settings.json" ] &&
    cp "$REAL_DIR/settings.json" "$DESIGNER_PREVIEW_CONFIG/lobole/settings.json"
  # ...and the MACHINE store. A setup names its machine by name, so a preview
  # without one refuses every CAM job with "no machine called '6090'" against
  # a machine plainly on disk -- the same trap the headless RPC path fell into
  # (docs/cam/setup.md#machine-profiles), arriving from the other side. Copied, so
  # an agent editing a machine here cannot touch the shop's.
  #
  # settings.json and machines/ only, explicitly. `cp -r` of the whole
  # directory would take anything else kept there with it, and a scratch dir
  # under /tmp is not where credentials belong.
  [ -d "$REAL_DIR/machines" ] &&
    cp -r "$REAL_DIR/machines" "$DESIGNER_PREVIEW_CONFIG/lobole/"
fi
export XDG_CONFIG_HOME="$DESIGNER_PREVIEW_CONFIG"

# --stop <port>: kill the instance on that port, by the PID IT ANNOUNCED.
# There was no supported way to stop one, so the reflex is `pkill -f designer`
# or `pgrep -f "remote 8842"` — and BOTH match the shell that runs them, so an
# agent tearing down its own scratch instance kills its own session instead.
#
# The pid is found WHATEVER XDG_STATE_HOME the instance was started with: an
# instance started under a scratch state dir announces itself in THAT
# registry, and a --stop from a shell without the variable found nothing.
# So, in order, the first candidate that is a live lobole serving this
# port wins:
#   1. the pid file --start writes in a state-independent run dir;
#   2. the instance itself: `ping` reports its pid, no registry needed;
#   3. the registry under this shell's XDG_STATE_HOME, then the default one.
# A candidate counts only when /proc says it IS a lobole with `--remote
# PORT` on its command line, so a stale file naming a recycled pid never
# kills a stranger. The registry it announced in is read from the process's
# own environment, and cleared with the pid file.
RUN_DIR="${XDG_RUNTIME_DIR:-${TMPDIR:-/tmp}}/designer-preview"
is_instance() { # pid port
  local pid="$1" port="$2" cmd
  [ -n "$pid" ] && [ -r "/proc/$pid/cmdline" ] || return 1
  cmd="$(tr '\0' ' ' <"/proc/$pid/cmdline")"
  case "$cmd" in
    *lobole*" --remote $port "*) return 0 ;;
  esac
  return 1
}
reg_pid() {
  sed -n 's/.*"pid"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p' "$1" 2>/dev/null
}
ping_pid() {
  python3 "$ROOT/tools/designer_remote.py" "$1" ping 2>/dev/null |
    python3 -c 'import json, sys; print(json.load(sys.stdin).get("pid", ""))' \
      2>/dev/null
}
if [ "${1:-}" = "--stop" ]; then
  PORT="${2:-8798}"
  PID=""
  for cand in "$(cat "$RUN_DIR/$PORT.pid" 2>/dev/null)" "$(ping_pid "$PORT")" \
      "$(reg_pid "${XDG_STATE_HOME:-$HOME/.local/state}/lobole/instances/$PORT.json")" \
      "$(reg_pid "$HOME/.local/state/lobole/instances/$PORT.json")"; do
    if is_instance "$cand" "$PORT"; then PID="$cand"; break; fi
  done
  if [ -z "$PID" ]; then
    # The pid file is the most reliable of the four candidates, so it is kept
    # when the stop FAILS: deleting it here used to remove the one record that
    # would have found the instance on a later attempt, which turned a stop
    # that matched nothing into a stop that could never match again.
    echo "headless-preview: no lobole instance serving port $PORT" >&2
    exit 1
  fi
  rm -f "$RUN_DIR/$PORT.pid"
  # Where it announced itself: its own XDG_STATE_HOME, read before it dies.
  ITS_STATE="$(tr '\0' '\n' <"/proc/$PID/environ" 2>/dev/null |
    sed -n 's/^XDG_STATE_HOME=//p')"
  ITS_REG="${ITS_STATE:-$HOME/.local/state}/lobole/instances/$PORT.json"
  # SIGTERM, then make sure. An instance in the middle of a FEM
  # factorization shuts down only once the factorization ends (it has no
  # interruption points), and "stopped" printed after a bare kill left one
  # holding 9 GB for an hour. So wait for it to go, and SIGKILL it if it has
  # not within the grace period.
  kill "$PID"
  for _ in $(seq 50); do
    kill -0 "$PID" 2>/dev/null || break
    sleep 0.2
  done
  if kill -0 "$PID" 2>/dev/null; then
    kill -KILL "$PID"
    echo "headless-preview: $PID ignored SIGTERM for 10 s; killed it" >&2
  fi
  echo "headless-preview: stopped $PID on port $PORT"
  [ "$(reg_pid "$ITS_REG")" = "$PID" ] && rm -f "$ITS_REG"
  exit 0
fi

# --start <port>: the same offscreen instance, BACKGROUNDED, waited for, and
# announced. Without it every caller writes `nohup ... &` followed by a sleep,
# and the sleep is always a guess -- one dogfooding round spent six and a half
# minutes sleeping 16 s at a time past an instance that is drivable in about
# one. `wait_ready` below is the answer and was reachable only from --render.
if [ "${1:-}" = "--start" ]; then
  PORT="${2:-8798}"
  LOG="${DESIGNER_PREVIEW_LOG:-${TMPDIR:-/tmp}/designer-preview-$PORT.log}"
  # Its own session, so it survives the shell that started it and takes no
  # SIGHUP when that shell goes away. `nohup` is the fallback where setsid is
  # not installed; both leave the app parented to init, which is what --stop
  # (and the instance registry it reads) already assumes.
  if command -v setsid >/dev/null; then
    setsid "$BIN" --offscreen --remote "$PORT" >"$LOG" 2>&1 </dev/null &
  else
    nohup "$BIN" --offscreen --remote "$PORT" >"$LOG" 2>&1 </dev/null &
  fi
  # setsid and nohup both exec in place here (a background job is not a
  # process-group leader), so $! is the app itself.
  APP=$!
  wait_ready "$PORT" "$APP" || { echo "headless-preview: see $LOG" >&2; exit 1; }
  # The pid file --stop reads first: outside any XDG_STATE_HOME, so a
  # --stop from a shell that does not share this one's finds it.
  mkdir -p "$RUN_DIR" && echo "$APP" >"$RUN_DIR/$PORT.pid"
  echo "headless-preview: ready on $PORT (pid $APP, log $LOG)"
  echo "headless-preview: stop it with $0 --stop $PORT"
  exit 0
fi

if [ "${1:-}" = "--render" ]; then
  PROJECT="$2"; OUT="$3"; PORT="${4:-8798}"
  # One-shot: serve offscreen, drive, tear down. The app dies with the trap.
  "$BIN" --offscreen --remote "$PORT" &
  APP=$!
  trap 'kill "$APP" 2>/dev/null || true' EXIT
  wait_ready "$PORT" "$APP"
  python3 "$ROOT/tools/designer_remote.py" "$PORT" open_project "{\"path\": \"$PROJECT\"}" >/dev/null
  python3 "$ROOT/tools/designer_remote.py" "$PORT" fit_view >/dev/null 2>&1 || true
  python3 "$ROOT/tools/designer_remote.py" "$PORT" screenshot "{\"path\": \"$OUT\"}" >/dev/null
  echo "headless-preview: wrote $OUT"
  exit 0
fi

if [ "${1:-}" = "--gui" ]; then
  PORT="${2:-8798}"
  resolve_xvfb
  # GLFW prefers Wayland whenever WAYLAND_DISPLAY is set, so from a shell in
  # a Wayland session the "Xvfb" instance opened a hidden window on the
  # user's own compositor instead, never presented, and stalled.
  unset WAYLAND_DISPLAY
  echo "headless-preview: starting WINDOWED instance on --remote $PORT (Xvfb; ui_action works)"
  exec "$XVFB_RUN" -a -s '-screen 0 1280x720x24' \
    nix-shell "$ROOT/shell.nix" --run "'$BIN' --hidden --remote $PORT"
fi

PORT="${1:-8798}"
echo "headless-preview: starting offscreen instance on --remote $PORT (no display server)"
exec "$BIN" --offscreen --remote "$PORT"
