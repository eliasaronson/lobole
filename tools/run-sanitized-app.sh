#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

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
bin="$here/build-asan/lobole"
if [[ ! -x "$bin" ]]; then
  echo "sanitized app not found: $bin" >&2
  echo "build it: cmake -B build-asan -G Ninja -DENABLE_SANITIZERS=ON && ninja -C build-asan designer" >&2
  exit 1
fi
# The binary carries its own options (src/common/SanitizerDefaults.cpp), so
# this adds only what is specific to an interactive session:
#   halt_on_error=0  keep going after the first report, so one find does not
#                    end the exploration.
#
# Leak detection is deliberately ON. It used to be off here, on the assumption
# that the Vulkan driver would bury the output — it does not: a real session
# exits with a report you can read, and turning it off is what kept a leak of
# a vertex/index buffer per body per render invisible for as long as the ray
# tracer has existed. If your driver does swamp it, run with
# ASAN_OPTIONS=detect_leaks=0 for that session rather than for everyone.
export ASAN_OPTIONS="halt_on_error=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}"
export UBSAN_OPTIONS="halt_on_error=0${UBSAN_OPTIONS:+:$UBSAN_OPTIONS}"
exec "$bin" "$@"
