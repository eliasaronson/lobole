#!/usr/bin/env bash
# headless-preview — a Designer instance that renders WITHOUT a visible window
# or your display session, for agents and CI to drive + screenshot themselves.
#
# Default is `designer --offscreen`: a real Vulkan renderer with no window and
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
BIN="$ROOT/build/designer"
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
# $XDG_CONFIG_HOME/designer (docs/ui/settings.md), and an agent calling
# set_settings — or just toggling shadows to compare a render — would
# otherwise change the config of the windowed instance on 8799. Point it at a
# per-port scratch dir, seeded from the real one so the preview still LOOKS
# like the user's app. Same reasoning as the per-shard isolation in
# gui-tests-xvfb.sh. Override by exporting XDG_CONFIG_HOME yourself.
if [ -z "${DESIGNER_PREVIEW_CONFIG:-}" ]; then
  DESIGNER_PREVIEW_CONFIG="${TMPDIR:-/tmp}/designer-preview-config-$$"
  mkdir -p "$DESIGNER_PREVIEW_CONFIG/designer"
  REAL_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/designer"
  [ -f "$REAL_DIR/settings.json" ] &&
    cp "$REAL_DIR/settings.json" "$DESIGNER_PREVIEW_CONFIG/designer/settings.json"
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
    cp -r "$REAL_DIR/machines" "$DESIGNER_PREVIEW_CONFIG/designer/"
fi
export XDG_CONFIG_HOME="$DESIGNER_PREVIEW_CONFIG"

# --stop <port>: kill the instance on that port, by the PID IT ANNOUNCED.
# There was no supported way to stop one, so the reflex is `pkill -f designer`
# or `pgrep -f "remote 8842"` — and BOTH match the shell that runs them, so an
# agent tearing down its own scratch instance kills its own session instead.
# The instance registry has the pid; use it.
if [ "${1:-}" = "--stop" ]; then
  PORT="${2:-8798}"
  REG="${XDG_STATE_HOME:-$HOME/.local/state}/designer/instances/$PORT.json"
  [ -f "$REG" ] || { echo "headless-preview: nothing announced on $PORT" >&2; exit 1; }
  PID="$(sed -n 's/.*"pid"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p' "$REG")"
  [ -n "$PID" ] || { echo "headless-preview: no pid in $REG" >&2; exit 1; }
  if kill "$PID" 2>/dev/null; then
    echo "headless-preview: stopped $PID on port $PORT"
  else
    echo "headless-preview: $PID is already gone; clearing $PORT" >&2
  fi
  rm -f "$REG"
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
  REG="${XDG_STATE_HOME:-$HOME/.local/state}/designer/instances/$PORT.json"
  PID="$(sed -n 's/.*"pid"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p' "$REG" 2>/dev/null)"
  echo "headless-preview: ready on $PORT (pid ${PID:-unknown}, log $LOG)"
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
