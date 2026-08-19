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
# The shared options (see tools/sanitizer-env.sh), plus two the interactive
# app needs and the test runs do not:
#   detect_leaks=0   the Vulkan driver / OCC / ImGui leave thousands of
#     still-reachable allocations at exit; without this, quitting dumps a huge,
#     useless report. (The HEADLESS suite keeps leak detection on — no driver.)
#   halt_on_error=0  keep the session alive after the first report, so one
#     find does not end the exploration.
export ASAN_OPTIONS="detect_leaks=0:halt_on_error=0"
export UBSAN_OPTIONS="halt_on_error=0"
source "$here/tools/sanitizer-env.sh"
exec "$bin" "$@"
