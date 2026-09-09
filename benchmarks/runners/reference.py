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
import math
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
# U = X, V = +Z. The obvious normal (0,1,0) gives V = -Z (V = normal x uAxis)
# and negates every height — the trap t3-gusset-bracket exists to catch.
XZ = {"origin": [0, 0, 0], "normal": [0, -1, 0], "uAxis": [1, 0, 0]}


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


def feat(d, **kw):
    return d.call("add_feature", feature=kw)


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



def t3_coil_spring(d):
    """Helix + sweep. Stacked rings would have the same envelope and no
    bspline face anywhere in them."""
    path = feat(d, type="Curve3D", kind="helix", origin=[0, 0, 0],
                axis=[0, 0, 1], radius=20, pitch=10, turns=5, name="Helix")["id"]
    prof = sketch(d, [circle(20, 0, 2)], XZ, name="Wire")
    feat(d, type="Sweep", profile_id=prof, path_id=path, mode="new_body",
         name="Coil")


def t3_taper_adapter(d):
    """Revolved frustum with a through bore. The profile is (radius, Z), so
    the bore is the pair of points at U=6 and the taper is 20 -> 12."""
    prof = [{"type": "line", "a": [6, 0], "b": [20, 0]},
            {"type": "line", "a": [20, 0], "b": [12, 30]},
            {"type": "line", "a": [12, 30], "b": [6, 30]},
            {"type": "line", "a": [6, 30], "b": [6, 0]}]
    sid = sketch(d, prof, XZ, name="Section")
    feat(d, type="Revolve", sketch_id=sid, axis={"kind": "z"}, angle_deg=360,
         mode="new_body", name="Adapter")


def t3_bent_tube(d):
    """A 90 degree bent rod. Sweeping a circle along a SKETCH arc is the only
    route to a true torus face — Curve3D has no arc kind, so a Pipe along a
    fit_spline gives bspline instead (both are accepted by the task)."""
    # start_angle/end_angle are RADIANS. There is no _deg spelling, and
    # `describe` lists the entity types by name with no field docs at all, so
    # nothing tells you: 90 is accepted and quietly sweeps 116.6 degrees
    # (90 mod 2pi), which is a plausible arc and the wrong part.
    path = sketch(d, [{"type": "arc", "center": [0, 0], "radius": 40,
                       "start_angle": 0, "end_angle": math.pi / 2}],
                  XY, name="Path")
    # The profile plane's normal is the path TANGENT at the start: the arc
    # leaves (40,0,0) heading +Y, so a plane normal of +X is refused.
    prof = sketch(d, [circle(0, 0, 10)],
                  plane((40, 0, 0), (0, 1, 0), (0, 0, 1)), name="Section")
    feat(d, type="Sweep", profile_id=prof, path_id=path, mode="new_body",
         name="Bend")


def _frame(d, name, origin, z=(0, 0, 1), x=(1, 0, 0)):
    return d.call("add_feature", feature={
        "type": "Frame", "name": name, "kind": "explicit",
        "origin": list(origin), "z_axis": list(z), "x_axis": list(x)})["id"]


def _mechanism(d):
    """T4/T5's by-hand build: three parts, one assembly, three mates, left at
    full reach.

    Mated between NAMED FRAMES (v86), which is the only way to say "the rod's
    first hole pivots on the crank's pin" without transcribing where either
    of them is. Origin-to-origin mates stack all three parts on the crank
    axis, which is what the first version of this did.
    """
    # --- the parts
    d.call("add_part", name="crank")
    d.call("add_feature", feature={
        "type": "Cylinder", "name": "Disc", "center": [0, 0, 0],
        "direction": [0, 0, 1], "radius": 28.0, "height": 8.0})
    d.call("add_feature", feature={
        "type": "Cylinder", "name": "Pin", "center": [20, 0, 8],
        "direction": [0, 0, 1], "radius": 4.0, "height": 12.0})
    _frame(d, "pin", (20, 0, 8))
    # The crank is the fixed part, so it also carries the ground frame the
    # piston slides along: Z of the frame is the slide direction.
    _frame(d, "ground_x", (0, 0, 8), z=(1, 0, 0), x=(0, 0, 1))

    d.call("add_part", name="rod")
    d.call("add_feature", feature={
        "type": "Box", "name": "Bar", "position": [-7, -6, 0],
        "width": 84.0, "height": 12.0, "depth": 6.0})
    holes = sketch(d, [circle(0, 0, 4.5), circle(70, 0, 4.5)])
    extrude(d, holes, 8.0, mode="cut", name="Holes")
    _frame(d, "big_end", (0, 0, 0))
    _frame(d, "small_end", (70, 0, 0))

    d.call("add_part", name="piston")
    d.call("add_feature", feature={
        "type": "Cylinder", "name": "Piston", "center": [-15, 0, 0],
        "direction": [1, 0, 0], "radius": 15.0, "height": 30.0})
    _frame(d, "wrist", (0, 0, 0))
    _frame(d, "slide", (0, 0, 0), z=(1, 0, 0), x=(0, 0, 1))

    # --- the assembly: crank fixed; the rod pivots on the pin at one end and
    # on the piston at the other; the piston is held to the X axis.
    d.call("add_assembly", name="mechanism")
    crank = d.call("add_occurrence", part="crank", fixed=True)["id"]
    rod = d.call("add_occurrence", part="rod")["id"]
    piston = d.call("add_occurrence", part="piston")["id"]
    d.call("add_mate", type="revolute", name="big end",
           a={"occurrence": crank, "frame": "pin"},
           b={"occurrence": rod, "frame": "big_end"}, flipped=False)
    d.call("add_mate", type="revolute", name="small end",
           a={"occurrence": rod, "frame": "small_end"},
           b={"occurrence": piston, "frame": "wrist"}, flipped=False)
    d.call("add_mate", type="slider", name="bore",
           a={"occurrence": crank, "frame": "ground_x"},
           b={"occurrence": piston, "frame": "slide"}, flipped=False)
    d.call("solve_assembly")


def t4_slider_crank(d):
    _mechanism(d)


def t5_crank_motion(d):
    _mechanism(d)


BUILDERS = {
    "t0-plate": t0_plate, "t0-spacer": t0_spacer,
    "t1-mount-plate": t1_mount_plate, "t1-l-bracket": t1_l_bracket,
    "t2-flange": t2_flange, "t2-stepped-shaft": t2_stepped_shaft,
    "t3-gusset-bracket": t3_gusset_bracket,
    "t3-parametric-bushing": t3_parametric_bushing,
    "t3-coil-spring": t3_coil_spring,
    "t3-taper-adapter": t3_taper_adapter,
    "t3-bent-tube": t3_bent_tube,
    "t4-slider-crank": t4_slider_crank,
    "t5-crank-motion": t5_crank_motion,
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
