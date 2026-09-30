#!/usr/bin/env bash
# A nix GC collects the store paths a CMake cache was configured against, and
# every build tree here goes stale at once. The symptoms do not name the cause:
#
#   Imported target "Vulkan::Vulkan" includes non-existent path ...
#   ccache: No such file or directory        (every compile "fails to compile")
#   undefined reference to `vterm_new'       (a half-cleared pkg-config module)
#   libnglib.so: cannot open shared object   (a binary linked against a corpse)
#
# This unsets exactly the cache entries that point at collected store paths and
# reconfigures, so cmake re-finds them in the current store. Cheap and safe:
# ccache is warm, so what is actually rebuilt is only what moved.
#
#   tools/fix-stale-nix-cache.sh              # every build tree it can find
#   tools/fix-stale-nix-cache.sh build        # just this one
#   tools/fix-stale-nix-cache.sh --dry-run
#
# The pkg-config half is the part worth knowing: pkg_check_modules
# short-circuits on a cached <PREFIX>_FOUND, so clearing only the stale
# pkgcfg_lib_<PREFIX>_<lib> leaves the module "found" with NO libraries to
# link — a configure that looks perfect and a link that fails. The whole
# prefix goes, or none of it.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

dry=0
dirs=()
for a in "$@"; do
  case "$a" in
    --dry-run) dry=1 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) dirs+=("$a") ;;
  esac
done
if [ ${#dirs[@]} -eq 0 ]; then
  for d in build build-asan build-tsan build-coverage \
           .ci-local/build .ci-local/build-no-te .ci-local/build-nocam \
           ../designer-wt*/build; do
    [ -f "$d/CMakeCache.txt" ] && dirs+=("$d")
  done
fi

stale_vars() {
  python3 - "$1" <<'PY'
import os, re, sys
pat = re.compile(r'/nix/store/[a-z0-9]{32}-[^;:"\s]*')
entries = []
for line in open(sys.argv[1]):
    line = line.rstrip('\n')
    if line.startswith(('//', '#')) or '=' not in line or ':' not in line:
        continue
    entries.append((line.split(':', 1)[0], line.split('=', 1)[1]))

stale = [n for n, v in entries
         if any(not os.path.exists('/nix/store/' + m.split('/')[3])
                for m in pat.findall(v))]

# Take the whole pkg-config prefix with any stale entry of it (see header).
prefixes = set()
for n in stale:
    m = (re.match(r'pkgcfg_lib_([A-Za-z0-9]+)_', n) or
         re.match(r'([A-Za-z0-9]+)_(CFLAGS|LDFLAGS|INCLUDEDIR|LIBDIR|PREFIX'
                  r'|INCLUDE_DIRS|LIBRARY_DIRS)(_|$)', n))
    if m:
        prefixes.add(m.group(1))
names = stale + [n for n, _ in entries
                 if any(n.startswith(p + '_') or
                        n.startswith('pkgcfg_lib_' + p + '_') for p in prefixes)]
print('\n'.join(dict.fromkeys(names)))
PY
}

# The other staleness: nothing was collected, but shell.nix now resolves a
# DIFFERENT toolchain than the one the tree was configured with. The tree
# still builds -- against the old compiler and headers -- and shares no ccache
# entry with a tree configured today (a worktree), and it is the tree the
# next GC breaks. CMake throws its whole cache away when the compiler
# changes, options included, so the options are carried over by hand.
current_cxx="$(nix-shell shell.nix --run 'command -v g++' 2>/dev/null | tail -1)"

fixed=0
for d in "${dirs[@]}"; do
  cached_cxx="$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$d/CMakeCache.txt")"
  if [ -n "$current_cxx" ] && [ -n "$cached_cxx" ] && [ "$cached_cxx" != "$current_cxx" ]; then
    keep="$(grep -E '^(CMAKE_BUILD_TYPE|ENABLE_[A-Z_]+|CMAKE_EXPORT_COMPILE_COMMANDS):' "$d/CMakeCache.txt" \
            | grep -v ':[A-Z]*=$' | sed -E 's/^([^:]+):[A-Z]+=(.*)$/-D\1=\2/' | tr '\n' ' ')"
    printf '%-28s toolchain moved: %s\n%-28s   -> %s\n' "$d" "$cached_cxx" "" "$current_cxx"
    if [ "$dry" = 1 ]; then
      printf '   would reconfigure fresh with: %s\n' "$keep"
      continue
    fi
    rm -rf "$d/CMakeCache.txt" "$d/CMakeFiles"
    if nix-shell shell.nix --run "cmake -S . -B $d -G Ninja $keep" > /tmp/fsnc.$$.log 2>&1; then
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
  args="$(printf '%s\n' "$vars" | sed 's/^/-U/' | tr '\n' ' ')"
  if [ "$dry" = 1 ]; then
    printf '   would run: cmake -S . -B %s %s\n' "$d" "$args"
    continue
  fi
  if nix-shell shell.nix --run "cmake -S . -B $d $args" > /tmp/fsnc.$$.log 2>&1; then
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
exit 0
