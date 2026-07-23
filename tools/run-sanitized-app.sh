#!/usr/bin/env bash
# Run the WINDOWED app under ASan + UBSan with this project's dependency-noise
# workarounds. Run on your own display (DISPLAY set) — watch the terminal for
# "ERROR: AddressSanitizer" while you exercise modeling / sketch / assembly /
# pick / drag flows. Catches CPU-side use-after-free/OOB in the live app that the
# unit tests never hit; it does NOT see GPU memory (use Vulkan validation layers
# for that).
#
# Build first: nix-shell --run "cmake -B build-asan -G Ninja -DENABLE_SANITIZERS=ON && ninja -C build-asan designer"
# Run:         nix-shell --run "tools/run-sanitized-app.sh"
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bin="$here/build-asan/designer"
if [[ ! -x "$bin" ]]; then
  echo "sanitized app not found: $bin" >&2
  echo "build it: cmake -B build-asan -G Ninja -DENABLE_SANITIZERS=ON && ninja -C build-asan designer" >&2
  exit 1
fi
# detect_leaks=0: the Vulkan driver / OCC / ImGui leave thousands of still-reachable
# allocations at exit; without this ASan dumps a huge, useless leak report on quit.
# detect_container_overflow=0 + the UBSan suppressions: same dep-boundary reasons
# as the test wrapper (see tools/run-sanitized-tests.sh).
export ASAN_OPTIONS="detect_container_overflow=0:detect_leaks=0:halt_on_error=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}"
export UBSAN_OPTIONS="suppressions=$here/tools/ubsan-suppressions.txt:print_stacktrace=1:halt_on_error=0${UBSAN_OPTIONS:+:$UBSAN_OPTIONS}"
exec "$bin" "$@"
