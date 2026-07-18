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
# Usage:  ./tools/gui-tests-xvfb.sh            # gui_smoke (the ImGui suite)
#         ./tools/gui-tests-xvfb.sh -L gui     # all GUI-labeled ctest tests
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/build"

if command -v xvfb-run >/dev/null; then
  XVFB_RUN=xvfb-run
else
  XVFB_RUN="$(nix-shell -p xvfb-run --run 'which xvfb-run' | tail -1)"
fi

if [ "$#" -gt 0 ]; then
  exec "$XVFB_RUN" -a -s '-screen 0 1600x1000x24' \
    nix-shell "$ROOT/shell.nix" --run "cd '$ROOT/build' && ctest $* --output-on-failure"
fi
exec "$XVFB_RUN" -a -s '-screen 0 1600x1000x24' \
  nix-shell "$ROOT/shell.nix" --run "cd '$ROOT/build' && ./designer --gui-tests --hidden"
