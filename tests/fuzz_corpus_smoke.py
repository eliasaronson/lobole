#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every minimised random-design case, replayed (docs/modeling/README.md#random-designs).

tests/fuzz/cases/<class>-<seed>.jsonl is what `tools/fuzz.py one <seed>
--minimise` wrote for a failure that has since been fixed: a header line
{"fuzz": {seed, class, gist, generator, design}}, then the --record session
of the minimised design. Each case is replayed twice into a headless
instance of its own, `new_project` between them:

  - the design, with every check of tools/fuzz_design.py after each step
    and at the end. A case passes when every check holds: the bug it was
    found by no longer happens;
  - the session as recorded, through python/lobole/replay.py (which skips
    the header): the literal calls still run, as a person replaying it by
    hand would see.

A class still open is not here: it is in docs/known-bugs.md with its case.

Usage: fuzz_corpus_smoke.py <path-to-designer-binary>
"""
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, HERE)
import fuzz_design as fd  # noqa: E402
from lobole.replay import replay  # noqa: E402
from smoke_app import launch  # noqa: E402

CASES = os.path.join(HERE, "fuzz", "cases")


def main():
    cases = sorted(glob.glob(os.path.join(CASES, "*.jsonl")))
    bad = []
    # An instance of its own per case: a known bug (docs/known-bugs.md, what
    # the kernel builds depends on what the process built before) lets one
    # case's run move the next one's result.
    for path in cases:
        name = os.path.basename(path)
        with open(path) as f:
            header = json.loads(f.readline()).get("fuzz")
        if not header or "design" not in header:
            bad.append(f"{name}: no {{\"fuzz\": {{..., design}}}} header")
            continue
        with launch(os.path.abspath(sys.argv[1])) as app:
            d = app.designer
            d.scratch_path = os.path.join(app.tmp, "fuzz.dsgn")
            # The checks first, on the fresh instance; then the literal
            # session, after a new_project.
            outcome = fd.run_design(d, header["design"])
            if outcome["class"] != "clean":
                bad.append(f"{name} (found as {header['class']}: "
                           f"{header['gist']}): {outcome['class']} at step "
                           f"{outcome['step']}: {outcome['message']}")
            d.call("new_project")
            if replay(app.port, path) != 0:
                bad.append(f"{name}: the recorded session no longer replays")
    if bad:
        print("fuzz_corpus_smoke: FAILED")
        for b in bad:
            print("  - " + b)
        return 1
    print(f"fuzz_corpus_smoke: {len(cases)} case(s) replay clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
