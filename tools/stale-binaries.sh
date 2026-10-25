#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# Is a build tree's app or test binary behind its source?
#
#   tools/stale-binaries.sh build     # prints "<binary> is older than <file>"
#                                     # per stale binary; exits 1 if any
#
# tools/ctest asks this before a suite: a binary nobody rebuilt tests the code
# it was linked from, not the code in the tree. femgpu_tests sat twelve days
# behind main because tools/build.sh never built it; it failed only because a
# nix GC had taken its libraries. Had they been there it would have passed,
# testing old code.
#
# A binary is behind when any file it is built from (`ninja -t inputs`, the
# sources, headers and libraries) is newer than it. `ninja -n` cannot answer
# this here: the tree re-checks its globbed directories on every build, which
# a dry run always reports as work to do. Runs in shell.nix's environment, for
# ninja; the caller provides it.
set -uo pipefail
dir="${1:?usage: stale-binaries.sh BUILD_DIR}"
[ -f "$dir/build.ninja" ] || exit 0
cd "$dir" || exit 2
stale=0
for target in designer designer_tests femgpu_tests; do
  bin="$(sed -n "s/^build $target: phony //p" build.ninja)"
  [ -n "$bin" ] && [ -e "$bin" ] || continue
  newer="$(ninja -t inputs "$target" 2>/dev/null | while read -r f; do
    if [ -f "$f" ] && [ "$f" -nt "$bin" ]; then
      echo "$f"
      break
    fi
  done)"
  if [ -n "$newer" ]; then
    echo "$bin is older than $newer"
    stale=1
  fi
done
exit "$stale"
