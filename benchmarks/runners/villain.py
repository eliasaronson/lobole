#!/usr/bin/env python3
"""The villain arm: build a box of the task's own bbox for EVERY task.

`reference.py` is the upper control — a task whose by-hand build cannot pass
its own checks is broken. This is the lower one, and it answers the opposite
question:

    does the task's check set actually distinguish the part from a block?

The villain reads each task's `bbox` check and extrudes exactly that block.
It knows nothing else about the part — no holes, no steps, no bosses. Every
check it passes is a check that is not doing any work, and a task the villain
passes OUTRIGHT is broken: the prompt could be ignored entirely and still
score. vcad's mecheval keeps DEFAULT_CUBE and FLOOD_ZONE in CI for exactly
this ("if any task accepts this output, a check is broken").

Exit status is 1 if any task is fully passed. Tasks the villain gets most of
the way through are reported as WEAK: not a failure, but a task carrying less
discriminating power than its check count suggests.

    nix-shell shell.nix --run "python3 benchmarks/runners/villain.py"
    ... --task t0-plate --keep /tmp/out      # one task, keep the STEP
"""
import argparse
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tests"))
sys.path.insert(0, os.path.join(ROOT, "benchmarks", "oracle"))
from smoke_app import launch          # noqa: E402
from grade import grade_file          # noqa: E402

XY = {"origin": [0, 0, 0], "normal": [0, 0, 1], "uAxis": [1, 0, 0]}

# A task the villain passes this fraction of is reported as WEAK. Two thirds
# is not a tuned number — it is the point at which a majority of the checks
# are satisfied by a part that was never built.
WEAK_FRACTION = 2.0 / 3.0


def bbox_of(task):
    """A block the villain can aim: the task's declared bbox, or the ceiling
    an `envelope` check gives it.

    A Fit task states an envelope rather than a size — there is no single
    right bbox for an accessory — so without the fallback the villain has
    nothing to build and the task has no floor. A block filling the envelope
    is the strongest cheese available there: it is the biggest thing the task
    permits.
    """
    for c in task["checks"]:
        if c["kind"] == "bbox" and "size" in c:
            return c["size"]
    for c in task["checks"]:
        if c["kind"] == "envelope" and "max_mm" in c:
            return c["max_mm"]
    return None


def build_block(d, size):
    """The whole villain: one extruded rectangle, cornered at the origin."""
    sx, sy, sz = size
    sid = d.call("add_feature", feature={
        "type": "Sketch", "name": "Villain", "plane": XY,
        "entities": [{"type": "rect", "c1": [0, 0], "c2": [sx, sy]}]})["id"]
    d.call("add_feature", feature={"type": "Extrude", "name": "Villain",
                                   "sketch_id": sid, "distance": sz,
                                   "mode": "new_body"})


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--task", default="")
    ap.add_argument("--keep", default="")
    ap.add_argument("--binary", default=os.path.join(ROOT, "build", "designer"))
    a = ap.parse_args()

    tdir = os.path.join(ROOT, "benchmarks", "tasks")
    tasks = [json.load(open(os.path.join(tdir, f)))
             for f in sorted(os.listdir(tdir)) if f.endswith(".json")]
    if a.task:
        tasks = [t for t in tasks if t["id"] == a.task]
        if not tasks:
            sys.exit(f"no such task: {a.task}")

    out = a.keep or tempfile.mkdtemp(prefix="bench-villain-")
    os.makedirs(out, exist_ok=True)
    broken, weak = [], []
    for t in tasks:
        size = bbox_of(t)
        if size is None:
            # No bbox to cheese. The villain has nothing to build and the task
            # is trivially safe from it; say so rather than skipping silently.
            print(f"n/a  {t['id']:24s} no bbox check — villain cannot aim")
            continue
        step = os.path.join(out, f"{t['id']}.step")
        with launch(a.binary) as app:
            d = app.designer
            d.call("new_project")
            build_block(d, size)
            d.call("export_step", path=step)
        r = grade_file(t, step)
        passed, total = r["passed"], r["total"]
        got = sorted(c["kind"] for c in r["checks"] if c["pass"])
        declared = bool(t.get("villain_passable"))
        if passed == total and declared:
            # Declared degenerate: the described part IS the block. Passing is
            # expected; NOT passing would mean the checks changed meaning.
            mark, note = "cube  ", "  (villain_passable: declared)"
        elif passed == total:
            mark, note = "BROKEN", "  <- the villain passes this task outright"
            broken.append(t["id"])
        elif declared:
            mark, note = "MOVED ", ("  <- declared villain_passable but no "
                                    "longer passes; the checks changed")
            broken.append(t["id"])
        elif passed >= total * WEAK_FRACTION:
            mark, note = "weak  ", ""
            weak.append(t["id"])
        else:
            mark, note = "ok    ", ""
        print(f"{mark} {t['id']:24s} villain scores {passed}/{total}"
              f"  passes: {', '.join(got) or '-'}{note}")

    print(f"\nSTEP files: {out}")
    if broken:
        print(f"\nBROKEN: {len(broken)} task(s) fully passed by a block that "
              f"ignored the prompt: {', '.join(broken)}")
        print("A task must not be satisfiable without building the part.")
    if weak:
        print(f"\nWEAK: {', '.join(weak)} — a majority of checks survive a "
              f"block. Consider a check that needs the actual features "
              f"(cyl_group, faces_by_kind, com).")
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
