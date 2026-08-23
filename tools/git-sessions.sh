#!/usr/bin/env bash
# Which Claude session wrote which commit.
#
# Several sessions work in this repo at once, and they all commit as the same
# git author — correctly: the work is Elias's, and the assistant is a
# Co-Authored-By. So `%an` cannot tell them apart, and two sessions once spent
# a message each guessing who wrote a commit neither had.
#
# They are already distinguishable: every commit carries a `Claude-Session:`
# trailer (109/109 over the three days this was first checked). It is just
# invisible in `git log --oneline`, which is why nobody looked.
#
#   tools/git-sessions.sh                 # summary: how many commits per session
#   tools/git-sessions.sh --log [<rev>]   # one line per commit, session tagged
#   tools/git-sessions.sh --who <commit>  # the session behind one commit
set -euo pipefail

session_of() {   # commit -> short session id, or "-"
  git log -1 --format=%B "$1" | sed -n 's|^Claude-Session:.*session_||p' | head -1
}

case "${1:-}" in
  --who)
    [ $# -ge 2 ] || { echo "usage: $0 --who <commit>" >&2; exit 2; }
    s="$(session_of "$2")"
    echo "${s:-no session trailer}"
    ;;
  --log)
    range="${2:-HEAD}"
    git log --format='%H %h %s' "$range" | while read -r full short subject; do
      s="$(session_of "$full")"
      printf '%s  %-10s %s\n' "$short" "${s:0:8}" "$subject"
    done
    ;;
  *)
    # Summary, most active first. `git log` is one process; the trailer grep
    # is over its output rather than a process per commit.
    git log --format='%B' | sed -n 's|^Claude-Session:.*session_||p' \
      | sort | uniq -c | sort -rn \
      | awk '{printf "  %5d commits  %s\n", $1, $2}'
    echo
    echo "  (tools/git-sessions.sh --log   for one line per commit)"
    ;;
esac
