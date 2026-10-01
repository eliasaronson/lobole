#!/usr/bin/env bash
# coverage — which of our own code the headless suite actually executes.
#
# An occasional audit build, like tools/ci-local.sh --asan and -DENABLE_ANALYZER:
# NOT part of CI. Instrumented + unoptimised, so the suite runs several times
# slower than the normal ~130 s — budget 20-40 minutes for a full pass.
#
# Why bother when the suite is green: green says the tests that exist pass. It
# says nothing about behaviour no test reaches, which is where this project's
# bugs have actually lived — a silent enum substitution, a dropped bool, an RPC
# that reported success on a broken model. Coverage is the only tool that finds
# the third kind: code nothing has ever run.
#
# Reading the output: a 0%-covered FUNCTION in src/core is the interesting
# signal. Low percentages in src/render or src/ui are expected — those need a
# GPU and are verified by the GUI suite and screenshots instead (see
# docs/architecture.md#tests).
#
# Usage:
#   tools/coverage.sh [builddir]      # default: build-coverage/
#   tools/coverage.sh --report-only   # re-analyse an existing run
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Self-wrap in nix-shell, like tools/test-fast.sh: cmake outside it cannot find
# glfw3/OCC/vulkan and the run dies at configure. This script used to expect the
# caller to have entered one, which is a rule nothing enforced and the docs did
# not state.
if [ -z "${IN_NIX_SHELL:-}" ]; then
  exec nix-shell "$ROOT/shell.nix" --run "'$0' $*"
fi

BUILD="${1:-$ROOT/build-coverage}"
[ "${1:-}" = "--report-only" ] && BUILD="$ROOT/build-coverage"

if [ "${1:-}" != "--report-only" ]; then
  echo "coverage: configuring $BUILD"
  cmake -S "$ROOT" -B "$BUILD" -GNinja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS='--coverage -O0 -g1' \
    -DCMAKE_C_FLAGS='--coverage -O0 -g1' \
    -DCMAKE_EXE_LINKER_FLAGS='--coverage' \
    -DCMAKE_CXX_COMPILER_LAUNCHER= -DCMAKE_C_COMPILER_LAUNCHER= >/dev/null

  echo "coverage: building (no ccache — instrumented objects differ)"
  ninja -C "$BUILD" designer designer_tests

  echo "coverage: running the headless suite (slow: -O0 + instrumentation)"
  "$ROOT/tools/ctest" --test-dir "$BUILD" -LE gui -j"$(nproc)" || true
fi

echo "coverage: analysing"
python3 "$ROOT/tools/coverage_report.py" "$BUILD" "$ROOT"
