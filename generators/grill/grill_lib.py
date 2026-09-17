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
from _lib import check, circle, line, plane_at_z, rect  # noqa: E402,F401


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
    sk = sketch(d, plane_at_z(z), [rect((-x / 2.0, -y / 2.0), (x / 2.0, y / 2.0))],
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
