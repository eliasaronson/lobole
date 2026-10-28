# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The woodcutter automaton's parts, one function each, every one in the
BOX frame (woodcutter_spec's docstring) where it stands at rest, with a
Frame at each joint. Two parts joined at a joint carry the same frame, so
every mate starts at zero.

The box here is the frame the mechanism needs -- the floor, the two end
walls with the camshaft's bores, and the top with the guide bosses -- as
one body, open at the front and back so the cams can be seen. Phase 3 of
docs/plans/woodcutter-automaton.md splits it into pegged panels.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import woodcutter_spec as S  # noqa: E402
from _lib import check, circle, line  # noqa: E402

X = [1.0, 0.0, 0.0]
Y = [0.0, 1.0, 0.0]
Z = [0.0, 0.0, 1.0]


# --- sketching ------------------------------------------------------------------

def plane_yz(x0):
    """The YZ plane at x0: u = y, v = z (V = X x Y = Z); extrudes go +X."""
    return {"origin": [x0, 0.0, 0.0], "normal": X, "uAxis": Y}


def plane_xy(z0):
    """The XY plane at z0: u = x, v = y; extrudes go +Z."""
    return {"origin": [0.0, 0.0, z0], "normal": Z, "uAxis": X}


def polygon(pts):
    return [line(pts[i], pts[(i + 1) % len(pts)]) for i in range(len(pts))]


def rect(u0, v0, u1, v1):
    return polygon([(u0, v0), (u1, v0), (u1, v1), (u0, v1)])


def stadium(p, q, r):
    """A slot of radius r round the segment p-q."""
    dx, dy = q[0] - p[0], q[1] - p[1]
    n = math.hypot(dx, dy)
    d = (dx / n, dy / n)
    nn = (-d[1], d[0])
    a = math.atan2(nn[1], nn[0])
    return [line((p[0] + r * nn[0], p[1] + r * nn[1]),
                 (q[0] + r * nn[0], q[1] + r * nn[1])),
            {"type": "arc", "center": [q[0], q[1]], "radius": r,
             "start_angle": a - math.pi, "end_angle": a},
            line((q[0] - r * nn[0], q[1] - r * nn[1]),
                 (p[0] - r * nn[0], p[1] - r * nn[1])),
            {"type": "arc", "center": [p[0], p[1]], "radius": r,
             "start_angle": a, "end_angle": a + math.pi}]


def ellipse(c, a, b, n=36):
    return polygon([(c[0] + a * math.cos(2 * math.pi * i / n),
                     c[1] + b * math.sin(2 * math.pi * i / n))
                    for i in range(n)])


def d_shape(c, r, flat_from_centre):
    """A D: a circle of radius r about c cut by a chord flat_from_centre
    below c (the flat on -v)."""
    h = flat_from_centre
    half = math.sqrt(r * r - h * h)
    a0 = math.atan2(-h, half)
    a1 = math.atan2(-h, -half)
    return [{"type": "arc", "center": [c[0], c[1]], "radius": r,
             "start_angle": a0, "end_angle": a1 + 2 * math.pi
             if a1 < a0 else a1},
            line((c[0] - half, c[1] - h), (c[0] + half, c[1] - h))]


class Builder:
    """Features into the active part."""

    def __init__(self, d):
        self.d = d

    def add(self, spec, what):
        return check(self.d.call("add_feature", feature=spec), what)

    def sketch(self, name, plane, ents):
        return self.add({"type": "Sketch", "name": name, "plane": plane,
                         "entities": ents}, name)["id"]

    def extrude(self, name, plane, ents, distance, mode="add"):
        sk = self.sketch(f"{name} sketch", plane, ents)
        return self.add({"type": "Extrude", "name": name, "sketch_id": sk,
                         "distance": distance, "mode": mode}, name)["id"]

    def slab(self, name, x0, x1, ents, mode="add"):
        """A profile in YZ extruded from x0 to x1."""
        return self.extrude(name, plane_yz(x0), ents, x1 - x0, mode)

    def rod_x(self, name, y, z, r, x0, x1, mode="add"):
        """A cylinder along X."""
        return self.slab(name, x0, x1, [circle((y, z), r)], mode)

    def rod_z(self, name, x, y, r, z0, z1, mode="add"):
        """A cylinder along Z."""
        return self.extrude(name, plane_xy(z0), [circle((x, y), r)],
                            z1 - z0, mode)

    def frame(self, name, origin, z_axis, x_axis):
        return self.add({"type": "Frame", "name": name, "kind": "explicit",
                         "origin": [float(v) for v in origin],
                         "z_axis": z_axis, "x_axis": x_axis},
                        f"frame {name}")["id"]


# --- the box ------------------------------------------------------------------------

def joint_frames(s):
    """Every frame two parts share: name -> (origin, z, x)."""
    f = {"shaft": ((0.0, 0.0, s["zc"]), X, Z)}
    for k in ("axe", "bird"):
        x = s[f"x_{k}"]
        lev = s["arm"] if k == "axe" else s["bird"]
        f[f"cam {k}"] = ((x - s["cam_w"] / 2.0, 0.0, s["zc"]), X, Z)
        f[f"guide {k}"] = ((x, 0.0, s["stage"]), Z, Y)
        f[f"rod pin {k}"] = ((x, 0.0, s["rod_pin_z"]), X, Y)
        f[f"lever pin {k}"] = ((x, lev["A0"][0], lev["A0"][1]), X, Y)
        f[f"pivot {k}"] = ((x, lev["P"][0], lev["P"][1]), X, Y)
        body = s[f"{k}_slabs"]["body"]
        f[f"stand {k}"] = (((body[0] + body[1]) / 2.0, lev["P"][0],
                            s["stage"]), Z, X)
    lev = s["axe_slabs"]["lever"]
    f["log"] = (((lev[0] + lev[1]) / 2.0, s["log_y"], s["stage"]), Z, X)
    return f


def frames(b, s, names):
    jf = joint_frames(s)
    for n in names:
        o, z, x = jf[n]
        b.frame(n, o, z, x)


def build_box(b, s):
    W, D, t, H, zc = s["width"], s["depth"], s["wall"], s["H"], s["zc"]
    y0, y1 = -D / 2.0 - t, D / 2.0 + t
    b.extrude("floor", plane_xy(-t), rect(-t, y0, W + t, y1), t, "new_body")
    for name, x0 in (("left wall", -t), ("right wall", W)):
        b.slab(name, x0, x0 + t, rect(y0, -t, y1, H + t))
    b.extrude("top", plane_xy(H), rect(-t, y0, W + t, y1), t)
    b.rod_x("shaft bores", 0.0, zc, s["d_bore"] / 2.0, -t - 1.0,
            W + t + 1.0, "cut")
    boss_z0 = s["stage"] - s["guide_len"]
    for k in ("axe", "bird"):
        x = s[f"x_{k}"]
        b.rod_z(f"{k} guide boss", x, 0.0, s["boss_r"], boss_z0, H + 0.01)
        b.rod_z(f"{k} guide", x, 0.0, s["d_guide"] / 2.0, boss_z0 - 1.0,
                s["stage"] + 1.0, "cut")
    frames(b, s, ["shaft", "guide axe", "guide bird", "stand axe",
                  "stand bird", "log"])


def build_shaft(b, s):
    """The camshaft: through both end walls, a flat between them that the
    cams are keyed to. Its crank end (phase 3) is the -X end."""
    W, t, r = s["width"], s["wall"], s["d_shaft"] / 2.0
    ext = W / 40.0
    b.rod_x("shaft", 0.0, s["zc"], r, -t - ext, W + t + ext, "new_body")
    zf = s["zc"] - r + s["flat"]
    b.slab("flat", s["g"], W - s["g"],
           rect(-r - 1.0, s["zc"] - r - 1.0, r + 1.0, zf), "cut")
    frames(b, s, ["shaft", "cam axe", "cam bird"])


def cam_spec(s, which, base, frame_id, segments=None):
    """The CamProfile feature: the cam cut from its motion, for a rounded
    knife of the rod's radius sliding radially, turned ccw about +X (the
    way the crank turns it)."""
    if segments is None:
        if which == "axe":
            _, segs = S.axe_segments()
        else:
            segs = S.bird_segments()
        segments = S.with_rise(segs, s[f"h_{which}"])
    return {"type": "CamProfile", "name": f"{which} cam",
            "frame": frame_id, "base_radius_mm": base,
            "width_mm": s["cam_w"], "turn": "ccw", "body": "new",
            "segments": segments,
            "follower": {"kind": "knife", "motion": "slide",
                         "radius_mm": s["tip"]}}


def build_cam(b, s, which, base, segments=None):
    """A cam keyed to the shaft: the CamProfile, then a D bore over the
    shaft's flat, the gap a side. Returns the CamProfile's add response."""
    o, z, x = joint_frames(s)[f"cam {which}"]
    fid = b.frame(f"cam {which}", o, z, x)
    r = b.add(cam_spec(s, which, base, fid, segments), f"{which} cam")
    rs = s["d_shaft"] / 2.0
    x0 = o[0]
    b.slab("bore", x0 - 1.0, x0 + s["cam_w"] + 1.0,
           d_shape((0.0, s["zc"]), rs + s["g"], rs - s["flat"] + s["g"]),
           "cut")
    return r


def build_rod(b, s, which):
    """A follower: the foot across the cam (a cylinder of the rod's
    radius: the rounded knife), the rod up through its guide, and the pin
    at its top reaching -X through the link."""
    x, r = s[f"x_{which}"], s["d_rod"] / 2.0
    foot = s[f"foot_{which}"]
    w = s["cam_w"]
    b.rod_x("foot", 0.0, foot, s["tip"], x - w / 2.0, x + w / 2.0,
            "new_body")
    b.rod_z("rod", x, 0.0, r, foot, s["rod_pin_z"] + r)
    link = s[f"{which}_slabs"]["link"]
    b.rod_x("pin", 0.0, s["rod_pin_z"], s["d_pin"] / 2.0, link[0], x)
    frames(b, s, [f"guide {which}", f"rod pin {which}"])


def build_link(b, s, which):
    lev = s["arm"] if which == "axe" else s["bird"]
    x0, x1 = s[f"{which}_slabs"]["link"]
    lo, hi = (0.0, s["rod_pin_z"]), lev["A0"]
    b.slab("link", x0, x1, stadium(lo, hi, s["eye"]), "new_body")
    for n, p in (("lower", lo), ("upper", hi)):
        b.rod_x(f"{n} hole", p[0], p[1], s["d_pin_hole"] / 2.0, x0 - 1.0,
                x1 + 1.0, "cut")
    frames(b, s, [f"rod pin {which}", f"lever pin {which}"])


def lever_pin(b, s, which):
    """The pin at the lever's A, reaching +X through the link."""
    lev = s["arm"] if which == "axe" else s["bird"]
    sl = s[f"{which}_slabs"]
    b.rod_x("link pin", lev["A0"][0], lev["A0"][1], s["d_pin"] / 2.0,
            sl["lever"][1] - 0.5, sl["link"][1])


def pivot_hole(b, s, which):
    lev = s["arm"] if which == "axe" else s["bird"]
    x0, x1 = s[f"{which}_slabs"]["lever"]
    b.rod_x("pivot hole", lev["P"][0], lev["P"][1], s["d_pin_hole"] / 2.0,
            x0 - 1.0, x1 + 1.0, "cut")


def build_arms(b, s):
    """The woodcutter's arms and axe, one plate: a bar from the shoulder,
    the head at its end."""
    arm = s["arm"]
    x0, x1 = s["axe_slabs"]["lever"]
    P = arm["P"]
    th = math.radians(S.ARM_DOWN_DEG)
    u = (math.cos(th), math.sin(th))
    n = (math.sin(th), -math.cos(th))
    R = s["arm_reach"]
    end = (P[0] + R * u[0], P[1] + R * u[1])
    b.slab("arm", x0, x1, stadium(P, end, s["eye"]), "new_body")
    head = [(P[0] + a * u[0] + c * n[0], P[1] + a * u[1] + c * n[1])
            for a, c in S.axe_outline(s)]
    b.slab("axe head", x0, x1, polygon(head))
    lever_pin(b, s, "axe")
    pivot_hole(b, s, "axe")
    frames(b, s, ["pivot axe", "lever pin axe"])


def build_woodcutter(b, s):
    """The figure: feet, legs, body and head, flat, with the shoulder pin
    reaching +X through the arms."""
    k = s["width"] / 120.0
    x0, x1 = s["axe_slabs"]["body"]
    py, pz = s["arm"]["P"]
    st = s["stage"]
    hip = st + 0.45 * (pz - st)
    b.slab("feet", x0, x1, rect(py - 9 * k, st, py + 9 * k, st + 3 * k),
           "new_body")
    b.slab("legs", x0, x1, rect(py - 5 * k, st + 2 * k, py + 5 * k, hip))
    b.slab("body", x0, x1, rect(py - 6.5 * k, hip - 1 * k, py + 6.5 * k,
                                pz + 4 * k))
    b.slab("head", x0, x1, [circle((py, pz + 11 * k), 6 * k)])
    b.slab("neck", x0, x1, rect(py - 2.5 * k, pz + 3 * k, py + 2.5 * k,
                                pz + 6 * k))
    b.rod_x("shoulder pin", py, pz, s["d_pin"] / 2.0, x1 - 0.5,
            s["axe_slabs"]["lever"][1])
    frames(b, s, ["stand axe", "pivot axe"])


def bird_points(s):
    """The bird's body centre and head centre, at rest. It turns on a pin
    through its body, the body hanging behind and a little below it: so
    its weight presses the tail onto the link through the whole peck. A
    bird on its feet, its body over the pivot, stands the body over the
    pin as it pecks, and its weight stops holding the rod down."""
    k = s["width"] / 120.0
    P = s["bird"]["P"]
    return (P[0] - 6 * k, P[1] - 1 * k), (P[0] + 9 * k, P[1] + 6 * k)


def beak_tip(s):
    k = s["width"] / 120.0
    head = bird_points(s)[1]
    return (head[0] + 9 * k, head[1] - 0.8 * k)


def build_bird(b, s):
    """The bird: a body behind the pin it turns on (so its weight keeps the
    rod on its cam), a head and beak in front, the tail lever down to the
    link's pin."""
    k = s["width"] / 120.0
    x0, x1 = s["bird_slabs"]["lever"]
    P, A = s["bird"]["P"], s["bird"]["A0"]
    b.slab("tail", x0, x1, stadium(P, A, s["eye"]), "new_body")
    body_c, head = bird_points(s)
    b.slab("body", x0, x1, ellipse(body_c, 11 * k, 6 * k))
    b.slab("head", x0, x1, [circle(head, 3.5 * k)])
    b.slab("neck", x0, x1, stadium((P[0] + 3 * k, P[1] + 2 * k), head,
                                   2 * k))
    tip = beak_tip(s)
    b.slab("beak", x0, x1, polygon([(head[0] + 2.5 * k, head[1] + 1.2 * k),
                                    tip,
                                    (head[0] + 2.5 * k, head[1] - 1.8 * k)]))
    lever_pin(b, s, "bird")
    pivot_hole(b, s, "bird")
    frames(b, s, ["pivot bird", "lever pin bird"])


def build_perch(b, s):
    """The bird's perch: a post on a foot, its pin reaching +X through the
    bird's feet."""
    k = s["width"] / 120.0
    x0, x1 = s["bird_slabs"]["body"]
    py, pz = s["bird"]["P"]
    st = s["stage"]
    b.slab("foot", x0, x1, rect(py - 8 * k, st, py + 8 * k, st + 3 * k),
           "new_body")
    b.slab("post", x0, x1, rect(py - s["eye"], st + 2 * k, py + s["eye"],
                                pz))
    b.slab("post head", x0, x1, [circle((py, pz), s["eye"])])
    b.rod_x("perch pin", py, pz, s["d_pin"] / 2.0, x1 - 0.5,
            s["bird_slabs"]["lever"][1])
    frames(b, s, ["stand bird", "pivot bird"])


def build_log(b, s):
    k = s["width"] / 120.0
    o = joint_frames(s)["log"][0]
    r = s["log_d"] / 2.0
    b.rod_x("log", s["log_y"], s["stage"] + r, r, o[0] - 15 * k,
            o[0] + 15 * k, "new_body")
    frames(b, s, ["log"])
