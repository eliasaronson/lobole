#!/usr/bin/env bash
# worktree-build — bootstrap a build in a git worktree, for running agents /
# workstreams in parallel on isolated copies of the repo.
#
# Parallel-worktree conventions (why this is cheap here):
#   - ccache is shared machine-wide (~/.cache/ccache), so a fresh worktree's
#     first `ninja` reuses cached object files — only cmake-configure + linking
#     are new cost, not a from-scratch compile.
#   - The GPU GUI suite self-serializes (tools/gui-tests-xvfb.sh flock), so
#     concurrent suites across worktrees don't flake.
#   - Ports: the human's dev-watch owns 8799 (the reviewed build); each
#     worktree drives its own render instance on a SCRATCH port, and this
#     script PICKS one and prints the command. It used to say "pass a distinct
#     one per worktree" — a value a human had to hold, in a repo that
#     everywhere else refuses to let one (tools/scratch_port.py asks the
#     kernel and the announced instances, and never hands out 8799).
#   - Integration is SERIAL: each worktree branch merges to main one at a time
#     with full suites between merges. Parallel *implementation*, sequential
#     *integration*. Only worth it when the work is genuinely file-disjoint
#     (different subsystems) — overlapping edits just move the cost to merge.
#
# Typical parallel setup from the main checkout:
#   git worktree add ../designer-wtA -b wtA-feature
#   git worktree add ../designer-wtB -b wtB-feature
#   (cd ../designer-wtA && tools/worktree-build.sh)   # or let an agent run it
#   (cd ../designer-wtB && tools/worktree-build.sh)
#   ... run an agent in each (Agent tool: isolation:"worktree" does the
#       worktree part automatically; this script gives it a working build/) ...
#   # then merge wtA to main, run suites, merge wtB, run suites.
#   git worktree remove ../designer-wtA   (when done)
#
# Usage:  tools/worktree-build.sh [--tests]   (run from inside the worktree)
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TARGETS="designer"
[ "${1:-}" = "--tests" ] && TARGETS="designer designer_tests"

# A fresh `git worktree` does NOT populate submodules (deps/imgui,
# deps/nativefiledialog, deps/imgui_test_engine) — cmake then fails at
# configure. Init them into this worktree first. Cheap when already present.
if [ ! -f "$ROOT/deps/imgui/imgui.cpp" ]; then
  echo "worktree-build: initializing submodules (fresh worktree)"
  git -C "$ROOT" submodule update --init --recursive
fi

echo "worktree-build: configuring + building [$TARGETS] in $ROOT/build (ccache shared)"
nix-shell "$ROOT/shell.nix" --run \
  "cmake -S '$ROOT' -B '$ROOT/build' -G Ninja >/dev/null && ninja -C '$ROOT/build' $TARGETS"
echo "worktree-build: done."
if PORT="$("$ROOT/tools/scratch_port.py")"; then
  echo "  scratch port for this worktree: $PORT   (8799 is main's dev-watch)"
  echo "  tools/headless-preview.sh $PORT"
else
  echo "  no scratch port free in 8800-8899 — pick one yourself, but not 8799"
fi
echo "  git worktree remove '$ROOT'          # when this branch is merged"
