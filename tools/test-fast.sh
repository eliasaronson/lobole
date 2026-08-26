#!/usr/bin/env bash
# The headless suite minus the long numeric gates — the inner loop.
#
#   tools/test-fast.sh            # ~8 s instead of ~81
#   tools/test-fast.sh -R sketch  # ...further filtered, ctest args pass through
#
# Why this exists, in numbers measured 2026-08-20 on this machine:
#
#   full headless suite (622 tests)            81 s
#   the SIMP topology gate alone               89 s of CPU (the long pole)
#   the next four topology/FEM gates       12-14 s each
#   everything else (615 tests)               8.3 s
#
# So the suite is not slow: one test is, and four more are heavy. They are
# real gates on real numerics — they belong in tools/ci-local.sh and in CI,
# and they do not belong in the loop you run after every edit, where they
# turn a 10-second check into a minute and a half of waiting for arithmetic
# that has nothing to do with the line you just changed.
#
# ALWAYS run the full suite before committing (tools/ci-local.sh --nix, or
# plain ctest): "fast" here means fewer tests, not weaker ones.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# The gates skipped here, by name. Kept as one regex so the reason above and
# the list below cannot drift apart.
SLOW='Topo:|Topology|RunTopology|TopologyOptimize|femgpu|fem_smoke|topo_smoke|offscreen_smoke'

exec nix-shell "$root/shell.nix" --run \
  "cd '$root/build' && ctest -LE gui -j\$(nproc) -E '$SLOW' --output-on-failure $*"
