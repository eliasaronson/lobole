#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# Self-test for tools/ci-local.sh's lanes, in about a minute. Run it after editing
# ci-local.sh; it is not part of any suite, since only an edit to that script
# can break what it checks.
#
# It runs a copy of ci-local.sh in a scratch root where tools/nix-run,
# tools/ctest, tools/gui-tests-xvfb.sh, tools/build-jobs, cmake and docker are
# stubs that sleep a moment and log when they start and stop, then checks
# what the real script promises (docs/architecture.md#ci-on-this-machine):
#   - the builds in flight never use more -j than one build was allowed;
#   - at most one suite runs at a time;
#   - a clean run exits 0;
#   - a lane whose build fails is reported, the other lanes still finish, and
#     the run exits non-zero;
#   - --serial runs one step at a time;
#   - a matrix entry's test exclusion and extra packages reach its container
#     whole, spaces and all (a planted entry, ubuntu:99.04, carries both).
# An argument names another copy of the script to test (a planted break).
set -uo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
script="${1:-$repo/tools/ci-local.sh}"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
BUDGET=12
failures=0

# run <name> <FAIL substring or ""> [ci-local args...]
# Leaves the run's events in $tmp/<name>/events and its exit status in $rc.
run() {
  local name="$1" fail="$2"; shift 2
  local out="$tmp/$name" fake="$tmp/$name/root"
  mkdir -p "$fake/tools" "$fake/.github/workflows" "$fake/tests" "$out/bin"
  cp "$script" "$fake/tools/ci-local.sh"
  cp "$repo/.github/workflows/ci.yml" "$fake/.github/workflows/"
  # A planted matrix entry whose exclusion and package list both hold spaces:
  # what a lane must hand its container as ONE argument each.
  python3 - "$fake/.github/workflows/ci.yml" <<'PY2'
import sys
p = sys.argv[1]
s = open(p).read()
anchor = "        include:\n"
assert anchor in s, "ci.yml has no matrix include: to plant an entry in"
s = s.replace(anchor, anchor +
    '          - image: ubuntu:99.04\n'
    '            exclude_tests: "Planted case one|Planted case two"\n'
    '            extra_packages: "planted-pkg-a planted-pkg-b"\n', 1)
open(p, "w").write(s)
PY2
  : > "$out/events"
  export EV="$out/events" FAIL="$fail"

  cat > "$out/bin/event" <<'EOF'
#!/usr/bin/env bash
echo "$(date +%s%N) $1 $2 $3 ${4:-0}" >> "$EV"
EOF
  # work <kind> <name> <seconds> <j>
  cat > "$out/bin/work" <<'EOF'
#!/usr/bin/env bash
event "$1" "$2" start "$4"; sleep "$3"; event "$1" "$2" end "$4"
echo "fake $1 $2 output line"
if [ -n "$FAIL" ] && [[ "$2" == *"$FAIL"* ]]; then echo "fake FAILURE in $2"; exit 1; fi
echo "100% tests passed"
EOF
  cat > "$out/bin/cmake" <<'EOF'
#!/usr/bin/env bash
if [ "$1" = --build ]; then
  tree="$2"; j=$(printf '%s\n' "$@" | sed -n 's/^-j//p')
  [[ "$tree" == *no-te* ]] && { mkdir -p "$tree"; printf '#!/bin/sh\nexit 1\n' > "$tree/lobole"; chmod +x "$tree/lobole"; }
  exec work build "$(basename "$tree")" 2 "$j"
fi
while [ $# -gt 0 ]; do [ "$1" = -B ] && mkdir -p "$2" && touch "$2/CMakeCache.txt"; shift; done
EOF
  cat > "$out/bin/docker" <<'EOF'
#!/usr/bin/env bash
case "$1" in
  build) printf '%s\t%s\n' "$4" "$(grep '^RUN' "$5/Dockerfile")" >> "$EV.dockerfiles"
         exec work image "${4}" 0.3 0 ;;
  ps) exit 0 ;;
  run)
    name=""; for ((i=1;i<=$#;i++)); do [ "${!i}" = --name ] && { k=$((i+1)); name="${!k}"; }; done
    cmd="${@: -1}"
    if [[ "$cmd" == *"cmake --build"* ]]; then
      j=$(sed -n 's/.*cmake --build \/b -j\([0-9]*\).*/\1/p' <<< "$cmd")
      exec work build "$name" 2 "$j"
    elif [[ "$cmd" == *ctest* ]]; then
      printf '%s\t%s\n' "$name" "$cmd" >> "$EV.suites"
      exec work suite "$name" 1.5 0
    else exit 0; fi ;;
esac
EOF
  cat > "$fake/tools/nix-run" <<EOF
#!/usr/bin/env bash
PATH="$out/bin:\$PATH" exec bash -c "\$1"
EOF
  printf '#!/usr/bin/env bash\necho %s\n' "$BUDGET" > "$fake/tools/build-jobs"
  cat > "$fake/tools/ctest" <<'EOF'
#!/usr/bin/env bash
exec work suite "ctest-$(basename "$2")" 1.5 0
EOF
  cat > "$fake/tools/gui-tests-xvfb.sh" <<'EOF'
#!/usr/bin/env bash
exec work suite gui 1.5 0
EOF
  cat > "$fake/tests/headless_smoke.py" <<'EOF'
import subprocess; subprocess.run(["work", "suite", "no-te-smoke", "0.5", "0"], check=True)
EOF
  chmod +x "$out/bin/"* "$fake/tools/"*
  PATH="$out/bin:$PATH" bash "$fake/tools/ci-local.sh" "$@" > "$out/stdout" 2>&1
  rc=$?
}

# peaks <events>: prints "<peak build -j> <peak suites> <peak steps> <ends>"
peaks() {
  python3 - "$1" <<'PY'
import sys
evs = sorted((l.split() for l in open(sys.argv[1])), key=lambda e: int(e[0]))
running = {}
pb = ps = pa = ends = 0
for t, kind, name, what, j in evs:
    if what == "start":
        running[(kind, name)] = int(j)
    else:
        running.pop((kind, name), None)
        ends += 1
    pb = max(pb, sum(v for (k, _), v in running.items() if k == "build"))
    ps = max(ps, sum(1 for (k, _) in running if k == "suite"))
    pa = max(pa, sum(1 for (k, _) in running if k != "image"))
print(pb, ps, pa, ends)
PY
}

check() { # check <what> <condition-result 0|1>
  if [ "$2" = 0 ]; then echo "  ok    $1"; else echo "  FAIL  $1"; failures=$((failures+1)); fi
}

echo "clean parallel run"
run clean ""
read -r pb ps pa ends < <(peaks "$tmp/clean/events")
check "exits 0 (got $rc)" "$([ "$rc" = 0 ]; echo $?)"
check "build -j in flight <= $BUDGET (peak $pb)" "$([ "$pb" -le "$BUDGET" ]; echo $?)"
check "one suite at a time (peak $ps)" "$([ "$ps" -le 1 ]; echo $?)"
check "lanes overlap (peak steps at once $pa)" "$([ "$pa" -ge 2 ]; echo $?)"
clean_ends=$ends
planted_image="$(grep 'designer-ci-ubuntu-99-04' "$tmp/clean/events.dockerfiles")"
planted_suite="$(grep -- '-ubuntu-99-04-suite' "$tmp/clean/events.suites")"
check "the planted image installs both its extra packages" \
  "$([[ "$planted_image" == *"planted-pkg-a planted-pkg-b"* ]]; echo $?)"
check "no word of its exclusion became a package" \
  "$([[ "$planted_image" != *Planted* ]]; echo $?)"
check "its suite excludes the whole alternation" \
  "$([[ "$planted_suite" == *'-E "Planted case one|Planted case two"'* ]]; echo $?)"

echo "a container build that fails"
run broken "24-04-build"
read -r pb ps pa ends < <(peaks "$tmp/broken/events")
check "exits non-zero (got $rc)" "$([ "$rc" != 0 ]; echo $?)"
check "the failure is reported" "$(grep -qi 'fail' "$tmp/broken/stdout"; echo $?)"
check "the other lanes finish ($ends steps of $clean_ends; only the broken lane's suite is skipped)" \
  "$([ "$ends" -ge $((clean_ends - 1)) ]; echo $?)"
check "build -j in flight <= $BUDGET (peak $pb)" "$([ "$pb" -le "$BUDGET" ]; echo $?)"

echo "--serial"
run serial "" --serial
read -r pb ps pa ends < <(peaks "$tmp/serial/events")
check "exits 0 (got $rc)" "$([ "$rc" = 0 ]; echo $?)"
check "one step at a time (peak $pa)" "$([ "$pa" -le 1 ]; echo $?)"

if [ "$failures" -gt 0 ]; then
  echo "ci-local-selftest: $failures check(s) failed"
  exit 1
fi
echo "ci-local-selftest: all checks passed"
