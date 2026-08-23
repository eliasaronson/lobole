#!/usr/bin/env bash
# Run the CI jobs on this machine, so a red build costs a coffee instead of a
# push. Everything here is DERIVED from .github/workflows/ci.yml — the package
# list, the container images, the excluded tests — so the two cannot drift.
# If CI changes, this follows.
#
#   tools/ci-local.sh              # everything: nix suite + both containers
#   tools/ci-local.sh --nix        # just the local (nix) suite, ~2 min
#   tools/ci-local.sh --containers # just the stock-Ubuntu containers
#   tools/ci-local.sh --image ubuntu:26.04
#   tools/ci-local.sh --asan       # ASan+UBSan build and suite, ~6 min warm
#
# Build trees live in .ci-local/ (gitignored) and are reused, so the second
# run of a container is a rebuild of what changed rather than a fresh compile.
#
# The GUI suite IS covered here (--no-gui opts out), in a shuffled order.
# GitHub never runs it — every ctest there is `-LE gui` — so this script is
# the only automated gate it has, which is an argument for including it
# rather than the reverse. Shuffled because three order-dependent failures in
# one week were each invisible until the shard partition happened to move.
#
# Not covered here: anything needing GitHub itself.
#
# Alternative: `act` runs the real workflow YAML in Docker, which is more
# faithful but pulls multi-GB runner images and emulates `container:` matrix
# jobs imperfectly. Use it when you are debugging the workflow file itself;
# use this when you are debugging your code.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
workflow="$root/.github/workflows/ci.yml"
cd "$root"

run_nix=1
run_containers=1
run_asan=0
run_gui=1
only_image=""
while [ $# -gt 0 ]; do
  case "$1" in
    --nix) run_containers=0 ;;
    --containers) run_nix=0 ;;
    # Opt-in, like the GitHub job it mirrors: an ASan build is a separate
    # ~4-minute compile, which is too much to put in front of every
    # "did I break the build?" run.
    --asan) run_asan=1; run_nix=0; run_containers=0 ;;
    # The GUI suite is ~95 s and needs a display; --no-gui is the escape
    # hatch for a machine without one (and for --containers, which has none).
    --no-gui) run_gui=0 ;;
    --image) only_image="$2"; shift ;;
    -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

# ---- read the workflow, so this script never states a fact of its own ------
read_workflow() {
  python3 - "$workflow" <<'PY'
import re, sys
text = open(sys.argv[1]).read()

# The stock-ubuntu job's apt list: the continuation lines that follow its
# install command, stopping at the first line that does not continue.
block = text.split("Install exactly what the README lists", 1)[1]
pkgs, collecting = [], False
for line in block.splitlines():
    if not collecting:
        collecting = "apt-get install" in line
        continue
    pkgs += line.strip().rstrip("\\").split()
    if not line.rstrip().endswith("\\"):
        break
print("PACKAGES=" + " ".join(p for p in pkgs if p and not p.startswith("-")))

# The matrix: image and the tests it excludes. "@@" separates them because
# the exclusion is itself a |-alternation.
for image, excl in re.findall(
        r"- image:\s*(\S+)\s*\n(?:\s*#[^\n]*\n)*\s*exclude_tests:\s*\"([^\"]*)\"",
        text):
    print(f"MATRIX={image}@@{excl}")
PY
}

eval "$(read_workflow | grep '^PACKAGES=' | sed 's/^PACKAGES=/PACKAGES="/; s/$/"/')"
mapfile -t MATRIX < <(read_workflow | sed -n 's/^MATRIX=//p')

step() { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }
ok()   { printf '\033[1;32m   %s\033[0m\n' "$*"; }
fail() { printf '\033[1;31m   %s\033[0m\n' "$*"; }

# Run a test command, keeping its whole output in .ci-local/<name>.log: three
# lines on success (the ctest summary), the last 40 plus the path on failure.
# The first version piped straight to `tail -3`, which meant a red run showed
# a count and threw the diagnosis away — the opposite of what a failure needs.
#   run_tests <name> <human label> <command...>
run_tests() {
  local name="$1" label="$2"; shift 2
  local log="$root/.ci-local/$name.log"
  mkdir -p "$root/.ci-local"
  if "$@" > "$log" 2>&1; then
    tail -3 "$log"
    ok "$label passed"
  else
    tail -40 "$log"
    fail "$label FAILED — full log: $log"
    failures=$((failures+1))
  fi
}

started=$SECONDS
failures=0

# ---- job 1: what build-and-test does, using this machine's toolchain -------
if [ "$run_nix" = 1 ]; then
  step "build-and-test (local toolchain)"
  nix-shell shell.nix --run "cmake -S . -B .ci-local/build -G Ninja -DCMAKE_BUILD_TYPE=Release" > /dev/null
  nix-shell shell.nix --run "cmake --build .ci-local/build -j$(nproc)" > /dev/null
  run_tests headless "headless suite" \
    nix-shell shell.nix --run "ctest --test-dir .ci-local/build -LE gui -j$(nproc) --output-on-failure"

  # The GUI suite. Shuffled, and its seed is in the log: a failure that
  # cannot be replayed is a failure nobody fixes. Sharded as usual — this is
  # a developer machine with cores to spare, unlike a hosted runner.
  if [ "$run_gui" = 1 ]; then
    run_tests gui "GUI suite (shuffled)" ./tools/gui-tests-xvfb.sh --shuffle
  fi

  step "build without the test engine"
  nix-shell shell.nix --run "cmake -S . -B .ci-local/build-no-te -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_TEST_ENGINE=OFF" > /dev/null
  nix-shell shell.nix --run "cmake --build .ci-local/build-no-te -j$(nproc)" > /dev/null
  if nix-shell shell.nix --run "python3 tests/headless_smoke.py .ci-local/build-no-te/designer" > /dev/null \
     && ! .ci-local/build-no-te/designer --gui-tests --hidden 2>/dev/null; then
    ok "engine-free build works and refuses --gui-tests"
  else
    fail "engine-free build FAILED"; failures=$((failures+1))
  fi
fi

# ---- the sanitizer job (opt-in here, weekly/manual on GitHub) --------------
if [ "$run_asan" = 1 ]; then
  step "sanitizers (ASan + UBSan)"
  # build-asan, not .ci-local/: the same tree tools/run-sanitized-tests.sh
  # uses, so an incremental developer build and this share their objects.
  source "$root/tools/sanitizer-env.sh"
  nix-shell shell.nix --run "cmake -S . -B build-asan -G Ninja -DENABLE_SANITIZERS=ON" > /dev/null
  nix-shell shell.nix --run "cmake --build build-asan -j$(nproc) --target designer designer_tests" > /dev/null
  run_tests asan "sanitized suite" \
    nix-shell shell.nix --run "ctest --test-dir build-asan -LE gui -j$(nproc) --output-on-failure -E '$SANITIZER_TEST_EXCLUDE'"
fi

# ---- job 2: the stock-Ubuntu containers ------------------------------------
if [ "$run_containers" = 1 ]; then
  command -v docker > /dev/null || { fail "docker not found — skipping containers"; exit $((failures > 0)); }
  for entry in "${MATRIX[@]}"; do
    image="${entry%%@@*}"
    exclude="${entry#*@@}"
    [ -n "$only_image" ] && [ "$image" != "$only_image" ] && continue
    tag="designer-ci-${image//[:.]/-}"
    build_dir=".ci-local/${image//[:.]/-}"
    mkdir -p "$build_dir"

    step "stock-ubuntu ($image)"
    # The image is the workflow's package list and nothing else — that is the
    # point of this job: it fails if the README's list is incomplete.
    printf 'FROM %s\nENV DEBIAN_FRONTEND=noninteractive\nRUN apt-get update && apt-get install -y --no-install-recommends ca-certificates %s && rm -rf /var/lib/apt/lists/*\n' \
      "$image" "$PACKAGES" > "$build_dir/Dockerfile"
    docker build -q -t "$tag" "$build_dir" > /dev/null

    # No '> /dev/null' on the build: run_tests keeps the whole log and prints
    # it only on failure, and a compile error with its output discarded leaves
    # an eleven-line log that says nothing (which is exactly what happened).
    ctest_cmd="ctest --test-dir /b -LE gui -j\$(nproc) --output-on-failure"
    [ -n "$exclude" ] && ctest_cmd="$ctest_cmd -E \"$exclude\""
    # `set -o pipefail`: without it the pipe to tail swallows ctest's exit
    # status and every run looks green.
    run_tests "${image//[:.]/-}" "$image" \
      docker run --rm -v "$root:/src:ro" -v "$root/$build_dir:/b" "$tag" bash -c "
         set -o pipefail
         cmake -S /src -B /b -G Ninja -DCMAKE_BUILD_TYPE=Release &&
         cmake --build /b -j\$(nproc) && $ctest_cmd"
  done
fi

printf '\n%s in %dm%02ds\n' \
  "$([ "$failures" = 0 ] && echo 'CI-local: everything passed' || echo "CI-local: $failures job(s) failed")" \
  $(( (SECONDS-started)/60 )) $(( (SECONDS-started)%60 ))
exit $(( failures > 0 ))
