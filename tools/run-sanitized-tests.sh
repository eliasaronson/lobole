#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# Run the headless unit tests under ASan + UBSan with this project's
# dependency-noise workarounds baked in (see tools/sanitizer-env.sh for what
# is suppressed and why).
#
# Build first:  nix-shell --run "cmake -B build-asan -G Ninja -DENABLE_SANITIZERS=ON && ninja -C build-asan designer_tests"
# Run:          nix-shell --run "tools/run-sanitized-tests.sh [catch2-filter...]"
# e.g.          nix-shell --run "tools/run-sanitized-tests.sh '[thread]'"
#
# This runs the unit binary directly, in ONE process — good for a filter,
# bad for the whole suite (serial, and the SIMP gate alone runs past 20
# minutes under ASan). For everything: tools/ci-local.sh --asan, which runs
# the same build through ctest -j in ~2.5 minutes.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bin="$here/build-asan/tests/designer_tests"
if [[ ! -x "$bin" ]]; then
  echo "sanitized test binary not found: $bin" >&2
  echo "build it: cmake -B build-asan -G Ninja -DENABLE_SANITIZERS=ON && ninja -C build-asan designer_tests" >&2
  exit 1
fi
source "$here/tools/sanitizer-env.sh"
exec "$bin" "$@"
