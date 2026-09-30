#!/usr/bin/env bash
# Build, holding the build directory's lock: the one way to build here.
#
#   tools/build.sh                       # build/: designer designer_tests
#   tools/build.sh build designer        # one target
#   tools/build.sh .ci-local/build-nocam # a whole other tree
#
# -j comes from free memory (tools/build-jobs). The lock is what
# tools/merge-branch.sh takes before it changes the tree: a merge that lands
# while a build is compiling gives a binary whose objects saw two versions of
# a header -- it died on its first autosave with a bad_alloc that looked like
# heap corruption (docs/plans/dev-loop.md). Prints FAILED lines and exits
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
fi
jobs="$("$root/tools/build-jobs")"
log="$(mktemp)"
trap 'rm -f "$log"' EXIT
exec 9>"$dir/.build.lock"
if ! flock -n 9; then
  echo "build: waiting for the lock on $dir (a build or a merge holds it)" >&2
  flock 9
fi
status=0
nix-shell "$root/shell.nix" --run "ninja -C '$dir' -j$jobs ${targets[*]:-}" > "$log" 2>&1 || status=$?
grep -E 'FAILED|error:|Terminated signal' "$log" | head -40 || true
if [ "$status" -ne 0 ]; then
  echo "build: FAILED (see above; -j$jobs)" >&2
  exit "$status"
fi
echo "build: ok ($dir, -j$jobs)"
