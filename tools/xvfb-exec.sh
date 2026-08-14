#!/usr/bin/env bash
# xvfb-exec — run a command on a virtual X display, serialized on the GPU.
#
# The gui-labeled ctest entries (gui_smoke, remote_smoke) drive a real
# renderer. Run against the developer's own display they stall whenever the
# screen locks or the window is fully occluded: the swapchain presents at
# about 1 fps in FIFO mode, so a suite that takes 15 seconds takes longer than
# its timeout and "fails" without a single real failure. Xvfb has no
# compositor and no lock screen, and the NVIDIA GPU renders and presents to it
# fine — the same reasoning as tools/gui-tests-xvfb.sh, which stays as the
# interactive entry point.
#
# The flock is the other half: two GUI suites at once (ctest -j, or a parallel
# agent in a worktree) contend for the GPU and flake each other.
#
# Usage:  tools/xvfb-exec.sh <command> [args...]
set -euo pipefail

LOCK=/tmp/designer-gui-gpu.lock

if [ "${DESIGNER_GUI_LOCKED:-}" != "1" ]; then
  export DESIGNER_GUI_LOCKED=1
  exec flock "$LOCK" "$0" "$@"
fi

if [ -n "${DESIGNER_NO_XVFB:-}" ]; then
  exec "$@" # opt out: run against whatever DISPLAY is set
fi

if command -v xvfb-run >/dev/null; then
  XVFB_RUN=xvfb-run
else
  # The nix shell does not carry xvfb-run; fetch it the same way
  # gui-tests-xvfb.sh does. Falling back to the real display is better than
  # not running at all.
  XVFB_RUN="$(nix-shell -p xvfb-run --run 'which xvfb-run' 2>/dev/null | tail -1 || true)"
  if [ -z "$XVFB_RUN" ] || [ ! -x "$XVFB_RUN" ]; then
    exec "$@"
  fi
fi

exec "$XVFB_RUN" -a -s '-screen 0 1600x1000x24' "$@"
