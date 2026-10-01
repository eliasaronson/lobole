# build-lock.sh -- the build directory's lock, sourced by the scripts that
# change a build tree or run what it built.
#
#   . tools/build-lock.sh
#   build_lock_exclusive "$dir" 9   # tools/build.sh, tools/merge-branch.sh
#   build_lock_shared "$dir" 9      # tools/ctest, tools/gui-tests-xvfb.sh
#
# A build or a merge changes the tree and takes the lock EXCLUSIVELY; a suite
# reads it and takes it SHARED, so suites run beside each other and a merge
# waits for every one of them to end. Without the shared side a merge relinked
# designer_tests in the middle of a suite run, and tests failed once and passed
# on the rerun (docs/architecture.md#the-development-loop).
#
# The lock is held on file descriptor $2 of the calling shell, so it lasts
# until that shell exits (or closes the fd). Run the command it guards with
# "$2>&-" where a child might outlive it: an app left running by a test must
# not hold the tree's lock after the suite has ended.

_build_lock() { # mode dir fd
  local mode="$1" dir="$2" fd="$3" what
  [ -d "$dir" ] || return 0
  eval "exec $fd>>\"$dir/.build.lock\""
  if [ "$mode" = -x ]; then what="a build, a merge or a suite"; else what="a build or a merge"; fi
  if ! flock "$mode" -n "$fd"; then
    echo "$(basename "$0"): waiting for the lock on $dir ($what holds it)" >&2
    flock "$mode" "$fd"
  fi
}
build_lock_exclusive() { _build_lock -x "$1" "${2:-9}"; }
build_lock_shared() { _build_lock -s "$1" "${2:-9}"; }
