#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# Every running lobole of this user: its pid, port, age and working
# directory, the directory marked when it has been deleted -- the instance
# of a worktree merged and removed hours ago, still holding a port and
# memory (docs/scripting/README.md#the-instance-registry).
#
#   tools/stray-instances.sh
#
# Read from /proc, so it finds an instance whatever XDG_STATE_HOME it
# announced itself under; the port comes from its claim in
# /tmp/lobole-ports-<uid> (LOBOLE_PORT_CLAIMS_DIR), else from the sockets
# it listens on. It lists and kills nothing: stop one by its pid
# (headless-preview.sh --stop PORT for one it started), never by a pattern.
set -euo pipefail
claims="${LOBOLE_PORT_CLAIMS_DIR:-/tmp/lobole-ports-$(id -u)}"
now="$(date +%s)"
ticks="$(getconf CLK_TCK)"
boot="$(awk '/^btime/ {print $2}' /proc/stat)"

port_of() { # pid
  local f
  for f in "$claims"/*; do
    [ -f "$f" ] || continue
    if [ "$(head -c 32 "$f" | tr -dc '0-9')" = "$1" ]; then
      basename "$f"
      return
    fi
  done
  # An instance older than the claims: the TCP ports it listens on.
  if command -v ss >/dev/null 2>&1; then
    ss -Hltnp 2>/dev/null |
      awk -v p="pid=$1," 'index($0, p) { n = split($4, a, ":"); print a[n] }' |
      sort -u | paste -sd, -
  fi
}

found=0
printf '%-8s %-6s %-9s %s\n' PID PORT AGE CWD
for d in /proc/[0-9]*; do
  pid="${d#/proc/}"
  [ -O "$d" ] || continue
  exe="$(readlink "$d/exe" 2>/dev/null)" || continue
  case "$(basename "${exe% (deleted)}")" in lobole|designer) ;; *) continue ;; esac
  cwd="$(readlink "$d/cwd" 2>/dev/null || echo '?')"
  start="$(awk '{print $22}' "$d/stat" 2>/dev/null)" || continue
  age=$(( now - boot - start / ticks ))
  if [ "$age" -ge 86400 ]; then when="$(( age / 86400 ))d$(( age % 86400 / 3600 ))h"
  elif [ "$age" -ge 3600 ]; then when="$(( age / 3600 ))h$(( age % 3600 / 60 ))m"
  else when="$(( age / 60 ))m$(( age % 60 ))s"; fi
  case "$cwd" in *" (deleted)") cwd="${cwd% (deleted)}  [DELETED]" ;; esac
  printf '%-8s %-6s %-9s %s\n' "$pid" "$(port_of "$pid")" "$when" "$cwd"
  found=$(( found + 1 ))
done
[ "$found" -gt 0 ] || echo "(no lobole running)"
