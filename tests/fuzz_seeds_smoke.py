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
    for seed in SEEDS:
        with launch(os.path.abspath(sys.argv[1])) as app:
            d = app.designer
            d.scratch_path = os.path.join(app.tmp, "fuzz.dsgn")
            outcome, design = fd.generate_and_run(d, seed)
        limits += len(outcome["limits"])
        if outcome["class"] != "clean":
            bad.append(f"seed {seed}: {outcome['class']} at step "
                       f"{outcome['step']}: {outcome['message']}\n"
                       f"    replay: tools/fuzz.py one {seed} --design")
    if bad:
        print("fuzz_seeds_smoke: FAILED")
        for b in bad:
            print("  - " + b)
        return 1
    print(f"fuzz_seeds_smoke: {len(SEEDS)} seeds of generator {GENERATOR} "
          f"clean ({limits} limits undone) in {time.time() - t0:.1f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
