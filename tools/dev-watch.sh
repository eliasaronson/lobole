#!/usr/bin/env bash
# dev-watch — keep a fresh-build Designer running on a fixed --remote port.
#
# Run this ONCE in your own GPU/display session (a normal terminal, or your
# window-manager autostart). It launches the windowed app on a fixed port and
# relaunches it whenever `build/designer` changes (i.e. after every rebuild),
# so an assistant — or you, via tools/designer_remote.py — can always drive and
# screenshot the CURRENT build without you restarting anything.
#
# Why a windowed instance and not --headless: --headless has no renderer, so it
# cannot take screenshots. The app must own a real swapchain, which means it has
# to run in your display session (not a sandboxed background process).
#
# This is relaunch-on-rebuild, not hot code reload (impossible for a compiled
# Vulkan app) — but it gives the same "always current" behaviour for driving.
#
# Usage:  ./tools/dev-watch.sh [port]   (default port 8799)
#         Ctrl-C to stop.
set -u

PORT="${1:-8799}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/designer"

if [ ! -x "$BIN" ]; then
  echo "dev-watch: no binary at $BIN — build it first (cmake --build build)." >&2
  exit 1
fi

LOG="${DEV_WATCH_LOG:-/tmp/designer-dev.log}"

# DEV_WATCH_VALIDATE=1 forces the Vulkan validation layer on (with
# synchronization + GPU-assisted validation) so device-lost / out-of-bounds
# GPU bugs are named in $LOG. Run this INSIDE `nix-shell` so VK_LAYER_PATH is
# set (shell.nix exports it); this block also derives it from nixpkgs as a
# fallback. Validation is slow — only for debugging, not day-to-day.
if [ "${DEV_WATCH_VALIDATE:-0}" = "1" ]; then
  if [ -z "${VK_LAYER_PATH:-}" ]; then
    VK_LAYER_PATH="$(nix-build --no-out-link '<nixpkgs>' -A vulkan-validation-layers 2>/dev/null)/share/vulkan/explicit_layer.d"
    export VK_LAYER_PATH
  fi
  export VK_INSTANCE_LAYERS="VK_LAYER_KHRONOS_validation"
  export VK_LAYER_ENABLES="VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT"
  echo "dev-watch: VALIDATION ON (VK_LAYER_PATH=$VK_LAYER_PATH)"
fi

stamp() { stat -c %Y "$BIN" 2>/dev/null || echo 0; }
launch() {
  # Tee app output to $LOG (readable by tooling/an assistant) and the terminal.
  # `stdbuf -oL -eL` keeps the log line-buffered so a crash's last lines flush.
  echo "===== launch $(date '+%H:%M:%S') pid-to-follow on --remote $PORT =====" >> "$LOG"
  stdbuf -oL -eL "$BIN" --remote "$PORT" > >(tee -a "$LOG") 2>&1 &
  APP=$!
  echo "dev-watch: Designer running (pid $APP) on --remote $PORT (log: $LOG)"
}

echo "dev-watch: watching $BIN; relaunches on rebuild. Ctrl-C to stop."
echo "dev-watch: app log -> $LOG"
launch
LAST="$(stamp)"
trap 'echo; echo "dev-watch: stopping"; kill "$APP" 2>/dev/null; exit 0' INT TERM

while true; do
  sleep 1
  NOW="$(stamp)"
  if [ "$NOW" != "$LAST" ]; then
    # The binary changed. Wait a beat for the link to finish, then relaunch.
    sleep 1
    LAST="$(stamp)"
    echo "dev-watch: rebuild detected — relaunching"
    kill "$APP" 2>/dev/null
    wait "$APP" 2>/dev/null
    launch
  elif ! kill -0 "$APP" 2>/dev/null; then
    # App exited on its own (you closed the window, or it crashed) — bring it
    # back so the port stays live for the next connection.
    echo "dev-watch: app exited — relaunching"
    launch
  fi
done
