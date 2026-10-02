"""What every stage of the pan/tilt head shares: where its output goes, the
connection, and the few helpers that are several RPCs."""
import argparse
import json
import math
import os
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
# What the run makes -- projects, renders, reports -- is regenerable, so it
# goes to the untracked scratch/, not beside the code.
# DESIGNER_SCRATCH moves it (tests/generator_runs.py runs it in a private
# directory); unset, the repo's scratch/.
OUT = Path(os.environ.get("DESIGNER_SCRATCH")
           or HERE.parent.parent / "scratch") / "pantilt"
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(HERE.parent.parent / "tools"))
sys.path.insert(0, str(HERE))
from designer_sdk import connect, Plane, Param  # noqa: E402,F401

PRINT = {"process": "fdm", "material": "petg"}


def uv_on(face, point):
    """Where a world point lands in the frame a primitive standing on
    `face` is drawn in (origin the face's centroid, u its frame's X):
    `at=uv_on(face, p)`. The face's X is the kernel's to choose, so it is
    asked, not assumed."""
    f = face.frame()
    d = [p - o for p, o in zip(point, f.origin)]
    return (sum(a * b for a, b in zip(d, f.x)),
            sum(a * b for a, b in zip(d, f.y)))


def args():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    return ap.parse_args()


class Clock:
    """Wall time per step, for the friction list: where the run waits."""

    def __init__(self):
        self.t0 = time.monotonic()
        self.last = self.t0
        self.rows = []

    def __call__(self, what):
        now = time.monotonic()
        self.rows.append((what, now - self.last))
        print(f"  [{now - self.t0:7.1f} s] {what} ({now - self.last:.1f} s)",
              flush=True)
        self.last = now


def printed(part):
    """Declare a part printed in PETG (set_print_process) and give it the
    material; returns a function mapping a size that must PRINT to the size
    to MODEL, asked of the app's allowance table (never typed here)."""
    part.activate()
    part.app.set_print_process(**PRINT)
    cache = {}

    def model(kind, size):
        key = (kind, round(float(size), 4))
        if key not in cache:
            cache[key] = part.app.print_allowance(
                kind=kind, size_mm=float(size))["model_mm"]
        return cache[key]
    return model


def frame(part, name, origin, z, x):
    """An explicit named frame: what a mate anchors to by name. An origin
    coordinate given as a Param (`named(S)`) stays its variable, so the
    frame follows it."""
    return part.frame(name, origin=tuple(
        c if isinstance(c, (Param, str)) else float(c) for c in origin),
        z=tuple(z), x=tuple(x))


class named(dict):
    """The project's variables as Params: `X = named(S)`, then `X.tilt_z`
    is the number AND the name, and Python arithmetic on them hands the
    document the formula (designer_sdk.Param). A layout written in these
    follows its variables; one written in `S[...]` floats is numbers."""

    def __init__(self, S):
        super().__init__({k: Param(v, k) for k, v in S.items()})

    def __getattr__(self, name):
        try:
            return self[name]
        except KeyError:
            raise AttributeError(name) from None


# The world planes a sketch is drawn on, as Plane.xy/xz/yz give them:
# (the axis the offset is along, the normal, the u axis), plain and flipped.
_PLANES = {
    ("xy", False): (2, (0, 0, 1), (1, 0, 0)),
    ("xy", True): (2, (0, 0, -1), (1, 0, 0)),
    ("xz", False): (1, (0, -1, 0), (1, 0, 0)),
    ("xz", True): (1, (0, 1, 0), (-1, 0, 0)),
    ("yz", False): (0, (1, 0, 0), (0, 1, 0)),
    ("yz", True): (0, (-1, 0, 0), (0, -1, 0)),
}


def plane(part, name, kind, at, flip=False):
    """Where a sketch stands when no face of the part is there to stand it
    on: the plane `Plane.<kind>(at, flip)` draws on, as a Frame whose
    origin is `at` -- a Param, so the plane is the variable's and follows
    it (a world plane at a number stays where it was put; check_literals
    reports one a face lies on). Sketch on it with
    `part.sketch(on=common.plane(...))`; its u and v are Plane's, so a
    profile drawn for Plane.<kind> is drawn the same on it."""
    axis, z, x = _PLANES[(kind, flip)]
    origin = [0.0, 0.0, 0.0]
    origin[axis] = at
    return frame(part, f"{name} plane", origin, z, x)


def groove(part, radius, rim_z, offset, name):
    """A DIN 471 groove (RetainingRingGroove) in the shaft of `radius` that
    runs along part Z: measured from its rim at height `rim_z` (the end it
    is measured from) to the groove's near flank `offset` along it -- a
    Param, so the document carries the formula. The standard and size are
    the face's (docs/parts/README.md#the-groove); returns the feature."""
    body = part.body()
    faces = [f for f in body.faces(kind="cylinder")
             if f.radius is not None and abs(f.radius - radius) < 1e-4]
    rims = [e for e in body.edges(circular=True)
            if e.radius and abs(e.radius - radius) < 1e-4
            and abs(e.midpoint[2] - float(rim_z)) < 1e-4]
    if len(faces) != 1 or len(rims) != 1:
        raise SystemExit(f"{part.name}: groove on the Ø{2 * radius:g} shaft "
                         f"found {len(faces)} faces, {len(rims)} rims")
    feature = {"type": "RetainingRingGroove", "name": name,
               "face": faces[0].ref, "from": rims[0].ref,
               "offset": float(offset)}
    if isinstance(offset, Param):
        feature["offset_expr"] = offset.expr
    return part.app.add_feature(feature=feature)


# Every bearing the head presses into a housing sits in the same seat: a
# counterbore the bearing's outside diameter wide and its width deep, cut
# into the face it is pressed in from. Drawn once, saved as a feature
# template (docs/modeling/templates.md), and placed as an instance on each
# housing -- the base plate's pocket, the pan wheel's counterbore and the
# yoke's two tilt seats -- so the seat is one definition with four sets of
# values, not four copies of two features.
SEAT = "bearing seat"


def define_bearing_seat(part, face, D, B):
    """Save the bearing seat template in the project's library, drawn on
    `face` of `part` with the variables `D` (the bearing's outside
    diameter) and `B` (its width) -- Params, so they become the template's
    parameters D and B -- and the centre's offset along the face's v as a
    third, `v`. The two features are taken out again: the part's own seat
    is an instance like every other (bearing_seat)."""
    sk = part.sketch(on=face, name="Bearing seat")
    sk.circle((0, 0), d=D)
    sketch = sk.add()
    cut = part.cut(sketch, B, reversed=True, name="Bearing seat")
    cut_id = cut.feature.id if hasattr(cut, "feature") else cut.id
    part.save_template(
        SEAT, [sketch, cut_id],
        literals=[{"feature": sketch, "path": "entities/0/cy",
                   "param": "v"}],
        rename={D.expr: "D", B.expr: "B"},
        comments={"D": "mm, the bearing's outside diameter as modelled "
                       "(a printed housing's allows for the print)",
                  "B": "mm, the bearing's width: how deep it is pressed in",
                  "v": "mm, the seat's centre along the face's v"},
        comment="a counterbore a bearing is pressed into: D across, B deep",
        replace=True)
    part.app.delete_feature(feature_id=cut_id)
    part.app.delete_feature(feature_id=sketch.id)


def bearing_seat(part, on, D, B, v=0.0, name="Bearing seat"):
    """An instance of the bearing seat template standing on `on` (a face or
    a frame): `D` across and `B` deep, its centre `v` along the frame's v.
    Each a number or a Param; a Param stays its variable."""
    return part.template(SEAT, on, {"D": D, "B": B, "v": v}, name=name)


def template_part(feats, name, type_):
    """The ids of the features named `name` of type `type_` in a part's
    get_state rows -- a Template instance named `name` answering with the
    features it expanded into of that type."""
    out = [f["id"] for f in feats if f["name"] == name and f["type"] == type_]
    out += [e["id"] for f in feats
            if f["name"] == name and f["type"] == "Template"
            for e in f.get("expanded", []) if e["type"] == type_]
    return out


def ring_groove_face(part):
    """The floor of the part's retaining ring groove, as a face ref."""
    floors = [f for f in part.body().faces(kind="cylinder")
              if "/groove_floor" in f.ref["element"]]
    if len(floors) != 1:
        raise SystemExit(f"{part.name}: {len(floors)} groove floors")
    return floors[0].ref


def save(app, name):
    path = OUT / name
    app.save_project(path=str(path))
    print("saved", path)


def requirements_met(app):
    """Check the project's requirements (docs/requirements/README.md) as a
    job and print each: what the head is FOR, held by the project rather
    than asserted here, so an edit after this run is checked against the
    same sentences. Returns every row and the ones that are not `pass` (a
    stale, unresolved or unchecked one is not met either)."""
    rows = app.verify_requirements(raise_on_failure=False)["requirements"]
    for x in rows:
        value = (f" {x['value']:.4g} {x.get('unit', '')}".rstrip()
                 if "value" in x else "")
        bound = x["bound"]
        limit = {"at_most": f" <= {x.get('limit', bound.get('value'))}",
                 "at_least": f" >= {x.get('limit', bound.get('value'))}",
                 "within": f" in {x.get('limit_min')}..{x.get('limit_max')}",
                 }.get(bound["kind"], "")
        note = f" ({x['note']})" if x.get("note") else ""
        print(f"  requirement {x['name']}: {x['state']}{value}{limit}{note}",
              flush=True)
    return rows, [x for x in rows if x["state"] != "pass"]


def stop_unless_met(bad):
    if bad:
        raise SystemExit("requirements not met: " + "; ".join(
            f"{x['name']}: {x['state']}" for x in bad))


# The parts this generator models (the bought ones are generated from their
# standard, and a stocked screw length can equal a variable by coincidence).
MODELLED = ("base_plate", "pan_wheel", "worm", "worm_block", "knob",
            "tilt_wheel", "cradle", "camera", "yoke")


def check_regen(app, what):
    st = app.call("get_state")
    if not st["regen"]["ok"]:
        raise SystemExit(f"REGEN FAILED in {what}: {json.dumps(st['regen'])}")
    return st


def part_named(app, name):
    for p in app.call("list_parts")["parts"]:
        if p["name"] == name:
            return p
    raise SystemExit(f"no part {name!r}")


def drop_default_part(app):
    """The empty part a new project opens with is not the head's."""
    for p in app.call("list_parts")["parts"]:
        if p["name"] == "Part 1":
            app.call("delete_part", part=p["id"])


def on_face_uv(face, point):
    """A world point (x, y, z), each a Param or a number, as (u, v) on a
    face-attached sketch of `face` -- the face's own frame, asked of the
    app (origin its centroid, u its X: the kernel's to choose). Kept as
    Param arithmetic, so a hole drawn at `X.screw_pcd / 2` on the face
    stays that expression; the face's centroid must be where the
    expression is measured from, so a face whose centroid is off the
    origin in its plane is refused rather than baked in as a number."""
    f = face.frame()
    off = [o - f.z[i] * sum(a * b for a, b in zip(f.origin, f.z))
           for i, o in enumerate(f.origin)]
    if max(abs(c) for c in off) > 1e-9:
        raise SystemExit(f"face {face.name}: its centroid {list(f.origin)} "
                         f"is off the axis; put the sketch on a plane")

    def dot(axis):
        out = 0.0
        for c, p in zip(axis, point):
            if abs(c) < 1e-12 or (not isinstance(p, Param) and p == 0):
                continue
            term = p if abs(c - 1) < 1e-12 else (
                -p if abs(c + 1) < 1e-12 else p * c)
            out = term if (not isinstance(out, Param) and out == 0) \
                else out + term
        return out
    return dot(f.x), dot(f.y)


def bolt_points(radius, n=4, start_deg=45.0):
    """`n` points on a circle, the first `start_deg` off +X."""
    return [(radius * math.cos(math.radians(start_deg + 360.0 * i / n)),
             radius * math.sin(math.radians(start_deg + 360.0 * i / n)))
            for i in range(n)]
