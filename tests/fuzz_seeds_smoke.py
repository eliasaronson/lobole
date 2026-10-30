#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The CI-sized random-design run (docs/modeling/README.md#random-designs).

A fixed list of seeds that must stay clean, each in an instance of its own:
every step built (or failed as a
fillet, chamfer, shell, draft or hole may, with a message, and was undone),
every check held -- volume the right way, valid bodies, the incremental part
equal to a rebuild from nothing, undo and redo, a save and reopen -- and three
Boolean identities each. Any failure class fails, and so does `generator`
(the generator asked for something the RPC refused).

The list is only meaningful against one generator version: a change to the
generator changes what each seed builds, so the list is chosen again with it
(tools/fuzz.py run, from the clean seeds of its report). The checks
themselves are shown to catch what they claim first, by
tools/fuzz_design.py's self-test.

An older OpenCASCADE gets some of them wrong itself, and KERNEL names those
seeds by the version `ping` reports: each must still fail, and as the class
written there, so the other seeds stay a full check on that kernel and a
kernel that starts getting one right says so.

Usage: fuzz_seeds_smoke.py <path-to-designer-binary>
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, HERE)
import fuzz_design as fd  # noqa: E402
from smoke_app import launch  # noqa: E402

GENERATOR = 1
# Chosen from a clean run of seeds 0-399 on generator 1, sized to ~30 s.
SEEDS = list(range(40))

# OpenCASCADE version prefix -> {seed: the class that kernel fails it as}.
# 7.6.3 (Ubuntu 24.04's apt): a cylinder and a sphere both standing on the
# XY plane, the sphere touching the cylinder's floor at one point, intersect
# to nothing or to the whole sphere -- so the identity fails. 7.9 answers
# every one of them.
KERNEL = {
    # 34 left the list when edge names took geometric numbers: a fillet or
    # chamfer in it now stops at a limit before the design reaches the
    # sphere, so 7.6 answers it clean, as 7.9 does. With format v216-v217's
    # edge naming 14 and 15, which had left with it, fail again, and 9 (a
    # sphere cut leaving a face BRepCheck rejected) and 33 answer clean.
    "7.6.": {13: "identity", 14: "identity", 15: "identity", 20: "identity",
             29: "identity"},
}


def kernel_expects(occ):
    for prefix, seeds in KERNEL.items():
        if occ.startswith(prefix):
            return seeds
    return {}


def main():
    if fd.GENERATOR_VERSION != GENERATOR:
        print(f"fuzz_seeds_smoke: the seed list was chosen for generator "
              f"{GENERATOR}, and fuzz_design.py is version "
              f"{fd.GENERATOR_VERSION}: choose the list again from a "
              f"tools/fuzz.py run")
        return 1
    if fd.self_test() != 0:
        return 1
    t0 = time.time()
    bad = []
    limits = 0
    # An instance of its own per seed: what a seed builds is meant to depend
    # on the seed alone, and a known bug (docs/known-bugs.md, a blend face's
    # edge names) lets one seed's run move the next one's names.
    occ = ""
    for seed in SEEDS:
        with launch(os.path.abspath(sys.argv[1])) as app:
            d = app.client
            occ = d.request("ping", {}).get("occ", "")
            d.scratch_path = os.path.join(app.tmp, "fuzz.lob")
            outcome, design = fd.generate_and_run(d, seed)
        limits += len(outcome["limits"])
        expected = kernel_expects(occ).get(seed, "clean")
        if outcome["class"] != expected:
            said = (f"{outcome['class']} at step {outcome['step']}: "
                    f"{outcome['message']}" if outcome["class"] != "clean"
                    else "clean")
            if expected != "clean":
                said += (f" (OpenCASCADE {occ} was expected to fail it as "
                         f"{expected}: if it now answers, drop the seed "
                         f"from KERNEL)")
            bad.append(f"seed {seed}: {said}\n"
                       f"    replay: tools/fuzz.py one {seed} --design")
    if bad:
        print("fuzz_seeds_smoke: FAILED")
        for b in bad:
            print("  - " + b)
        return 1
    known = len(kernel_expects(occ))
    print(f"fuzz_seeds_smoke: {len(SEEDS)} seeds of generator {GENERATOR} "
          f"clean ({limits} limits undone) in {time.time() - t0:.1f} s"
          + (f"; {known} failed as OpenCASCADE {occ} is known to"
             if known else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
