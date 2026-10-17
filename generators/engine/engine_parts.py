# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The printed inline four's parts, one function each.

Every part is modelled where it sits in the assembled engine at crank 0, in
the ENGINE FRAME of engine_spec (X the crank axis, front at -X, Z up), or
for a moving part in its own frame (the rod about its big end, the piston
about its wrist pin, the valve on its seat, the camshaft about its axis),
and carries the named frames its mates take. PRINT says how each one lies
on the bed: a rotation applied to the part as modelled, before it is lifted
onto the bed (build.py's plates).
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import engine_spec as S  # noqa: E402
from _lib import arc_through, check, circle, line  # noqa: E402

lines = S.lines
SQ2 = math.sqrt(2.0)


def q_axis(axis, deg):
    h = math.radians(deg) / 2.0
    s = math.sin(h)
    return [axis[0] * s, axis[1] * s, axis[2] * s, math.cos(h)]


# How each part prints, as a rotation of the part as modelled (then lifted
# onto the bed and moved on it). Each is said where the part is built.
UP = [0.0, 0.0, 0.0, 1.0]                 # as modelled
FLIP_X = q_axis((1, 0, 0), 180.0)         # turned over about X
X_UP = q_axis((0, 1, 0), -90.0)           # +X up (-X on the bed)
X_DOWN = q_axis((0, 1, 0), 90.0)          # +X on the bed
Y_DOWN = q_axis((1, 0, 0), 90.0)          # -Y on the bed
PRINT = {}


# --- sketch entities -----------------------------------------------------------

def poly(pts):
    return [line(pts[i], pts[(i + 1) % len(pts)]) for i in range(len(pts))]


def arc_ccw(c, r, a0, a1):
    """An arc counter-clockwise from angle a0 to a1 (radians, a1 > a0)."""
    return {"type": "arc", "center": [float(c[0]), float(c[1])],
            "radius": float(r), "start_angle": float(a0),
            "end_angle": float(a1)}


def pol(c, r, a):
    return (c[0] + r * math.cos(a), c[1] + r * math.sin(a))


def keel(r, h, c=(0.0, 0.0)):
    """A round of radius r with a 90-degree keel towards -u, cut flat at
    u = -h: it lies on the bed on its flat, and no face of it looks down
    past 45 degrees. Turned to the keel, a bore of this shape keys what is
    pressed on (r / sqrt 2 < h < r sqrt 2)."""
    if not (r / SQ2 < h < r * SQ2):
        raise ValueError(f"a keel of radius {r} cut at {h}")
    a = math.radians(135.0)
    lo, hi = pol(c, r, -a), pol(c, r, a)
    w = r * SQ2 - h
    return [arc_ccw(c, r, -a, a), line(hi, (c[0] - h, c[1] + w)),
            line((c[0] - h, c[1] + w), (c[0] - h, c[1] - w)),
            line((c[0] - h, c[1] - w), lo)]


def d_shape(r, c=(0.0, 0.0)):
    """A round flattened at u = -r/sqrt 2: the widest flat that leaves no
    face looking down past 45 degrees when it lies on it."""
    a = math.radians(135.0)
    return [arc_ccw(c, r, -a, a), line(pol(c, r, a), pol(c, r, -a))]


def teardrop(r, c=(0.0, 0.0), up=(0.0, 1.0)):
    """A round HOLE with a 45-degree point on its roof, `up` as it prints:
    no face over it looks down past 45 and the whole round is kept, so a
    pin pressed in meets its full circle."""
    ua = math.atan2(up[1], up[0])
    a0, a1 = ua + math.radians(45.0), ua + math.radians(315.0)
    apex = (c[0] + r * SQ2 * math.cos(ua), c[1] + r * SQ2 * math.sin(ua))
    return [arc_ccw(c, r, a0, a1), line(pol(c, r, a1), apex),
            line(apex, pol(c, r, a0))]


def hull2(c1, r1, c2, r2):
    """The hull of two circles (r1 >= r2) as arcs and two tangent lines."""
    dx, dy = c2[0] - c1[0], c2[1] - c1[1]
    d = math.hypot(dx, dy)
    g = math.atan2(dy, dx)
    b = math.acos((r1 - r2) / d)
    # Big arc: from g + b round the back to g - b + 2 pi; small arc g - b
    # to g + b.
    p1a, p1b = pol(c1, r1, g + b), pol(c1, r1, g - b)
    p2a, p2b = pol(c2, r2, g + b), pol(c2, r2, g - b)
    return [arc_ccw(c1, r1, g + b, g - b + 2.0 * math.pi), line(p1b, p2b),
            arc_ccw(c2, r2, g - b, g + b), line(p2a, p1a)]


def rect(u0, v0, u1, v1):
    return poly([(u0, v0), (u1, v0), (u1, v1), (u0, v1)])


# --- planes ---------------------------------------------------------------------

def p_x(x):
    """Normal +X: (u, v) = (y, z)."""
    return {"origin": [x, 0.0, 0.0], "normal": [1, 0, 0], "uAxis": [0, 1, 0]}


def p_xm(x):
    """Normal -X: (u, v) = (y, -z)."""
    return {"origin": [x, 0.0, 0.0], "normal": [-1, 0, 0],
            "uAxis": [0, 1, 0]}


def p_z(z):
    """Normal +Z: (u, v) = (x, y)."""
    return {"origin": [0.0, 0.0, z], "normal": [0, 0, 1], "uAxis": [1, 0, 0]}


def p_zm(z):
    """Normal -Z: (u, v) = (x, -y)."""
    return {"origin": [0.0, 0.0, z], "normal": [0, 0, -1],
            "uAxis": [1, 0, 0]}


def p_xz():
    """The XZ plane, normal -Y: (u, v) = (x, z)."""
    return {"origin": [0.0, 0.0, 0.0], "normal": [0, -1, 0],
            "uAxis": [1, 0, 0]}


def p_y(y):
    """Normal +Y: (u, v) = (x, -z)."""
    return {"origin": [0.0, y, 0.0], "normal": [0, 1, 0], "uAxis": [1, 0, 0]}


class Part:
    """Features into one part (generators/clock/clock_parts.py's
    Builder)."""

    def __init__(self, d, name, proc, material="pla", print_q=UP):
        self.d, self.name, self.material = d, name, material
        d.call("add_part", name=name)
        if proc is not None:
            d.call("set_print_process", **proc)
        self.n = 0
        PRINT[name] = print_q

    def _name(self, what):
        self.n += 1
        return f"{what} {self.n}"

    def sketch(self, what, plane, ents):
        return check(self.d.call("add_feature", feature={
            "type": "Sketch", "name": self._name(what + " sketch"),
            "plane": plane, "entities": ents}), f"{self.name}: {what}")["id"]

    def extrude(self, what, plane, ents, dist, mode="add", **kw):
        sk = self.sketch(what, plane, ents)
        return check(self.d.call("add_feature", feature={
            "type": "Extrude", "name": self._name(what), "sketch_id": sk,
            "distance": float(dist), "mode": mode, **kw}),
            f"{self.name}: {what}")["id"]

    def revolve(self, what, pts, axis_origin=(0, 0, 0), mode="add",
                plane=None):
        """A solid of revolution about the vertical axis through
        axis_origin: `pts` are (r, z) in that axis' frame, drawn on the XZ
        plane through it."""
        ox, oy, _ = axis_origin
        plane = plane or {"origin": [0.0, oy, 0.0], "normal": [0, -1, 0],
                          "uAxis": [1, 0, 0]}
        sk = self.sketch(what, plane, poly([(ox + r, z) for r, z in pts]))
        return check(self.d.call("add_feature", feature={
            "type": "Revolve", "name": self._name(what), "sketch_id": sk,
            "axis": {"kind": "custom", "origin": [ox, oy, 0.0],
                     "direction": [0.0, 0.0, 1.0]},
            "angle_deg": 360.0, "mode": mode}), f"{self.name}: {what}")["id"]

    def box(self, what, lo, hi, mode="add"):
        return self.extrude(what, p_z(lo[2]), rect(lo[0], lo[1], hi[0],
                                                   hi[1]),
                            hi[2] - lo[2], mode)

    def along_x(self, what, x0, x1, ents, mode="add"):
        return self.extrude(what, p_x(x0), ents, x1 - x0, mode)

    def cyl_x(self, what, x0, x1, r, c=(0.0, 0.0), mode="add"):
        return self.along_x(what, x0, x1, [circle(c, r)], mode)

    def frame(self, name, origin, z=(0, 0, 1), x=(1, 0, 0)):
        check(self.d.call("add_feature", feature={
            "type": "Frame", "name": name, "kind": "explicit",
            "origin": [float(v) for v in origin], "z_axis": list(z),
            "x_axis": list(x)}), f"{self.name}: frame {name}")

    def finish(self, solids=1):
        """One body, its material set on the feature that made it (a later
        cut takes set_material and sets nothing)."""
        bodies = self.d.call("list_elements")["bodies"]
        if len(bodies) != solids:
            raise SystemExit(f"engine: {self.name} is {len(bodies)} bodies, "
                             f"not {solids}")
        for b in bodies:
            self.d.call("set_material", feature_id=b["created_by"],
                        material=self.material)
        return self

    def press(self, dia, where=None):
        """Mark the round faces of diameter `dia` (passing `where(face)`) a
        PRINTED PRESS (docs/tolerances/README.md#printed-press-fits)."""
        n = 0
        for b in self.d.call("list_elements")["bodies"]:
            for f in b.get("faces", []):
                if f.get("kind") != "cylinder" or \
                        abs(2.0 * f.get("radius", 0.0) - dia) > 1e-6:
                    continue
                if where is not None and not where(f):
                    continue
                check(self.d.call("set_tolerance", feature_id=b["created_by"],
                                  element=f["name"], printed="press"),
                      f"{self.name}: printed press")
                n += 1
        if n == 0:
            raise SystemExit(f"engine: {self.name} has no Ø{dia:.3f} face "
                             f"to press")
        return n


# --- the bottom end -------------------------------------------------------------

def web_outline(s, z_pin):
    """A crank web in (y, z): from the flat up past the pin, from the pin
    end to the counterweight end, its corners off at 45 degrees."""
    zt, zb = abs(z_pin) + s.r_web, -(abs(z_pin) + s.r_web)
    y0, y1 = -s.h_flat, s.y_web
    c = 0.35 * s.r_web
    return poly([(y0, zb + c), (y0 + c, zb), (y1 - c, zb), (y1, zb + c),
                 (y1, zt - c), (y1 - c, zt), (y0 + c, zt), (y0, zt - c)])


def crankshaft(d, s, proc):
    """Five mains, four pins at 0/180/180/0, eight webs, a keeled nose and
    tail; everything cut flat h_flat under the axis, where it prints (on
    the plane of its throws: -Y on the bed)."""
    p = Part(d, "crankshaft", proc, print_q=Y_DOWN)
    first = True
    for x in s.x_main:
        p.cyl_x("main journal", x - s.l_j / 2.0, x + s.l_j / 2.0, s.r_j,
                mode="new_body" if first else "add")
        first = False
    for k, x in enumerate(s.x_cyl):
        zp = s.R * math.cos(math.radians(S.THROW_DEG[k]))
        p.cyl_x("crank pin", x - s.l_pin / 2.0, x + s.l_pin / 2.0, s.r_j,
                c=(0.0, zp))
        for x0 in (x - s.l_pin / 2.0 - s.t_web, x + s.l_pin / 2.0):
            p.along_x("web", x0, x0 + s.t_web, web_outline(s, zp))
    x0 = s.x_main[0] - s.l_j / 2.0
    p.along_x("nose", s.x_nose, x0, keel(s.r_stub, s.h_flat))
    x1 = s.x_main[-1] + s.l_j / 2.0
    p.along_x("tail", x1, s.x_tail, keel(s.r_stub, s.h_flat))
    big = 4.0 * (s.R + s.r_web)
    p.box("flat", (s.x_nose - 1, -big, -big), (s.x_tail + 1, -s.h_flat, big),
          mode="cut")
    p.frame("main", (0, 0, 0), z=(1, 0, 0), x=(0, 0, 1))
    for k, x in enumerate(s.x_cyl):
        zp = s.R * math.cos(math.radians(S.THROW_DEG[k]))
        p.frame(f"pin {k + 1}", (x, 0, zp), z=(1, 0, 0), x=(0, 0, 1))
    p.finish()
    p.press(2.0 * s.r_stub)
    return p


def rod_outline_parts(s):
    """The rod's outline in (y, z) of its own frame, as overlapping pieces
    (one extrude each)."""
    return [
        [circle((0.0, 0.0), s.r_be)],
        rect(-s.y_ear, -s.z_lap, s.y_ear, s.z_lap),
        poly([(-s.w_shank0 / 2.0, 0.0), (s.w_shank0 / 2.0, 0.0),
              (s.w_shank1 / 2.0, s.L), (-s.w_shank1 / 2.0, s.L)]),
        [circle((0.0, s.L), s.r_se)],
    ]


def lap_box(s, side, half, lower):
    """A lap tongue's box: `side` +1/-1 in y, `half` 'A' (x < 0) or 'B'
    (x > 0), below the split (lower) or above."""
    y0, y1 = sorted((side * s.r_jb, side * s.y_ear * 1.01))
    x0, x1 = (-s.w_r / 2.0, 0.0) if half == "A" else (0.0, s.w_r / 2.0)
    z0, z1 = (-s.z_lap, 0.0) if lower else (0.0, s.z_lap)
    return (x0, y0, z0), (x1, y1, z1)


def rod(d, s, proc, name="rod", length=None):
    """The rod in its own frame: big end at the origin, the small end
    `length` up +Z, the eyes along X. It prints flat, half A (x < 0) on the
    bed, so its lap tongue lies on the bed."""
    L = length if length is not None else s.L
    p = Part(d, name, proc, print_q=X_UP)
    w = s.w_r
    outline = rod_outline_parts(s)
    outline[2] = poly([(-s.w_shank0 / 2.0, 0.0), (s.w_shank0 / 2.0, 0.0),
                       (s.w_shank1 / 2.0, L), (-s.w_shank1 / 2.0, L)])
    outline[3] = [circle((0.0, L), s.r_se)]
    for i, ents in enumerate(outline):
        p.along_x("rod", -w / 2.0, w / 2.0, ents,
                  mode="new_body" if i == 0 else "add")
    p.along_x("bores", -w / 2.0 - 1, w / 2.0 + 1,
              [circle((0, 0), s.r_jb), circle((0, L), s.r_seb)], mode="cut")
    big = 2.0 * s.y_ear
    p.box("split", (-w, -big, -big), (w, big, 0.0), mode="cut")
    for side in (1, -1):
        lo, hi = lap_box(s, side, "A", True)
        p.box("lap", lo, hi, mode="add")
        lo, hi = lap_box(s, side, "B", False)
        p.box("lap notch", lo, hi, mode="cut")
    p.along_x("cap pins", -w / 2.0 - 1, w / 2.0 + 1,
              [circle((s.y_cp, 0.0), s.d_cp / 2.0),
               circle((-s.y_cp, 0.0), s.d_cp / 2.0)], mode="cut")
    p.frame("big_end", (0, 0, 0), z=(1, 0, 0), x=(0, 0, 1))
    p.frame("small_end", (0, 0, L), z=(1, 0, 0), x=(0, 0, 1))
    for j, y in ((1, s.y_cp), (2, -s.y_cp)):
        p.frame(f"cap pin {j}", (0, y, 0))
    p.finish()
    p.press(s.d_cp)
    return p


def rod_cap(d, s, proc):
    """The big end's cap, in the rod's frame: below the split, with the
    lap tongues above it in half B. It prints half B down."""
    p = Part(d, "rod cap", proc, print_q=X_DOWN)
    w = s.w_r
    p.along_x("cap", -w / 2.0, w / 2.0, [circle((0, 0), s.r_be)],
              mode="new_body")
    p.along_x("cap", -w / 2.0, w / 2.0,
              rect(-s.y_ear, -s.z_lap, s.y_ear, s.z_lap))
    p.along_x("bore", -w / 2.0 - 1, w / 2.0 + 1, [circle((0, 0), s.r_jb)],
              mode="cut")
    big = 2.0 * s.y_ear
    p.box("split", (-w, -big, 0.0), (w, big, big), mode="cut")
    for side in (1, -1):
        lo, hi = lap_box(s, side, "B", False)
        p.box("lap", lo, hi, mode="add")
        lo, hi = lap_box(s, side, "A", True)
        p.box("lap notch", lo, hi, mode="cut")
    p.along_x("cap pins", -w / 2.0 - 1, w / 2.0 + 1,
              [circle((s.y_cp, 0.0), s.d_cp / 2.0),
               circle((-s.y_cp, 0.0), s.d_cp / 2.0)], mode="cut")
    p.finish()
    p.press(s.d_cp)
    return p


def pin_x(d, s, proc, name, dia, length, centre=(0.0, 0.0, 0.0)):
    """A plain pin along X about `centre`: printed standing."""
    p = Part(d, name, proc, print_q=X_UP)
    p.cyl_x(name, centre[0] - length / 2.0, centre[0] + length / 2.0,
            dia / 2.0, c=(centre[1], centre[2]), mode="new_body")
    p.finish()
    p.press(dia)
    return p


def piston(d, s, proc, name="piston"):
    """In its own frame, the wrist pin's axis along X at the origin: a cup
    with its crown up, two bosses, the pin's bore. It prints crown down
    (turned over about X), so the bore's teardrop points down here."""
    p = Part(d, name, proc, print_q=FLIP_X)
    r = s.d_p / 2.0
    ri = r - s.t_skirt
    top = s.h_cp
    p.revolve("cup", [(ri, -s.s_below), (r, -s.s_below), (r, top), (0, top),
                      (0, top - s.t_crown), (ri, top - s.t_crown)],
              mode="new_body")
    if math.hypot(ri, s.r_boss) >= r - lines(1):
        raise SystemExit("engine: a piston boss runs out of the skirt")
    for sgn in (1, -1):
        x0, x1 = sorted((sgn * s.x_boss, sgn * (ri + lines(1))))
        p.box("boss", (x0, -s.r_boss, -s.r_boss),
              (x1, s.r_boss, top - s.t_crown + lines(1)))
    p.along_x("pin bore", -r - 1, r + 1,
              teardrop(s.r_wpb, up=(0.0, -1.0)), mode="cut")
    p.frame("wrist_pin", (0, 0, 0), z=(1, 0, 0), x=(0, 0, 1))
    p.frame("slide", (0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("crown", (0, 0, top), z=(0, 0, 1), x=(1, 0, 0))
    p.finish()
    p.press(s.d_wp)
    return p


# --- block, bedplate, sump ----------------------------------------------------

def block_outline(s):
    hw, hb = s.w_case / 2.0, s.w_barrel / 2.0
    z1 = s.z_flare + (hw - hb)
    return poly([(-hw, 0.0), (hw, 0.0), (hw, s.z_flare), (hb, z1),
                 (hb, s.z_deck), (-hb, s.z_deck), (-hb, z1),
                 (-hw, s.z_flare)])


def cavity_outline(s):
    """The crankcase inside, in (y, z): the walls' width up to where the
    45-degree flare (a wall's thickness inside the outer one) meets the
    bores' width, then up to the barrels."""
    hw = s.w_case / 2.0
    yi = s.y_case
    yr = s.y_rect
    za = s.z_flare - s.t_wall * (SQ2 - 1.0)
    zb = s.z_flare + (hw - s.t_wall * SQ2 - yr)
    return poly([(-yi, -1.0), (yi, -1.0), (yi, za), (yr, zb), (yr, s.z_bb),
                 (-yr, s.z_bb), (-yr, zb), (-yi, za)])


def window_outline(s, x):
    """A cutaway window in (x, -z) (p_y's frame): pointed top and bottom,
    45-degree edges, so it prints either way up."""
    a = s.w_window / 2.0
    z0, z1 = s.z_win
    return poly([(x, -z0), (x + a, -(z0 + a)), (x + a, -(z1 - a)), (x, -z1),
                 (x - a, -(z1 - a)), (x - a, -(z0 + a))])


def block(d, s, proc):
    """The upper block, split at the crank's centre line: four bores in a
    narrow barrel block over a crankcase that flares out at 45 degrees,
    the mains' upper halves in five bulkheads, a cutaway window into each
    bore on the +Y side, the TDC pointer on the rear face. It prints deck
    down (turned over about X)."""
    p = Part(d, "block", proc, print_q=FLIP_X)
    p.along_x("block", s.x_f, s.x_r, block_outline(s), mode="new_body")
    for k in range(4):
        x0 = s.x_main[k] + s.b_t / 2.0
        x1 = s.x_main[k + 1] - s.b_t / 2.0
        p.along_x("crankcase", x0, x1, cavity_outline(s), mode="cut")
    p.extrude("bores", p_z(s.z_bb - 1.0),
              [circle((x, 0.0), s.bore / 2.0) for x in s.x_cyl],
              s.z_deck - s.z_bb + 2.0, mode="cut")
    if s.e_mouth > 0.0:
        # Rod notches: where a rod's corners pass the bore's mouth, a
        # 45-degree relief across the rod's width only, so the siamesed
        # walls between bores keep their full thickness.
        r, e = s.bore / 2.0, s.e_mouth
        prof = poly([(-(r + e), s.z_bb - 1.0), (r + e, s.z_bb - 1.0),
                     (r + e, s.z_bb), (r, s.z_bb + e), (-r, s.z_bb + e),
                     (-(r + e), s.z_bb)])
        half = s.w_r / 2.0 + s.c_stack
        for x in s.x_cyl:
            p.along_x("rod notch", x - half, x + half, prof, mode="cut")
    p.cyl_x("mains", s.x_f - 1, s.x_r + 1, s.r_jb, mode="cut")
    p.extrude("split dowels", p_z(0.0), [circle(xy, s.d_dw / 2.0)
                                         for xy in s.dowels_split],
              s.dw_depth, mode="cut")
    p.extrude("deck dowels", p_zm(s.z_deck),
              [circle((x, -y), s.d_dw / 2.0) for x, y in s.dowels_deck],
              s.dw_depth, mode="cut")
    p.extrude("windows", p_y(0.0), [e for x in s.x_cyl
                                    for e in window_outline(s, x)],
              s.w_case, mode="cut")
    # The TDC pointer: a diamond in the rear face over the flywheel's rim.
    zc, a = s.z_pointer, s.pointer
    p.extrude("pointer", p_xm(s.x_r), poly([(0, -(zc - a)), (a, -zc),
                                            (0, -(zc + a)), (-a, -zc)]),
              s.d_engrave, mode="cut")
    p.frame("crank", (0, 0, 0), z=(1, 0, 0), x=(0, 0, 1))
    for k, x in enumerate(s.x_cyl):
        p.frame(f"bore {k + 1}", (x, 0, s.z_bb), z=(0, 0, 1), x=(1, 0, 0))
    for k, (x, y) in enumerate(s.dowels_split):
        p.frame(f"split dowel {k + 1}", (x, y, 0.0))
    for k, (x, y) in enumerate(s.dowels_deck):
        p.frame(f"deck dowel {k + 1}", (x, y, s.z_deck))
    p.finish()
    p.press(s.d_dw)
    return p


def bedplate(d, s, proc):
    """The mains' lower halves in five bulkheads, walls round the crank's
    sweep, open below to the sump. It prints as it sits."""
    p = Part(d, "bedplate", proc)
    hw = s.w_case / 2.0
    p.box("bedplate", (s.x_f, -hw, s.z_bp), (s.x_r, hw, 0.0),
          mode="new_body")
    for k in range(4):
        x0 = s.x_main[k] + s.b_t / 2.0
        x1 = s.x_main[k + 1] - s.b_t / 2.0
        p.box("crankcase", (x0, -s.y_case, s.z_bp - 1.0),
              (x1, s.y_case, 1.0), mode="cut")
    p.cyl_x("mains", s.x_f - 1, s.x_r + 1, s.r_jb, mode="cut")
    p.extrude("split dowels", p_zm(0.0), [circle((x, -y), s.d_dw / 2.0)
                                          for x, y in s.dowels_split],
              s.dw_depth, mode="cut")
    p.extrude("sump dowels", p_z(s.z_bp), [circle(xy, s.d_dw / 2.0)
                                           for xy in s.dowels_split],
              s.dw_depth, mode="cut")
    for k, (x, y) in enumerate(s.dowels_split):
        p.frame(f"sump dowel {k + 1}", (x, y, s.z_bp))
    p.finish()
    p.press(s.d_dw)
    return p


def sump(d, s, proc):
    """An open tray under the bedplate; the engine stands on it."""
    p = Part(d, "sump", proc)
    hw = s.w_case / 2.0
    p.box("sump", (s.x_f, -hw, s.z_sump), (s.x_r, hw, s.z_bp),
          mode="new_body")
    p.box("well", (s.x_main[0] + s.b_t / 2.0, -s.y_case,
                   s.z_sump + s.t_floor),
          (s.x_main[-1] - s.b_t / 2.0, s.y_case, s.z_bp + 1.0), mode="cut")
    p.extrude("dowels", p_zm(s.z_bp), [circle((x, -y), s.d_dw / 2.0)
                                       for x, y in s.dowels_split],
              s.dw_depth, mode="cut")
    p.finish()
    p.press(s.d_dw)
    return p


def dowel(d, s, proc):
    """A dowel, centred on its frame's origin, along Z: pressed into both
    halves of a joint. It prints standing."""
    p = Part(d, "dowel", proc)
    p.extrude("dowel", p_z(-s.l_dw / 2.0), [circle((0, 0), s.d_dw / 2.0)],
              s.l_dw, mode="new_body")
    p.finish()
    p.press(s.d_dw)
    return p


# --- the head -------------------------------------------------------------------

def head(d, s, proc):
    """On the deck: per valve a seat (a counterbore and a 45-degree cone),
    a throat, a port at 45 degrees out to the side (intake +Y, exhaust -Y),
    a guide and a bucket bore; over them the cam's trough and saddles
    between towers. It prints deck down, as it sits."""
    p = Part(d, "head", proc)
    hw = s.w_head / 2.0
    zd = s.z_deck
    p.box("head", (s.x_f, -hw, zd), (s.x_r, hw, s.z_cam), mode="new_body")
    g = s.g_seat
    for lb in s.lobes:
        x = lb["x"]
        p.revolve("seat", [(0.0, zd - 1.0), (s.r_v + g, zd - 1.0),
                           (s.r_v + g, zd + s.hz_cb),
                           (s.r_t, zd + s.hz_throat),
                           (s.r_guide, zd + s.hz_guide0),
                           (0.0, zd + s.hz_guide0)],
                  axis_origin=(x, 0.0, 0.0), mode="cut")
    p.extrude("guides", p_z(zd + s.hz_guide0 - 0.5),
              [circle((lb["x"], 0.0), s.r_guide) for lb in s.lobes],
              s.hz_floor - s.hz_guide0 + 1.0, mode="cut")
    p.extrude("bucket bores", p_z(zd + s.hz_floor),
              [circle((lb["x"], 0.0), s.r_bkb) for lb in s.lobes],
              s.hz_cam + 1.0, mode="cut")
    reach = (hw + s.r_t + 2.0) * SQ2
    for kind, sgn in (("intake", 1.0), ("exhaust", -1.0)):
        n = [0.0, sgn / SQ2, 1.0 / SQ2]
        plane = {"origin": [0.0, 0.0, zd + s.hz_port], "normal": n,
                 "uAxis": [1, 0, 0]}
        p.extrude(f"{kind} ports", plane,
                  [circle((lb["x"], 0.0), s.r_t) for lb in s.lobes
                   if lb["kind"] == kind], reach, mode="cut")
    for x0, x1 in s.troughs:
        p.cyl_x("cam trough", x0, x1, s.r_trough, c=(0.0, s.z_cam),
                mode="cut")
    p.cyl_x("cam saddles", s.x_f - 1, s.x_r + 1, s.r_bar_b,
            c=(0.0, s.z_cam), mode="cut")
    p.extrude("deck dowels", p_z(zd), [circle(xy, s.d_dw / 2.0)
                                       for xy in s.dowels_deck],
              s.dw_depth, mode="cut")
    p.extrude("cover dowels", p_zm(s.z_cam),
              [circle((x, -y), s.d_dw / 2.0) for x, y in s.dowels_cover],
              s.dw_depth, mode="cut")
    p.frame("cam", (0, 0, s.z_cam), z=(1, 0, 0), x=(0, 0, 1))
    for i, lb in enumerate(s.lobes):
        p.frame(f"guide {i + 1}", (lb["x"], 0, zd), z=(0, 0, -1),
                x=(1, 0, 0))
        p.frame(f"spring {i + 1}", (lb["x"], 0, zd + s.hz_floor))
    for k, (x, y) in enumerate(s.dowels_cover):
        p.frame(f"cover dowel {k + 1}", (x, y, s.z_cam))
    p.finish()
    p.press(s.d_dw)
    return p


def cam_cover(d, s, proc):
    """The cam's upper bearings and its cover in one: the saddles' upper
    halves in ribs between the troughs. It prints top down."""
    p = Part(d, "cam cover", proc, print_q=FLIP_X)
    hw = s.w_cover / 2.0
    p.box("cover", (s.x_f, -hw, s.z_cam), (s.x_r, hw, s.z_cam + s.h_cover),
          mode="new_body")
    for x0, x1 in s.troughs:
        p.cyl_x("cam trough", x0, x1, s.r_trough, c=(0.0, s.z_cam),
                mode="cut")
    p.cyl_x("cam saddles", s.x_f - 1, s.x_r + 1, s.r_bar_b,
            c=(0.0, s.z_cam), mode="cut")
    p.extrude("dowels", p_z(s.z_cam), [circle(xy, s.d_dw / 2.0)
                                       for xy in s.dowels_cover],
              s.dw_depth, mode="cut")
    p.finish()
    p.press(s.d_dw)
    return p


# --- valve train ---------------------------------------------------------------

def valve(d, s, proc):
    """In its own frame: the face on the origin, the stem up +Z to its tip
    in the bucket. It prints head down, as modelled."""
    p = Part(d, "valve", proc)
    p.revolve("valve", [(0.0, 0.0), (s.r_v, 0.0), (s.r_v, s.t_e),
                        (s.r_s, s.t_e + s.r_v - s.r_s), (s.r_s, s.hz_tip),
                        (0.0, s.hz_tip)], mode="new_body")
    p.frame("seat", (0, 0, 0), z=(0, 0, -1), x=(1, 0, 0))
    p.frame("face", (0, 0, 0))
    p.frame("bucket", (0, 0, s.hz_u0))
    p.finish()
    p.press(s.d_s)
    return p


def bucket(d, s, proc):
    """The bucket, in the valve's frame: pressed on the stem's tip, the
    cam on its crown and the spring under it. It prints crown down."""
    p = Part(d, "bucket", proc, print_q=FLIP_X)
    p.extrude("bucket", p_z(s.hz_u0), [circle((0, 0), s.r_bk)], s.h_bk,
              mode="new_body")
    p.extrude("stem", p_z(s.hz_u0 - 1.0), [circle((0, 0), s.r_s)],
              s.hz_tip - s.hz_u0 + 1.0, mode="cut")
    p.finish()
    p.press(s.d_s)
    return p


def bellows(s, length):
    """A TPU bellows' section (r, z): a zigzag wall at no flatter than 45
    degrees between r_sp_i and r_sp_o, ending on its inner turns."""
    th = s.t_sp * SQ2                 # the wall across, horizontally
    ri, ro = s.r_sp_i + th / 2.0, s.r_sp_o - th / 2.0
    dr = ro - ri
    n = max(2, int(length / dr) // 2 * 2)
    dz = length / n
    mid = [((ri if j % 2 == 0 else ro), j * dz) for j in range(n + 1)]
    outer = [(r + th / 2.0, z) for r, z in mid]
    inner = [(r - th / 2.0, z) for r, z in mid]
    return outer + inner[::-1], n, dz, dr


def spring(d, s, length, name):
    """The valve spring: TPU, standing on its lower turn. Not a printed
    PLA part, so no print process (no TPU table)."""
    p = Part(d, name, None, material="tpu")
    pts, n, dz, dr = bellows(s, length)
    p.revolve("bellows", pts, mode="new_body")
    p.frame("base", (0, 0, 0))
    p.finish()
    return p, n, dz, dr


# --- cam --------------------------------------------------------------------------

def cam_bar(d, s, proc):
    """The camshaft's bar, in the cam frame (its axis X at the origin): a
    round with a flat it prints on, that keys the lobes and the pulley."""
    p = Part(d, "cam bar", proc, print_q=Y_DOWN)
    p.along_x("bar", s.x_cam0, s.x_cam1, d_shape(s.r_bar), mode="new_body")
    p.frame("axis", (0, 0, 0), z=(1, 0, 0), x=(0, 0, 1))
    p.finish()
    p.press(s.d_bar)
    return p


def lobe_ents(s, peak_cam_deg):
    """The lobe's outline in (y, z) of the cam frame: the hull of the base
    circle and the nose circle, the nose pointing at the bucket (-Z) when
    the cam has turned `peak_cam_deg`."""
    a = -math.radians(peak_cam_deg)
    nose = (s.cam_d * math.sin(a), -s.cam_d * math.cos(a))
    return hull2((0.0, 0.0), s.r_base, nose, s.r_nose) + d_shape(s.r_bar)


def lobe(d, s, proc, i, lb):
    p = Part(d, f"lobe {i + 1}", proc, print_q=X_UP)
    x0 = lb["x"] - s.w_lobe / 2.0
    p.along_x("lobe", x0, x0 + s.w_lobe, lobe_ents(s, lb["peak_cam_deg"]),
              mode="new_body")
    # Its number, so the lobes go back on the bar in their places: on the
    # nose, between the bore and the tip.
    a = -math.radians(lb["peak_cam_deg"])
    ax, az = math.sin(a), -math.cos(a)
    rr = (s.r_bar + s.r_lobe) / 2.0
    p.extrude("number", p_x(x0 + s.w_lobe - s.d_engrave),
              [{"type": "text", "text": str(i + 1),
                "anchor": [ax * rr, az * rr],
                "height": s.label_h, "font": "sans", "angle_deg": 0.0,
                "halign": "center", "valign": "middle"}],
              s.d_engrave + 1.0, mode="cut")
    p.finish()
    p.press(s.d_bar)
    return p


def groove_poly(s, r_p, a, delta=0.0):
    """One groove at angle a: the belt's tooth grown by the gap, open past
    the tip -- with the tooth `delta` radians on along the straight line
    the belt leaves on (or comes in on, delta < 0) and the pulley turned
    with it, so the grooves cut at every delta are the envelope a tooth
    sweeps going in and out (the pulley's teeth then clear the belt's off
    the arc too)."""
    g = s.belt_gr
    t20 = math.tan(math.radians(20.0))
    c20 = math.cos(math.radians(20.0))
    cd, sd = math.cos(-delta), math.sin(-delta)
    t = (-math.sin(a), math.cos(a))
    n = (-math.cos(a), -math.sin(a))
    p = (r_p * math.cos(a) + r_p * delta * t[0],
         r_p * math.sin(a) + r_p * delta * t[1])
    b0 = s.belt_pld - g - lines(2)
    b1 = s.belt_pld + s.belt_h + g
    hw0 = s.belt_base / 2.0 + g / c20 + (s.belt_pld - b0) * t20
    hw1 = s.belt_base / 2.0 + g / c20 - (b1 - s.belt_pld) * t20

    def pt(u, v):
        q = (p[0] + t[0] * u + n[0] * v, p[1] + t[1] * u + n[1] * v)
        return (cd * q[0] - sd * q[1], sd * q[0] + cd * q[1])
    return [pt(-hw0, b0), pt(hw0, b0), pt(hw1, b1), pt(-hw1, b1)]


def groove_ents(s, r_p, z, phase, delta=0.0):
    out = []
    for j in range(z):
        out += poly(groove_poly(s, r_p, phase + 2.0 * math.pi * j / z,
                                delta))
    return out


def inside(pt, pts):
    """Whether a point is in a convex polygon (either winding)."""
    sgn = 0
    for i in range(len(pts)):
        a, b = pts[i], pts[(i + 1) % len(pts)]
        c = (b[0] - a[0]) * (pt[1] - a[1]) - (b[1] - a[1]) * (pt[0] - a[0])
        if abs(c) < 1e-12:
            continue
        if sgn == 0:
            sgn = 1 if c > 0 else -1
        elif (c > 0) != (sgn > 0):
            return False
    return True


def groove_poses(s, r_p, r_tip):
    """The deltas whose cut still removes pulley: a pose whose grown tooth
    is wholly out past the tips, or inside the poses already kept, cuts
    nothing (sampled on a grid over it)."""
    kept, polys = [], []
    for dl in sorted(groove_deltas(s, r_p), key=abs):
        q = groove_poly(s, r_p, 0.0, dl)
        xs, ys = [v[0] for v in q], [v[1] for v in q]
        new = False
        for i in range(41):
            for k in range(41):
                pt = (min(xs) + (max(xs) - min(xs)) * i / 40.0,
                      min(ys) + (max(ys) - min(ys)) * k / 40.0)
                if math.hypot(*pt) < r_tip - 1e-3 and inside(pt, q) and \
                        not any(inside(pt, o) for o in polys):
                    new = True
                    break
            if new:
                break
        if new:
            kept.append(dl)
            polys.append(q)
    return sorted(kept)


def groove_deltas(s, r_p):
    """The poses a groove is cut at: on until the tooth's inner face is
    out past the tips (the line leaves the pitch circle by a^2 / 2r)."""
    a_out = math.sqrt(2.0 * r_p * s.belt_h)
    dm = a_out / r_p
    n = max(3, int(math.ceil(dm / 0.04)))
    return [dm * k / n for k in range(-n, n + 1)]


def pulley(d, s, proc, name, r_p, z, phase, x_teeth, x_flange, x_hub,
           bore_ents, print_q):
    """A toothed pulley about the X axis: teeth over x_teeth, a flange over
    x_flange, a hub over x_hub (each (x0, x1), or None)."""
    p = Part(d, name, proc, print_q=print_q)
    r_land = r_p - s.belt_pld
    r_tip = r_land - s.belt_gr
    r_root = r_tip - s.belt_h - s.belt_gr
    p.along_x("teeth", x_teeth[0], x_teeth[1], [circle((0, 0), r_tip)],
              mode="new_body")
    for dl in groove_poses(s, r_p, r_tip):
        p.along_x("grooves", x_teeth[0] - 0.01, x_teeth[1] + 0.01,
                  groove_ents(s, r_p, z, phase, dl), mode="cut")
    if x_flange:
        p.along_x("flange", x_flange[0], x_flange[1],
                  [circle((0, 0), r_land + s.belt_t + lines(2))])
    if x_hub and x_hub[1] - x_hub[0] > 1e-6:
        p.along_x("hub", x_hub[0], x_hub[1], [circle((0, 0), r_root)])
    lo = min(v[0] for v in (x_teeth, x_flange, x_hub) if v)
    hi = max(v[1] for v in (x_teeth, x_flange, x_hub) if v)
    p.along_x("bore", lo - 1, hi + 1, bore_ents, mode="cut")
    p.finish()
    return p, r_tip, r_root


def cam_collar(d, s, proc):
    p = Part(d, "cam collar", proc, print_q=X_UP)
    x0 = s.x_r + s.g_lobe_ax
    p.along_x("collar", x0, x0 + s.t_collar, [circle((0, 0), s.r_collar)]
              + d_shape(s.r_bar), mode="new_body")
    p.finish()
    p.press(s.d_bar)
    return p


# --- front and rear ---------------------------------------------------------------

def flywheel(d, s, proc):
    """On the tail stub: a disc with a tick every 10 degrees on its rear
    face, longer every 30, and cylinder 1 and 4's TDC and 2 and 3's
    marked. It prints rear face up."""
    p = Part(d, "flywheel", proc, print_q=X_UP)
    x0 = s.x_r + s.g_ax
    x1 = x0 + s.t_fw
    p.along_x("flywheel", x0, x1, [circle((0, 0), s.r_fw)] +
              keel(s.r_stub, s.h_flat), mode="new_body")
    ticks = []
    w = s.tick_w / 2.0
    for k in range(36):
        a = math.radians(90.0 + 10.0 * k)
        ln = s.tick * (2.0 if k % 3 == 0 else 1.0)
        r0, r1 = s.r_fw - ln, s.r_fw + 1.0
        t = (-math.sin(a), math.cos(a))
        c = (math.cos(a), math.sin(a))
        ticks += poly([(c[0] * r0 - t[0] * w, c[1] * r0 - t[1] * w),
                       (c[0] * r1 - t[0] * w, c[1] * r1 - t[1] * w),
                       (c[0] * r1 + t[0] * w, c[1] * r1 + t[1] * w),
                       (c[0] * r0 + t[0] * w, c[1] * r0 + t[1] * w)])
    p.extrude("ticks", p_x(x1 - s.d_engrave), ticks, s.d_engrave + 1.0,
              mode="cut")
    rt = s.r_fw - 2.0 * s.tick - s.label_h
    p.extrude("TDC marks", p_x(x1 - s.d_engrave), [
        {"type": "text", "text": "1 4", "anchor": [0.0, rt],
         "height": s.label_h, "font": "sans", "angle_deg": 0.0,
         "halign": "center", "valign": "middle"},
        {"type": "text", "text": "2 3", "anchor": [0.0, -rt],
         "height": s.label_h, "font": "sans", "angle_deg": 180.0,
         "halign": "center", "valign": "middle"}],
        s.d_engrave + 1.0, mode="cut")
    p.finish()
    p.press(2.0 * s.r_stub)
    return p


def crank_arm(d, s, proc):
    """The hand crank's arm on the nose, in front of the pulley; its handle
    is pressed in at r_arm, up at crank 0."""
    p = Part(d, "crank arm", proc, print_q=X_UP)
    x0 = s.x_nose
    r = s.r_arm_hub
    ents = [arc_ccw((0, 0), r, math.pi, 2.0 * math.pi),
            line((r, 0.0), (r, s.r_arm)),
            arc_ccw((0, s.r_arm), r, 0.0, math.pi),
            line((-r, s.r_arm), (-r, 0.0))]
    p.along_x("arm", x0, x0 + s.t_arm, ents + keel(s.r_stub, s.h_flat),
              mode="new_body")
    p.cyl_x("handle hole", x0 - 1, x0 + s.t_arm + 1, s.d_hpin / 2.0,
            c=(0.0, s.r_arm), mode="cut")
    p.finish()
    p.press(2.0 * s.r_stub)
    p.press(s.d_hpin)
    return p


def handle(d, s, proc):
    """The handle: a grip forward of the arm and a pin pressed into it. It
    prints grip down, the pin up."""
    p = Part(d, "handle", proc, print_q=X_UP)
    x0 = s.x_nose
    p.cyl_x("grip", x0 - s.l_handle, x0, s.d_handle / 2.0, c=(0, s.r_arm),
            mode="new_body")
    p.cyl_x("pin", x0, x0 + s.t_arm, s.d_hpin / 2.0, c=(0, s.r_arm))
    p.finish()
    p.press(s.d_hpin)
    return p


def belt_ents(s, path, rho_off):
    """The racetrack `rho_off` out from the pitch line (negative: in)."""
    r1, r2 = s.r_p1 + rho_off, s.r_p2 + rho_off
    ph = path.phi
    c2 = (0.0, s.c_belt)
    a1, b1 = math.pi + ph, 2.0 * math.pi - ph
    a2, b2 = -ph, math.pi + ph
    return [arc_ccw((0, 0), r1, a1, b1), line(pol((0, 0), r1, b1),
                                              pol(c2, r2, a2)),
            arc_ccw(c2, r2, a2, b2), line(pol(c2, r2, b2),
                                          pol((0, 0), r1, a1))]


def belt(d, s):
    """The timing belt in TPU, as it runs on the pulleys at crank 0: a
    band on the pitch line's outside and N teeth in. It prints flat (no
    print process: there is no TPU table)."""
    p = Part(d, "timing belt", None, material="tpu", print_q=X_UP)
    path = s.belt_path
    p.along_x("band", s.x_belt0, s.x_belt1,
              belt_ents(s, path, -s.belt_pld) +
              belt_ents(s, path, s.belt_t - s.belt_pld), mode="new_body")
    teeth = []
    for pt, t, n in s.belt_teeth:
        teeth += poly(S.trapezoid(pt, t, n, s.belt_pld, s.belt_base,
                                  s.belt_tip, s.belt_h, into=lines(1)))
    p.along_x("teeth", s.x_belt0, s.x_belt1, teeth)
    p.finish()
    return p
