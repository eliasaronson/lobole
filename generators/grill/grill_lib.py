# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""What every grill module needs: sheet parts, edge picks, blanks, checks.

The point of picking edges BY GEOMETRY rather than by stored name is that a
generator must say which edge it means. Which of a rim's two edges is picked
decides which way a wall folds, so `edges_at` takes a predicate over the
midpoint and refuses when it finds nothing.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from lobole.sdk import Plane  # noqa: E402
from _lib import check, circle, line, rect  # noqa: E402,F401


# --- parts ----------------------------------------------------------------

def part(d, name, stock=None):
    """A fresh part tab, its sheet stock declared. Returns its slot id."""
    slot = d.call("add_part", name=name)["id"]
    d.call("set_active_part", part=name)
    if stock:
        material, gauge = stock
        d.call("set_sheet_metal_rules", material=material, gauge=gauge)
    return slot


def thickness(d):
    return d.call("get_sheet_metal_rules")["rules"]["thickness_mm"]


def bend_radius(d):
    return d.call("get_sheet_metal_rules")["rules"]["bend_radius_mm"]


# --- features -------------------------------------------------------------

def sketch(d, plane, entities, name="Sketch"):
    return d.call("add_feature", feature={
        "type": "Sketch", "name": name, "plane": plane,
        "entities": entities})["id"]


def blank(d, x, y, z=0.0, name="Blank"):
    """A rectangular blank centred on the origin, its inside face at z."""
    sk = sketch(d, Plane.xy(z).json(), [rect((-x / 2.0, -y / 2.0), (x / 2.0, y / 2.0))],
                name + " profile")
    r = d.call("add_feature", feature={
        "type": "BaseFlange", "name": name, "sketch_id": sk})
    check(r, name)
    return r["id"]


def profile_blank(d, plane, entities, name="Blank"):
    """A blank from an arbitrary closed profile."""
    sk = sketch(d, plane, entities, name + " profile")
    r = d.call("add_feature", feature={
        "type": "BaseFlange", "name": name, "sketch_id": sk})
    check(r, name)
    return r["id"]


def edges_at(d, pred, body=0, what="edge", detail=False):
    """Straight edges whose midpoint satisfies `pred`. Refuses an empty pick."""
    b = d.call("list_elements")["bodies"][body]
    hits = []
    for e in b["edges"]:
        mid = e.get("midpoint")
        if e.get("kind") != "line" or mid is None:
            continue
        if pred(mid):
            hits.append((e["name"], mid) if detail else e["name"])
    if not hits:
        raise SystemExit(f"no {what} matched the pick among "
                         f"{len(b['edges'])} edges")
    return hits


def outer_edge(d, pred, axis, sign, what="edge"):
    """Of the edges matching `pred`, the one furthest along `axis` * `sign`.

    Which of a wall's two top edges is picked decides which way its rim
    folds — the outer one folds away from the box — and they are a
    thickness apart, so this asks for the extreme rather than a position.
    """
    hits = edges_at(d, pred, what=what, detail=True)
    return max(hits, key=lambda h: sign * h[1][axis])[0]


def refs(feature_id, names):
    return [{"feature_id": feature_id, "element": n} for n in names]


def flange(d, base, names, length, name="Flange", angle_deg=90.0,
           position="bend_outside", **kw):
    f = {"type": "EdgeFlange", "name": name, "length": length,
         "angle_deg": angle_deg, "position": position,
         "edges": refs(base, names)}
    f.update(kw)
    r = d.call("add_feature", feature=f)
    check(r, name)
    return r["id"]


def hem(d, base, names, length, name="Hem", **kw):
    f = {"type": "Hem", "name": name, "length": length,
         "edges": refs(base, names)}
    f.update(kw)
    r = d.call("add_feature", feature=f)
    check(r, name)
    return r["id"]


def corner_relief(d, base, name="Corner relief", **kw):
    f = {"type": "CornerRelief", "name": name, "body_id": base}
    f.update(kw)
    r = d.call("add_feature", feature=f)
    check(r, name)
    return r["id"]


def normal_cut(d, plane, entities, name="Cut", **kw):
    """A cut square to the sheet wherever the profile lands."""
    sk = sketch(d, plane, entities, name + " profile")
    f = {"type": "NormalCut", "name": name, "sketch_id": sk}
    f.update(kw)
    r = d.call("add_feature", feature=f)
    check(r, name)
    return r["id"]


def frame(d, name, origin, z_axis=(0, 0, 1), x_axis=(1, 0, 0)):
    """A named datum frame — what a mate anchors to."""
    r = d.call("add_feature", feature={
        "type": "Frame", "name": name, "kind": "explicit",
        "origin": list(origin), "z_axis": list(z_axis),
        "x_axis": list(x_axis)})
    check(r, "frame " + name)
    return r["id"]


def flat_config(d, base, name="Flat"):
    """The blank, authored OFF and switched on by a configuration.

    A folded part and its blank occupy the same space, so the flat pattern
    replaces the shape rather than adding a body — which is exactly why it is
    suppressed and lives in a configuration.
    """
    r = d.call("add_feature", feature={
        "type": "FlatPattern", "name": "Flat pattern", "body_id": base})
    check(r, "flat pattern")
    flat_id = r["id"]
    d.call("suppress_feature", feature_id=flat_id, suppressed=True)
    cfg = d.call("add_configuration", name=name)["id"]
    d.call("set_configuration", id=cfg, unsuppressed_features=[flat_id])
    return cfg


# --- what a shop and a reviewer get out ------------------------------------

def body_of(d):
    return d.call("get_state")["bodies"][0]


def sheet_ok(d, what):
    """check_sheet_metal, and the check that it actually checked something."""
    chk = d.call("check_sheet_metal")
    if chk["violations"]:
        raise SystemExit(f"{what}: sheet metal violations {chk['violations']}")
    return chk


def geometry_ok(d, what):
    chk = d.call("check_geometry")
    bad = [i for i in chk.get("issues", []) if i.get("severity") != "info"]
    if bad:
        raise SystemExit(f"{what}: geometry issues {bad}")
    return chk


# --- world-coordinate sheet work -------------------------------------------
# Most of the grill's parts are drawn where they LIVE, in world coordinates,
# so that a socket on one part is the world position of the part it carries
# and a hole in two parts is one number in grill_spec. That needs a sketch
# plane placed on a world face, and the in-plane (u, v) of a world point on
# it: V = normal x uAxis, so v is not always the axis one expects, and every
# sign below is that cross product rather than a guess.

def sheet_plane(axis, level, toward):
    """(plane, uv): a plane on `axis` at `level`, its normal `toward` (+-1).

    Material grows along the normal, so a panel whose finished face is at
    `level` is drawn here and comes out on the `toward` side of it.
    """
    s = 1.0 if toward > 0 else -1.0
    if axis == "z":
        return ({"origin": [0, 0, level], "normal": [0, 0, s],
                 "uAxis": [1, 0, 0]},
                lambda p: (p[0], s * p[1]))
    if axis == "x":
        return ({"origin": [level, 0, 0], "normal": [s, 0, 0],
                 "uAxis": [0, 1, 0]},
                lambda p: (p[1], s * p[2]))
    if axis == "y":
        return ({"origin": [0, level, 0], "normal": [0, s, 0],
                 "uAxis": [1, 0, 0]},
                lambda p: (p[0], -s * p[2]))
    raise SystemExit(f"axis must be x, y or z, not {axis!r}")


def sheet_panel(d, axis, level, toward, corner_a, corner_b, name="Blank"):
    """A rectangular blank between two WORLD corners on that plane."""
    plane, uv = sheet_plane(axis, level, toward)
    return profile_blank(d, plane, [rect(uv(corner_a), uv(corner_b))],
                         name=name)


def holes(d, axis, level, toward, centres, diameter, name="Holes"):
    """Clearance holes square to the sheet, at WORLD centres.

    The plane looks at the panel from `level` along `toward`: a plane that
    sees two panels has no single sheet to cut, so it is placed between them.
    """
    plane, uv = sheet_plane(axis, level, toward)
    return normal_cut(d, plane, [circle(uv(c), diameter / 2.0) for c in centres],
                      name=name)


def drill(d, axis, level, toward, centres, diameter, depth, name="Drill"):
    """A plain drilled hole in a SOLID part (not sheet): a cut extrude."""
    plane, uv = sheet_plane(axis, level, toward)
    sk = sketch(d, plane, [circle(uv(c), diameter / 2.0) for c in centres],
                name + " profile")
    r = d.call("add_feature", feature={
        "type": "Extrude", "name": name, "sketch_id": sk,
        "distance": depth, "mode": "cut"})
    check(r, name)
    return r["id"]


def tapped(d, axis, level, toward, centres, size, depth, name="Tapped"):
    """A tapped blind hole: ONE feature that knows what it is for.

    `fit: {thread, kind: "tapped"}` derives the tap drill from the thread and
    records the thread on the bore it cuts, so a screw in it ENGAGES rather
    than clashing. Drilled by hand it took three coordinated things — the tap
    drill diameter, a Thread feature, and engagement allowed on it — and
    missing any one of them was 40 mm3 of silent interference.
    """
    import grill_spec as _S
    if size not in _S.TAP:
        raise SystemExit(f"no tap drill for {size}")
    # A Hole drills along -normal, so the plane looks back the way the hole
    # goes in.
    plane, uv = sheet_plane(axis, level, -toward)
    sk = sketch(d, plane, [{"type": "point", "p": list(uv(c))} for c in centres],
                name + " centres")
    r = d.call("add_feature", feature={
        "type": "Hole", "name": name, "sketch_id": sk, "hole_kind": "simple",
        "depth": depth, "diameter": _S.TAP[size],
        "fit": {"thread": size, "kind": "tapped"}})
    check(r, name)
    return r["id"]


def bracket_frame(out, up):
    """The frame a folded angle is welded by: `out` is where its flat leg
    points, `up` where its upstand goes — both world directions.

    The part is drawn with its flat leg along -y from the heel and its
    upstand along +z, so the socket maps local -y to `out` and local z to
    `up`; x follows from the two, and getting it from the cross product is
    what stops a ledge being welded on inside out.
    """
    y = [-o for o in out]
    z = list(up)
    x = [y[1] * z[2] - y[2] * z[1], y[2] * z[0] - y[0] * z[2],
         y[0] * z[1] - y[1] * z[0]]
    return {"z_axis": z, "x_axis": x}


def rim(d, part, centre, axis, level, diameter, occurrence=None):
    """The circular edge a bolt head sits against: {feature_id, element}.

    A circle edge's midpoint is a point ON the circle, not its centre, so a
    hole is found by its axial position and its distance from the centre —
    which is also what tells two coaxial rims (the two ends of one hole)
    apart.
    """
    ai = {"x": 0, "y": 1, "z": 2}[axis]
    want = diameter / 2.0
    best = None
    for body in d.request("list_elements", {"part": part})["bodies"]:
        for e in body["edges"]:
            if e.get("kind") != "circle":
                continue
            if abs(e.get("radius", 0.0) - want) > 0.05:
                continue
            mid = e["midpoint"]
            if abs(mid[ai] - level) > 0.05:
                continue
            off = [mid[i] - centre[i] for i in range(3)]
            off[ai] = 0.0
            if abs(sum(o * o for o in off) ** 0.5 - want) > 0.05:
                continue
            best = {"feature_id": body["created_by"], "element": e["name"]}
    if best is None:
        raise SystemExit(
            f"{part}: no rim of {diameter} at {axis}={level} around {centre}")
    if occurrence is not None:
        best["occurrence"] = occurrence
    return best
