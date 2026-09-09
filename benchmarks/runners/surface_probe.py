#!/usr/bin/env python3
"""B2 — what surface types does each Designer operation actually leave?

The `surfaces` check kind (grade.py) asks HOW a part was built by counting the
face types in the exported STEP. Which type a given operation produces is a
PREDICTION until it is measured: a loft between two circles may well be a
`bspline` rather than a `cone`, `Refine` heals the B-Rep and may convert
between them, and STEP export gets its own say. Writing task checks against a
guess is how two of the first eight tasks shipped with wrong expected numbers.

So: build one probe per operation, export, and print the census the oracle
reads. A probe that FAILS is a result too — it says that operation cannot be
driven that way, and no task should depend on it.

    nix-shell shell.nix --run "python3 benchmarks/runners/surface_probe.py"
    ... --probe revolve_frustum --keep /tmp/probes    # one, keep the STEP
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
from grade import read_facts          # noqa: E402

XY = {"origin": [0, 0, 0], "normal": [0, 0, 1], "uAxis": [1, 0, 0]}
# U = X, V = +Z. (V = normal x uAxis, so the OBVIOUS normal (0,1,0) gives
# V = -Z and every height comes out negated — the V-axis trap that
# t3-gusset-bracket exists to catch.)
XZ = {"origin": [0, 0, 0], "normal": [0, -1, 0], "uAxis": [1, 0, 0]}


def plane(origin, normal=(0, 0, 1), u=(1, 0, 0)):
    return {"origin": list(origin), "normal": list(normal), "uAxis": list(u)}


def circle(cx, cy, r, **kw):
    return {"type": "circle", "center": [cx, cy], "radius": r, **kw}


def sketch(d, entities, pl=None, name="S"):
    return d.call("add_feature", feature={"type": "Sketch", "name": name,
                                          "plane": pl or XY,
                                          "entities": entities})["id"]


def feat(d, **kw):
    return d.call("add_feature", feature=kw)


# --- probes: one operation each, named for what it should produce ----------
# Each returns a one-line note on what it built, for the report.

def extrude_cylinder(d):
    """Baseline. Sketch-and-extrude is the construction everything else is
    being distinguished FROM, so its census is the control."""
    sid = sketch(d, [circle(0, 0, 20)])
    feat(d, type="Extrude", sketch_id=sid, distance=30, mode="new_body",
         name="E")
    return "circle extruded 30"


def stack_taper(d):
    """The WRONG way to build a taper: six discs of falling radius. Same bbox
    and roughly the same volume as a frustum; this is the census a real taper
    check has to be able to reject."""
    for i in range(6):
        sid = sketch(d, [circle(0, 0, 20 - 1.5 * i)], plane((0, 0, 5 * i)))
        feat(d, type="Extrude", sketch_id=sid, distance=5,
             mode="new_body" if i == 0 else "add", name=f"D{i}")
    return "6 stacked discs r20->r12.5"


def revolve_frustum(d):
    """A straight profile line revolved about Z — the textbook cone."""
    prof = [{"type": "line", "a": [0, 0], "b": [20, 0]},
            {"type": "line", "a": [20, 0], "b": [12, 30]},
            {"type": "line", "a": [12, 30], "b": [0, 30]},
            {"type": "line", "a": [0, 30], "b": [0, 0]}]
    sid = sketch(d, prof, XZ)
    feat(d, type="Revolve", sketch_id=sid, axis={"kind": "z"},
         angle_deg=360, mode="new_body", name="R")
    return "trapezoid revolved 360 about Z"


def revolve_torus(d):
    """A circle offset from the axis, revolved — the ring/bend case."""
    sid = sketch(d, [circle(30, 0, 8)], XZ)
    feat(d, type="Revolve", sketch_id=sid, axis={"kind": "z"},
         angle_deg=360, mode="new_body", name="R")
    return "circle at R30 r8 revolved 360 about Z"


def loft_circles(d):
    """Two circles at different Z. Geometrically a frustum — but lofts are
    usually built as an approximating surface, which is the whole question."""
    a = sketch(d, [circle(0, 0, 20)], plane((0, 0, 0)), name="A")
    b = sketch(d, [circle(0, 0, 12)], plane((0, 0, 30)), name="B")
    feat(d, type="Loft", sketch_ids=[a, b], mode="new_body", name="L")
    return "loft r20 -> r12 over 30"


def sweep_helix(d):
    """A coil spring: circle swept along a Curve3D helix."""
    path = feat(d, type="Curve3D", kind="helix", origin=[0, 0, 0],
                axis=[0, 0, 1], radius=20, pitch=10, turns=3, name="H")["id"]
    prof = sketch(d, [circle(20, 0, 2)], XZ, name="Wire")
    feat(d, type="Sweep", profile_id=prof, path_id=path, mode="new_body",
         name="Sw")
    return "r2 wire swept along helix R20 pitch10 x3"


def pipe_helix(d):
    """The same helix as a Pipe — the one-call spelling of the same part."""
    path = feat(d, type="Curve3D", kind="helix", origin=[0, 0, 0],
                axis=[0, 0, 1], radius=20, pitch=10, turns=3, name="H")["id"]
    feat(d, type="Pipe", path_id=path, outer_diameter=4, wall_thickness=1,
         mode="new_body", name="Pi")
    return "OD4 wall1 pipe along the same helix"


def pipe_spline(d):
    """A bent tube. Curve3D has no ARC kind — only fit_spline, control_poly
    and helix — so a bend is a spline, and whether that reads as `torus` or
    `bspline` decides what t3-bent-tube can check."""
    path = feat(d, type="Curve3D", kind="fit_spline",
                points=[[0, 0, 0], [40, 0, 0], [60, 0, 20], [60, 0, 60]],
                name="P")["id"]
    feat(d, type="Pipe", path_id=path, outer_diameter=12, wall_thickness=2,
         mode="new_body", name="Pi")
    return "OD12 wall2 tube along a 4-point fit spline"


def sweep_sketch_arc(d):
    """A sweep whose PATH is a sketch containing an arc — the other way to
    reach a bend, and the one that might give a true torus."""
    path = sketch(d, [{"type": "arc", "center": [0, 0], "radius": 40,
                       "start_angle": 0, "end_angle": math.pi / 2}],
                  XY, name="Path")   # RADIANS — there is no _deg spelling
    prof = sketch(d, [circle(0, 0, 6)], plane((40, 0, 0), (0, 1, 0), (0, 0, 1)),
                  name="Prof")
    feat(d, type="Sweep", profile_id=prof, path_id=path, mode="new_body",
         name="Sw")
    return "r6 profile swept along a 90 deg sketch arc R40"


def shell_box(d):
    """Shell doubles a face count without changing any surface KIND — worth
    knowing before a task tries to check a wall thickness with a census."""
    sid = sketch(d, [{"type": "rect", "c1": [0, 0], "c2": [60, 40]}])
    box = feat(d, type="Extrude", sketch_id=sid, distance=30,
               mode="new_body", name="Box")
    faces = d.call("list_elements", body=0)["bodies"][0]["faces"]
    top = next(f for f in faces if f["normal"][2] > 0.9)
    feat(d, type="Shell", body_id=box["id"],
         # `element` is the FULL name list_elements reports ("2/cap1"),
         # feature-id prefix included, even though feature_id is passed
         # alongside it. The suffix alone is refused.
         open_faces=[{"feature_id": box["id"], "element": top["name"]}],
         thickness=3, name="Sh")
    return "60x40x30 box shelled 3 through its top"


PROBES = [
    ("extrude_cylinder", extrude_cylinder),
    ("stack_taper", stack_taper),
    ("revolve_frustum", revolve_frustum),
    ("revolve_torus", revolve_torus),
    ("loft_circles", loft_circles),
    ("sweep_helix", sweep_helix),
    ("pipe_helix", pipe_helix),
    ("pipe_spline", pipe_spline),
    ("sweep_sketch_arc", sweep_sketch_arc),
    ("shell_box", shell_box),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--probe", default="")
    ap.add_argument("--keep", default="")
    ap.add_argument("--binary", default=os.path.join(ROOT, "build", "designer"))
    a = ap.parse_args()

    probes = [p for p in PROBES if not a.probe or p[0] == a.probe]
    if not probes:
        sys.exit(f"no such probe: {a.probe}")
    out = a.keep or tempfile.mkdtemp(prefix="bench-probe-")
    os.makedirs(out, exist_ok=True)

    rows = []
    with launch(a.binary) as app:
        d = app.designer
        for name, fn in probes:
            step = os.path.join(out, f"{name}.step")
            note, err = "", None
            try:
                d.call("new_project")
                note = fn(d)
                d.call("export_step", path=step)
            except Exception as e:                      # a failed probe is data
                err = str(e)[:200]
            if err:
                rows.append((name, note, err, None))
                continue
            facts = read_facts(step)
            if not facts.get("ok"):
                rows.append((name, note, facts.get("error", "oracle said no"),
                             None))
                continue
            rows.append((name, note, None, facts))

    print("\n| probe | built | solids | census |")
    print("|---|---|---|---|")
    for name, note, err, facts in rows:
        if err:
            print(f"| `{name}` | {note or '—'} | — | **failed**: {err} |")
            continue
        census = facts.get("faces_by_kind", {})
        cells = " ".join(f"{k}:{v}" for k, v in sorted(census.items()))
        print(f"| `{name}` | {note} | {facts.get('solids')} | {cells} |")

    print(f"\nSTEP files: {out}")
    print(json.dumps({n: (f or {}).get("faces_by_kind")
                      for n, _, _, f in rows}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
