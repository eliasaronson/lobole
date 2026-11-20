# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# cpu-claims.sh -- how many CPUs the other builds and suites on this machine
# are using, sourced by the scripts that pick a job count.
#
#   . tools/cpu-claims.sh
#   cpu_claim 8              # this shell runs 8 jobs until it exits
#   cpu_claimed_by_others    # CPUs claimed by every other live claimant
#   busy_cpus "$(nproc)"     # CPUs busy right now, over one second
#   free_cpus                # nproc less the larger of the two
#
# A one-second /proc/stat sample (what tools/ctest took alone) sees only
# work that has already started: K agents starting their suites in the same
# minute each saw an idle machine and each ran -j22. A claim is written
# BEFORE the jobs start, so the second suite sees the first one's -j at
# once, and the sample still sees what claims nothing (a generator's worker
# pool, a human's build). tools/ctest, tools/build-jobs and
# tools/gui-tests-xvfb.sh claim what they run.
#
# A claim is a file named for the claiming shell's pid, holding its job
# count, in one directory per user under /tmp. A claim whose pid is gone is
# ignored and removed, so a SIGKILLed suite leaks nothing that counts. Not a
# jobserver: nothing waits for a token, it only sizes -j at the start.

_cpu_claims_dir="/tmp/lobole-cpu-claims-$(id -u)"

cpu_claim() { # jobs -- claimed for this shell ($$) until it exits
  # No EXIT trap (it would replace the caller's): a claim lasts exactly as
  # long as its pid, and the next reader removes it once the pid is gone.
  mkdir -p "$_cpu_claims_dir" 2>/dev/null || return 0
  echo "$1" > "$_cpu_claims_dir/$$" 2>/dev/null || true
}

cpu_claimed_by_others() {
  local f pid n total=0
  for f in "$_cpu_claims_dir"/*; do
    [ -f "$f" ] || continue
    pid="${f##*/}"
    [ "$pid" = "$$" ] && continue
    case "$pid" in *[!0-9]*) continue ;; esac
    if ! kill -0 "$pid" 2>/dev/null; then
      rm -f "$f"
      continue
    fi
    n="$(cat "$f" 2>/dev/null)"
    case "$n" in '' | *[!0-9]*) continue ;; esac
    total=$((total + n))
  done
  echo "$total"
}

busy_cpus() { # ncpus -- busy over one second, from /proc/stat
  local a b
  [ -r /proc/stat ] || return 1
  a="$(head -1 /proc/stat)" || return 1
  sleep 1
  b="$(head -1 /proc/stat)" || return 1
  awk -v a="$a" -v b="$b" -v n="$1" 'BEGIN {
    split(a, x); split(b, y); t = 0; idle = 0
    for (i = 2; i <= 9; i++) t += y[i] - x[i]
    idle = (y[5] - x[5]) + (y[6] - x[6])
    if (t <= 0) exit 1
    printf "%d\n", (t - idle) / t * n + 0.5 }'
}

free_cpus() { # CPUs neither busy now nor claimed by another suite or build
  local n busy claimed used
  n="$(nproc)"
  busy="$(busy_cpus "$n")" || busy=0
  claimed="$(cpu_claimed_by_others)"
  used=$busy
  [ "$claimed" -gt "$used" ] && used=$claimed
  [ "$used" -gt "$n" ] && used=$n
  echo $((n - used))
}
