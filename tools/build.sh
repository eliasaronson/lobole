#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# Build, holding the build directory's lock: the one way to build here.
#
#   tools/build.sh                       # build/: designer designer_tests
#                                        #   femgpu_tests
#   tools/build.sh build designer        # one target
#   tools/build.sh .ci-local/build-nocam # a whole other tree
#
# -j comes from free memory (tools/build-jobs). The lock (tools/build-lock.sh)
# is taken exclusively, as tools/merge-branch.sh takes it before it changes the
# tree, and waits for a suite holding it shared (tools/ctest): a merge that lands
# while a build is compiling gives a binary whose objects saw two versions of
# a header -- it died on its first autosave with a bad_alloc that looked like
# heap corruption (docs/architecture.md#the-development-loop). Prints FAILED lines and exits
# non-zero on a failed build.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dir="${1:-build}"
[ $# -gt 0 ] && shift
targets=("$@")
[ -d "$root/$dir" ] && dir="$root/$dir"
[ -f "$dir/build.ninja" ] || { echo "build: $dir has no build.ninja (configure it first)" >&2; exit 2; }
if [ ${#targets[@]} -eq 0 ] && [ "$(basename "$dir")" = build ]; then
  targets=(designer designer_tests)
  # The GPU solver's tests are a binary of their own; left out, it went on
  # testing the code it was last linked from (tools/stale-binaries.sh).
  grep -q '^build femgpu_tests: phony' "$dir/build.ninja" &&
    targets+=(femgpu_tests)
fi
jobs="$("$root/tools/build-jobs")"
log="$(mktemp)"
trap 'rm -f "$log"' EXIT
# shellcheck source=build-lock.sh
. "$root/tools/build-lock.sh"
build_lock_exclusive "$dir" 9
status=0
"$root/tools/nix-run" "ninja -C '$dir' -j$jobs ${targets[*]:-}" > "$log" 2>&1 || status=$?
grep -E 'FAILED|error:|Terminated signal' "$log" | head -40 || true
if [ "$status" -ne 0 ]; then
  echo "build: FAILED (see above; -j$jobs)" >&2
  exit "$status"
fi
echo "build: ok ($dir, -j$jobs)"
