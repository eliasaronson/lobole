#!/usr/bin/env bash
# headless-preview — a Designer instance that renders WITHOUT a visible window
# or your display session, for agents and CI to drive + screenshot themselves.
#
# The trick: `designer --hidden` already renders the real scene (materials,
# shadows, SSAO) to an offscreen framebuffer — `screenshot` reads it back.
# It only needs *a* display server for the (invisible) window it creates, so we
# run it under Xvfb. No GPU window is shown; the NVIDIA GPU renders to Xvfb
# fine (the same path the GUI test suite uses).
#
# vs. dev-watch.sh: dev-watch runs a WINDOWED instance in YOUR session on the
# canonical port (8799 = the build you're looking at). This runs an isolated
# offscreen instance on a SCRATCH port — so parallel agents / git worktrees
# each get their own without colliding with your main instance. Convention:
#   dev-watch 8799  = main, the binary you review
#   headless-preview = agents/CI/worktrees, scratch ports
#
# Usage:
#   tools/headless-preview.sh [port]            # serve until Ctrl-C (default 8798)
#   tools/headless-preview.sh --render PROJECT OUT.png [port]
#                                               # one-shot: open, screenshot, exit
#
# Drive it with tools/designer_remote.py <port> ... or the MCP server
# (DESIGNER_PORT=<port>). Screenshots work exactly as against dev-watch.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/designer"
[ -x "$BIN" ] || { echo "headless-preview: no binary at $BIN — build it first." >&2; exit 1; }

if command -v xvfb-run >/dev/null; then
  XVFB_RUN=xvfb-run
else
  # Fall back to nixpkgs. gui-tests-xvfb.sh does the same; the nested nix-shell
  # can fail (e.g. a GC'd/HOME-less env) and yield an empty string, so guard it
  # instead of exec'ing nothing ("exec: : not found").
  XVFB_RUN="$(nix-shell -p xvfb-run --run 'command -v xvfb-run' 2>/dev/null | tail -1)"
  if [ -z "$XVFB_RUN" ] || ! command -v "$XVFB_RUN" >/dev/null 2>&1; then
    echo "headless-preview: xvfb-run not found (not on PATH, and the nixpkgs" >&2
    echo "  fallback failed). Install it or run inside 'nix-shell -p xvfb-run'." >&2
    exit 1
  fi
fi

# Block until the remote answers ping, so callers know it's drivable.
wait_ready() {
  local port="$1" i
  for i in $(seq 1 100); do
    if python3 "$ROOT/tools/designer_remote.py" "$port" ping >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.2
  done
  echo "headless-preview: instance on $port never became ready" >&2
  return 1
}

if [ "${1:-}" = "--render" ]; then
  PROJECT="$2"; OUT="$3"; PORT="${4:-8798}"
  # One-shot: serve, drive, tear down. The Xvfb + app die with the subshell.
  "$XVFB_RUN" -a -s '-screen 0 1280x720x24' \
    nix-shell "$ROOT/shell.nix" --run "'$BIN' --hidden --remote $PORT" &
  APP=$!
  trap 'kill "$APP" 2>/dev/null || true' EXIT
  wait_ready "$PORT"
  python3 "$ROOT/tools/designer_remote.py" "$PORT" open_project "{\"path\": \"$PROJECT\"}" >/dev/null
  python3 "$ROOT/tools/designer_remote.py" "$PORT" fit_view >/dev/null 2>&1 || true
  python3 "$ROOT/tools/designer_remote.py" "$PORT" screenshot "{\"path\": \"$OUT\"}" >/dev/null
  echo "headless-preview: wrote $OUT"
  exit 0
fi

PORT="${1:-8798}"
echo "headless-preview: starting offscreen instance on --remote $PORT (Xvfb)"
exec "$XVFB_RUN" -a -s '-screen 0 1280x720x24' \
  nix-shell "$ROOT/shell.nix" --run "'$BIN' --hidden --remote $PORT"
