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
#   tools/ci-local.sh --serial     # one job after another, output live
#
# The jobs run as LANES, concurrently: one for the nix chain (its builds and
# suites in the order above) and one per container image. The compile budget
# (tools/build-jobs) is divided among the lanes still running, so the builds
# in flight never add up to more than one build was allowed; suites take
# turns under a machine-wide lock, each at full -j$(nproc), so no
# deadline-based test sees more load than it would alone. What overlaps is
# one lane's compile with another lane's suite. Each lane's output is kept
# and printed whole at the end, in lane order, then every step's seconds.
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
serial=0
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
    # Today's order, one job at a time with its output on the terminal as it
    # happens: for a run being debugged, or a machine that should stay quiet.
    --serial) serial=1 ;;
    -h|--help) sed -n '5,35p' "$0"; exit 0 ;;
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
    # ${{ matrix.extra_packages }} is per image, read with the matrix below.
    pkgs += re.sub(r"\$\{\{.*?\}\}", "", line).strip().rstrip("\\").split()
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

# The matrix: image, the tests it excludes and its extra packages. "@@"
# separates them because the exclusion is itself a |-alternation.
for image, excl, extra in re.findall(
        r"- image:\s*(\S+)\s*\n(?:\s*#[^\n]*\n)*\s*exclude_tests:\s*\"([^\"]*)\""
        r"\s*\n(?:\s*#[^\n]*\n)*\s*extra_packages:\s*\"([^\"]*)\"",
        text):
    print(f"MATRIX={image}@@{excl}@@{extra}")
PY
}

eval "$(read_workflow | grep '^PACKAGES=' | sed 's/^PACKAGES=/PACKAGES="/; s/$/"/')"
eval "$(read_workflow | grep '^GUI_PACKAGES=' | sed 's/^GUI_PACKAGES=/GUI_PACKAGES="/; s/$/"/')"
eval "$(read_workflow | grep '^GUI_RUNS_ON=' | sed 's/^GUI_RUNS_ON=/GUI_RUNS_ON="/; s/$/"/')"
mapfile -t MATRIX < <(read_workflow | sed -n 's/^MATRIX=//p')

step() { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }
ok()   { printf '\033[1;32m   %s\033[0m\n' "$*"; }
fail() { printf '\033[1;31m   %s\033[0m\n' "$*"; }

started=$SECONDS
failures=0
lane=""  # the lane a step belongs to; set in each lane's subshell
lanes_dir="$root/.ci-local/lanes"
# One suite at a time, MACHINE-wide: a second ci-local run in another
# worktree puts the same load on the same deadline-based tests as a second
# lane in this one. The same pattern as gui-tests-xvfb.sh's GPU lock.
suite_lock=/tmp/lobole-ci-local-suite.lock
# Compilers from free memory, not cores (tools/build-jobs, ~1.5 GB each): at
# one per core the containers' builds took the host's memory.
jobs="$("$root/tools/build-jobs")"

# The -j for a build starting NOW: the budget divided by the lanes still
# running. A lane builds only while it runs, and every build in flight took
# its share when at least as many lanes were alive as are now, so the shares
# of the builds in flight never add up to more than `jobs` -- what one build
# was allowed when the jobs ran one after another. A fixed split would hold
# too, but would leave the last lane compiling on a fraction of the budget
# after the others had finished.
build_share() {
  if [ "$serial" = 1 ]; then echo "$jobs"; return; fi
  local alive share
  alive=$(find "$lanes_dir" -maxdepth 1 -name '*.alive' | wc -l)
  [ "$alive" -ge 1 ] || alive=1
  share=$((jobs / alive))
  echo $((share > 0 ? share : 1))
}

# Run one step with its whole output in <log>, record how long it took (and,
# for a suite, how long it queued for the suite lock) for the table at the
# end, and return its status. A suite holds the lock for exactly its own
# command: the lock's fd is closed for the command, so a process the suite
# leaves behind cannot go on holding it.
#   logged <log> <step label> <build|suite> <command...>
logged() {
  local log="$1" label="$2" kind="$3"; shift 3
  local t0=$SECONDS waited=0 rc=0 fd
  mkdir -p "$(dirname "$log")"
  if [ "$kind" = suite ]; then
    exec {fd}>>"$suite_lock"
    flock "$fd"
    waited=$((SECONDS - t0)); t0=$SECONDS
    "$@" > "$log" 2>&1 {fd}>&- || rc=$?
    exec {fd}>&-
  else
    "$@" > "$log" 2>&1 || rc=$?
  fi
  printf '%s\t%s\t%d\t%d\t%s\n' "$lane" "$label" $((SECONDS - t0)) "$waited" \
    "$([ "$rc" = 0 ] && echo ok || echo FAILED)" >> "$lanes_dir/$lane.times"
  return "$rc"
}

# Run a test command, keeping its whole output in .ci-local/<name>.log: three
# lines on success (the ctest summary), the last 40 plus the path on failure.
# The first version piped straight to `tail -3`, which meant a red run showed
# a count and threw the diagnosis away — the opposite of what a failure needs.
# It runs under the suite lock.
#   run_tests <name> <human label> <command...>
run_tests() {
  local name="$1" label="$2"; shift 2
  local log="$root/.ci-local/$name.log"
  if logged "$log" "$label" suite "$@"; then
    tail -3 "$log"
    ok "$label passed"
  else
    tail -40 "$log"
    fail "$label FAILED — full log: $log"
    failures=$((failures+1))
  fi
}

# A step that ends its lane when it fails (a build: nothing after it in the
# lane has a binary to run). The other lanes go on.
#   run_fatal <log> <step label> <command...>
run_fatal() {
  local log="$1" label="$2"; shift 2
  logged "$log" "$label" build "$@" && return 0
  tail -40 "$log"
  fail "$label FAILED — full log: $log"
  failures=$((failures+1))
  exit 1
}

# Configure and build, keeping the output in .ci-local/<name>-build.log.
# Ninja prints a failing compile on its STDOUT, so `> /dev/null` here used to
# swallow the diagnosis whole: a broken build exited 1 with nothing on screen
# but a stray CMake warning. Same rule as run_tests — quiet when green, the
# last 40 lines and a path when red. The -j is the lane's share (build_share).
#   run_build <name> <cmake-configure-args...> -- <cmake-build-args...>
run_build() {
  local name="$1"; shift
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
  run_fatal "$root/.ci-local/$name-build.log" "$name build" \
    "$root/tools/nix-run" "cmake ${cfg[*]} && cmake --build ${bld[*]} -j$(build_share)"
  echo "$stamp" > "$tree/.nix-env-stamp"
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

# Every container is NAMED after this run and its lane, so an interrupted run
# stops exactly the containers it started: killing a `docker run` client
# leaves its container running in the daemon.
container_name() { echo "ci-local-$$-$lane-$1"; }

# ---- job 1: what build-and-test does, using this machine's toolchain -------
lane_nix() {
  step "build-and-test (local toolchain)"
  run_build release -S . -B .ci-local/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -- .ci-local/build
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
    -- .ci-local/build-no-te
  run_tests no-te "engine-free build works and refuses --gui-tests" engine_free_check

  # ...and without CAM, the job's last two steps. Both configurations exist
  # for the same reason and break the same way: nobody compiles them by
  # accident, so the only thing that holds them up is a job that does. Two
  # were broken on the same afternoon -- an engine-free compile and a
  # Spectra-less link -- and neither could be seen from a nix build.
  step "build without CAM"
  run_build no-cam -S . -B .ci-local/build-nocam -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_CAM=OFF \
    -- .ci-local/build-nocam
  run_tests nocam "CAM-free suite" \
    "$root/tools/ctest" --test-dir .ci-local/build-nocam -LE gui -j"$(nproc)" --output-on-failure
}

engine_free_check() {
  "$root/tools/nix-run" "python3 tests/headless_smoke.py .ci-local/build-no-te/lobole" || return 1
  if .ci-local/build-no-te/lobole --gui-tests --hidden; then
    echo "the engine-free build ran --gui-tests instead of refusing them"
    return 1
  fi
}

# ---- the sanitizer job (opt-in here, weekly/manual on GitHub) --------------
lane_asan() {
  step "sanitizers (ASan + UBSan)"
  # build-asan, not .ci-local/: the same tree tools/run-sanitized-tests.sh
  # uses, so an incremental developer build and this share their objects.
  source "$root/tools/sanitizer-env.sh"
  run_build asan -S . -B build-asan -G Ninja -DENABLE_SANITIZERS=ON \
    -- build-asan --target designer designer_tests
  run_tests asan "sanitized suite" \
    "$root/tools/ctest" --test-dir build-asan -LE "$SANITIZER_LABEL_EXCLUDE" -j"$(nproc)" --output-on-failure -E "$SANITIZER_TEST_EXCLUDE"
}

# ---- the race job (opt-in; separate tree, ASan and TSan cannot combine) -----
lane_tsan() {
  step "ThreadSanitizer"
  # Its own tree because -fsanitize=thread and -fsanitize=address cannot be
  # linked into one binary. Not part of any scheduled run: this is an
  # occasional audit like tools/coverage.sh, aimed at the parts of the tree
  # that stopped being single-threaded — the import worker, OCC meshing across
  # threads, async FEM, the remote server.
  run_build tsan -S . -B build-tsan -G Ninja -DENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -- build-tsan --target designer designer_tests
  # -j4, not -j$(nproc): TSan multiplies each process's memory footprint, and
  # a saturated machine turns a race hunt into a timeout hunt.
  run_tests tsan "race suite" \
    "$root/tools/ctest" --test-dir build-tsan -LE gui -j4 --output-on-failure
}

# ---- job 2: the stock-Ubuntu containers, one lane per image ----------------
# In a worktree, .git is a FILE naming the main checkout's .git/worktrees/X
# by absolute path, and with only /src mounted every git call in the suite
# (spdx_gate, format_version_gate) died "not a git repository". The common
# git dir is mounted at its own path, read-only, so that pointer resolves.
git_mount=()
if [ -f "$root/.git" ]; then
  common="$(git -C "$root" rev-parse --path-format=absolute --git-common-dir)"
  git_mount=(--mount "type=bind,src=$common,dst=$common,readonly")
fi

#   lane_image <matrix entry: image@@exclude@@extra>
lane_image() {
  local entry="$1" image rest exclude extra tag build_dir
  image="${entry%%@@*}"
  rest="${entry#*@@}"
  exclude="${rest%%@@*}"
  extra="${rest#*@@}"
  tag="designer-ci-${image//[:.]/-}"
  build_dir=".ci-local/${image//[:.]/-}"
  mkdir -p "$build_dir"

  step "stock-ubuntu ($image)"
  # The image is the workflow's package list and nothing else — that is the
  # point of this job: it fails if the README's list is incomplete.
  printf 'FROM %s\nENV DEBIAN_FRONTEND=noninteractive\nRUN apt-get update && apt-get install -y --no-install-recommends ca-certificates %s && rm -rf /var/lib/apt/lists/*\n' \
    "$image" "$PACKAGES $extra" > "$build_dir/Dockerfile"
  run_fatal "$root/.ci-local/$lane-image.log" "$image image" \
    docker build -q -t "$tag" "$build_dir"
  reclaim_tree "$tag" "$build_dir"

  # The build and the suite are two containers, so the suite takes its turn
  # at the suite lock while another lane compiles. Neither's output is
  # discarded: the log keeps it and is printed on failure, and a compile
  # error with its output thrown away leaves an eleven-line log that says
  # nothing (which is exactly what happened).
  local run=(docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp
             -v "$root:/src:ro" "${git_mount[@]}" -v "$root/$build_dir:/b")
  run_fatal "$root/.ci-local/$lane-build.log" "$image build" \
    "${run[@]}" --name "$(container_name build)" "$tag" bash -c "
      cmake -S /src -B /b -G Ninja -DCMAKE_BUILD_TYPE=Release &&
      cmake --build /b -j$(build_share)"
  local ctest_cmd="ctest --test-dir /b -LE gui -j\$(nproc) --output-on-failure"
  [ -n "$exclude" ] && ctest_cmd="$ctest_cmd -E \"$exclude\""
  run_tests "$lane" "$image suite" \
    "${run[@]}" --name "$(container_name suite)" "$tag" bash -c "$ctest_cmd"
}

# ---- the gui-suite job's pixel gate, in the runner's driver ----------------
# Not the whole job: the GUI suite itself runs here under nix already. What
# only a matching container can answer is the PIXEL comparison, whose golden
# is keyed to the driver that drew it -- llvmpipe from ubuntu:24.04, which is
# what the runner installs and is NOT the mesa in shell.nix. The image is the
# workflow's own gui-suite package list, so it cannot drift from CI.
lane_pixel() {
  # The job runs on a RUNNER, not in a container; the closest an image can
  # sit to it is the same Ubuntu release, which is where its driver comes
  # from. Derived, so moving the job moves this.
  local image="${GUI_RUNS_ON/ubuntu-/ubuntu:}"
  local tag="designer-ci-gui"
  local build_dir=".ci-local/gui-build"
  mkdir -p "$build_dir"
  step "gui-suite pixel gate ($image)"
  printf 'FROM %s\nENV DEBIAN_FRONTEND=noninteractive\nRUN apt-get update && apt-get install -y --no-install-recommends ca-certificates python3 %s && rm -rf /var/lib/apt/lists/*\n' \
    "$image" "$GUI_PACKAGES" > "$build_dir/Dockerfile"
  run_fatal "$root/.ci-local/pixel-image.log" "$image image" \
    docker build -q -t "$tag" "$build_dir"
  # The ICD discovery step of the job, verbatim in spirit: lavapipe's filename
  # moved once already (lvp_icd.x86_64.json -> lvp_icd.json).
  local pixel_cmd="PYTHONPATH=/src/python python3 tests/render_pixel_smoke.py /b/lobole --require-baseline"
  [ "$pixel_record" = 1 ] && pixel_cmd="PYTHONPATH=/src/python python3 tests/render_pixel_smoke.py /b/lobole --record"
  # --user: a root-owned PNG in tests/golden/ is a file nobody can commit.
  # The ccache mount must exist first, or docker creates it owned by root and
  # the --user build cannot write it ("ccache: Permission denied").
  mkdir -p "$root/.ci-local/ccache"
  reclaim_tree "$tag" "$build_dir"
  reclaim_tree "$tag" .ci-local/ccache
  run_tests pixel "pixel gate" \
    docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp --name "$(container_name pixel)" \
      -v "$root:/src" -v "$root/$build_dir:/b" -v "$root/.ci-local/ccache:/tmp/ccache" \
      -e CCACHE_DIR=/tmp/ccache "$tag" bash -c "
         set -o pipefail
         export VK_DRIVER_FILES=\$(ls /usr/share/vulkan/icd.d/lvp_icd*.json /usr/lib/*/vulkan/icd.d/lvp_icd*.json 2>/dev/null | head -1)
         [ -n \"\$VK_DRIVER_FILES\" ] || { echo 'no lavapipe ICD in the image'; exit 1; }
         cmake -S /src -B /b -G Ninja -DCMAKE_BUILD_TYPE=Release \
               -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache &&
         cmake --build /b -j$(build_share) --target designer &&
         cd /src && $pixel_cmd"
}

# ---- the lanes ---------------------------------------------------------------
# One lane per independent chain, listed in the order a serial run takes them
# and the report prints them.
lane_names=() lane_cmds=()
add_lane() { lane_names+=("$1"); shift; lane_cmds+=("$*"); }
[ "$run_nix" = 1 ] && add_lane nix lane_nix
[ "$run_asan" = 1 ] && add_lane asan lane_asan
[ "$run_tsan" = 1 ] && add_lane tsan lane_tsan
if [ "$run_containers" = 1 ]; then
  if command -v docker > /dev/null; then
    for entry in "${MATRIX[@]}"; do
      image="${entry%%@@*}"
      [ -n "$only_image" ] && [ "$image" != "$only_image" ] && continue
      add_lane "${image//[:.]/-}" lane_image "$entry"
    done
  else
    fail "docker not found — skipping containers"
  fi
fi
if [ "$run_pixel" = 1 ]; then
  command -v docker > /dev/null || { fail "docker not found"; exit 1; }
  add_lane pixel lane_pixel
fi

mkdir -p "$lanes_dir"
find "$lanes_dir" -mindepth 1 -maxdepth 1 -type f -delete
for name in "${lane_names[@]}"; do touch "$lanes_dir/$name.alive"; done

# A lane is a subshell, so its failure count is its own: it leaves the count
# in a file on the way out however it goes out (a failed build ends it with
# `exit 1`), and stops counting toward the build budget.
#   run_lane <index>
run_lane() {
  lane="${lane_names[$1]}"
  failures=0  # a serial lane would otherwise start from the lanes before it
  trap 'echo "$failures" > "$lanes_dir/$lane.failures"; rm -f -- "${lanes_dir:?}/${lane:?}.alive"' EXIT
  : > "$lanes_dir/$lane.times"
  # shellcheck disable=SC2086  # a lane function and its argument, split on purpose
  ${lane_cmds[$1]}
}

# A finished lane's failures, added to the run's: a lane that stopped without
# counting anything (a set -e exit) still failed.
# (Not called in $(...): the count must reach this shell's `failures`.)
#   collect_lane <name> <exit status>   (sets lane_failures)
collect_lane() {
  lane_failures="$(cat "$lanes_dir/$1.failures" 2>/dev/null || echo 0)"
  [ "$2" != 0 ] && [ "$lane_failures" = 0 ] && lane_failures=1
  failures=$((failures + lane_failures))
}

elapsed() { printf '%dm%02ds' $(( (SECONDS-started)/60 )) $(( (SECONDS-started)%60 )); }

# Each lane's process group, and the containers this run named: what an
# interrupted run stops. Background jobs in a script ignore SIGINT, so
# without this a Ctrl-C would leave every lane building.
declare -A live=() lane_of=()
interrupted() {
  trap - INT TERM
  local pid
  for pid in "${!live[@]}"; do kill -TERM -- "-$pid" 2>/dev/null || true; done
  if command -v docker > /dev/null; then
    docker ps -q --filter "name=^ci-local-$$-" | xargs -r docker kill > /dev/null 2>&1 || true
  fi
  fail "interrupted"
  exit 130
}
trap interrupted INT TERM

if [ "$serial" = 1 ] || [ "${#lane_names[@]}" -le 1 ]; then
  for i in "${!lane_names[@]}"; do
    rc=0; ( run_lane "$i" ) || rc=$?
    collect_lane "${lane_names[$i]}" "$rc"
  done
else
  echo "CI-local: ${#lane_names[@]} lanes (${lane_names[*]}); builds share -j$jobs, suites take turns"
  echo "  each lane's output goes to $lanes_dir/<lane>.out and is printed at the end"
  set -m  # each lane in its own process group, so `interrupted` stops all of it
  for i in "${!lane_names[@]}"; do
    # stdin from /dev/null: a background process group that reads the
    # terminal is stopped (SIGTTIN), and the lane would hang unseen.
    ( run_lane "$i" ) < /dev/null > "$lanes_dir/${lane_names[$i]}.out" 2>&1 &
    live[$!]=1; lane_of[$!]=$i
  done
  set +m
  while [ "${#live[@]}" -gt 0 ]; do
    rc=0; wait -n -p done_pid "${!live[@]}" || rc=$?
    unset "live[$done_pid]"
    name="${lane_names[${lane_of[$done_pid]}]}"
    collect_lane "$name" "$rc"
    if [ "$lane_failures" = 0 ]; then ok "lane $name passed at $(elapsed)"
    else fail "lane $name: $lane_failures failure(s), at $(elapsed)"; fi
  done
  for name in "${lane_names[@]}"; do
    printf '\n\033[1m######## lane %s ########\033[0m\n' "$name"
    cat "$lanes_dir/$name.out"
  done
fi
trap - INT TERM

# Every step's seconds: whether the overlap pays, and where the next minute
# is. "queued" is how long a suite waited for the suite lock.
if [ "${#lane_names[@]}" -gt 0 ]; then
  printf '\n%-12s %-50s %7s %7s  %s\n' lane step seconds queued status
  for name in "${lane_names[@]}"; do
    [ -f "$lanes_dir/$name.times" ] || continue
    while IFS=$'\t' read -r l s secs q st; do
      printf '%-12s %-50s %7d %7d  %s\n' "$l" "$s" "$secs" "$q" "$st"
    done < "$lanes_dir/$name.times"
  done
fi

printf '\n%s in %s\n' \
  "$([ "$failures" = 0 ] && echo 'CI-local: everything passed' || echo "CI-local: $failures job(s) failed")" \
  "$(elapsed)"
exit $(( failures > 0 ))
