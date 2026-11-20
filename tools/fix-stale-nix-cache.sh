#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# A nix GC collects the store paths a CMake cache was configured against, and
# every build tree here goes stale at once. The symptoms do not name the cause:
#
#   Imported target "Vulkan::Vulkan" includes non-existent path ...
#   ccache: No such file or directory        (every compile "fails to compile")
#   undefined reference to `vterm_new'       (a half-cleared pkg-config module)
#   libnglib.so: cannot open shared object   (a binary linked against a corpse)
#   OpenCASCADE not found                    (a configure ran after the GC)
#
# This unsets exactly the cache entries that point at collected store paths and
# reconfigures, so cmake re-finds them in the current store. Cheap and safe:
# ccache is warm, so what is actually rebuilt is only what moved.
#
#   tools/fix-stale-nix-cache.sh              # every build tree it can find
#   tools/fix-stale-nix-cache.sh build        # just this one
#   tools/fix-stale-nix-cache.sh --dry-run
#   tools/fix-stale-nix-cache.sh --check build  # exit 1 and say why if stale;
#                                             # changes nothing
#
# It also unsets an entry naming an older build of a package shell.nix now
# resolves elsewhere: that tree would still build, against the old one. Two
# shapes of it, and neither is collected, so neither is caught by asking the
# store: the same name under another hash (an OpenCASCADE patch makes a new
# opencascade-occt-7.9.3 beside the old one), and another VERSION of the same
# package (Catch2_DIR naming catch2-3.16.0 while the headers came from
# 3.16.1: undefined Catch::AutoReg at link). Both also cost every compile
# that includes the package its ccache entry, since the -isystem path is
# part of the hash: three trees on three kernels shared almost nothing.
#
# tools/build.sh, tools/worktree-build.sh and tools/stale-binaries.sh ask
# --check before they build or test, so a tree is put back on the current
# environment on its next build, and a suite refuses one that is not.
#
# The pkg-config half is the part worth knowing: pkg_check_modules
# short-circuits on a cached <PREFIX>_FOUND, so clearing only the stale
# pkgcfg_lib_<PREFIX>_<lib> leaves the module "found" with NO libraries to
# link — a configure that looks perfect and a link that fails. The whole
# prefix goes, or none of it.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

dry=0 check=0
dirs=()
for a in "$@"; do
  case "$a" in
    --dry-run) dry=1 ;;
    --check) check=1 dry=1 ;;
    -h|--help) sed -n '2,38p' "$0"; exit 0 ;;
    *) dirs+=("$a") ;;
  esac
done
if [ ${#dirs[@]} -eq 0 ]; then
  for d in build build-asan build-tsan build-coverage \
           .ci-local/build .ci-local/build-no-te .ci-local/build-nocam \
           ../lobole-wt*/build; do
    [ -f "$d/CMakeCache.txt" ] && dirs+=("$d")
  done
fi

# The store paths shell.nix resolves to now. A cache entry naming another
# build of the same package (the same name after the hash) is stale too:
# nothing was collected, but the package moved -- an OpenCASCADE patch added
# to nix/occt-overlay.nix gives a new opencascade-occt beside the old one,
# and a tree still configured against the old one builds without the patch.
#
# In shell.nix's environment through tools/nix-run (0.1 s; nix-shell is 5),
# or this one where there is no nix (a CI container): nothing is "current"
# there, so only collected paths and not-found packages count.
in_env() {
  if command -v nix-instantiate >/dev/null; then
    "$root/tools/nix-run" "$1"
  else
    bash -c "$1"
  fi
}
current_store="$(mktemp)"
trap 'rm -f "$current_store"' EXIT
if [ -n "${LOBOLE_FSNC_CURRENT:-}" ]; then
  # The smoke's: the store paths to call current, one per line.
  cp "$LOBOLE_FSNC_CURRENT" "$current_store"
elif command -v nix-instantiate >/dev/null; then
  in_env 'env' 2>/dev/null |
    grep -o '/nix/store/[a-z0-9]\{32\}-[^/:;" ]*' | sort -u > "$current_store" || true
fi

reasons="$(mktemp)"
trap 'rm -f "$current_store" "$reasons"' EXIT
stale_vars() {
  python3 - "$1" "$current_store" "$reasons" <<'PY'
import os, re, sys
pat = re.compile(r'/nix/store/[a-z0-9]{32}-[^;:"\s]*')
entries = []
for line in open(sys.argv[1]):
    line = line.rstrip('\n')
    if line.startswith(('//', '#')) or '=' not in line or ':' not in line:
        continue
    entries.append((line.split(':', 1)[0], line.split('=', 1)[1]))

# A store name is pname-version[-output]: the pname runs to the first dash
# before a digit, the output is a last all-letter component (dev, lib, bin).
def key(name):
    m = re.match(r'(.+?)-(\d.*?)(-[a-z]+)?$', name)
    return (m.group(1), m.group(3) or '') if m else None

# name after the hash -> the hashes shell.nix resolves it to now; and
# (pname, output) -> the names (versions) it resolves to now.
current, versions = {}, {}
for p in open(sys.argv[2]).read().split():
    base = p.split('/')[3]
    current.setdefault(base[33:], set()).add(base[:32])
    k = key(base[33:])
    if k:
        versions.setdefault(k, set()).add(base[33:])

def why(base):
    name, h = base[33:], base[:32]
    if not os.path.exists('/nix/store/' + base):
        return 'collected'
    if name in current and h not in current[name]:
        return 'rebuilt (now ' + sorted(current[name])[0][:8] + '-' + name + ')'
    k = key(name)
    if name not in current and k in versions:
        return 'another version (now ' + ', '.join(sorted(versions[k])) + ')'
    return None

stale, reasons = [], {}
for n, v in entries:
    for m in pat.findall(v):
        r = why(m.split('/')[3])
        if r:
            stale.append(n)
            reasons.setdefault(m.split('/')[3][33:], r)
            break

# A package's config directory cached as not found: a configure that ran
# after a GC took the package (OpenCASCADE, then the whole tree failed on a
# missing header). Nothing points at a store path any more, so the check
# above calls it clean; cleared, cmake searches for it again. A healthy tree
# has no such entry.
stale += [n for n, v in entries
          if n.endswith('_DIR') and v.endswith('-NOTFOUND') and n not in stale]

# Take the whole pkg-config prefix with any stale entry of it (see header).
# A prefix holds underscores (LOBOLE_DBUS); the library after it does not.
# Read as the first word, pkgcfg_lib_LOBOLE_DBUS_dbus-1 took every LOBOLE_*
# entry with it, the build's own options among them.
prefixes = set()
for n in stale:
    m = (re.match(r'pkgcfg_lib_([A-Za-z0-9_]+)_[^_]+$', n) or
         re.match(r'([A-Za-z0-9_]+?)_(STATIC_)?(CFLAGS|LDFLAGS|INCLUDEDIR|LIBDIR'
                  r'|PREFIX|INCLUDE_DIRS|LIBRARY_DIRS)(_|$)', n))
    if m:
        prefixes.add(m.group(1))
names = stale + [n for n, _ in entries
                 if any(n.startswith(p + '_') or
                        n.startswith('pkgcfg_lib_' + p + '_') for p in prefixes)]
print('\n'.join(dict.fromkeys(names)))
# What moved, for the one line a build or a suite prints about it.
with open(sys.argv[3], 'w') as f:
    for name, r in sorted(reasons.items()):
        f.write('%s: %s\n' % (name, r))
PY
}

# The other staleness: nothing was collected, but shell.nix now resolves a
# DIFFERENT toolchain than the one the tree was configured with. The tree
# still builds -- against the old compiler and headers -- and shares no ccache
# entry with a tree configured today (a worktree), and it is the tree the
# next GC breaks. CMake throws its whole cache away when the compiler
# changes, options included, so the options are carried over by hand.
current_cxx="$(in_env 'command -v g++' 2>/dev/null | tail -1)"

fixed=0 stale_trees=0
for d in "${dirs[@]}"; do
  cached_cxx="$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$d/CMakeCache.txt")"
  # The source the tree was configured from -- a worktree's own checkout,
  # not the one this script runs from -- read before its cache is deleted.
  # `-S .` pointed every worktree's build/ at this checkout's source.
  src="$(sed -n 's/^CMAKE_HOME_DIRECTORY:[A-Z]*=//p' "$d/CMakeCache.txt")"
  if [ -z "$src" ] || [ ! -f "$src/CMakeLists.txt" ]; then
    printf '%-28s its cache names no source tree (%s); skipped\n' "$d" "${src:-none}" >&2
    continue
  fi
  if [ -n "$current_cxx" ] && [ -n "$cached_cxx" ] && [ "$cached_cxx" != "$current_cxx" ]; then
    keep="$(grep -E '^(CMAKE_BUILD_TYPE|ENABLE_[A-Z_]+|CMAKE_EXPORT_COMPILE_COMMANDS):' "$d/CMakeCache.txt" \
            | grep -v ':[A-Z]*=$' | sed -E 's/^([^:]+):[A-Z]+=(.*)$/-D\1=\2/' | tr '\n' ' ')"
    printf '%-28s toolchain moved: %s\n%-28s   -> %s\n' "$d" "$cached_cxx" "" "$current_cxx"
    stale_trees=$((stale_trees+1))
    [ "$check" = 1 ] && continue
    if [ "$dry" = 1 ]; then
      printf '   would reconfigure fresh from %s with: %s\n' "$src" "$keep"
      continue
    fi
    rm -rf "$d/CMakeCache.txt" "$d/CMakeFiles"
    if in_env "cmake -S '$src' -B '$d' -G Ninja $keep" > /tmp/fsnc.$$.log 2>&1; then
      echo "   reconfigured fresh (everything recompiles once, then shares ccache)"
      fixed=$((fixed+1))
    else
      tail -20 /tmp/fsnc.$$.log
      echo "   RECONFIGURE FAILED for $d" >&2
      rm -f /tmp/fsnc.$$.log
      exit 1
    fi
    rm -f /tmp/fsnc.$$.log
    continue
  fi
  vars="$(stale_vars "$d/CMakeCache.txt")"
  if [ -z "$vars" ]; then
    printf '%-28s clean\n' "$d"
    continue
  fi
  n=$(printf '%s\n' "$vars" | wc -l)
  printf '%-28s %s stale cache entries\n' "$d" "$n"
  sed 's/^/   /' "$reasons" | head -8
  stale_trees=$((stale_trees+1))
  [ "$check" = 1 ] && continue
  args="$(printf '%s\n' "$vars" | sed 's/^/-U/' | tr '\n' ' ')"
  if [ "$dry" = 1 ]; then
    printf '   would run: cmake -S %s -B %s %s\n' "$src" "$d" "$args"
    continue
  fi
  if in_env "cmake -S '$src' -B '$d' $args" > /tmp/fsnc.$$.log 2>&1; then
    echo "   reconfigured"
    fixed=$((fixed+1))
  else
    tail -20 /tmp/fsnc.$$.log
    echo "   RECONFIGURE FAILED for $d" >&2
    rm -f /tmp/fsnc.$$.log
    exit 1
  fi
  rm -f /tmp/fsnc.$$.log
done

# A tree can also hold binaries linked against paths that were collected after
# they were built; the cache is clean but the .so is gone. Only a relink fixes
# that, and only a build knows which targets need one.
[ "$fixed" -gt 0 ] && echo && echo "Rebuild the trees you use; a link error naming a missing .so means the same GC took a binary's dependency, and the relink fixes it."
if [ "$check" = 1 ] && [ "$stale_trees" -gt 0 ]; then
  exit 1
fi
exit 0
