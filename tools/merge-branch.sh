#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# Land a worktree branch on main: cherry-pick, build, test, remove the worktree.
#
#   tools/merge-branch.sh BRANCH            # test-fast after the build
#   tools/merge-branch.sh BRANCH --full     # the headless suite and the GUI
#                                           # suite, and the heavy suites
#                                           # whose ground the branch touches
#   tools/merge-branch.sh BRANCH --release  # every suite, whatever changed:
#                                           # the big generators, the full
#                                           # action matrix, every reader-
#                                           # fuzz case
#   tools/merge-branch.sh BRANCH --no-skip  # the mode's every suite, not
#                                           # narrowed by what changed
#   tools/merge-branch.sh BRANCH --dry-run  # print the plan; change nothing
#   tools/merge-branch.sh BRANCH --pick-only  # pick and number, then stop:
#                                           # no build, no suite, no removal
#
# What runs follows from the paths the branch changes (tools/merge_plan.py,
# docs/ui/testing.md#what-a-merge-runs), and the script prints the decision:
# a docs-only branch runs the conventions and the format gates and builds
# only if the binaries are behind; a branch of tests, python or tools runs
# the tests those paths can reach; anything else runs all the mode runs.
# The big generators (~45 min), the full action matrix (~16 min) and every
# reader-fuzz case (a third of the old headless suite) ran on every --full
# merge, an hour of six to twelve busy cores that slowed every other agent;
# they now run on --full only when the branch touches what they cover, and
# always on --release.
#
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
branch="" mode=fast no_skip=0 dry=0 pick=1 pick_only=0
for a in "$@"; do
  case "$a" in
    --full) mode=full ;;
    --release) mode=release ;;
    --no-skip) no_skip=1 ;;
    --dry-run) dry=1 ;;
    --no-pick) pick=0 ;;
    --pick-only) pick_only=1 ;;
    -h|--help) sed -n '2,50p' "$0"; exit 0 ;;
    -*) echo "merge-branch: unknown option $a" >&2; exit 2 ;;
    *) branch="$a" ;;
  esac
done
[ -n "$branch" ] || { echo "merge-branch: name the branch" >&2; exit 2; }
git rev-parse --verify -q "$branch" >/dev/null || { echo "merge-branch: no branch $branch" >&2; exit 2; }
# A dry run reads the branch against main from anywhere, and changes nothing.
[ "$dry" = 1 ] || [ "$(git rev-parse --abbrev-ref HEAD)" = main ] ||
  { echo "merge-branch: not on main" >&2; exit 2; }
if [ "$dry" = 0 ] && [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo "merge-branch: main has uncommitted changes; commit or set them aside first" >&2
  exit 2
fi
wt="$(git worktree list --porcelain | awk -v b="refs/heads/$branch" '/^worktree /{w=$2} $0=="branch "b{print w}')"
# Absolute: ctest reads the test list from inside the build directory.
state="$(git rev-parse --absolute-git-dir)/merge-branch"
base_file="$state/$branch.base"

# The paths the branch changes, for the plan: the files of the commits to be
# picked (kept, so a --no-pick after a conflict plans the same).
changed_file="$state/$branch.changed"
mkdir -p "$state"
if [ "$pick" = 1 ]; then
  pending="$(git rev-list --reverse --cherry-pick --right-only --no-merges main..."$branch")"
  # shellcheck disable=SC2086
  { [ -z "$pending" ] || git show --name-only --format= $pending; } |
    sed '/^$/d' | sort -u > "$changed_file.new"
  mv "$changed_file.new" "$changed_file"
elif [ ! -f "$changed_file" ]; then
  : > "$changed_file" # nothing known: the plan runs everything
fi
plan_args=(--mode "$mode")
[ "$no_skip" = 1 ] && plan_args+=(--no-skip)
plan="$(python3 "$root/tools/merge_plan.py" classify "${plan_args[@]}" "$changed_file")"
echo "$plan" | grep '^merge-plan:' || true
plan_get() { echo "$plan" | sed -n "s/^$1=//p"; }
if [ "$dry" = 1 ]; then
  echo "merge-branch: --dry-run: $(wc -l < "$changed_file") path(s) in" \
       "$(echo "${pending:-}" | sed '/^$/d' | wc -l) commit(s); would run:"
  for k in build headless gui generators action_matrix reader_fuzz_all fem_reference; do
    printf '  %-16s %s\n' "$k" "$(plan_get "$k")"
  done
  # Main's tree after the pick is the branch's own only when the branch is
  # main plus its commits, and no vNEXT is numbered on the way.
  if [ -n "$wt" ] && git merge-base --is-ancestor main "$branch" &&
     [ "$(git rev-list --count main.."$branch")" = "$(echo "${pending:-}" | sed '/^$/d' | wc -l)" ]; then
    t="$(git rev-parse "$branch^{tree}")"
    echo "  stamps against $branch's tree $(echo "$t" | cut -c1-12) (if nothing is numbered):"
    for key in "-LE gui|slow" "-LE gui" gui-suite "-C generators -L generators" \
               "-C action_matrix -L action_matrix" "-C reader_fuzz -L reader_fuzz_all" \
               "-C fem_reference -L fem_reference"; do
      # shellcheck disable=SC2086
      if [ "$key" = "-LE gui|slow" ]; then set -- -LE 'gui|slow'; else set -- $key; fi
      printf '    %-34s %s\n' "$key" \
        "$(python3 "$root/tools/suite_stamp.py" find "$wt/build" "$root/build" "$t" "$@" || true)"
    done
  else
    echo "  no stamp can be reused: $branch is not rebased onto main"
  fi
  exit 0
fi

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

# A branch that adds a submodule (deps/manifold) leaves main's checkout of it
# empty, and cmake fails regenerating build.ninja.
git -C "$root" submodule status | awk '/^-/ { print $2 }' | while read -r sub; do
  echo "merge-branch: checking out the new submodule $sub"
  git -C "$root" submodule update --init -- "$sub"
done
ran=() skipped=() reused=()
# A suite that already passed in the worktree on EXACTLY the tree main now
# has, on the toolchain main's build/ has, is not run again
# (tools/suite_stamp.py). Only a branch rebased onto main before it is
# landed gives main that tree; a format bump numbered here changes it.
main_tree="$(git rev-parse 'HEAD^{tree}')"
stamped() { # what... (the key the worktree's run was stamped with)
  local why
  [ -n "$wt" ] && [ -d "$wt/build/suite-stamps" ] || return 1
  if why="$(python3 "$root/tools/suite_stamp.py" find "$wt/build" "$root/build" \
              "$main_tree" "$@")"; then
    reused+=("$* ($why)")
    return 0
  fi
  return 1
}
if [ "$(plan_get build)" = if-stale ] &&
   "$root/tools/nix-run" "$(printf '%q %q' "$root/tools/stale-binaries.sh" "$root/build")" >/dev/null; then
  skipped+=("build (docs only, and the binaries are current)")
else
  "$root/tools/build.sh"
  ran+=(build)
fi
# The suite's whole output, a failing test's own included, is kept: the
# summary below is all that is printed, and a failure that passes on the
# rerun is otherwise gone (LastTest.log is the next run's).
suite_log="$root/build/merge-branch-suite.log"
echo "merge-branch: the suite's whole output goes to $suite_log"
: > "$suite_log"
headless="$(plan_get headless)"
if [ "$headless" = subset ] && stamped -LE gui; then
  headless=reused
elif [ "$headless" = subset ]; then
  names="$state/$branch.tests"
  if "$root/tools/nix-run" "$(printf 'python3 %q select --build %q %q %q' \
       "$root/tools/merge_plan.py" "$root/build" "$changed_file" "$names")"; then
    "$root/tools/ctest" --tests-from-file "$names" \
      -j"$("$root/tools/build-jobs" 700)" --output-on-failure 2>&1 |
      tee -a "$suite_log" | grep -E 'tests passed|\(Failed\)'
    ran+=("headless subset ($(wc -l < "$names") tests)")
    skipped+=("the rest of the headless suite (the changed paths cannot reach it)")
  else
    headless=all
  fi
  rm -f "$names"
fi
if [ "$headless" = all ] && [ "$mode" = fast ] && stamped -LE 'gui|slow'; then
  :
elif [ "$headless" = all ] && [ "$mode" != fast ] && stamped -LE gui; then
  :
elif [ "$headless" = all ]; then
  if [ "$mode" = fast ]; then
    "$root/tools/test-fast.sh" 2>&1 | tee -a "$suite_log" |
      grep -E 'tests passed|\(Failed\)'
    ran+=("headless fast lane")
  else
    "$root/tools/ctest" -LE gui -j"$("$root/tools/build-jobs" 700)" --output-on-failure 2>&1 |
      tee -a "$suite_log" | grep -E 'tests passed|\(Failed\)'
    ran+=("headless suite")
  fi
fi
if [ "$(plan_get generators)" = yes ] && stamped -C generators -L generators; then
  :
elif [ "$(plan_get generators)" = yes ]; then
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
  ran+=("big generators")
else
  skipped+=("big generators: $(echo "$plan" | sed -n 's/^merge-plan: generators *no *//p')")
fi
if [ "$(plan_get gui)" = yes ] && stamped gui-suite; then
  :
elif [ "$(plan_get gui)" = yes ]; then
  # The verdict, and on a failure what failed and where it is archived with
  # its seed: the last line alone ("SOME FAILED") invites a re-run, which
  # passes and hides an order-dependent failure the archive can replay.
  "$root/tools/gui-tests-xvfb.sh" |
    grep -E '^--- shard|test failed|Error |archived:|^GUI tests: [0-9]+/|^MISMATCH|^shards:' |
    sed 's/\x1b\[[0-9;]*m//g'
  ran+=("GUI suite")
elif [ "$mode" != fast ]; then
  skipped+=("GUI suite: nothing the GUI tests run changed")
fi
if [ "$(plan_get action_matrix)" = yes ] && stamped -C action_matrix -L action_matrix; then
  :
elif [ "$(plan_get action_matrix)" = yes ]; then
  # The full action matrix (docs/modeling/README.md#action-matrix) against
  # the committed baseline: every ordinary action on every face, edge and
  # body of the corpus, which the headless suite only samples. Last, and
  # four shards at a time: each is an instance of its own.
  "$root/tools/ctest" -C action_matrix -L action_matrix -j4 \
    --output-on-failure 2>&1 | tee -a "$suite_log" |
    grep -E 'tests passed|\(Failed\)|\(Timeout\)|^  - '
  ran+=("full action matrix")
else
  skipped+=("full action matrix: $(echo "$plan" | sed -n 's/^merge-plan: action_matrix *no *//p')")
fi
if [ "$(plan_get reader_fuzz_all)" = yes ] && stamped -C reader_fuzz -L reader_fuzz_all; then
  :
elif [ "$(plan_get reader_fuzz_all)" = yes ]; then
  # Every reader-fuzz case (docs/versioning/README.md#reader-fuzzing), by
  # memory: a family peaks at 1.4 GB.
  "$root/tools/ctest" -C reader_fuzz -L reader_fuzz_all \
    -j"$("$root/tools/build-jobs" 1500)" --output-on-failure 2>&1 |
    tee -a "$suite_log" | grep -E 'tests passed|\(Failed\)|\(Timeout\)'
  ran+=("every reader-fuzz case")
else
  skipped+=("every reader-fuzz case: $(echo "$plan" | sed -n 's/^merge-plan: reader_fuzz_all *no *//p')")
fi
if [ "$(plan_get fem_reference)" = yes ] && stamped -C fem_reference -L fem_reference; then
  :
elif [ "$(plan_get fem_reference)" = yes ]; then
  # The FEM reference cases (docs/simulation/README.md#reference-cases):
  # every published benchmark and closed form, in-house and on CalculiX,
  # one instance; the planted Gauss weight must be caught.
  "$root/tools/ctest" -C fem_reference -L fem_reference -j1 \
    --output-on-failure 2>&1 | tee -a "$suite_log" |
    grep -E 'tests passed|\(Failed\)|\(Timeout\)|^fem-reference: (FAIL|PASSING|planted)'
  ran+=("FEM reference cases")
else
  skipped+=("FEM reference cases: $(echo "$plan" | sed -n 's/^merge-plan: fem_reference *no *//p')")
fi
echo "merge-branch: ran: $(IFS=,; echo "${ran[*]:-nothing}" | sed 's/,/, /g')"
for k in "${skipped[@]}"; do echo "merge-branch: skipped $k"; done
for k in "${reused[@]}"; do
  echo "merge-branch: reused the worktree's pass of $k"
done
[ "${#reused[@]}" -gt 0 ] || [ -z "$wt" ] ||
  echo "merge-branch: no stamp reused (main's tree $(echo "$main_tree" | cut -c1-12)" \
       "is not one the worktree passed a suite on, or it ran none)"

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
