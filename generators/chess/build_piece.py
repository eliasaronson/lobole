#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The turned piece: one part, six configurations.

    python3 generators/chess/build_piece.py --port N

The king's height (`king_h`, project scope) is the only literal size. Each
configuration overrides the piece's height and base radius as fractions of
it (FIDE's proportions) and turns on its own profile and head details; the
authored model is the king.
"""
import argparse
import math
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
# What the run makes -- projects, programs, renders, reports -- is
# regenerable, so it goes to the untracked scratch/, not beside the code.
# LOBOLE_SCRATCH moves it (tests/generator_runs.py runs it in a private
# directory); unset, the repo's scratch/.
OUT = Path(os.environ.get("LOBOLE_SCRATCH")
           or HERE.parent.parent / "scratch") / "chess"
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(HERE))
from lobole.sdk import connect, Param, Plane  # noqa: E402
import profiles  # noqa: E402

KING_H = 85.0
# The steel bar the weights are cut from, bought: the underside bore takes it,
# and the blanks plug that bore to the same size (build_blanks.py).
SLUG_D = 12.0
# height and base DIAMETER as fractions of the king's height
SIZES = {
    "king": (1.00, 0.45),
    "queen": (0.89, 0.45),
    "bishop": (0.74, 0.40),
    "knight": (0.64, 0.40),
    "rook": (0.58, 0.40),
    "pawn": (0.53, 0.34),
}
PARAMS = []
TIGHT = []  # inside corners with no room for a fillet the finishing ball fits
ORDER = ["king", "queen", "bishop", "knight", "rook", "pawn"]


def draw_profile(sk, kind, piece_h, base_r, u):
    """The closed (x, r) loop of profiles.segments, every coordinate a Param."""
    def at(anchor, p):
        a, b = p
        if anchor == "base":
            return (base_r * a, base_r * b)
        return (piece_h - u * a if a else piece_h, u * b if b else Param(0, "0"))

    zero = (Param(0, "0"), Param(0, "0"))
    cur = zero
    h, d = SIZES[kind]
    dims = (h * KING_H, d / 2 * KING_H, 1.0)
    for seg in profiles.filleted(kind, dims, tight=TIGHT):
        t, anchor = seg[0], seg[1]
        if t == "L":
            nxt = at(anchor, seg[3])
            sk.line(cur, nxt)
            cur = nxt
        elif t == "S":
            nxt = at("head", seg[3])
            sk.line(cur, nxt)
            cur = nxt
        elif t == "A":
            p, q, s = seg[2], seg[3], seg[4]
            # the circle in the anchor's own units, where the scale is
            # uniform: an arc stays an arc at any size
            if anchor == "head":
                # head points are measured DOWN from the top: flip a so the
                # arc's sense is the model's
                p, q, s = [(-a, b) for a, b in (p, q, s)]
            (cx, cy), rr = profiles.circle3(p, q, s)
            a0 = math.atan2(p[1] - cy, p[0] - cx)
            a1 = math.atan2(q[1] - cy, q[0] - cx)
            a2 = math.atan2(s[1] - cy, s[0] - cx)
            tau = 2 * math.pi
            if (a1 - a0) % tau < (a2 - a0) % tau:
                start, end = a0, a2
            else:
                start, end = a2, a0
            if anchor == "base":
                centre = (base_r * cx, base_r * cy)
                radius = base_r * rr
            else:
                centre = (piece_h + u * cx, u * cy)
                radius = u * rr
            sk.arc(centre, radius, math.degrees(start), math.degrees(end))
            back = seg[4]
            cur = at(anchor, back)
        elif t == "C":
            sk.line(cur, zero)


def build(app):
    part = app.part("piece")
    part.activate()
    part.var("king_h", KING_H, comment="the one literal: the king's height",
             scope="project")
    u = part.param("u", "king_h / 85",
                   comment="heads are drawn in mm at an 85 mm king")
    piece_h = part.param("piece_h", "king_h",
                         comment="this piece's height (a configuration sets it)")
    base_r = part.param("base_r", f"{SIZES['king'][1] / 2} * king_h",
                        comment="radius of the foot (a configuration sets it)")

    feats = {}
    for kind in ORDER:
        sk = part.sketch(Plane.xz(), name=f"{kind.capitalize()} profile")
        draw_profile(sk, kind, piece_h, base_r, u)
        s = sk.add()
        body = part.revolve(s, axis="x", name=kind.capitalize())
        feats[kind] = [s.id, body.created_by]
        # Off in the authored model (the king) from the start: six turned
        # pieces overlapping at the origin make every later add fuse all six.
        if kind != "king":
            for fid in feats[kind]:
                app.call("suppress_feature", feature_id=fid, suppressed=True)
    return part, feats


KNIGHT_HEAD = [  # (x at the nominal 54.4 mm knight, z) -- the side outline
    (17, -9), (26, -10.5), (35, -10), (45, -7.5), (51, -5.5), (54.4, -4.5),
    (54.8, 2.5), (52, 3.2), (49, 4), (44, 9), (39, 13.5), (35, 14.8),
    (31.5, 13), (31, 9), (33, 5), (29, 3), (23, 5), (17, 7.5)]
KNIGHT_H = 54.4


def box_cut(part, plane, lo, hi, depth, name, regions=None):
    """A rectangle lo..hi (sketch u, v; Params) cut symmetric about the
    plane by `depth` in all."""
    sk = part.sketch(plane, name=f"{name} sketch")
    (u0, v0), (u1, v1) = lo, hi
    sk.polyline([(u0, v0), (u1, v0), (u1, v1), (u0, v1)], close=True)
    s = sk.add()
    r = part.app.add_feature(feature=dict(
        {"type": "Extrude", "name": name, "sketch_id": s.id, "mode": "cut",
         "end_mode": "symmetric", "distance": float(depth),
         "distance_expr": depth.expr}))
    return [s.id, r["id"]]


def details(app, part, piece_h, base_r, u):
    """Each piece's own cuts, and the underside every piece shares."""
    P = Plane.xz()
    out = {k: [] for k in ORDER}
    # Rook: four crenellation notches, 6 wide, down to r = 8.
    ids = box_cut(part, P, (piece_h - u * 6, u * 8), (piece_h + u * 2, u * 14),
                  u * 6, "Crenel")
    pat = part.pattern(ids[1], 4, circular="x", name="Crenels")
    out["rook"] += ids + [pat["id"]]
    # Bishop: the mitre, a notch 3.2 wide down across the top of the head
    # to a floor 2 mm above the axis -- one pocket from above for a 3 mm
    # cutter. (A slit slanted through the head has no floor any pocket can
    # clear. The blind one measured 0.99 mm across, and was: its 3.2 mm ends
    # were drawn square to (dx, dz) while its sides ran along (-dx, dz).)
    ids = box_cut(part, P, (piece_h - u * 9.0, u * 2.0),
                  (piece_h - u * 5.8, u * 14.0), u * 30, "Mitre")
    out["bishop"] += ids
    # Queen: eight notches round the crown's rim.
    ids = box_cut(part, P, (piece_h - u * 9.8, u * 8.8),
                  (piece_h - u * 6.6, u * 14), u * 3.4, "Crown notch")
    pat = part.pattern(ids[1], 8, circular="x", name="Crown notches")
    out["queen"] += ids + [pat["id"]]
    # King: the cross, cut from its round boss -- four corners away in the
    # side view, then flats either side.
    sk = part.sketch(P, name="Cross corners sketch")
    for (a0, a1) in ((10.5, 7.5), (4.5, -1.0)):
        for sign in (1, -1):
            z0, z1 = u * (2 * sign), u * (8 * sign)
            sk.polyline([(piece_h - u * a0, z0), (piece_h - u * a1, z0),
                         (piece_h - u * a1, z1), (piece_h - u * a0, z1)],
                        close=True)
    s = sk.add()
    r = app.add_feature(feature={"type": "Extrude", "name": "Cross corners",
                                 "sketch_id": s.id, "mode": "cut",
                                 "end_mode": "symmetric", "distance": 30.0,
                                 "distance_expr": (u * 30).expr})
    out["king"] += [s.id, r["id"]]
    sk = part.sketch(Plane.xy(), name="Cross flats sketch")
    for sign in (1, -1):
        y0, y1 = u * (2 * sign), u * (9 * sign)
        sk.polyline([(piece_h - u * 10.5, y0), (piece_h + u * 1, y0),
                     (piece_h + u * 1, y1), (piece_h - u * 10.5, y1)],
                    close=True)
    s = sk.add()
    r = app.add_feature(feature={"type": "Extrude", "name": "Cross flats",
                                 "sketch_id": s.id, "mode": "cut",
                                 "end_mode": "symmetric", "distance": 30.0,
                                 "distance_expr": (u * 30).expr})
    out["king"] += [s.id, r["id"]]
    # Knight: the head, a side outline 11 thick joined to the turned base.
    sk = part.sketch(P, name="Knight head sketch")
    sk.spline([(piece_h - u * (KNIGHT_H - x), u * z) for x, z in KNIGHT_HEAD],
              closed=True)
    s = sk.add()
    r = app.add_feature(feature={"type": "Extrude", "name": "Knight head",
                                 "sketch_id": s.id, "mode": "add",
                                 "end_mode": "symmetric", "distance": 11.0,
                                 "distance_expr": (u * 11).expr})
    out["knight"] += [s.id, r["id"]]
    return out


def underside(app, part, base_r):
    """Felt recess and the steel weight's bore, on every piece."""
    slug_d = part.param("slug_d", SLUG_D, comment="the steel bar the weights are "
                        "cut from, bought")
    recess = part.sketch(Plane.yz(0), name="Felt recess sketch")
    recess.circle((0, 0), d=base_r * 1.8)
    s = recess.add()
    rec = app.add_feature(feature={"type": "Extrude", "name": "Felt recess",
                                   "sketch_id": s.id, "mode": "cut",
                                   "distance": float(base_r * 0.05),
                                   "distance_expr": (base_r * 0.05).expr})
    bore = part.sketch(Plane.yz(0), name="Weight bore sketch")
    bore.circle((0, 0), d=slug_d)
    s = bore.add()
    b = app.add_feature(feature={"type": "Extrude", "name": "Weight bore",
                                 "sketch_id": s.id, "mode": "cut",
                                 "distance": float(base_r * 0.7),
                                 "distance_expr": (base_r * 0.7).expr})
    return rec["id"], b["id"]


def configure(app, part, feats):
    for kind in ORDER:
        if kind == "king":
            continue
        for fid in feats[kind][2:]:  # the head details; the revolve is off
            app.call("suppress_feature", feature_id=fid, suppressed=True)
    ids = {}
    for kind in ORDER:
        h, d = SIZES[kind]
        cid = app.call("add_configuration", name=kind)["id"]
        others = [f for k in ORDER if k != kind for f in feats[k]]
        app.call("set_configuration", id=cid,
                 parameter_overrides=[
                     {"name": "piece_h", "expression": f"{h} * king_h"},
                     {"name": "base_r", "expression": f"{d / 2} * king_h"}],
                 suppressed_features=others,
                 unsuppressed_features=feats[kind])
        ids[kind] = cid
    return ids


def knight_fillet(app, part, ids, feats):
    """Round the head's two side outlines. Picked in the knight's own
    configuration, the only one its edges exist in, then kept off in the
    authored model and on in the knight's."""
    app.call("set_active_configuration", id=ids["knight"])
    # The outline's two curves: the head extrude's spline edges on its caps,
    # asked for by what made them rather than found at y = +-5.5 u.
    u = part.value_of("u")
    head = part.feature("Knight head")
    body = part.bodies()[0]
    edges = [e for role in ("start_cap", "end_cap")
             for e in body.edges(made_by=head, role=role, kind="other")]
    assert len(edges) == 2, [e.name for e in edges]
    r = app.add_feature(feature={
        "type": "Fillet", "name": "Knight head round",
        "edges": [e.ref for e in edges], "radius": 1.5 * u,
        "radius_expr": "1.5 * u"})
    # 2 mm does not fit: the outline turns tighter than that at the mouth
    # and under the chin, and the fillet says so.
    assert r["regen"]["ok"], r["regen"]
    fid = r["id"]
    app.call("set_active_configuration", id=0)
    app.call("suppress_feature", feature_id=fid, suppressed=True)
    feats["knight"].append(fid)
    app.call("set_configuration", id=ids["knight"],
             unsuppressed_features=feats["knight"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    args = ap.parse_args()
    app = connect(args.port)
    app.new()
    part, feats = build(app)
    PARAMS[:] = [part.param_of("piece_h"), part.param_of("base_r"),
                 part.param_of("u")]
    extra = details(app, part, *PARAMS)
    for k in ORDER:
        feats[k] += extra[k]
    underside(app, part, PARAMS[1])
    ids = configure(app, part, feats)
    knight_fillet(app, part, ids, feats)
    for kind in ORDER:
        app.call("set_active_configuration", id=ids[kind])
        st = app.call("get_state")
        assert st["regen"]["ok"], (kind, st["regen"])
        bodies = [b for b in st.get("bodies", []) if b.get("visible", True)]
        vol = sum(b.get("volume_mm3", 0) for b in bodies)
        print(f"{kind:7s} bodies {len(bodies)} volume {vol:9.0f} mm3 "
              f"bbox {part.bbox()}")
        part.render(str(OUT / f"piece_{kind}.png"), view="iso")
    # The empty part a new project opens with is not the set's.
    for p in app.parts():
        if p["name"] == "Part 1":
            app.call("delete_part", part=p["id"])
    app.save_project(path=str(OUT / "piece.dsgn"))
    for t in TIGHT:
        print("  tight inside corner (kind, x, r, fillet mm):", t)


if __name__ == "__main__":
    main()
