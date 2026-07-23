#!/usr/bin/env bash
# Run the headless unit tests under ASan + UBSan with this project's
# dependency-noise workarounds baked in:
#   - detect_container_overflow=0  : our std containers cross into non-instrumented
#     OpenCASCADE, which otherwise false-positives as container-overflow.
#   - UBSan suppressions           : silence the benign OCC alignment reports
#     (see tools/ubsan-suppressions.txt) while keeping every real check.
#
# Build first:  nix-shell --run "cmake -B build-asan -G Ninja -DENABLE_SANITIZERS=ON && ninja -C build-asan designer_tests"
# Run:          nix-shell --run "tools/run-sanitized-tests.sh [catch2-filter...]"
# e.g.          nix-shell --run "tools/run-sanitized-tests.sh '[thread]'"
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bin="$here/build-asan/tests/designer_tests"
if [[ ! -x "$bin" ]]; then
  echo "sanitized test binary not found: $bin" >&2
  echo "build it: cmake -B build-asan -G Ninja -DENABLE_SANITIZERS=ON && ninja -C build-asan designer_tests" >&2
  exit 1
fi
export ASAN_OPTIONS="detect_container_overflow=0:halt_on_error=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}"
export UBSAN_OPTIONS="suppressions=$here/tools/ubsan-suppressions.txt:print_stacktrace=1:halt_on_error=0${UBSAN_OPTIONS:+:$UBSAN_OPTIONS}"
exec "$bin" "$@"
