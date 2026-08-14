#!/usr/bin/env bash
# gui-tests-xvfb — run the GUI test suite on a virtual X display.
#
# Why: on the real display, the hidden test window is throttled to ~1 fps by
# the X Present extension whenever the session is locked (or the window is
# fully occluded), so `ctest -R gui_smoke` times out. Xvfb has no compositor,
# no occlusion and no lock: the suite runs at full speed regardless of the
# desktop's state. The NVIDIA GPU renders and presents to Xvfb fine (verified
# RTX 4060, IMMEDIATE present mode, 43/43 in ~15 s).
#
# Usage:  ./tools/gui-tests-xvfb.sh              # gui_smoke (the ImGui suite)
#         ./tools/gui-tests-xvfb.sh -f fillet    # only matching test cases
#         ./tools/gui-tests-xvfb.sh -L gui       # all GUI-labeled ctest tests
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/build"

# One GPU suite at a time, machine-wide: concurrent runs (parallel agents /
# worktrees) contend for the GPU and flake. A solo run acquires the lock in
# microseconds; a contended one waits its turn instead of corrupting both.
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

if [ "${1:-}" = "-f" ]; then
  exec "$XVFB_RUN" -a -s '-screen 0 1600x1000x24' \
    nix-shell "$ROOT/shell.nix" --run \
    "cd '$ROOT/build' && ./designer --gui-tests --hidden --gui-test-filter '$2'"
fi
if [ "$#" -gt 0 ]; then
  # This script already provides the display and holds the GPU lock, so the
  # per-test tools/xvfb-exec.sh wrapper must not start a second Xvfb inside
  # this one (DESIGNER_GUI_LOCKED already tells it to skip the lock).
  exec "$XVFB_RUN" -a -s '-screen 0 1600x1000x24' \
    nix-shell "$ROOT/shell.nix" --run \
    "cd '$ROOT/build' && DESIGNER_NO_XVFB=1 ctest $* --output-on-failure"
fi
exec "$XVFB_RUN" -a -s '-screen 0 1600x1000x24' \
  nix-shell "$ROOT/shell.nix" --run "cd '$ROOT/build' && ./designer --gui-tests --hidden"
