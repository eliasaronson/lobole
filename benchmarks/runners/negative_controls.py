#!/usr/bin/env python3
"""The other half of the reference arm: build each part the WRONG way.

`reference.py` proves a task is passable. It cannot prove a task is
DISCRIMINATING — that a plausible-but-wrong construction fails it. Without
that, adding a check only proves everything passes, which is how a benchmark
sits at 1.000 while measuring nothing.

So: for each construction-sensitive task, build the sketch-and-extrude
approximation an agent would reach for if it could not do the real operation,
grade it, and report which checks caught it. The run FAILS if a wrong build
passes everything — that is the check not working.

The taper is the sharp case, and it needs a FINE staircase to be sharp. Six
discs are caught by `bbox` too (the widest sits at its step's midpoint radius,
38.7 across instead of 40), so the census is not what stopped them. Sixty
discs fix that: measured, they pass solids, bbox, volume, the bore and the
centre of mass — every dimensional check this benchmark had before the census
existed — and are caught by nothing else. That is the result this file is for.

    nix-shell shell.nix --run "python3 benchmarks/runners/negative_controls.py"
"""
import argparse
import json
import math
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tests"))
sys.path.insert(0, os.path.join(ROOT, "benchmarks", "oracle"))
sys.path.insert(0, HERE)
from smoke_app import launch          # noqa: E402
from grade import grade_file          # noqa: E402
from reference import XY, XZ, circle, feat, plane, sketch   # noqa: E402


def wrong_coil_spring(d):
    """Five separate rings on the pitch spacing: what a spring looks like to
    someone who cannot sweep. Flat rings, so every face is a cylinder or a
    plane and there is no helix anywhere."""
    for i in range(5):
        sid = sketch(d, [circle(0, 0, 22), circle(0, 0, 18)],
                     plane((0, 0, 10 * i - 2)), name=f"R{i}")
        feat(d, type="Extrude", sketch_id=sid, distance=4,
             mode="new_body" if i == 0 else "add", name=f"Ring{i}")


def wrong_taper_adapter(d):
    """Six discs at the step MIDPOINT radii — the closest a staircase can get
    to a cone. Same bbox, same bore, volume within 0.06%."""
    for i in range(6):
        z0 = 5 * i
        r = 20 - (8.0 / 30.0) * (z0 + 2.5)     # radius at the step's midpoint
        sid = sketch(d, [circle(0, 0, r), circle(0, 0, 6)],
                     plane((0, 0, z0)), name=f"S{i}")
        feat(d, type="Extrude", sketch_id=sid, distance=5,
             mode="new_body" if i == 0 else "add", name=f"Step{i}")


def wrong_bent_tube(d):
    """The arc replaced by six straight chords: a valid solid, correctly made,
    of the wrong shape. Every face is a surface of extrusion and no torus
    survives.

    The profile is squared to the FIRST CHORD, not to +Y. A profile even a few
    degrees off perpendicular to the path start makes the sweep come back not
    connected, and the app now refuses that outright — which would make this a
    control that tests the refusal instead of the census."""
    pts = [(40 * math.cos(math.pi / 2 * k / 6),
            40 * math.sin(math.pi / 2 * k / 6)) for k in range(7)]
    segs = [{"type": "line", "a": [pts[k][0], pts[k][1]],
             "b": [pts[k + 1][0], pts[k + 1][1]]} for k in range(6)]
    path = sketch(d, segs, XY, name="Chords")
    dx, dy = pts[1][0] - pts[0][0], pts[1][1] - pts[0][1]
    length = math.hypot(dx, dy)
    prof = sketch(d, [circle(0, 0, 10)],
                  plane((40, 0, 0), (dx / length, dy / length, 0), (0, 0, 1)),
                  name="Section")
    feat(d, type="Sweep", profile_id=prof, path_id=path, mode="new_body",
         name="Bend")


def wrong_taper_adapter_fine(d):
    """The same staircase with sixty steps instead of six.

    This is the one that matters. Six steps are caught by `bbox` as well —
    the widest disc sits at its step's midpoint radius, 38.7 across instead
    of 40 — so the census is not doing the work. Refine the staircase and
    that goes away: at 0.5 mm steps the envelope is 39.87 across (inside the
    0.2 tolerance), the volume converges on the true frustum, and the bore,
    the centre of mass and the solid count were never wrong. Every
    dimensional check passes. The part is still not a cone."""
    n, h = 60, 0.5
    for i in range(n):
        z0 = h * i
        r = 20 - (8.0 / 30.0) * (z0 + h / 2)
        sid = sketch(d, [circle(0, 0, r), circle(0, 0, 6)],
                     plane((0, 0, z0)), name=f"S{i}")
        feat(d, type="Extrude", sketch_id=sid, distance=h,
             mode="new_body" if i == 0 else "add", name=f"Step{i}")


WRONG = {
    "t3-coil-spring": [("five separate flat rings", wrong_coil_spring)],
    "t3-taper-adapter": [
        ("six stacked discs at the step midpoints", wrong_taper_adapter),
        ("SIXTY stacked discs — every dimension right, still not a cone",
         wrong_taper_adapter_fine),
    ],
    "t3-bent-tube": [("six straight chords instead of the arc",
                      wrong_bent_tube)],
}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--task", default="")
    ap.add_argument("--keep", default="")
    ap.add_argument("--binary", default=os.path.join(ROOT, "build", "designer"))
    a = ap.parse_args()

    items = [(k, w) for k, v in WRONG.items() if not a.task or k == a.task
             for w in v]
    if not items:
        sys.exit(f"no negative control for: {a.task}")
    out = a.keep or tempfile.mkdtemp(prefix="bench-neg-")
    os.makedirs(out, exist_ok=True)

    worst = 0
    for tid, (what, build) in sorted(items, key=lambda x: x[0]):
        task = json.load(open(os.path.join(ROOT, "benchmarks", "tasks",
                                           tid + ".json")))
        step = os.path.join(out, tid + ".step")
        with launch(a.binary) as app:
            d = app.designer
            d.call("new_project")
            build(d)
            d.call("export_step", path=step)
        r = grade_file(task, step)
        failed = [c for c in r["checks"] if not c["pass"]]
        caught = [c for c in failed if c["kind"] == "surfaces"]
        # The wrong build must NOT pass the task. Better still, it should be
        # the surface census that stops it — if only bbox caught it, the task
        # was already safe and the census is not what is doing the work.
        ok = bool(failed)
        worst |= 0 if ok else 1
        mark = "ok  " if ok else "LEAK"
        print(f"{mark} {tid:22s} {what}")
        print(f"       passed {r['passed']}/{r['total']}; "
              f"surfaces caught it: {'yes' if caught else 'NO'}")
        for c in failed:
            got = c.get("got")
            if isinstance(got, dict) and "census" in got:
                got = got["census"]
            print(f"       - {c['kind']:16s} got {got}")
        if not failed:
            print("       EVERY CHECK PASSED — this task cannot tell the "
                  "wrong construction from the right one.")
    print(f"\nSTEP files: {out}")
    return worst


if __name__ == "__main__":
    sys.exit(main())
