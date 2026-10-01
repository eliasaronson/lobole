#!/usr/bin/env bash
# Land a worktree branch on main: cherry-pick, build, test, remove the worktree.
#
#   tools/merge-branch.sh BRANCH            # test-fast after the build
#   tools/merge-branch.sh BRANCH --full     # the headless suite, the big
#                                           # generators and the GUI suite
#
# The cherry-pick happens under build/'s lock, held exclusively
# (tools/build-lock.sh), so it never lands while a build of this tree is
# compiling or a suite (tools/ctest, the GUI runner) runs from it. A conflict
# stops here with the cherry-pick in progress, for a person to resolve; `git cherry-pick
# --continue` and then this script again with --no-pick finishes the rest.
# The worktree is removed only when every commit is on main and it holds
# nothing uncommitted -- `git worktree remove` refuses trees with submodules,
# so it is rm -rf, which is why both are checked (CLAUDE.md).
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
branch="" full=0 pick=1
for a in "$@"; do
  case "$a" in
    --full) full=1 ;;
    --no-pick) pick=0 ;;
    -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
    *) branch="$a" ;;
  esac
done
[ -n "$branch" ] || { echo "merge-branch: name the branch" >&2; exit 2; }
git rev-parse --verify -q "$branch" >/dev/null || { echo "merge-branch: no branch $branch" >&2; exit 2; }
[ "$(git rev-parse --abbrev-ref HEAD)" = main ] || { echo "merge-branch: not on main" >&2; exit 2; }
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo "merge-branch: main has uncommitted changes; commit or set them aside first" >&2
  exit 2
fi
wt="$(git worktree list --porcelain | awk -v b="refs/heads/$branch" '/^worktree /{w=$2} $0=="branch "b{print w}')"

if [ "$pick" = 1 ]; then
  commits="$(git rev-list --reverse --cherry-pick --right-only --no-merges main..."$branch")"
  if [ -z "$commits" ]; then
    echo "merge-branch: nothing on $branch that main lacks"
  else
    # Exclusive: it waits for a build, and for every suite running from
    # build/ (tools/ctest, the GUI runner hold it shared).
    # shellcheck source=build-lock.sh
    . "$root/tools/build-lock.sh"
    build_lock_exclusive "$root/build" 9
    echo "merge-branch: cherry-picking $(echo "$commits" | wc -l) commit(s)"
    # What was picked, so a commit whose conflict was resolved by hand (its
    # patch no longer matches the branch's) still counts as landed below.
    mkdir -p "$(git rev-parse --git-dir)/merge-branch"
    echo "$commits" > "$(git rev-parse --git-dir)/merge-branch/$branch"
    # shellcheck disable=SC2086
    if ! git cherry-pick $commits; then
      echo "merge-branch: CONFLICT. Resolve, 'git cherry-pick --continue', then: $0 $branch --no-pick" >&2
      exit 1
    fi
    exec 9>&-
  fi
fi

"$root/tools/build.sh"
if [ "$full" = 1 ]; then
  "$root/tools/ctest" -LE gui -j"$("$root/tools/build-jobs" 700)" | grep -E 'tests passed|\(Failed\)'
  # The big generators, each from nothing on its own instance
  # (generators/README.md#the-big-generators-as-tests): after the headless
  # suite, not beside it -- two at a time is what memory and one GPU take.
  # The verdict, and on a failure the test's own message: why (exit code,
  # budget, no success marker), where its output was kept, and the last
  # lines the generator printed -- not the app's whole log, which follows.
  "$root/tools/ctest" -C generators -L generators -j2 --output-on-failure |
    awk '/tests passed|\(Failed\)|\(Timeout\)/ { print; next }
         /^[a-z0-9_]+: (exit|over its budget)/ { n = 40 }
         n > 0 { print; n-- }'
  # The verdict, and on a failure what failed and where it is archived with
  # its seed: the last line alone ("SOME FAILED") invites a re-run, which
  # passes and hides an order-dependent failure the archive can replay.
  "$root/tools/gui-tests-xvfb.sh" |
    grep -E '^--- shard|test failed|Error |archived:|^GUI tests: [0-9]+/|^MISMATCH' |
    sed 's/\x1b\[[0-9;]*m//g'
else
  "$root/tools/test-fast.sh" | grep -E 'tests passed|\(Failed\)'
fi

# Every commit of the branch now on main (by patch), and nothing uncommitted.
if [ -n "$wt" ] && [ -d "$wt" ]; then
  picked="$(git rev-parse --git-dir)/merge-branch/$branch"
  left="$(git rev-list --cherry-pick --right-only --no-merges main..."$branch" \
          | { if [ -f "$picked" ]; then grep -vxF -f "$picked" || true; else cat; fi; } | wc -l)"
  dirty="$(git -C "$wt" status --porcelain --untracked-files=no)"
  if [ "$left" -eq 0 ] && [ -z "$dirty" ]; then
    git worktree unlock "$wt" 2>/dev/null || true
    # A generator's output lives in the worktree's untracked scratch/, which
    # the rm below would take with it. Carry it over, never over main's own.
    if [ -d "$wt/scratch" ]; then
      mkdir -p "$root/scratch"
      cp -a -n "$wt/scratch/." "$root/scratch/"
      echo "merge-branch: kept $wt/scratch in scratch/ (existing files untouched)"
    fi
    rm -rf "$wt"
    git worktree prune
    git branch -D "$branch" >/dev/null
    rm -f "$picked"
    echo "merge-branch: $branch landed; worktree $wt removed"
  else
    echo "merge-branch: kept $wt ($left commit(s) not on main, uncommitted: ${dirty:+yes})"
  fi
fi
