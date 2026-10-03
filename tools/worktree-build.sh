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
#   # then land each in turn from main: tools/merge-branch.sh wtA-feature
#   # cherry-picks, builds, runs the suites and removes the worktree (a
#   # tree with submodules is one `git worktree remove` refuses; CLAUDE.md).
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
#
# They are cloned from MAIN's checkout, not the network: every submodule's
# URL is rewritten (url.<local>.insteadOf, for this one command only — no
# config is written, and a worktree's config is main's) to the same
# submodule in the main checkout, which already holds its objects. A clone
# from GitHub stalled a fresh worktree for minutes once. Only when main's
# copy lacks the pinned commit (main has not run `submodule update` since
# the pin moved) does it fall back to the network, and says so. Not
# --recursive: the one nested submodule (imgui_test_engine's own test
# suite's implot) is built by nothing here, main's checkout does not hold
# it, and recursing fetched it from GitHub every time.
MAIN="$(cd "$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)"
if [ ! -f "$ROOT/deps/imgui/imgui.cpp" ]; then
  echo "worktree-build: initializing submodules from $MAIN (fresh worktree)"
  LOCAL=(-c protocol.file.allow=always)
  while read -r key path; do
    name="${key#submodule.}"; name="${name%.path}"
    url="$(git -C "$ROOT" config -f .gitmodules --get "submodule.$name.url")"
    if [ -e "$MAIN/$path/.git" ]; then
      LOCAL+=(-c "url.$MAIN/$path.insteadOf=$url")
    fi
  done < <(git -C "$ROOT" config -f .gitmodules --get-regexp '\.path$')
  if ! git "${LOCAL[@]}" -C "$ROOT" submodule update --init; then
    echo "worktree-build: main's checkout lacks a pinned submodule commit;" \
         "fetching from the network"
    git -C "$ROOT" submodule update --init
  fi
fi

# The SAME configuration as main's build/, or no object is shared: a Debug
# worktree beside a Release main missed ccache on every file, and ran the
# suite slower and differently (watchdog_smoke failed in every Debug tree).
BUILD_TYPE="$(sed -n 's/^CMAKE_BUILD_TYPE:[A-Z]*=//p' "$MAIN/build/CMakeCache.txt" 2>/dev/null)"
BUILD_TYPE="${BUILD_TYPE:-Release}"
# Memory, not cores, is what runs out: about 1.5 GB a compiler. Four
# worktrees at one job per core got the machine's OOM killer to take the
# agent host itself.
JOBS="$("$ROOT/tools/build-jobs")"

echo "worktree-build: configuring + building [$TARGETS] in $ROOT/build ($BUILD_TYPE like main, -j$JOBS by free memory; ccache shared)"
"$ROOT/tools/nix-run" \
  "cmake -S '$ROOT' -B '$ROOT/build' -G Ninja -DCMAKE_BUILD_TYPE=$BUILD_TYPE >/dev/null && ninja -C '$ROOT/build' -j$JOBS $TARGETS"
echo "worktree-build: done."
if PORT="$("$ROOT/tools/scratch_port.py")"; then
  echo "  scratch port for this worktree: $PORT   (8799 is main's dev-watch)"
  echo "  tools/headless-preview.sh $PORT"
else
  echo "  no scratch port free in 8800-8899 — pick one yourself, but not 8799"
fi
echo "  tools/merge-branch.sh <branch>   # from main: lands it, removes this worktree"
