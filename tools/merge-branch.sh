#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# Land a worktree branch on main: cherry-pick, build, test, remove the worktree.
#
#   tools/merge-branch.sh BRANCH            # test-fast after the build
#   tools/merge-branch.sh BRANCH --full     # the headless suite, the big
#                                           # generators and the GUI suite
#   tools/merge-branch.sh BRANCH --pick-only  # pick and number, then stop:
#                                           # no build, no suite, no removal
#
# The cherry-pick happens under build/'s lock, held exclusively
# (tools/build-lock.sh), so it never lands while a build of this tree is
# compiling or a suite (tools/ctest, the GUI runner) runs from it. A conflict
# stops here with the cherry-pick in progress, for a person to resolve; `git cherry-pick
# --continue` and then this script again with --no-pick finishes the rest.
# A conflict ONLY in the files made from the MethodDoc rows
# (python/lobole/rpc.py, api/rpc-surface.json) is not one: two branches that
# each add an RPC both regenerate them, the rows themselves merge, and the
# files are made again from the merged rows and the pick goes on.
# The worktree is removed only when every commit is on main and it holds
# nothing uncommitted -- `git worktree remove` refuses trees with submodules,
# so it is rm -rf, which is why both are checked (CLAUDE.md).
#
# A branch's format bump is written vNEXT and numbered here, after the pick
# and under the same lock, in a commit of its own: tools/format-version.py
# land gives it main's next number (docs/versioning/format.md#a-bump-takes-its-number-when-it-lands).
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
branch="" full=0 pick=1 pick_only=0
for a in "$@"; do
  case "$a" in
    --full) full=1 ;;
    --no-pick) pick=0 ;;
    --pick-only) pick_only=1 ;;
    -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
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
state="$(git rev-parse --git-dir)/merge-branch"
base_file="$state/$branch.base"

# The files made from the MethodDoc rows. A pick that conflicts in these
# alone has them made again from the merged rows (which conflicted nowhere)
# and goes on; anything else is a person's to resolve. Returns non-zero when
# the conflict is not only theirs, or remaking them fails.
generated="python/lobole/rpc.py api/rpc-surface.json"
remake_generated() {
  local unmerged f
  unmerged="$(git diff --name-only --diff-filter=U)"
  [ -n "$unmerged" ] || return 1
  for f in $unmerged; do
    case " $generated " in *" $f "*) ;; *) return 1 ;; esac
  done
  echo "merge-branch: $(echo $unmerged) conflicted alone: made again from the rows"
  # Main's side first: the snapshot's own fields (released, its version)
  # are read before it is rewritten, and a file full of markers reads as
  # nothing.
  # shellcheck disable=SC2086
  git checkout --ours -- $unmerged &&
    python3 "$root/tools/generate_rpc_contracts.py" >/dev/null &&
    python3 "$root/tests/api_surface_gate.py" --update >/dev/null || return 1
  # shellcheck disable=SC2086
  git add -- $generated
  # Either may stop on the next commit's conflict: the caller looks again.
  if git diff --cached --quiet; then
    # Made again, the commit changes nothing main does not already have.
    git cherry-pick --skip || true
  else
    GIT_EDITOR=true git cherry-pick --continue || true
  fi
}

# The branch's vNEXT format bumps get main's next numbers, in one commit; then
# nothing may be pending. Run under build/'s lock, before the build.
number_format() {
  python3 "$root/tools/format-version.py" land --base "$(cat "$base_file")" --label "$branch"
  python3 "$root/tools/format-version.py" check --landed
  rm -f "$base_file"
}

if [ "$pick" = 1 ]; then
  commits="$(git rev-list --reverse --cherry-pick --right-only --no-merges main..."$branch")"
  if [ -z "$commits" ]; then
    echo "merge-branch: nothing on $branch that main lacks"
  elif git diff main..."$branch" -- src/core/io/SerializationInternal.h |
       grep -q '^-inline constexpr int kVersionLanded = '; then
    # It chose a number, which is what collided: a branch raises
    # kVersionPending and writes vNEXT, and the landing numbers it.
    echo "merge-branch: $branch edits kVersionLanded itself;" \
         "a branch raises kVersionPending and writes vNEXT" \
         "(docs/versioning/format.md#a-bump-takes-its-number-when-it-lands)" >&2
    exit 2
  else
    # Exclusive: it waits for a build, and for every suite running from
    # build/ (tools/ctest, the GUI runner hold it shared).
    # shellcheck source=build-lock.sh
    . "$root/tools/build-lock.sh"
    build_lock_exclusive "$root/build" 9
    echo "merge-branch: cherry-picking $(echo "$commits" | wc -l) commit(s)"
    # What was picked, so a commit whose conflict was resolved by hand (its
    # patch no longer matches the branch's) still counts as landed below.
    mkdir -p "$state"
    echo "$commits" > "$state/$branch"
    # Where main stood, so the numbering sees only the lines the branch added.
    git rev-parse HEAD > "$base_file"
    # shellcheck disable=SC2086
    if ! git cherry-pick $commits; then
      # Each later commit may stop on the generated files again.
      while [ -e "$(git rev-parse --git-path CHERRY_PICK_HEAD)" ] &&
            remake_generated; do :; done
      if [ -e "$(git rev-parse --git-path CHERRY_PICK_HEAD)" ] ||
         [ -e "$(git rev-parse --git-path sequencer)" ]; then
        echo "merge-branch: CONFLICT. Resolve, 'git cherry-pick --continue', then: $0 $branch --no-pick" >&2
        exit 1
      fi
    fi
    number_format
    exec 9>&-
  fi
elif [ -f "$base_file" ]; then
  # --no-pick after a conflict resolved by hand: the numbering is still owed.
  # shellcheck source=build-lock.sh
  . "$root/tools/build-lock.sh"
  build_lock_exclusive "$root/build" 9
  number_format
  exec 9>&-
fi

if [ "$pick_only" = 1 ]; then
  echo "merge-branch: picked (--pick-only): not built, not tested, $branch kept"
  exit 0
fi

"$root/tools/build.sh"
# The suite's whole output, a failing test's own included, is kept: the
# summary below is all that is printed, and a failure that passes on the
# rerun is otherwise gone (LastTest.log is the next run's).
suite_log="$root/build/merge-branch-suite.log"
echo "merge-branch: the suite's whole output goes to $suite_log"
if [ "$full" = 1 ]; then
  "$root/tools/ctest" -LE gui -j"$("$root/tools/build-jobs" 700)" --output-on-failure 2>&1 |
    tee "$suite_log" | grep -E 'tests passed|\(Failed\)'
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
  "$root/tools/test-fast.sh" 2>&1 | tee "$suite_log" |
    grep -E 'tests passed|\(Failed\)'
fi

# Every commit of the branch now on main (by patch), and nothing uncommitted.
if [ -n "$wt" ] && [ -d "$wt" ]; then
  picked="$state/$branch"
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
