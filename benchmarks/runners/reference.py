#!/usr/bin/env python3
"""The reference arm: build every task by hand, grade it, report.

This is not a competitor — it is the harness's own self-test and the ceiling
the agent arms are measured against. A task whose reference build cannot pass
its own checks is a broken task (or a real app bug), and finding that out costs
nothing here and costs an entire paid sweep if it is found later.

    nix-shell shell.nix --run "python3 benchmarks/runners/reference.py"
    ... --task t2-flange --keep /tmp/out     # one task, keep the STEP
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


def plane(origin, normal=(0, 0, 1), u=(1, 0, 0)):
    return {"origin": list(origin), "normal": list(normal), "uAxis": list(u)}


def sketch(d, entities, pl=None, name="S"):
    r = d.call("add_feature", feature={"type": "Sketch", "name": name,
                                       "plane": pl or XY, "entities": entities})
    return r["id"]


def extrude(d, sid, dist, mode="new_body", name="E", **kw):
    return d.call("add_feature", feature={"type": "Extrude", "name": name,
                                          "sketch_id": sid, "distance": dist,
                                          "mode": mode, **kw})


def circle(cx, cy, r, **kw):
    return {"type": "circle", "center": [cx, cy], "radius": r, **kw}


# --- one builder per task, keyed by task id --------------------------------

def t0_plate(d):
    extrude(d, sketch(d, [{"type": "rect", "c1": [0, 0], "c2": [120, 80]}]), 10)


def t0_spacer(d):
    extrude(d, sketch(d, [circle(0, 0, 20), circle(0, 0, 6)]), 25)


def t1_mount_plate(d):
    ents = [{"type": "rect", "c1": [0, 0], "c2": [100, 60]}]
    ents += [circle(x, y, 3.25) for x, y in
             ((12, 12), (88, 12), (12, 48), (88, 48))]
    extrude(d, sketch(d, ents), 8)


def t1_l_bracket(d):
    extrude(d, sketch(d, [{"type": "rect", "c1": [0, 0], "c2": [80, 60]}]), 6,
            name="Base")
    extrude(d, sketch(d, [{"type": "rect", "c1": [0, 0], "c2": [6, 60]}]), 50,
            mode="add", name="Wall")


def t2_flange(d):
    import math
    ents = [circle(0, 0, 45), circle(0, 0, 15)]
    for i in range(6):
        a = math.radians(60 * i)
        ents.append(circle(34 * math.cos(a), 34 * math.sin(a), 3.5))
    extrude(d, sketch(d, ents), 12)


def t2_stepped_shaft(d):
    extrude(d, sketch(d, [circle(0, 0, 15)]), 20, name="Big")
    extrude(d, sketch(d, [circle(0, 0, 9)], plane((0, 0, 20))), 35,
            mode="add", name="Small")


def t3_gusset_bracket(d):
    extrude(d, sketch(d, [{"type": "rect", "c1": [0, 0], "c2": [90, 50]}]), 8,
            name="Base")
    extrude(d, sketch(d, [{"type": "rect", "c1": [0, 0], "c2": [8, 50]}]), 60,
            mode="add", name="Wall")
    # Sketch plane at Y=22 with normal +Y: V = normal x uAxis = Y x X = -Z, so
    # the triangle's V coordinates are NEGATED heights. (The V-axis trap: get
    # this wrong and the gusset extrudes into empty space and cuts nothing.)
    tri = plane((0, 22, 0), (0, 1, 0), (1, 0, 0))
    ents = [{"type": "line", "a": [8, -8], "b": [48, -8]},
            {"type": "line", "a": [48, -8], "b": [8, -48]},
            {"type": "line", "a": [8, -48], "b": [8, -8]}]
    extrude(d, sketch(d, ents, tri), 6, mode="add", name="Gusset")


def t3_parametric_bushing(d):
    d.call("set_variable", name="bore", expression="12")
    d.call("set_variable", name="wall", expression="6")
    sid = sketch(d, [circle(0, 0, 16, r_expr="(bore + 2 * wall) / 2"),
                     circle(0, 0, 6, r_expr="bore / 2")])
    extrude(d, sid, 30)
    d.call("set_variable", name="bore", expression="20")


BUILDERS = {
    "t0-plate": t0_plate, "t0-spacer": t0_spacer,
    "t1-mount-plate": t1_mount_plate, "t1-l-bracket": t1_l_bracket,
    "t2-flange": t2_flange, "t2-stepped-shaft": t2_stepped_shaft,
    "t3-gusset-bracket": t3_gusset_bracket,
    "t3-parametric-bushing": t3_parametric_bushing,
}


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

    out = a.keep or tempfile.mkdtemp(prefix="bench-ref-")
    os.makedirs(out, exist_ok=True)
    worst = 0
    for t in tasks:
        step = os.path.join(out, f"{t['id']}.step")
        with launch(a.binary) as app:
            d = app.designer
            d.call("new_project")
            BUILDERS[t["id"]](d)
            d.call("export_step", path=step)
        r = grade_file(t, step)
        bad = [c for c in r["checks"] if not c["pass"]]
        mark = "ok  " if not bad else "FAIL"
        print(f"{mark} {t['id']:24s} {r['passed']}/{r['total']}")
        for c in bad:
            print(f"       {c['kind']}: want "
                  f"{ {k: v for k, v in c.items() if k not in ('kind','why','pass','got','note')} }"
                  f" got {c.get('got')} {c.get('note','')}")
            worst = 1
    print(f"\nSTEP files: {out}")
    return worst


if __name__ == "__main__":
    sys.exit(main())
