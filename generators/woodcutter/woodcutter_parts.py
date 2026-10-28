# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The woodcutter automaton's parts, one function each, every one in the
BOX frame (woodcutter_spec's docstring) where it stands at rest, with a
Frame at each joint. Two parts joined at a joint carry the same frame, so
every mate starts at zero.

The box is five printed panels -- the floor, the back, two end walls with
the bushings' bores, and the top with the guide bosses and the figures'
slots -- pegged at their edges, open at the front so the cams can be seen.
The pins, pegs, bushings and the crank's handle are parts of their own,
each modelled once and placed by its mates (or, a peg, by its transform).
Every part also says the way it prints up (`PRINT_UP`, its own frame).
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
NOZZLE = S.NOZZLE_MM
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


def d_dir(c, r, h, ang):
    """A D: a circle of radius r about c cut by a chord h from c, square to
    the direction `ang` (radians in the sketch's u, v), the flat on that
    side."""
    a = math.acos(h / r)
    a0, a1 = ang + a, ang - a + 2.0 * math.pi
    p0 = (c[0] + r * math.cos(a0), c[1] + r * math.sin(a0))
    p1 = (c[0] + r * math.cos(a1), c[1] + r * math.sin(a1))
    return [{"type": "arc", "center": [c[0], c[1]], "radius": r,
             "start_angle": a0, "end_angle": a1}, line(p1, p0)]


def plane(origin, normal, u):
    """A sketch plane; v = normal x u."""
    return {"origin": [float(v) for v in origin],
            "normal": [float(v) for v in normal], "uAxis": [float(v) for v in u]}


def uv(pl, p):
    """A world point's (u, v) on plane `pl`."""
    n, u, o = pl["normal"], pl["uAxis"], pl["origin"]
    v = (n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2],
         n[0] * u[1] - n[1] * u[0])
    dd = [p[i] - o[i] for i in range(3)]
    return (sum(dd[i] * u[i] for i in range(3)),
            sum(dd[i] * v[i] for i in range(3)))


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

    def box(self, name, lo, hi, mode="add"):
        return self.extrude(name, plane_xy(lo[2]),
                            rect(lo[0], lo[1], hi[0], hi[1]), hi[2] - lo[2],
                            mode)

    def holes(self, name, origin, normal, u, pts, dia, depth, printed=None):
        """A Hole feature: blind holes `depth` deep drilled against
        `normal` from the plane through `origin`, at the world points
        `pts` (on that plane). `printed` "teardrop" where the hole lies
        across the part's print direction."""
        pl = plane(origin, normal, u)
        ents = [{"type": "point", "p": list(uv(pl, q))} for q in pts]
        sk = self.sketch(f"{name} centres", pl, ents)
        spec = {"type": "Hole", "name": name, "sketch_id": sk,
                "hole_kind": "simple", "diameter": float(dia),
                "depth": float(depth), "through": False}
        if printed:
            spec["printed"] = printed
        return self.add(spec, name)["id"]

    def frame(self, name, origin, z_axis, x_axis):
        return self.add({"type": "Frame", "name": name, "kind": "explicit",
                         "origin": [float(v) for v in origin],
                         "z_axis": z_axis, "x_axis": x_axis},
                        f"frame {name}")["id"]


# --- the box ------------------------------------------------------------------------

def joint_frames(s):
    """Every frame two parts share: name -> (origin, z, x)."""
    zc = s["zc"]
    f = {"shaft": ((0.0, 0.0, zc), X, Z),
         "bushing L": ((s["x_bush_l"], 0.0, zc), X, Z),
         "bushing R": ((s["x_bush_r"], 0.0, zc), [-1.0, 0.0, 0.0], Z),
         "crank": ((s["x_crank1"], 0.0, zc), X, Z),
         "handle": ((s["x_crank0"], 0.0, zc + s["crank_r"]), X, Y)}
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


# --- the box --------------------------------------------------------------------

def peg_joints(s):
    """Every peg: (joint, centre, axis), where the joint is the two panels
    it joins. Vertical pegs stand in the floor (the back's) and under the
    top (the walls' and the back's); pegs along X go from each wall into
    the floor's and the back's end edges."""
    W, D, t, H = s["width"], s["depth"], s["wall"], s["H"]
    yb = -D / 2.0 - t / 2.0         # the back panel's mid-thickness
    out = []
    for x in (W / 4.0, 3.0 * W / 4.0):
        out.append(("floor/back", (x, yb, 0.0), Z))
        out.append(("back/top", (x, yb, H), Z))
    for side, xw, xe in (("left", -t / 2.0, 0.0), ("right", W + t / 2.0, W)):
        for y in (-D / 4.0, D / 4.0):
            out.append((f"floor/{side} wall", (xe, y, -t / 2.0), X))
            out.append((f"{side} wall/top", (xw, y, H), Z))
        for z in (H / 3.0, 2.0 * H / 3.0):
            out.append((f"back/{side} wall", (xe, yb, z), X))
    return out


def _peg_holes(b, s, name, which, normal, u, printed=None):
    pts = [c for j, c, _ in peg_joints(s) if j == which]
    if pts:
        b.holes(name, pts[0], normal, u, pts, s["d_peg_bore"],
                s["peg_hole"], printed)


def build_floor(b, s):
    """The floor: under the back, between the end walls; prints flat. The
    back's pegs stand in its top face, the walls' go into its end edges
    (across the print: teardrops)."""
    W, D, t = s["width"], s["depth"], s["wall"]
    b.box("floor", (0.0, -D / 2.0 - t, -t), (W, D / 2.0 + t, 0.0),
          "new_body")
    _peg_holes(b, s, "back pegs", "floor/back", Z, X)
    _peg_holes(b, s, "left pegs", "floor/left wall",
               [-1.0, 0.0, 0.0], Y, "teardrop")
    _peg_holes(b, s, "right pegs", "floor/right wall", X, Y,
               "teardrop")


def build_back(b, s):
    """The back: on the floor, between the end walls, under the top; prints
    on its outside face, so every peg hole in its edges is a teardrop."""
    W, D, t, H = s["width"], s["depth"], s["wall"], s["H"]
    b.box("back", (0.0, -D / 2.0 - t, 0.0), (W, -D / 2.0, H), "new_body")
    _peg_holes(b, s, "floor pegs", "floor/back",
               [0.0, 0.0, -1.0], X, "teardrop")
    _peg_holes(b, s, "top pegs", "back/top", Z, X,
               "teardrop")
    _peg_holes(b, s, "left pegs", "back/left wall",
               [-1.0, 0.0, 0.0], Y, "teardrop")
    _peg_holes(b, s, "right pegs", "back/right wall", X, Y,
               "teardrop")


def build_wall(b, s, side):
    """An end wall, beside the floor and the back, under the top: the
    bushing's bore, the floor's and the back's pegs in its inside face, the
    top's in its top edge (teardrops: the wall prints on its outside
    face)."""
    W, D, t, H = s["width"], s["depth"], s["wall"], s["H"]
    x0 = -t if side == "left" else W
    inward = X if side == "left" else [-1.0, 0.0, 0.0]
    b.slab("wall", x0, x0 + t, rect(-D / 2.0 - t, -t, D / 2.0 + t, H),
           "new_body")
    b.rod_x("bushing bore", 0.0, s["zc"], s["d_wall_bore"] / 2.0, x0 - 1.0,
            x0 + t + 1.0, "cut")
    _peg_holes(b, s, "floor pegs", f"floor/{side} wall",
               inward, Y)
    _peg_holes(b, s, "back pegs", f"back/{side} wall",
               inward, Y)
    _peg_holes(b, s, "top pegs", f"{side} wall/top", Z, X,
               "teardrop")
    frames(b, s, ["shaft"])


def slot(s, x0, x1, y0, y1):
    """A through slot for a tab x0..x1 by y0..y1, the press on each side."""
    p = s["press"] / 2.0
    return (x0 - p, y0 - p, x1 + p, y1 + p)


def figure_slots(s):
    """The slots through the top: each figure's tab and the log's rib."""
    out = {}
    for k in ("axe", "bird"):
        x0, x1 = s[f"{k}_slabs"]["body"]
        py = s["arm" if k == "axe" else "bird"]["P"][0]
        out[k] = slot(s, x0, x1, py - s["tab_len"] / 2.0,
                      py + s["tab_len"] / 2.0)
    lx = sum(s["axe_slabs"]["lever"]) / 2.0
    hl = log_half_len(s)
    out["log"] = slot(s, lx - hl, lx + hl, s["log_y"] - s["rib_w"] / 2.0,
                      s["log_y"] + s["rib_w"] / 2.0)
    return out


def build_top(b, s):
    """The top: the stage the figures stand on, over the walls and the
    back; the guide bosses under it with their D bores, the figures' slots
    through it, the walls' and the back's pegs from below. Prints upside
    down, the stage on the bed and the bosses up."""
    W, D, t, H = s["width"], s["depth"], s["wall"], s["H"]
    b.box("top", (-t, -D / 2.0 - t, H), (W + t, D / 2.0 + t, H + t),
          "new_body")
    boss_z0 = s["stage"] - s["guide_len"]
    for k in ("axe", "bird"):
        x = s[f"x_{k}"]
        b.rod_z(f"{k} guide boss", x, 0.0, s["boss_r"], boss_z0, H + 0.01)
        b.extrude(f"{k} guide", plane_xy(boss_z0 - 1.0),
                  d_dir((x, 0.0), s["d_guide"] / 2.0,
                        s["rod_flat"] + s["g"], math.pi),
                  s["guide_len"] + 2.0, "cut")
    for n, (x0, y0, x1, y1) in figure_slots(s).items():
        b.box(f"{n} slot", (x0, y0, H - 1.0), (x1, y1, H + t + 1.0), "cut")
    down = [0.0, 0.0, -1.0]
    _peg_holes(b, s, "wall pegs", "left wall/top", down, X)
    _peg_holes(b, s, "right wall pegs", "right wall/top",
               down, X)
    _peg_holes(b, s, "back pegs", "back/top", down, X)
    frames(b, s, ["guide axe", "guide bird", "stand axe", "stand bird",
                  "log"])


def build_peg(b, s):
    """A peg: along Z about the origin, flat on -Y to print lying."""
    r = s["d_peg"] / 2.0
    L = s["peg_len"]
    b.extrude("peg", plane_xy(-L / 2.0),
              d_dir((0.0, 0.0), r, r / math.sqrt(2.0), -math.pi / 2.0), L,
              "new_body")


# --- the camshaft and its supports -------------------------------------------------

def build_shaft(b, s):
    """The camshaft: from the crank's outer face to the right bushing's,
    flat the whole length: the cams, the bushings and the crank are keyed
    to the flat, and the shaft prints lying on it."""
    r = s["d_shaft"] / 2.0
    b.rod_x("shaft", 0.0, s["zc"], r, s["x_shaft0"], s["x_shaft1"],
            "new_body")
    zf = s["zc"] - r + s["flat"]
    b.slab("flat", s["x_shaft0"] - 1.0, s["x_shaft1"] + 1.0,
           rect(-r - 1.0, s["zc"] - r - 1.0, r + 1.0, zf), "cut")
    frames(b, s, ["shaft", "cam axe", "cam bird", "bushing L", "bushing R",
                  "crank"])


def shaft_press_bore(s, c):
    """The D a part is pressed onto the shaft's flat by: the shaft's D,
    the press a side larger (smaller, for a negative press)."""
    rs = s["d_shaft"] / 2.0
    p = s["press"] / 2.0
    return d_dir(c, rs + p, rs - s["flat"] + p, -math.pi / 2.0)


def build_bushing(b, s):
    """A bushing, modelled as the left one: its flange outside the wall,
    its sleeve in the wall's bore, pressed on the shaft's D. Prints flange
    down."""
    zc, x0 = s["zc"], s["x_bush_l"]
    b.rod_x("flange", 0.0, zc, s["r_flange"], x0 - s["flange"], x0,
            "new_body")
    b.rod_x("sleeve", 0.0, zc, s["r_bush"], x0, -s["g"])
    b.slab("bore", x0 - s["flange"] - 1.0, 1.0,
           shaft_press_bore(s, (0.0, zc)), "cut")
    o, z, x = joint_frames(s)["bushing L"]
    b.frame("bushing", o, z, x)


def build_crank(b, s):
    """The crank: a plate pressed on the shaft's -X end, the handle's pin
    pressed in at its radius."""
    zc, R = s["zc"], s["crank_r"]
    x0, x1 = s["x_crank0"], s["x_crank1"]
    b.slab("crank", x0, x1, stadium((0.0, zc), (0.0, zc + R), s["r_hub"]),
           "new_body")
    b.slab("hub bore", x0 - 1.0, x1 + 1.0, shaft_press_bore(s, (0.0, zc)),
           "cut")
    b.rod_x("handle bore", 0.0, zc + R, (s["d_handle"] + s["press"]) / 2.0,
            x0 - 1.0, x1 + 1.0, "cut")
    frames(b, s, ["crank", "handle"])


def build_grip(b, s):
    """The handle's grip: a sleeve turning on the handle pin. Prints
    standing."""
    zc, R, g = s["zc"], s["crank_r"], s["g"]
    x1 = s["x_crank0"] - g
    x0 = x1 - s["grip_len"]
    b.rod_x("grip", 0.0, zc + R, s["r_grip"], x0, x1, "new_body")
    b.rod_x("bore", 0.0, zc + R, s["d_grip_bore"] / 2.0, x0 - 1.0, x1 + 1.0,
            "cut")
    frames(b, s, ["handle"])


def build_handle_pin(b, s):
    """The handle's pin: pressed through the crank, the grip turning on it
    between the crank and its head. Prints lying on a flat."""
    zc, R, g = s["zc"], s["crank_r"], s["g"]
    r = s["d_handle"] / 2.0
    h = r / math.sqrt(2.0)
    x_end = s["x_crank0"] - g - s["grip_len"] - g
    b.slab("shank", x_end, s["x_crank1"],
           d_dir((0.0, zc + R), r, h, -math.pi / 2.0), "new_body")
    b.slab("head", x_end - s["head"], x_end + 0.01,
           d_dir((0.0, zc + R), s["r_head"], h, -math.pi / 2.0))
    frames(b, s, ["handle"])


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
    """A cam keyed to the shaft: the CamProfile, then a D bore pressed on
    the shaft's flat. Returns the CamProfile's add response."""
    o, z, x = joint_frames(s)[f"cam {which}"]
    fid = b.frame(f"cam {which}", o, z, x)
    r = b.add(cam_spec(s, which, base, fid, segments), f"{which} cam")
    x0 = o[0]
    b.slab("bore", x0 - 1.0, x0 + s["cam_w"] + 1.0,
           shaft_press_bore(s, (0.0, s["zc"])), "cut")
    return r


def build_rod(b, s, which):
    """A follower: a D rod (its flat on -X, where it prints lying), its
    foot a cylinder of the knife's tip radius across the cam (the rounded
    knife) and about as wide as the rod, the pin's hole pressed through its top.
    The pin is a part of its own, so the rod goes up its guide from
    below."""
    x, r = s[f"x_{which}"], s["d_rod"] / 2.0
    foot = s[f"foot_{which}"]
    top = s["rod_pin_z"] + r
    # The foot reaches a nozzle's width past the rod's side: ending flush
    # with it, its end face is tangent to the rod's round, and that seam
    # meshes open (check_print's shells: not watertight).
    b.rod_x("foot", 0.0, foot, s["tip"], x - r - NOZZLE, x + r + NOZZLE,
            "new_body")
    b.rod_z("rod", x, 0.0, r, foot, top)
    xf = x - s["rod_flat"]
    b.box("flat", (x - r - 1.0, -s["tip"] - 1.0, foot - s["tip"] - 1.0),
          (xf, s["tip"] + 1.0, top + 1.0), "cut")
    b.rod_x("pin hole", 0.0, s["rod_pin_z"], s["d_pin_press"] / 2.0,
            x - r - 1.0, x + r + 1.0, "cut")
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


def lever_pin_hole(b, s, which):
    """The hole at the lever's A that the link's pin is pressed into."""
    lev = s["arm"] if which == "axe" else s["bird"]
    x0, x1 = s[f"{which}_slabs"]["lever"]
    b.rod_x("link pin hole", lev["A0"][0], lev["A0"][1],
            s["d_pin_press"] / 2.0, x0 - 1.0, x1 + 1.0, "cut")


def build_pin(b, s, which):
    """A pin, along X, D-flatted to print lying: `which` the rod's (pressed
    through the rod's top, the link turning on it), the lever's (pressed
    in the lever at A, the link turning on it) or the pivot's (pressed in
    the figure's body, the lever turning on it). Each modelled at the
    axe's joint; the bird's is the same pin on the same frame. The flat is
    on the side the joint's load is not: under the rod's pin (the link
    bears down on it), over the lever's (the link pushes it up)."""
    sl = s["axe_slabs"]
    r = s["d_pin"] / 2.0
    h = r / math.sqrt(2.0)
    if which == "rod":
        c, x0, x1 = (0.0, s["rod_pin_z"]), sl["link"][0], \
            s["x_axe"] + s["d_rod"] / 2.0
        ang, fr = -math.pi / 2.0, "rod pin axe"
    elif which == "lever":
        c, x0, x1 = s["arm"]["A0"], sl["lever"][0], sl["link"][1]
        ang, fr = math.pi / 2.0, "lever pin axe"
    else:
        c, x0, x1 = s["arm"]["P"], sl["body"][0], sl["lever"][1]
        ang, fr = -math.pi / 2.0, "pivot axe"
    b.slab("pin", x0, x1, d_dir(c, r, h, ang), "new_body")
    frames(b, s, [fr])


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
    lever_pin_hole(b, s, "axe")
    pivot_hole(b, s, "axe")
    frames(b, s, ["pivot axe", "lever pin axe"])


def build_woodcutter(b, s):
    """The figure: feet with a tab into its slot in the top, legs, body
    and head, flat, with the shoulder's pin pressed in."""
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
    tab(b, s, x0, x1, py)
    b.rod_x("shoulder pin hole", py, pz, s["d_pin_press"] / 2.0, x0 - 1.0,
            x1 + 1.0, "cut")
    frames(b, s, ["stand axe", "pivot axe"])


def tab(b, s, x0, x1, py):
    """A figure's tab: under its feet, through the top, as long as the
    slot it is pressed into and the panel deep."""
    st = s["stage"]
    b.slab("tab", x0, x1, rect(py - s["tab_len"] / 2.0, st - s["wall"],
                               py + s["tab_len"] / 2.0, st + 0.01))


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
    lever_pin_hole(b, s, "bird")
    pivot_hole(b, s, "bird")
    frames(b, s, ["pivot bird", "lever pin bird"])


def build_perch(b, s):
    """The bird's perch: a post on a foot with a tab into its slot, the
    bird's pin pressed into its head."""
    k = s["width"] / 120.0
    x0, x1 = s["bird_slabs"]["body"]
    py, pz = s["bird"]["P"]
    st = s["stage"]
    b.slab("foot", x0, x1, rect(py - 8 * k, st, py + 8 * k, st + 3 * k),
           "new_body")
    b.slab("post", x0, x1, rect(py - s["eye"], st + 2 * k, py + s["eye"],
                                pz))
    b.slab("post head", x0, x1, [circle((py, pz), s["eye"])])
    tab(b, s, x0, x1, py)
    b.rod_x("pin hole", py, pz, s["d_pin_press"] / 2.0, x0 - 1.0, x1 + 1.0,
            "cut")
    frames(b, s, ["stand bird", "pivot bird"])


def log_half_len(s):
    return 15.0 * s["width"] / 120.0


def build_log(b, s):
    """The log, on a rib along its underside pressed into its slot in the
    top. Prints standing on an end."""
    o = joint_frames(s)["log"][0]
    r = s["log_d"] / 2.0
    hl = log_half_len(s)
    st = s["stage"]
    b.rod_x("log", s["log_y"], st + r, r, o[0] - hl, o[0] + hl, "new_body")
    w = s["rib_w"] / 2.0
    b.slab("rib", o[0] - hl, o[0] + hl, rect(s["log_y"] - w, st - s["wall"],
                                             s["log_y"] + w, st + r / 2.0))
    frames(b, s, ["log"])


# The way each part prints, up, in its own (the box's) frame: plates lie
# flat, the panels on their outside faces (the top on its stage), the rods,
# the pins, the pegs and the shaft lying on their flats, so a load across
# any of them is along its layers.
MX = [-1.0, 0.0, 0.0]
PRINT_UP = {
    "floor": Z, "back": Y, "left wall": X, "right wall": MX,
    "top": [0.0, 0.0, -1.0], "peg": Y, "camshaft": Z, "bushing": X,
    "crank": X, "grip": X, "handle pin": Z,
    "axe cam": X, "bird cam": X, "axe rod": X, "bird rod": X,
    "axe link": X, "bird link": X, "arms": X, "bird": X, "woodcutter": X,
    "perch": X, "log": X,
    "rod pin": Z, "lever pin": [0.0, 0.0, -1.0], "pivot pin": Z,
}
ORIENT = {
    "floor": "flat", "back": "on its outside face",
    "left wall": "on its outside face", "right wall": "on its outside face",
    "top": "upside down, on the stage", "peg": "lying on its flat",
    "camshaft": "lying on its flat", "bushing": "flange down",
    "crank": "flat", "grip": "standing", "handle pin": "lying on its flat",
    "axe cam": "flat", "bird cam": "flat",
    "axe rod": "lying on its flat", "bird rod": "lying on its flat",
    "axe link": "flat", "bird link": "flat", "arms": "flat", "bird": "flat",
    "woodcutter": "flat", "perch": "flat", "log": "standing on an end",
    "rod pin": "lying on its flat", "lever pin": "lying on its flat",
    "pivot pin": "lying on its flat",
}
