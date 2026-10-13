#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# Run the CI jobs on this machine, so a red build costs a coffee instead of a
# push. Everything here is DERIVED from .github/workflows/ci.yml — the package
# list, the container images, the excluded tests — so the two cannot drift.
# If CI changes, this follows.
#
#   tools/ci-local.sh              # everything: nix suite + both containers
#   tools/ci-local.sh --nix        # just the local (nix) jobs, ~5 min:
#                                  # the suite, then the two configurations
#                                  # nobody builds by accident (no test
#                                  # engine, no CAM)
#   tools/ci-local.sh --containers # just the stock-Ubuntu containers
#   tools/ci-local.sh --image ubuntu:26.04
#   tools/ci-local.sh --pixel      # the gui-suite job's PIXEL gate, in a
#                                  # container that matches the runner
#   tools/ci-local.sh --pixel-record  # ...and record the baseline it wants
#   tools/ci-local.sh --asan       # ASan+UBSan build and suite, ~6 min warm
#   tools/ci-local.sh --tsan       # ThreadSanitizer build and suite
#
# Build trees live in .ci-local/ (gitignored) and are reused, so the second
# run of a container is a rebuild of what changed rather than a fresh compile.
#
# The GUI suite IS covered here (--no-gui opts out), in a shuffled order, on
# this machine's GPU. GitHub's gui-suite job runs it serially on software
# Vulkan (llvmpipe), and its pixel gate is --pixel's. Shuffled because three
# order-dependent failures in one week were each invisible until the shard
# partition happened to move.
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
run_tsan=0
run_gui=1
run_pixel=0
pixel_record=0
only_image=""
while [ $# -gt 0 ]; do
  case "$1" in
    --nix) run_containers=0 ;;
    --containers) run_nix=0 ;;
    # Opt-in, like the GitHub job it mirrors: an ASan build is a separate
    # ~4-minute compile, which is too much to put in front of every
    # "did I break the build?" run.
    --asan) run_asan=1; run_nix=0; run_containers=0 ;;
    --tsan) run_tsan=1; run_nix=0; run_containers=0 ;;
    # The gui-suite job's pixel gate. Its baseline is keyed to the DRIVER, so
    # it can only be checked — or recorded — where the driver is the runner's:
    # llvmpipe from the same Ubuntu image, not the nix mesa this machine
    # builds against. Nothing else here covered that step, and it was red for
    # two weeks with no local way to see it.
    --pixel) run_pixel=1; run_nix=0; run_containers=0 ;;
    --pixel-record) run_pixel=1; pixel_record=1; run_nix=0; run_containers=0 ;;
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
# The gui-suite job's own apt list — the same shape, and the first "apt-get
# install" in the file, since that job comes first. It carries the pieces the
# pixel gate needs (mesa-vulkan-drivers) and the README's list does not.
gui = text.split("gui-suite:", 1)[1]
gpkgs, collecting = [], False
for line in gui.splitlines():
    if not collecting:
        collecting = "apt-get install" in line
        continue
    gpkgs += line.strip().rstrip("\\").split()
    if not line.rstrip().endswith("\\"):
        break
print("GUI_PACKAGES=" + " ".join(p for p in gpkgs if p and not p.startswith("-")))
# ...and the runner it runs on ("ubuntu-24.04"), which names the image whose
# driver its pixel baseline belongs to. Stated here so moving the job to a
# newer runner moves the local check with it.
print("GUI_RUNS_ON=" + re.search(r"runs-on:\s*(\S+)", gui).group(1))

# The matrix: image and the tests it excludes. "@@" separates them because
# the exclusion is itself a |-alternation.
for image, excl in re.findall(
        r"- image:\s*(\S+)\s*\n(?:\s*#[^\n]*\n)*\s*exclude_tests:\s*\"([^\"]*)\"",
        text):
    print(f"MATRIX={image}@@{excl}")
PY
}

eval "$(read_workflow | grep '^PACKAGES=' | sed 's/^PACKAGES=/PACKAGES="/; s/$/"/')"
eval "$(read_workflow | grep '^GUI_PACKAGES=' | sed 's/^GUI_PACKAGES=/GUI_PACKAGES="/; s/$/"/')"
eval "$(read_workflow | grep '^GUI_RUNS_ON=' | sed 's/^GUI_RUNS_ON=/GUI_RUNS_ON="/; s/$/"/')"
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

# Configure and build, keeping the output in .ci-local/<name>-build.log.
# Ninja prints a failing compile on its STDOUT, so `> /dev/null` here used to
# swallow the diagnosis whole: a broken build exited 1 with nothing on screen
# but a stray CMake warning. Same rule as run_tests — quiet when green, the
# last 40 lines and a path when red.
#   run_build <name> <cmake-configure-args...> -- <cmake-build-args...>
run_build() {
  local name="$1"; shift
  local log="$root/.ci-local/$name-build.log"
  local cfg=() bld=()
  while [ "$1" != "--" ]; do cfg+=("$1"); shift; done
  shift
  bld=("$@")
  mkdir -p "$root/.ci-local"
  # A CMake cache keeps the store paths it found, so a tree configured before
  # shell.nix or nixpkgs moved goes on linking the old libraries, and passes
  # (docs/packaging/README.md#gcc-16-and-ipa-cp-clones). The environment's
  # link flags name every library's store path: when they change, the tree is
  # configured afresh.
  local tree="${bld[0]}" stamp
  stamp="$("$root/tools/nix-run" 'echo "$NIX_LDFLAGS $NIX_CFLAGS_COMPILE"' | sha256sum | cut -c1-16)"
  if [ -f "$tree/CMakeCache.txt" ] && [ "$(cat "$tree/.nix-env-stamp" 2>/dev/null)" != "$stamp" ]; then
    step "$name: the nix environment moved; configuring $tree afresh"
    rm -f "$tree/CMakeCache.txt"
  fi
  if "$root/tools/nix-run" "cmake ${cfg[*]}" > "$log" 2>&1 \
     && "$root/tools/nix-run" "cmake --build ${bld[*]}" >> "$log" 2>&1; then
    echo "$stamp" > "$tree/.nix-env-stamp"
    return 0
  fi
  tail -40 "$log"
  fail "$name build FAILED — full log: $log"
  exit 1
}

# The containers run as the invoking user (`--user`), so what they write into
# .ci-local/ is a file this user can rebuild over and delete. They used to run
# as root, and a worktree that had run them could not be removed without
# docker ("Permission denied" under .ci-local/ubuntu-24-04/). A tree left by
# such a run would now fail the --user build half way, so it is handed back
# first: once, through the same image, which is the only thing here that is
# root.
#   reclaim_tree <image tag> <dir under $root>
reclaim_tree() {
  local tag="$1" dir="$2"
  [ -n "$(find "$root/$dir" ! -user "$(id -u)" -print -quit 2>/dev/null)" ] || return 0
  step "reclaiming root-owned files in $dir (left by a run before --user)"
  docker run --rm -v "$root/$dir:/t" "$tag" chown -R "$(id -u):$(id -g)" /t \
    || { fail "could not chown $dir; remove it with: docker run --rm -v \"$root/$dir:/t\" $tag rm -rf /t/*"; exit 1; }
}

started=$SECONDS
failures=0
# Compilers from free memory, not cores (tools/build-jobs, ~1.5 GB each): at
# one per core the containers' builds took the host's memory.
jobs="$("$root/tools/build-jobs")"

# ---- job 1: what build-and-test does, using this machine's toolchain -------
if [ "$run_nix" = 1 ]; then
  step "build-and-test (local toolchain)"
  run_build release -S . -B .ci-local/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -- .ci-local/build -j"$jobs"
  run_tests headless "headless suite" \
    "$root/tools/ctest" --test-dir .ci-local/build -LE gui -j"$(nproc)" --output-on-failure

  # The GUI suite. Shuffled, and its seed is in the log: a failure that
  # cannot be replayed is a failure nobody fixes. Sharded as usual — this is
  # a developer machine with cores to spare, unlike a hosted runner.
  if [ "$run_gui" = 1 ]; then
    run_tests gui "GUI suite (shuffled)" ./tools/gui-tests-xvfb.sh --shuffle
  fi

  step "build without the test engine"
  run_build no-te -S . -B .ci-local/build-no-te -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_TEST_ENGINE=OFF \
    -- .ci-local/build-no-te -j"$jobs"
  if "$root/tools/nix-run" "python3 tests/headless_smoke.py .ci-local/build-no-te/lobole" > /dev/null \
     && ! .ci-local/build-no-te/lobole --gui-tests --hidden 2>/dev/null; then
    ok "engine-free build works and refuses --gui-tests"
  else
    fail "engine-free build FAILED"; failures=$((failures+1))
  fi

  # ...and without CAM, the job's last two steps. Both configurations exist
  # for the same reason and break the same way: nobody compiles them by
  # accident, so the only thing that holds them up is a job that does. Two
  # were broken on the same afternoon -- an engine-free compile and a
  # Spectra-less link -- and neither could be seen from a nix build.
  step "build without CAM"
  run_build no-cam -S . -B .ci-local/build-nocam -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_CAM=OFF \
    -- .ci-local/build-nocam -j"$jobs"
  run_tests nocam "CAM-free suite" \
    "$root/tools/ctest" --test-dir .ci-local/build-nocam -LE gui -j"$(nproc)" --output-on-failure
fi

# ---- the sanitizer job (opt-in here, weekly/manual on GitHub) --------------
if [ "$run_asan" = 1 ]; then
  step "sanitizers (ASan + UBSan)"
  # build-asan, not .ci-local/: the same tree tools/run-sanitized-tests.sh
  # uses, so an incremental developer build and this share their objects.
  source "$root/tools/sanitizer-env.sh"
  run_build asan -S . -B build-asan -G Ninja -DENABLE_SANITIZERS=ON \
    -- build-asan -j"$jobs" --target designer designer_tests
  run_tests asan "sanitized suite" \
    "$root/tools/ctest" --test-dir build-asan -LE gui -j"$(nproc)" --output-on-failure -E "$SANITIZER_TEST_EXCLUDE"
fi

# ---- the race job (opt-in; separate tree, ASan and TSan cannot combine) -----
if [ "$run_tsan" = 1 ]; then
  step "ThreadSanitizer"
  # Its own tree because -fsanitize=thread and -fsanitize=address cannot be
  # linked into one binary. Not part of any scheduled run: this is an
  # occasional audit like tools/coverage.sh, aimed at the parts of the tree
  # that stopped being single-threaded — the import worker, OCC meshing across
  # threads, async FEM, the remote server.
  run_build tsan -S . -B build-tsan -G Ninja -DENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -- build-tsan -j$(nproc) --target designer designer_tests
  # -j4, not -j$(nproc): TSan multiplies each process's memory footprint, and
  # a saturated machine turns a race hunt into a timeout hunt.
  run_tests tsan "race suite" \
    "$root/tools/ctest" --test-dir build-tsan -LE gui -j4 --output-on-failure
fi

# ---- job 2: the stock-Ubuntu containers ------------------------------------
if [ "$run_containers" = 1 ]; then
  command -v docker > /dev/null || { fail "docker not found — skipping containers"; exit $((failures > 0)); }
  # In a worktree, .git is a FILE naming the main checkout's .git/worktrees/X
  # by absolute path, and with only /src mounted every git call in the suite
  # (spdx_gate, format_version_gate) died "not a git repository". The common
  # git dir is mounted at its own path, read-only, so that pointer resolves.
  git_mount=()
  if [ -f "$root/.git" ]; then
    common="$(git -C "$root" rev-parse --path-format=absolute --git-common-dir)"
    git_mount=(--mount "type=bind,src=$common,dst=$common,readonly")
  fi
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
    reclaim_tree "$tag" "$build_dir"

    # No '> /dev/null' on the build: run_tests keeps the whole log and prints
    # it only on failure, and a compile error with its output discarded leaves
    # an eleven-line log that says nothing (which is exactly what happened).
    ctest_cmd="ctest --test-dir /b -LE gui -j\$(nproc) --output-on-failure"
    [ -n "$exclude" ] && ctest_cmd="$ctest_cmd -E \"$exclude\""
    # `set -o pipefail`: without it the pipe to tail swallows ctest's exit
    # status and every run looks green.
    run_tests "${image//[:.]/-}" "$image" \
      docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
        -v "$root:/src:ro" "${git_mount[@]}" -v "$root/$build_dir:/b" "$tag" bash -c "
         set -o pipefail
         cmake -S /src -B /b -G Ninja -DCMAKE_BUILD_TYPE=Release &&
         cmake --build /b -j$jobs && $ctest_cmd"
  done
fi

# ---- the gui-suite job's pixel gate, in the runner's driver ----------------
# Not the whole job: the GUI suite itself runs here under nix already. What
# only a matching container can answer is the PIXEL comparison, whose golden
# is keyed to the driver that drew it -- llvmpipe from ubuntu:24.04, which is
# what the runner installs and is NOT the mesa in shell.nix. The image is the
# workflow's own gui-suite package list, so it cannot drift from CI.
if [ "$run_pixel" = 1 ]; then
  command -v docker > /dev/null || { fail "docker not found"; exit 1; }
  # The job runs on a RUNNER, not in a container; the closest an image can
  # sit to it is the same Ubuntu release, which is where its driver comes
  # from. Derived, so moving the job moves this.
  image="${GUI_RUNS_ON/ubuntu-/ubuntu:}"
  tag="designer-ci-gui"
  build_dir=".ci-local/gui-build"
  mkdir -p "$build_dir"
  step "gui-suite pixel gate ($image)"
  printf 'FROM %s\nENV DEBIAN_FRONTEND=noninteractive\nRUN apt-get update && apt-get install -y --no-install-recommends ca-certificates python3 %s && rm -rf /var/lib/apt/lists/*\n' \
    "$image" "$GUI_PACKAGES" > "$build_dir/Dockerfile"
  docker build -q -t "$tag" "$build_dir" > /dev/null
  # The ICD discovery step of the job, verbatim in spirit: lavapipe's filename
  # moved once already (lvp_icd.x86_64.json -> lvp_icd.json).
  pixel_cmd="python3 tests/render_pixel_smoke.py /b/lobole --require-baseline"
  [ "$pixel_record" = 1 ] && pixel_cmd="python3 tests/render_pixel_smoke.py /b/lobole --record"
  # --user: a root-owned PNG in tests/golden/ is a file nobody can commit.
  # The ccache mount must exist first, or docker creates it owned by root and
  # the --user build cannot write it ("ccache: Permission denied").
  mkdir -p "$root/.ci-local/ccache"
  reclaim_tree "$tag" "$build_dir"
  reclaim_tree "$tag" .ci-local/ccache
  run_tests pixel "pixel gate" \
    docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
      -v "$root:/src" -v "$root/$build_dir:/b" -v "$root/.ci-local/ccache:/tmp/ccache" \
      -e CCACHE_DIR=/tmp/ccache "$tag" bash -c "
         set -o pipefail
         export VK_DRIVER_FILES=\$(ls /usr/share/vulkan/icd.d/lvp_icd*.json /usr/lib/*/vulkan/icd.d/lvp_icd*.json 2>/dev/null | head -1)
         [ -n \"\$VK_DRIVER_FILES\" ] || { echo 'no lavapipe ICD in the image'; exit 1; }
         cmake -S /src -B /b -G Ninja -DCMAKE_BUILD_TYPE=Release \
               -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache &&
         cmake --build /b -j$jobs --target designer &&
         cd /src && $pixel_cmd"
fi

printf '\n%s in %dm%02ds\n' \
  "$([ "$failures" = 0 ] && echo 'CI-local: everything passed' || echo "CI-local: $failures job(s) failed")" \
  $(( (SECONDS-started)/60 )) $(( (SECONDS-started)%60 ))
exit $(( failures > 0 ))
