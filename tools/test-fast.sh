#!/usr/bin/env bash
# The headless suite minus the long numeric gates — the inner loop.
#
#   tools/test-fast.sh            # ~10 s instead of ~79
#   tools/test-fast.sh -R sketch  # ...further filtered, ctest args pass through
#
# What it skips is the `slow` LABEL, not a list of names kept here. A Catch2
# case earns it with a `[slow]` tag beside its other tags, a python smoke with
# `LABELS slow` where it is registered — either way at the point it is WRITTEN,
# by whoever knows it is heavy (tests/CMakeLists.txt turns tags into labels
# with ADD_TAGS_AS_LABELS).
#
# That indirection is the whole point. This script used to name the slow tests
# by regex, and a regex in tools/ cannot know about a test added in tests/: it
# listed the topology and FEM gates, never learned about the CAM ones that
# arrived later, and the inner loop had silently gone from 8 s to 49 s while
# every doc still quoted the old number
# (docs/architecture.md#tests).
#
# Measured 2026-08-31 on this machine, `ctest -j$(nproc)`:
#
#   full headless suite (925 tests)            79 s
#   the 17 `slow` tests                       311 s of CPU between them
#     of which the SIMP topology gate           77 s
#     and four CAM clearing cases              138 s
#   everything else (908 tests)                10 s
#
# They are real gates on real numerics and real toolpaths — they belong in
# tools/ci-local.sh and in CI, and not in the loop you run after every edit.
#
# ALWAYS run the full suite before committing (tools/ci-local.sh --nix, or
# tools/ctest -LE gui): "fast" here means fewer tests, not weaker ones.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Parallel by memory, not cores: each app-driving test is its own process
# (tools/build-jobs).
jobs="$("$root/tools/build-jobs" 700)"
exec "$root/tools/ctest" -LE 'gui|slow' -j"$jobs" --output-on-failure "$@"
