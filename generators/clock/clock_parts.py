# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The clock's parts, one function each, every one in the MOVEMENT frame
(clock_spec's docstring): a turning part about its own axis at the XY
origin, at the heights it has between the plates, so every mate is origin
to a frame on the back plate and no part needs an offset. Each prints the
way it is modelled, lifted onto the bed, except the bob and the rating
nut, which stand on end (build.py's print plates)."""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import clock_spec as S  # noqa: E402
import cycloidal as C  # noqa: E402
from _lib import arc_through, check, circle, line  # noqa: E402

L = S.lines


def plane_z(z):
    return {"origin": [0.0, 0.0, z], "normal": [0, 0, 1], "uAxis": [1, 0, 0]}


def plane_xz():
    """The XZ plane: u = x, v = z (normal -Y, so V is +Z)."""
    return {"origin": [0.0, 0.0, 0.0], "normal": [0, -1, 0],
            "uAxis": [1, 0, 0]}


def polar(r, a_deg, c=(0.0, 0.0)):
    a = math.radians(a_deg)
    return (c[0] + r * math.cos(a), c[1] + r * math.sin(a))


def polygon(pts):
    return [line(pts[i], pts[(i + 1) % len(pts)]) for i in range(len(pts))]


def arc_pts(c, r, a0_deg, a1_deg):
    """An arc entity from angle a0 to a1 about c (either sense)."""
    mid = (a0_deg + a1_deg) / 2.0
    return arc_through(polar(r, a0_deg, c), polar(r, mid, c),
                       polar(r, a1_deg, c))


def moved(ents, dx=0.0, dy=0.0, rot_deg=0.0):
    """Sketch entities turned about the origin, then moved."""
    c, s = math.cos(math.radians(rot_deg)), math.sin(math.radians(rot_deg))

    def p(v):
        return [c * v[0] - s * v[1] + dx, s * v[0] + c * v[1] + dy]
    out = []
    for e in ents:
        e = dict(e)
        for k in ("a", "b", "center"):
            if k in e:
                e[k] = p(e[k])
        if e["type"] == "arc":
            e["start_angle"] += math.radians(rot_deg)
            e["end_angle"] += math.radians(rot_deg)
        out.append(e)
    return out


class Builder:
    """Features into the active part (generators/counter/build.py's)."""

    def __init__(self, d):
        self.d = d

    def sketch(self, name, plane, ents):
        return check(self.d.call("add_feature", feature={
            "type": "Sketch", "name": name, "plane": plane,
            "entities": ents}), name)["id"]

    def extrude(self, name, plane, ents, distance, mode="new_body", **kw):
        sk = self.sketch(f"{name} sketch", plane, ents)
        return check(self.d.call("add_feature", feature={
            "type": "Extrude", "name": name, "sketch_id": sk,
            "distance": distance, "mode": mode, **kw}), name)["id"]

    def prism(self, name, ents, z0, z1, mode="new_body"):
        return self.extrude(name, plane_z(z0), ents, z1 - z0, mode)

    def revolve(self, name, profile, axis=None):
        """A solid of revolution about Z: `profile` is (r, z), r >= 0."""
        sk = self.sketch(f"{name} profile", plane_xz(),
                         polygon([(r, z) for r, z in profile]))
        return check(self.d.call("add_feature", feature={
            "type": "Revolve", "name": name, "sketch_id": sk,
            "axis": axis or {"kind": "z"}, "angle_deg": 360.0,
            "mode": "new_body"}), name)["id"]

    def boolean(self, name, target, tool, op):
        return check(self.d.call("add_feature", feature={
            "type": "Boolean", "name": name, "target_id": target,
            "tool_id": tool, "op": op}), name)["id"]

    def union(self, name, ids):
        out = ids[0]
        for i, t in enumerate(ids[1:]):
            out = self.boolean(f"{name} +{i + 1}", out, t, "union")
        return out

    def frame(self, name, origin, z=(0, 0, 1), x=(1, 0, 0)):
        check(self.d.call("add_feature", feature={
            "type": "Frame", "name": name, "kind": "explicit",
            "origin": list(origin), "z_axis": list(z), "x_axis": list(x)}),
            name)


def new_part(d, name, proc, material="pla"):
    d.call("add_part", name=name)
    if proc is not None:
        d.call("set_print_process", **proc)
    return Builder(d)


def finish(d, body, material):
    """The material on the active part's one body, by the feature that
    CREATED it: set_material on any later feature (a cut on it) answers ok
    and sets nothing."""
    bodies = d.call("list_elements")["bodies"]
    if len(bodies) != 1:
        raise SystemExit(f"clock: a part of {len(bodies)} bodies")
    d.call("set_material", feature_id=bodies[0]["created_by"],
           material=material)
    return body


def thread_on(d, part_name, radius, name, th, where=None):
    """A modelled 45-degree thread on every cylinder face of `radius` (and
    `where`, a test on the face): the jar's threads."""
    part = d.part(part_name, create=False)
    faces = [f for f in part.body().faces(kind="cylinder")
             if abs(f.radius - radius) < 1e-6 and (where is None or where(f))]
    if not faces:
        raise SystemExit(f"clock: no Ø{2 * radius:.3f} face on {part_name} "
                         f"to thread")
    out = []
    for k, f in enumerate(faces):
        t = part.thread(f, f"{name} {k + 1}" if len(faces) > 1 else name,
                        modeled=True, pitch=th["pitch"],
                        flank_angle_deg=th["flank_deg"])
        out.append(t.id)
        part = d.part(part_name, create=False)
        faces = [g for g in part.body().faces(kind="cylinder")
                 if abs(g.radius - radius) < 1e-6 and
                 (where is None or where(g))]
    return out


# --- shared shapes -------------------------------------------------------------

def d_bore(r, flat_from_r):
    """A D-shaped bore: the circle of radius r cut by the chord
    `flat_from_r` from the centre (on +X)."""
    a = math.degrees(math.acos(flat_from_r / r))
    p0, p1 = polar(r, -a), polar(r, a)
    return [arc_through(p1, polar(r, 180.0), p0), line(p0, p1)]


def press_d_bore(s):
    """The bore a part is pressed on an arbor's D by: half the hole
    allowance over the arbor all round, the flat likewise."""
    over = (s["press_d"] - s["arbor_d"]) / 2.0
    return d_bore(s["press_d"] / 2.0, s["arbor_d"] / 2.0 - s["flat"] + over)


def crossings(b, s, name, z0, z1, r_in, r_out, n=5, spoke=None):
    """The windows between a wheel's spokes, cut through: n annular
    sectors, `spoke` wide at the hub."""
    if r_out - r_in < L(6):
        return None
    spoke = spoke or L(6)
    ents = []
    for k in range(n):
        a0 = 360.0 * k / n
        a1 = 360.0 * (k + 1) / n
        di = math.degrees(spoke / 2.0 / r_in) + math.degrees(
            spoke / 2.0 / r_in)
        do = math.degrees(spoke / r_out)
        i0, i1 = a0 + di / 2 + 360.0 / n * 0, a1 - di / 2
        o0, o1 = a0 + do / 2, a1 - do / 2
        pa, pb = polar(r_in, i0), polar(r_in, i1)
        qa, qb = polar(r_out, o0), polar(r_out, o1)
        ents += [line(pa, qa), arc_pts((0, 0), r_out, o0, o1), line(qb, pb),
                 arc_pts((0, 0), r_in, i1, i0)]
    return b.prism(f"{name} crossings", ents, z0, z1, mode="cut")


def hub_tube(b, s, name, z0, z1):
    return b.prism(name, [circle((0, 0), s["hub_r"])], z0, z1)


# --- the escape wheel -------------------------------------------------------------

def escape_wheel_ents(e):
    """The escape wheel's toothed outline (clock_spec.wheel_outline), with
    each tip land a true arc."""
    R, h = e["R"], e["tooth_h"]
    pitch = e["pitch_deg"]
    tau = math.degrees(e["tip_mm"] / R)
    eps = math.degrees(math.atan(h * math.tan(math.radians(
        S.FRONT_UNDERCUT_DEG)) / (R - h)))
    _, theta0 = S.wheel_outline(e)
    t0 = math.degrees(theta0)
    ents = []
    for k in range(S.Z_ESCAPE):
        th = t0 + pitch * k
        root = polar(R - h, th + eps)
        lead = polar(R, th)
        trail = polar(R, th + tau)
        nxt = polar(R - h, th + pitch + eps)
        ents += [line(root, lead), arc_pts((0, 0), R, th, th + tau),
                 line(trail, nxt)]
    return ents


def build_escape_wheel(d, s, proc, name="escape wheel"):
    """Wheel, pinion and the hub tube to the front plate, one body: the
    wheel on the bed, the pinion standing on it, the tube on that."""
    b = new_part(d, name, proc)
    e, z = s["escape"], s["z"]
    m = s["module"]
    wheel = b.prism(f"{name} teeth", escape_wheel_ents(e), *z["escape"])
    pin = b.prism(f"{name} pinion", C.outline(m, s["gears"]["escape_pinion"]),
                  *z["escape_pinion"])
    tube = hub_tube(b, s, f"{name} tube", z["escape_pinion"][1],
                    s["H"] - s["end_shake"])
    body = b.union(name, [wheel, pin, tube])
    tip_p = s["tip_r"]["escape_pinion"]
    crossings(b, s, name, *z["escape"], tip_p + L(4),
              e["R"] - e["tooth_h"] - L(6), n=5)
    body = b.prism(f"{name} bore", press_d_bore(s), z["escape"][0],
                   s["H"], mode="cut")
    return finish(d, body, "pla")


# --- the anchor -----------------------------------------------------------------

def pallet_ents(e, side, deeper_deg=0.0):
    """One arm and pallet (clock_spec.pallet_outline) with its two faces
    about the pivot as true arcs: a locking face that is an arc about the
    pivot is what makes the escapement dead-beat."""
    R, w = e["R"], e["pallet_w"]
    rho1, rho2 = R - w / 2.0, R + w / 2.0
    base, sgn = (225.0, 1.0) if side == "left" else (315.0, -1.0)
    d0 = e["delta_deg"] + deeper_deg
    lift, kappa = e["lift_deg"], e["kappa_deg"]
    kw = math.degrees(e["arm_w"] / rho2)
    r_h = e["hub_r"]

    def ang(psi):
        return base + sgn * psi

    def P(r, psi):
        return polar(r, ang(psi))
    if side == "left":
        c1, c2 = (rho1, d0 + lift), (rho2, d0)      # deep on rho1, lock rho2
    else:
        c1, c2 = (rho1, d0), (rho2, d0 + lift)      # lock on rho1, deep rho2
    h1, a1 = P(r_h, -kappa), P(rho1, -kappa)
    a2, h2 = P(rho2, -kappa - kw), P(r_h, -kappa - kw)
    return [line(h1, a1),
            arc_pts((0, 0), rho1, ang(-kappa), ang(c1[1])),
            line(P(*c1), P(*c2)),
            arc_pts((0, 0), rho2, ang(c2[1]), ang(-kappa - kw)),
            line(a2, h2), line(h2, h1)]


def build_anchor(d, s, proc, name="anchor", deeper_deg=0.0):
    b = new_part(d, name, proc)
    e, z = s["escape"], s["z"]
    ids = [b.prism(f"{name} {side}", pallet_ents(e, side, deeper_deg),
                   *z["anchor"]) for side in ("left", "right")]
    ids.append(b.prism(f"{name} hub", [circle((0, 0), s["hub_r"])],
                       *z["anchor"]))
    ids.append(hub_tube(b, s, f"{name} tube", z["anchor"][1],
                        s["H"] - s["end_shake"]))
    body = b.union(name, ids)
    body = b.prism(f"{name} bore", press_d_bore(s), z["anchor"][0], s["H"],
                   mode="cut")
    return finish(d, body, "pla")


# --- the train wheels --------------------------------------------------------------

def build_wheel(d, s, proc, name, wheel_key, pinion_key, z_wheel, z_pinion,
                tube_to=None, spokes=5):
    """A wheel and its pinion (and a hub tube on to `tube_to`), one body."""
    b = new_part(d, name, proc)
    m = s["module"]
    gs = s["gears"]
    ids = [b.prism(f"{name} teeth", C.outline(m, gs[wheel_key]), *z_wheel)]
    top = z_wheel[1]
    if pinion_key:
        ids.append(b.prism(f"{name} pinion", C.outline(m, gs[pinion_key]),
                           *z_pinion))
        top = z_pinion[1]
    if tube_to is not None and tube_to > top + 1e-9:
        ids.append(hub_tube(b, s, f"{name} tube", top, tube_to))
    body = b.union(name, ids)
    inner = (s["tip_r"][pinion_key] if pinion_key else s["hub_r"]) + L(4)
    root = gs[wheel_key]["rp"] - gs[wheel_key]["h_f"]
    crossings(b, s, name, *z_wheel, inner, root - L(6), n=spokes)
    return b, body


def build_third(d, s, proc):
    z = s["z"]
    b, body = build_wheel(d, s, proc, "third wheel", "third", "third_pinion",
                          z["third"], z["third_pinion"],
                          tube_to=s["H"] - s["end_shake"])
    body = b.prism("third wheel bore", press_d_bore(s), z["third"][0],
                   s["H"], mode="cut")
    return finish(d, body, "pla")


def ratchet_ents(s, phase_deg=0.0):
    """The drum's ratchet, N_RATCHET teeth: each a radial steep face facing
    clockwise (it drives the pawls clockwise, the way the weight turns the
    drum), and a slope down from its tip anticlockwise to the next root."""
    dr = s["drum"]
    rt, rr = dr["ratchet_tip_r"], dr["ratchet_root_r"]
    step = 360.0 / S.N_RATCHET
    ents = []
    for i in range(S.N_RATCHET):
        a = phase_deg + i * step
        ents += [line(polar(rr, a), polar(rt, a)),
                 line(polar(rt, a), polar(rr, a + step))]
    return ents


def pawl_shape(s, k):
    """Pawl k: a tooth in the gap clockwise of a ratchet face, the gap a
    side off that face and off the root, and an arm two lines wide on an
    arc outside the ratchet's tips, rooted 100 degrees ANTICLOCKWISE of the
    tooth so the drive pulls the arm rather than buckling it. Returns its
    outline and the slot (grown outward by the ride over a tooth) it stands
    in."""
    dr, g = s["drum"], s["g"]
    rt, rr, h = dr["ratchet_tip_r"], dr["ratchet_root_r"], dr["ratchet_h"]
    step = 360.0 / S.N_RATCHET
    a_face = k * 360.0 / S.N_PAWLS
    off = math.degrees(g / rr)
    r_ai = rt + g
    r_ao = r_ai + L(2)
    a_cw = a_face - 0.6 * step
    a_root = a_face + 100.0
    pts = [polar(rr + 1.5 * g, a_face - off),
           polar(rr + 0.6 * h + 3.0 * g, a_cw),
           polar(r_ao, a_cw)]
    pts += [polar(r_ao, a_cw + (a_root + 4.0 - a_cw) * i / 16)
            for i in range(1, 17)]
    pts += [polar(r_ai, a_root + 4.0)]
    pts += [polar(r_ai, a_root - (a_root - (a_face - off)) * i / 48)
            for i in range(0, 49)]
    ride = h + 2.0 * g
    si, so = rr + g - L(2), r_ao + ride
    a0, a1 = a_cw - math.degrees(L(2) / r_ao), a_root
    slot = ([polar(si, a0 + (a1 - a0) * i / 16) for i in range(17)] +
            [polar(so, a1 - (a1 - a0) * i / 16) for i in range(17)])
    return pts, slot, so


def build_centre(d, s, proc):
    """The centre wheel and its two flexure pawls, one body: each pawl a
    wall from the bed up to the top of the drum's ratchet, standing in a
    slot cut through the wheel round it, joined to the wheel at its root
    only."""
    z, dr = s["z"], s["drum"]
    b = new_part(d, "centre wheel", proc)
    m, gs = s["module"], s["gears"]
    zw = z["centre"]
    wheel = b.prism("centre wheel teeth", C.outline(m, gs["centre"]), *zw)
    shapes = [pawl_shape(s, k) for k in range(S.N_PAWLS)]
    for k, (_, slot, _) in enumerate(shapes):
        b.prism(f"centre wheel slot {k + 1}", polygon(slot), *zw, mode="cut")
    ids = [wheel] + [b.prism(f"centre wheel pawl {k + 1}", polygon(pts),
                            zw[0], dr["z0"] + dr["t_flange"])
                    for k, (pts, _, _) in enumerate(shapes)]
    body = b.union("centre wheel", ids)
    root = gs["centre"]["rp"] - gs["centre"]["h_f"]
    crossings(b, s, "centre wheel", *zw, shapes[0][2] + L(6), root - L(6),
              n=6)
    body = b.prism("centre wheel bore", press_d_bore(s), zw[0], zw[1],
                   mode="cut")
    return finish(d, body, "pla")


def build_drum(d, s, proc, name="drum", stage1=False):
    """The cord drum. Stage 2: on the centre arbor at the gap, its back
    flange the ratchet the pawls drive, a sleeve forward to the cord's
    45-degree flanges. Stage 1: on the escape arbor in front of the front
    plate, pressed on its D, with no ratchet."""
    dr = s["drum"]
    b = new_part(d, name, proc)
    rc, cone, core = dr["r_core"], dr["cone"], dr["core"]
    rt = dr["ratchet_tip_r"]
    if stage1:
        z0 = s["H"] + s["t_plate"] + s["end_shake"]
        prof = [(0.0, z0), (rt, z0), (rt, z0 + dr["t_flange"]),
                (rc, z0 + dr["t_flange"]),
                (rc, z0 + dr["t_flange"] + core),
                (rt, z0 + dr["t_flange"] + core + cone),
                (0.0, z0 + dr["t_flange"] + core + cone)]
        body = b.revolve(name, prof)
        z1 = prof[-1][1]
        body = b.prism(f"{name} bore", press_d_bore(s), z0, z1, mode="cut")
        tie_r = rc + s["cord_mm"] / 2.0 + L(1)
        body = b.prism(f"{name} tie", [circle((tie_r, 0.0),
                                              s["cord_mm"] / 2.0 + L(1))],
                       z0, z0 + dr["t_flange"], mode="cut")
        s.setdefault("drum1", {})["z"] = (z0, z1)
        return finish(d, body, "pla")
    z0, z1 = dr["z0"], dr["z1"]
    zf = z0 + dr["t_flange"]
    prof = [(0.0, z0), (dr["ratchet_root_r"] - L(1), z0),
            (dr["ratchet_root_r"] - L(1), zf), (dr["sleeve_r"], zf),
            (dr["sleeve_r"], dr["z_cone_rear"]), (rt, dr["z_core0"]),
            (rc, dr["z_core0"]), (rc, dr["z_cone0"]),
            (rt, dr["z_cone0"] + cone), (0.0, dr["z_cone0"] + cone)]
    body = b.revolve(name, prof)
    teeth = b.prism(f"{name} ratchet", ratchet_ents(s), z0, zf)
    body = b.boolean(f"{name} + ratchet", body, teeth, "union")
    body = b.prism(f"{name} bore", [circle((0, 0), s["run_d"] / 2.0
                                          - s["seat_step"])],
                   z0, z1, mode="cut")
    # The cord's end is tied through a hole in the rear flange.
    tie_r = rc + s["cord_mm"] / 2.0 + L(1)
    body = b.prism(f"{name} tie", [circle((tie_r, 0.0),
                                          s["cord_mm"] / 2.0 + L(1))],
                   dr["z_cone_rear"], dr["z_core0"], mode="cut")
    return finish(d, body, "pla")


# --- arbors and spacers --------------------------------------------------------------

def build_arbor(d, s, proc, name, z0, z1, d_ranges, seat=None):
    """A printed arbor standing on its back end: a rod of arbor_d with a D
    flat over each of `d_ranges` (z0, z1), each D's upper end a 45-degree
    ramp so it faces nothing down. `seat` (z0, z1) is a band a quarter
    line smaller, back to full size up a 45-degree cone: what something
    turns on there (the drum) runs at the gap on it, and it is a face of
    its own for a study to load."""
    b = new_part(d, name, proc)
    r = s["arbor_d"] / 2.0
    prof = [(0.0, z0), (r, z0)]
    if seat:
        dr = s["seat_step"]
        prof += [(r, seat[0]), (r - dr, seat[0]), (r - dr, seat[1]),
                 (r, seat[1] + dr)]
    body = b.revolve(name, prof + [(r, z1), (0.0, z1)])
    f = s["flat"]
    for k, (a, c) in enumerate(d_ranges):
        x0 = r - f
        ramp = f + 1.0
        pts = [(x0, a), (r + 1.0, a), (r + 1.0, c + ramp), (x0, c)]
        sk = {"origin": [0.0, r + 1.0, 0.0], "normal": [0, -1, 0],
              "uAxis": [1, 0, 0]}
        body = check(d.call("add_feature", feature={
            "type": "Extrude", "name": f"{name} flat {k + 1}",
            "sketch_id": b.sketch(f"{name} flat {k + 1} sketch", sk,
                                  polygon(pts)),
            "distance": 2.0 * (r + 1.0), "mode": "cut"}), name)["id"]
    return finish(d, body, "pla")


def build_spacer(d, s, proc, name, z0, z1):
    b = new_part(d, name, proc)
    body = b.prism(name, [circle((0, 0), s["hub_r"]),
                          circle((0, 0), s["run_d"] / 2.0)], z0, z1)
    return finish(d, body, "pla")


# --- the frame ---------------------------------------------------------------------

def hull(points):
    pts = sorted(set(points))
    if len(pts) <= 2:
        return pts

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    lo, hi = [], []
    for p in pts:
        while len(lo) >= 2 and cross(lo[-2], lo[-1], p) <= 0:
            lo.pop()
        lo.append(p)
    for p in reversed(pts):
        while len(hi) >= 2 and cross(hi[-2], hi[-1], p) <= 0:
            hi.pop()
        hi.append(p)
    return lo[:-1] + hi[:-1]


def plate_outline(circles, n=24):
    """The convex hull of circles (x, y, r), as points."""
    pts = []
    for x, y, r in circles:
        for i in range(n):
            pts.append((round(x + r * math.cos(2 * math.pi * i / n), 6),
                        round(y + r * math.sin(2 * math.pi * i / n), 6)))
    return hull(pts)


def build_plate(d, s, proc, name, fr, back):
    """A frame plate: the hull of its pillars' pads and its arbors' bosses,
    a pivot hole per arbor at the gap, a clearance hole per pillar (on the
    back plate countersunk at 45 degrees to seat the pillar's cone), and on
    the back plate a tab over the top with a keyhole to hang it by. The
    front plate carries the minute wheel's stud on its outer face."""
    b = new_part(d, name, proc)
    t = s["t_plate"]
    z0, z1 = (-t, 0.0) if back else (s["H"], s["H"] + t)
    pt = s["pillar"]
    circles = [(x, y, pt["pad_r"]) for x, y in fr["pillars"]]
    circles += [(x, y, s["hub_r"] + L(6)) for x, y in fr["axes"].values()]
    if back:
        circles += fr.get("tab", [])
    outline = plate_outline(circles)
    plate = body = b.prism(name, polygon(outline), z0, z1)
    holes = [circle(xy, s["run_d"] / 2.0) for xy in fr["axes"].values()]
    holes += [circle(xy, pt["hole_d"] / 2.0) for xy in fr["pillars"]]
    body = b.prism(f"{name} holes", holes, z0, z1, mode="cut")
    if back:
        # The countersink: a 45-degree cone from the shoulder's radius at
        # the inner face down to the hole.
        for k, (x, y) in enumerate(fr["pillars"]):
            sink = pt["sink_r"] - pt["hole_d"] / 2.0
            prof = [(pt["hole_d"] / 2.0 - 0.5, -sink), (pt["hole_d"] / 2.0,
                                                        -sink),
                    (pt["sink_r"], 0.0), (pt["sink_r"], 0.5),
                    (pt["hole_d"] / 2.0 - 0.5, 0.5)]
            sk = b.sketch(f"{name} sink {k + 1} profile", {
                "origin": [x, y, 0.0], "normal": [0, -1, 0],
                "uAxis": [1, 0, 0]}, polygon(prof))
            body = check(d.call("add_feature", feature={
                "type": "Revolve", "name": f"{name} sink {k + 1}",
                "sketch_id": sk, "axis": {"kind": "custom",
                                          "origin": [x, y, 0.0],
                                          "direction": [0, 0, 1]},
                "angle_deg": 360.0, "mode": "cut"}), "sink")["id"]
        if fr.get("keyhole"):
            x, y, r_big, r_small, run = fr["keyhole"]
            ents = [circle((x, y), r_big)]
            body = b.prism(f"{name} keyhole", ents, z0, z1, mode="cut")
            slot = [(x - r_small, y), (x + r_small, y),
                    (x + r_small, y + run), (x - r_small, y + run)]
            body = b.prism(f"{name} keyhole slot", polygon(slot) , z0, z1,
                           mode="cut")
            body = b.prism(f"{name} keyhole top", [circle((x, y + run),
                                                          r_small)],
                           z0, z1, mode="cut")
    else:
        mw = fr.get("minute_wheel")
        if mw is not None:
            stud = b.prism(f"{name} stud", [circle(mw, s["stud_d"] / 2.0)],
                           z1 - 0.5, fr["stud_top"])
            body = b.boolean(f"{name} + stud", plate, stud, "union")
    for k, (axis, xy) in enumerate(fr["axes"].items()):
        b.frame(axis, (xy[0], xy[1], 0.0))
    if not back and fr.get("minute_wheel") is not None:
        mw = fr["minute_wheel"]
        b.frame("minute wheel", (mw[0], mw[1], 0.0))
    if back:
        b.frame("beat", (fr["beat"][0], fr["beat"][1], -t - 20.0))
    return finish(d, body, "pla")


def build_pillar(d, s, proc):
    """A pillar standing on its rear end: the standoff to the wall, the
    back plate's thread (its nut clamps the plate on the pillar's 45-degree
    cone, seated in the plate's countersink), the shoulder between the
    plates, and the front thread that carries the front plate, the dial's
    spacer and ring, and its nut."""
    pt = s["pillar"]
    th = s["pillar_thread"]
    b = new_part(d, "pillar", proc)
    r_ext = th["ext_d"] / 2.0
    r_root = r_ext - th["depth"]
    r_so = r_root - L(2)
    r_sh = pt["shoulder_r"]
    z_w, H = pt["z_wall"], s["H"]
    zt0, zt1 = pt["rear_thread"]
    zf0, zf1 = pt["front_thread"]
    # The reliefs either side of the shoulder stand a line inside the
    # thread's root, so the groove's cut runs out into air rather than
    # along a surface it shares.
    r_rel = r_root - L(1)
    cone = r_sh - r_rel
    prof = [(0.0, z_w), (r_so, z_w), (r_so, zt0 - (r_ext - r_so)),
            (r_ext, zt0), (r_ext, zt1), (r_rel, zt1), (r_rel, -cone),
            (r_sh, 0.0), (r_sh, H), (r_rel, H), (r_rel, zf0 - (r_ext -
                                                              r_rel)),
            (r_ext, zf0), (r_ext, zf1 - 0.5), (r_ext - 0.5, zf1),
            (0.0, zf1)]
    body = b.revolve("pillar", prof)
    finish(d, body, "pla")
    thread_on(d, "pillar", r_ext, "pillar thread", th)
    return body


def knurl(r, n=24, depth=0.08):
    return polygon([polar(r if i % 2 == 0 else (1.0 - depth) * r,
                          360.0 * i / (2 * n)) for i in range(2 * n)])


def build_nut(d, s, proc, name="pillar nut", th_key="pillar_thread",
              od=None, h=None, notches=0):
    th = s[th_key]
    b = new_part(d, name, proc)
    od = od or 2.0 * th["major"]
    h = h or S.up_to(0.8 * th["major"], s["layer"])
    body = b.prism(name, knurl(od / 2.0) + [circle((0, 0),
                                                   th["nut_bore_d"] / 2.0)],
                   0.0, h)
    for k in range(notches):
        a = 360.0 * k / notches
        w = S.lines(2)
        p = [polar(od / 2.0 * 0.80, a - math.degrees(w / od)),
             polar(od / 2.0 + 1.0, a - math.degrees(w / od)),
             polar(od / 2.0 + 1.0, a + math.degrees(w / od)),
             polar(od / 2.0 * 0.80, a + math.degrees(w / od))]
        body = b.prism(f"{name} notch {k + 1}", polygon(p), h - 2 * s["layer"],
                       h, mode="cut")
    finish(d, body, "pla")
    thread_on(d, name, th["nut_bore_d"] / 2.0, f"{name} thread", th)
    return body, h


# --- in front of the front plate ------------------------------------------------------

def build_cannon(d, s, proc):
    """The cannon pinion: on the centre arbor's round end by a light press,
    which is the clock's setting clutch (turn the minute hand and it slips),
    its pipe D-flatted where the minute hand is pressed on."""
    z = s["z"]
    b = new_part(d, "cannon pinion", proc)
    m = s["module"]
    pin = b.prism("cannon pinion teeth", C.outline(m, s["gears"]["cannon"]),
                  *z["cannon"])
    pipe = b.prism("cannon pipe", [circle((0, 0), s["cannon_pipe_d"] / 2.0)],
                   z["cannon"][1], z["cannon_pipe"][1])
    body = b.union("cannon pinion", [pin, pipe])
    body = b.prism("cannon bore", [circle((0, 0), s["press_d"] / 2.0)],
                   z["cannon"][0], z["cannon_pipe"][1], mode="cut")
    # The D on the pipe's outside, from just below the minute hand to the
    # top: the flat runs out through the top face.
    r = s["cannon_pipe_d"] / 2.0
    f = s["flat"]
    zd = z["minute_hand"][0] - s["end_shake"]
    body = b.prism("cannon pipe flat", polygon([
        (r - f, -r - 1), (r + 1, -r - 1), (r + 1, r + 1), (r - f, r + 1)]),
        zd, z["cannon_pipe"][1], mode="cut")
    return finish(d, body, "pla")


def build_minute_wheel(d, s, proc):
    z = s["z"]
    b, body = build_wheel(d, s, proc, "minute wheel", "minute_wheel",
                          "minute_pinion", z["minute_wheel"],
                          z["minute_pinion"], spokes=4)
    body = b.prism("minute wheel bore", [circle((0, 0),
                                               s["mw_bore_d"] / 2.0)],
                   z["minute_wheel"][0], z["minute_pinion"][1], mode="cut")
    return finish(d, body, "pla")


def build_hour_wheel(d, s, proc):
    z = s["z"]
    b = new_part(d, "hour wheel", proc)
    m = s["module"]
    wheel = b.prism("hour wheel teeth", C.outline(m, s["gears"]["hour"]),
                    *z["hour"])
    pipe = b.prism("hour pipe", [circle((0, 0), s["hour_pipe_d"] / 2.0)],
                   z["hour"][1], z["hour_pipe"][1])
    body = b.union("hour wheel", [wheel, pipe])
    root = s["gears"]["hour"]["rp"] - s["gears"]["hour"]["h_f"]
    crossings(b, s, "hour wheel", *z["hour"], s["hour_pipe_d"] / 2.0 + L(4),
              root - L(6), n=4)
    body = b.prism("hour wheel bore", [circle((0, 0),
                                             s["hour_bore_d"] / 2.0)],
                   z["hour"][0], z["hour_pipe"][1], mode="cut")
    return finish(d, body, "pla")


def hand_outline(length, hub_r, tail):
    """A tapered hand pointing up (+Y) at rotation 0, which is 12 o'clock:
    a counterweight tail behind the hub, the blade two lines at its tip."""
    w_tip = L(3)
    pts = [(-hub_r, -tail), (hub_r, -tail), (hub_r, 0.0),
           (w_tip / 2.0, length), (-w_tip / 2.0, length), (-hub_r, 0.0)]
    return polygon(pts)


def build_hand(d, s, proc, name, length, z_key, pipe_d, d_flat):
    z0, z1 = s["z"][z_key]
    b = new_part(d, name, proc)
    hub_r = pipe_d / 2.0 + L(S.WALL_LINES)
    blade = b.prism(f"{name} blade", hand_outline(length, hub_r,
                                                  0.25 * length), z0, z1)
    hub = b.prism(f"{name} hub", [circle((0, 0), hub_r)], z0,
                  z0 + s["hand_boss"])
    body = b.union(name, [blade, hub])
    bore_d = pipe_d + s["press_d"] - s["arbor_d"]
    if d_flat:
        over = (bore_d - pipe_d) / 2.0
        ents = d_bore(bore_d / 2.0, pipe_d / 2.0 - s["flat"] + over)
    else:
        ents = [circle((0, 0), bore_d / 2.0)]
    body = b.prism(f"{name} bore", ents, z0, z0 + s["hand_boss"], mode="cut")
    return finish(d, body, "pla")


def build_dial_ring(d, s, proc):
    """The chapter ring: from the minute wheel's centre (so it keeps the
    minute wheel on its stud) out to the hands' reach, on two lugs at the
    lower pillars, an hour tick every 30 degrees and a minute tick every 6
    engraved three layers deep."""
    rg = s["ring"]
    z0, z1 = s["z"]["ring"]
    b = new_part(d, "dial ring", proc)
    ring = b.prism("dial ring", [circle((0, 0), rg["r_out"]),
                                 circle((0, 0), rg["r_in"])], z0, z1)
    ids = [ring]
    for k, (x, y) in enumerate(rg["pillars"]):
        a = math.degrees(math.atan2(y, x))
        da = math.degrees(rg["lug_r"] / rg["r_out"])
        pts = plate_outline([(x, y, rg["lug_r"])]) + [
            polar(rg["r_out"] - 2.0, a - da), polar(rg["r_out"] - 2.0,
                                                    a + da)]
        ids.append(b.prism(f"dial lug {k + 1}", polygon(hull(pts)), z0, z1))
    body = b.union("dial ring", ids)
    holes = [circle(xy, s["pillar"]["hole_d"] / 2.0) for xy in rg["pillars"]]
    body = b.prism("dial ring holes", holes, z0, z1, mode="cut")
    depth = 3 * s["layer"]
    r1 = rg["r_out"] - L(3)
    ticks = []
    for k in range(60):
        hour = k % 5 == 0
        w = L(4) if hour else L(2)
        ln = (0.12 if hour else 0.05) * rg["r_out"]
        a = 90.0 - 6.0 * k
        c, sn = math.cos(math.radians(a)), math.sin(math.radians(a))
        ux, uy = -sn, c
        p0 = (c * (r1 - ln), sn * (r1 - ln))
        p1 = (c * r1, sn * r1)
        ticks.append(polygon([
            (p0[0] - ux * w / 2, p0[1] - uy * w / 2),
            (p1[0] - ux * w / 2, p1[1] - uy * w / 2),
            (p1[0] + ux * w / 2, p1[1] + uy * w / 2),
            (p0[0] + ux * w / 2, p0[1] + uy * w / 2)]))
    body = b.prism("dial ticks", [e for t in ticks for e in t], z1 - depth,
                   z1, mode="cut")
    return finish(d, body, "pla")


def build_dial_spacer(d, s, proc):
    b = new_part(d, "dial spacer", proc)
    pt = s["pillar"]
    body = b.prism("dial spacer", [circle((0, 0), pt["shoulder_r"]),
                                   circle((0, 0), pt["hole_d"] / 2.0)],
                   s["F"], s["z"]["ring"][0])
    return finish(d, body, "pla")


# --- the pendulum -------------------------------------------------------------------

def rod_z(s):
    return s["rod_z"]


def plane_y(y):
    """A plane at height y facing up the rod: u = x, v = -z."""
    return {"origin": [0.0, y, 0.0], "normal": [0, 1, 0], "uAxis": [1, 0, 0]}


def build_rod(d, s, proc, length, name="pendulum rod"):
    """The rod, hanging from the anchor arbor (the origin) to `length` below
    it: a flat bar printed on its back face, round at the top round the D it
    is pressed on by, and its lower end a 45-degree thread D-flatted on that
    face (pendulum_shapes)."""
    rod = s["rod"]
    th = s["rating_thread"]
    z0, z1 = rod_z(s)
    w = rod["w"]
    pend = s["pend"]
    y_t0 = -length + pend["thread_len"]
    b = new_part(d, name, proc)
    bar = b.prism(f"{name} bar", [
        line((-w / 2, y_t0), (w / 2, y_t0)), line((w / 2, y_t0), (w / 2, 0.0)),
        arc_through((w / 2, 0.0), (0.0, w / 2), (-w / 2, 0.0)),
        line((-w / 2, 0.0), (-w / 2, y_t0))], z0, z1)
    za = z0 + rod["thread_axis_z"]
    r = rod["thread_r"]
    sk = b.sketch(f"{name} thread profile", {
        "origin": [0.0, 0.0, 0.0], "normal": [1, 0, 0], "uAxis": [0, 1, 0]},
        polygon([(-length, za), (y_t0, za), (y_t0, za + r),
                 (-length, za + r)]))
    cyl = check(d.call("add_feature", feature={
        "type": "Revolve", "name": f"{name} thread end", "sketch_id": sk,
        "axis": {"kind": "custom", "origin": [0.0, 0.0, za],
                 "direction": [0, 1, 0]}, "angle_deg": 360.0,
        "mode": "new_body"}), name)["id"]
    body = b.boolean(name, bar, cyl, "union")
    body = b.prism(f"{name} bore", press_d_bore(s), z0, z1, mode="cut")
    finish(d, body, "pla")
    thread_on(d, name, r, "rating thread", th)
    b.frame("thread", (0.0, S.nut_y(s, length), za), z=(0, 1, 0),
            x=(1, 0, 0))
    # The D-flat: nothing below the bar's back face.
    body = b.prism(f"{name} flat", polygon([
        (-r - 1, -length - 1), (r + 1, -length - 1), (r + 1, y_t0 + 1),
        (-r - 1, y_t0 + 1)]), z0 - r - 1.0, z0, mode="cut")
    return body


def build_bob(d, s, proc, y0, name="bob"):
    """The bob: a block with rounded ends, the bar through it, standing on
    the rating nut at y0."""
    pend = s["pend"]
    W, D, Hb = pend["bob_w"], s["bob_depth"], pend["bob_h"]
    zc = s["z_rod_mid"]          # the bob's middle, behind the rod's
    b = new_part(d, name, proc)
    r = D / 2.0
    vc = -zc
    ents = [line((-W / 2 + r, vc - r), (W / 2 - r, vc - r)),
            arc_through((W / 2 - r, vc - r), (W / 2, vc), (W / 2 - r, vc + r)),
            line((W / 2 - r, vc + r), (-W / 2 + r, vc + r)),
            arc_through((-W / 2 + r, vc + r), (-W / 2, vc),
                        (-W / 2 + r, vc - r))]
    body = b.extrude(name, plane_y(y0), ents, Hb)
    sw, st = s["bob_slot"]
    z0, _ = rod_z(s)
    slot = polygon([(-sw / 2, -(z0 - s["g"])), (sw / 2, -(z0 - s["g"])),
                    (sw / 2, -(z0 - s["g"] + st)),
                    (-sw / 2, -(z0 - s["g"] + st))])
    body = b.extrude(f"{name} slot", plane_y(y0), slot, Hb, mode="cut")
    return finish(d, body, "pla")


def build_rating_nut(d, s, proc, y0, name="rating nut"):
    """The rating nut, on the rod's thread at y0 (its lower face): knurled,
    24 notches round its top to count turns by."""
    th = s["rating_thread"]
    nut = s["nut"]
    z0, _ = rod_z(s)
    za = z0 + s["rod"]["thread_axis_z"]
    b = new_part(d, name, proc)
    od, h = nut["d"], nut["h"]
    ents = moved(knurl(od / 2.0), 0.0, -za) + [circle((0.0, -za),
                                                      th["nut_bore_d"] / 2.0)]
    body = b.extrude(name, plane_y(y0), ents, h)
    for k in range(nut["notches"]):
        a = 360.0 * k / nut["notches"]
        w = L(2)
        p = [polar(od / 2.0 * 0.82, a - math.degrees(w / od), (0, -za)),
             polar(od / 2.0 + 1.0, a - math.degrees(w / od), (0, -za)),
             polar(od / 2.0 + 1.0, a + math.degrees(w / od), (0, -za)),
             polar(od / 2.0 * 0.82, a + math.degrees(w / od), (0, -za))]
        body = b.extrude(f"{name} notch {k + 1}", plane_y(y0 + h - 2 *
                                                        s["layer"]),
                         polygon(p), 2 * s["layer"], mode="cut")
    finish(d, body, "pla")
    thread_on(d, name, th["nut_bore_d"] / 2.0, "rating nut thread", th)
    b.frame("thread", (0.0, y0, za), z=(0, 1, 0), x=(1, 0, 0))
    return body


def build_beat(d):
    """The hidden beat: a cube on a revolute that drives the escapement
    (docs/assemblies/relations.md#intermittent). Not printed, not seen."""
    b = new_part(d, "beat", None)
    return b.prism("beat", polygon([(-1, -1), (1, -1), (1, 1), (-1, 1)]),
                   0.0, 2.0)
