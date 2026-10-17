#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A printed Strandbeest: Theo Jansen's walker, twelve legs on a keyed
crankshaft, every joint a printed pin with a printed cap pressed on.

    python3 generators/strandbeest/build.py --port 8802 \\
        --profile "K5 PLA on P1S" --fresh
    python3 generators/strandbeest/build.py --port 8802 --size 12 --stage 1

Everything derives from `--size`, the crank radius (Jansen's 15 at the
default), the print profile's gap (`print_allowance` kind `gap` at each
diameter, half of it a side), its press (kind `press`), its layer height
and its bed (`bed_mm`), and the P1S's nozzle: nothing here is a literal in millimetres
(generators/strandbeest/README.md#where-every-size-comes-from).

What it builds (generators/strandbeest/README.md):
- the leg's six bodies, front and back, in the layers `linkage.py`
  assigns; a headed pin for every joint but the crank pin and the frame
  pivot, and a cap pressed on each;
- frame plates tied by pressed standoffs, the frame pivots printed on
  them; a crankshaft of discs, one throw each, joined by crank pins
  printed lying down in the discs' D-sockets, the phases set by where each
  disc's sockets are; a hand crank;
- every pin in a load path studied as it prints under the walker's own
  loads (the leg's statics, its weight and the floor's friction), a
  factor of 2 or the run stops;
- stage 1, one leg pair on its crank and the coupon's press row, as one
  plate; stage 2, the walker, as plates for the bed;
- the assemblies, each with a 360-degree study at 2 degrees a sample, and
  every check: solved, on the built branch (the knees' signs), no clash,
  the clearance between adjacent layers, the foot's stride, lift and
  flatness against the scaled Jansen curve, and the centre of mass over
  the feet on the ground at every sample;
- the negative controls, which must fail: a bar 10 % long, two crossing
  bars on one layer.

Output (generated, untracked): scratch/strandbeest/ -- stage1/ and stage2/
with their STLs and .dsgn, and strandbeest.json, the receipt.
"""
import argparse
import json
import math
import os
import sys
import time
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
from _lib import (PRINT_ITEMS, bed_zones, check, check_print,  # noqa: E402
                  circle, connect, export_print_job, fresh_project,
                  job_receipt, line, pin_strength, print_receipt)
from lobole.sdk import Plane  # noqa: E402
import linkage as lk  # noqa: E402
import print_coupon  # noqa: E402

ROOT = Path(HERE).resolve().parent.parent
OUT = Path(os.environ["LOBOLE_SCRATCH"]) / "strandbeest" if \
    os.environ.get("LOBOLE_SCRATCH") else ROOT / "scratch" / "strandbeest"

# The machine the profile is for, a Bambu Lab P1S: its stock nozzle, which
# the allowance table assumes. Not a property of the part, so not derived
# from it. Its bed is the print profile's `bed_mm`.
NOZZLE_MM = 0.4
MIN_WALL_MM = 4 * NOZZLE_MM
FDM_LAYER_MM = 0.2  # Bambu Studio's "0.20 mm" preset: the FDM fallback
SIDES = lk.SIDES
# A pin printed standing: its slicer settings (set_print_settings), the
# rest of the walker at the preset's. Counts, not sizes.
PIN_PRINT = {"walls": 3}


# ---- sizes ------------------------------------------------------------------

def whole_layers(v, layer):
    """v rounded to whole layers, at least one: the slicer prints nothing
    between them."""
    return layer * max(1, round(v / layer))


def solve(size, layer, allow):
    """Every size from the crank radius, the layer and the process's
    allowances (`allow(kind, diameter)` -> mm on the diameter). Refuses
    what will not print."""
    L = lk.lengths(size)
    s = {"size": size, "layer": layer, "lengths": L}
    dp = 0.33 * size                     # a joint pin, and the crank pin
    s["dp"] = dp
    s["gap_pin"] = allow("gap", dp)      # diametral, round a pin
    s["gz"] = s["gap_pin"] / 2.0         # between two layers: a side
    s["wall"] = max(0.5 * dp, MIN_WALL_MM)
    s["hole"] = dp + s["gap_pin"]
    s["w"] = s["hole"] + 2.0 * s["wall"]  # a bar's width
    s["ds"] = 0.65 * dp                  # the stub a cap is pressed on
    s["press"] = allow("press", s["ds"])
    s["cap_bore"] = s["ds"] + s["press"]
    s["dh"] = s["w"] - 2.0 * s["gz"]     # a head's and a cap's diameter
    s["t"] = whole_layers(0.2 * size, layer)        # a bar
    s["th"] = whole_layers(0.6 * s["t"], layer)     # a head
    s["tc"] = s["t"]                                # a cap
    s["tp"] = whole_layers(1.5 * s["t"], layer)     # a plate and a disc
    s["tf"] = s["t"]                     # a crankshaft end flange
    s["pitch"] = s["t"] + s["gz"]        # layer to layer
    # The crankshaft's discs: the crank pin and its wall on the disc.
    s["rd"] = L["m"] + dp / 2.0 + s["w"] / 2.0
    s["gap_disc"] = allow("gap", 2.0 * s["rd"])
    s["rf"] = s["rd"] + s["w"]           # the end flanges
    s["socket"] = whole_layers(0.6 * s["tp"], layer)  # a stub's depth
    s["key_bore"] = s["ds"] + s["press"]
    # The crank pins and the handle are pins of their own, pressed into
    # D-shaped sockets at the press allowance at their size.
    s["press_pin"] = allow("press", dp)
    s["pin_bore"] = dp + s["press_pin"]
    s["standoff"] = 1.5 * dp
    s["handle_d"] = 2.0 * dp
    s["handle_h"] = 2.5 * s["w"]
    s["arm"] = s["rd"] + s["w"] + 3.0 * s["w"]  # hand crank: to handle
    if s["w"] / 2.0 - s["hole"] / 2.0 < MIN_WALL_MM - 1e-9:
        raise SystemExit(f"strandbeest: {s['wall']:.2f} mm round a pin at "
                         f"--size {size:g}: a larger --size")
    if s["ds"] < 4 * NOZZLE_MM:
        raise SystemExit(f"strandbeest: a Ø{s['ds']:.2f} stub is under four "
                         f"nozzle widths: a larger --size")
    return s


def z_bottom(s, ell):
    """A layer's underside, from the plate it stands on."""
    return s["gz"] + ell * s["pitch"]


def slab_height(s, top):
    """Plate to plate: every layer to the cap layer `top`, and the gap
    over it."""
    return z_bottom(s, top + 1)


# ---- the leg's geometry ---------------------------------------------------

def body_frames(L, mirror=False):
    """Each body's joints in its own frame (origin at its first joint, x at
    its second), from the leg at crank angle 0: lengths do not move."""
    p = lk.pose(L, 0.0, mirror)
    out = {}
    for bar, joints in lk.BARS.items():
        o, x = p[joints[0]], p[joints[1]]
        out[bar] = {j: lk.local(o, x, p[j]) for j in joints}
    return out


def world_pose(L, theta, mirror=False):
    """Each body's origin and angle in the leg's frame at crank angle
    theta: where to place it so its mates start closed."""
    p = lk.pose(L, theta, mirror)
    return {bar: (p[j[0]], lk.bar_angle(p, bar))
            for bar, j in lk.BARS.items()}, p


def stadium(p1, p2, r):
    ux, uy = p2[0] - p1[0], p2[1] - p1[1]
    d = math.hypot(ux, uy)
    ux, uy = ux / d, uy / d
    nx, ny = -uy, ux
    a = math.atan2(uy, ux)
    return [line((p1[0] + r * nx, p1[1] + r * ny),
                 (p2[0] + r * nx, p2[1] + r * ny)),
            {"type": "arc", "center": list(p2), "radius": r,
             "start_angle": a - math.pi / 2, "end_angle": a + math.pi / 2},
            line((p2[0] - r * nx, p2[1] - r * ny),
                 (p1[0] - r * nx, p1[1] - r * ny)),
            {"type": "arc", "center": list(p1), "radius": r,
             "start_angle": a + math.pi / 2,
             "end_angle": a + 3 * math.pi / 2}]


def tri_frame(vs, r):
    """A triangle of bars: the triangle grown by r with round corners, less
    it shrunk by r."""
    if lk.cross(vs[0], vs[1], vs[2]) < 0:
        vs = [vs[0], vs[2], vs[1]]
    n = []
    for i in range(3):
        a, b = vs[i], vs[(i + 1) % 3]
        ux, uy = b[0] - a[0], b[1] - a[1]
        d = math.hypot(ux, uy)
        n.append((uy / d, -ux / d))   # outward, counter-clockwise
    ents = []
    for i in range(3):
        a, b = vs[i], vs[(i + 1) % 3]
        ents.append(line((a[0] + r * n[i][0], a[1] + r * n[i][1]),
                         (b[0] + r * n[i][0], b[1] + r * n[i][1])))
        nb = n[(i + 1) % 3]
        a0 = math.atan2(n[i][1], n[i][0])
        a1 = math.atan2(nb[1], nb[0])
        while a1 < a0:
            a1 += 2 * math.pi
        ents.append({"type": "arc", "center": list(b), "radius": r,
                     "start_angle": a0, "end_angle": a1})
    inner = []
    for i in range(3):
        n0, n1 = n[i - 1], n[i]
        k = 1.0 + n0[0] * n1[0] + n0[1] * n1[1]
        inner.append((vs[i][0] - r * (n0[0] + n1[0]) / k,
                      vs[i][1] - r * (n0[1] + n1[1]) / k))
    if lk.cross(*inner) <= 0:
        raise SystemExit("strandbeest: a triangle too small for its bars")
    ents += [line(inner[i], inner[(i + 1) % 3]) for i in range(3)]
    return ents


def hull(points):
    pts = sorted(set(points))
    if len(pts) < 3:
        return pts

    def half(seq):
        out = []
        for p in seq:
            while len(out) >= 2 and lk.cross(out[-2], out[-1], p) <= 0:
                out.pop()
            out.append(p)
        return out
    lo, hi = half(pts), half(reversed(pts))
    return lo[:-1] + hi[:-1]


def circle_hull(circles, n=24):
    """The outline round a set of circles: their convex hull, the circles
    as n-gons circumscribed so the outline never cuts inside one."""
    pts = []
    for (cx, cy), r in circles:
        R = r / math.cos(math.pi / n)
        for k in range(n):
            a = 2 * math.pi * (k + 0.5) / n
            pts.append((round(cx + R * math.cos(a), 6),
                        round(cy + R * math.sin(a), 6)))
    h = hull(pts)
    return [line(h[i], h[(i + 1) % len(h)]) for i in range(len(h))]


# ---- the layout -----------------------------------------------------------

def layout(s, samples):
    """The layers (linkage.assign_layers), the slab they make between two
    plates, and where the standoffs that tie the plates can stand: the
    nearest spots above and below the crank that no bar, head or cap ever
    comes near over the turn."""
    L = s["lengths"]
    clear = s["w"] / 4.0
    prox = lk.proximity(L, s["w"], samples)
    lay = lk.assign_layers(prox, s["w"], s["dh"] / 2.0, s["dp"] / 2.0, clear)
    top = lay["top"]
    s["top"] = top
    s["slab"] = slab_height(s, top)
    s["clear"] = clear
    # A spot is free when every bar's skeleton stays w/2 + the standoff's
    # radius + clear away, every joint (a head, a cap) dh/2 + radius + clear.
    poses = {sd: [lk.pose(L, 2 * math.pi * i / samples, sd == "back")
                  for i in range(samples)] for sd in SIDES}
    rs = s["standoff"] / 2.0

    def free(pt):
        if math.hypot(*pt) < s["rd"] + s["gap_disc"] / 2.0 + s["w"] + rs:
            return False
        for sd in SIDES:
            for p in poses[sd]:
                for bar, js in lk.BARS.items():
                    e = lk.edges([p[j] for j in js])
                    if lk.skel_dist([(pt, pt)], e) < s["w"] / 2 + rs + clear:
                        return False
                for j in "BCDEF":
                    if math.hypot(pt[0] - p[j][0], pt[1] - p[j][1]) < \
                            s["dh"] / 2 + rs + clear:
                        return False
        return True
    spots = []
    step = s["dp"] / 4.0
    for sign in (1.0, -1.0):
        y = s["rd"]
        while not free((0.0, sign * y)):
            y += step
            if y > 4 * L["j"]:
                raise SystemExit("strandbeest: no free spot for a standoff "
                                 "on the crank's vertical")
        spots.append((0.0, sign * (y + step)))  # a step inside the free run
    s["standoffs"] = spots
    return lay


def slab_base(s, p):
    """Throw p's slab starts on plate p's top face, plates tp thick between
    slabs, plate 0's top at z = 0."""
    return p * (s["slab"] + s["tp"])


def layer_of(lay, side, bar):
    return lay["layers"][(side, bar)]


def pin_span(lay, side, joint):
    ls = [layer_of(lay, side, b) for b in lk.JOINTS[joint]]
    return min(ls), max(ls)


def shoulder(s, hi):
    """Where a pin's stub starts over its top bar: a gap of play."""
    return z_bottom(s, hi) + s["t"] + s["gz"]


# ---- building parts -------------------------------------------------------

class Part:
    """Features into one part, which this makes and selects."""

    def __init__(self, d, name, proc, material):
        self.d, self.name = d, name
        d.call("add_part", name=name)
        d.call("set_print_process", **proc)
        self.material = material
        self.n = 0

    def sketch(self, z, ents, what):
        self.n += 1
        return check(self.d.call("add_feature", feature={
            "type": "Sketch", "name": f"{what} {self.n}",
            "plane": Plane.xy(z).json(), "entities": ents}),
            f"{self.name}: {what}")["id"]

    def extrude(self, z, ents, distance, mode, what, reversed_=False):
        sk = self.sketch(z, ents, what)
        f = {"type": "Extrude", "name": f"{what} {self.n}", "sketch_id": sk,
             "distance": distance, "mode": mode}
        if reversed_:
            f["reversed"] = True
        return check(self.d.call("add_feature", feature=f),
                     f"{self.name}: {what}")["id"]

    def cylinders(self, z, centres, d, h, mode, what):
        return self.extrude(z, [circle(c, d / 2.0) for c in centres], h,
                            mode, what)

    def frame(self, name, origin, x_axis=(1.0, 0.0, 0.0)):
        check(self.d.call("add_feature", feature={
            "type": "Frame", "name": name, "kind": "explicit",
            "origin": [float(v) for v in origin], "z_axis": [0, 0, 1],
            "x_axis": [float(v) for v in x_axis]}), f"{self.name}: {name}")

    def finish(self, bodies=1):
        els = self.d.call("list_elements")["bodies"]
        if len(els) != bodies:
            raise SystemExit(f"strandbeest: {self.name} is {len(els)} "
                             f"bodies, not {bodies}: "
                             f"{[b['name'] for b in els]}")
        for b in els:
            self.d.call("set_material", feature_id=b["created_by"],
                        material=self.material)
        return self


def printed_press(d, what, dia, centres=None):
    """Mark every round face of Ø`dia` of the active part a PRINTED PRESS
    (docs/tolerances/README.md#printed-press-fits): banded by the print
    profile's press allowance, so the exact clash check relieves the press
    and lists it. `centres`, when given, keeps only faces on those axes."""
    n = 0
    for b in d.call("list_elements")["bodies"]:
        for f in b.get("faces", []):
            if f.get("kind") != "cylinder" or \
                    abs(2.0 * f.get("radius", 0.0) - dia) > 1e-6:
                continue
            c = f.get("center") or f.get("axis_origin")
            if centres is not None and c is not None and not any(
                    math.hypot(c[0] - x, c[1] - y) < 1e-6
                    for x, y in centres):
                continue
            check(d.call("set_tolerance", feature_id=b["created_by"],
                         element=f["name"], printed="press"),
                  f"{what}: printed press")
            n += 1
    if n == 0:
        raise SystemExit(f"strandbeest: {what} has no Ø{dia:.3f} face to "
                         f"press")
    return n


def build_bar(d, s, lay, frames, side, bar, proc, mat, name=None,
              layer=None):
    """One of the leg's six bodies, flat, its joint holes at the pin's gap;
    a Frame per joint on the SLAB's base plane (z = -its layer's height),
    so every mate between two bodies is frame to frame at z = 0."""
    js = lk.BARS[bar]
    pts = frames[bar]
    ell = layer if layer is not None else layer_of(lay, side, bar)
    part = Part(d, name or f"{side} {bar}", proc, mat)
    r = s["w"] / 2.0
    holes = [circle(pts[j], s["hole"] / 2.0) for j in js if j != "G"]
    if len(js) == 2:
        outline = stadium(pts[js[0]], pts[js[1]], r)
    else:
        outline = tri_frame([pts[j] for j in js], r)
    part.extrude(0.0, outline + holes, s["t"], "new_body", "body")
    for j in js:
        part.frame(j, (pts[j][0], pts[j][1], -z_bottom(s, ell)))
    return part.finish()


def build_pin(d, s, lay, side, joint, proc, mat):
    """A joint's pin, printed standing on its head: the head under the
    lowest bar, the shaft through every bar of the joint, a shoulder a
    gap's play over the top one and the stub its cap is pressed on. Its
    Frame is on the slab's base plane."""
    lo, hi = pin_span(lay, side, joint)
    z0 = z_bottom(s, lo) - s["th"]          # the head's underside, slab z
    zs = shoulder(s, hi)
    part = Part(d, f"pin {side} {joint}", proc, mat)
    # Standing, a pin's shaft is all wall: three loops, not the preset's
    # two, so the shaft a bar turns on is not two lines thick.
    d.call("set_print_settings", part=part.name, **PIN_PRINT)
    part.cylinders(0.0, [(0, 0)], s["dh"], s["th"], "new_body", "head")
    part.cylinders(s["th"], [(0, 0)], s["dp"], zs - z0 - s["th"], "add",
                   "shaft")
    part.cylinders(zs - z0, [(0, 0)], s["ds"], s["tc"], "add", "stub")
    part.frame("J", (0.0, 0.0, -z0))
    part.frame("cap", (0.0, 0.0, zs - z0))  # where its cap seats
    part.finish()
    printed_press(d, part.name, s["ds"])
    return {"lo": lo, "hi": hi, "z0": z0, "shoulder": zs,
            "length": zs - z0 + s["tc"]}


def build_cap(d, s, proc, mat):
    part = Part(d, "cap", proc, mat)
    part.extrude(0.0, [circle((0, 0), s["dh"] / 2.0),
                       circle((0, 0), s["cap_bore"] / 2.0)], s["tc"],
                 "new_body", "cap")
    part.frame("seat", (0.0, 0.0, 0.0))
    part.finish()
    printed_press(d, "cap", s["cap_bore"])
    return part


def b_pins(s, lay):
    """The frame pivots printed on a plate: each side's pin from the plate
    through its bars to a shoulder, its cap's stub over it."""
    out = []
    for side in SIDES:
        _, hi = pin_span(lay, side, "B")
        x = -s["lengths"]["a"] if side == "front" else s["lengths"]["a"]
        out.append({"side": side, "at": (x, -s["lengths"]["l"]),
                    "shoulder": shoulder(s, hi)})
    return out


def build_plate(d, s, lay, kind, proc, mat):
    """A frame plate, in the leg's frame (the crank axis at the origin):
    round the crankshaft's disc, both frame pivots and the standoffs.
    `first` carries the pivots and the standoffs and has no sockets, `last`
    has the sockets alone, `middle` both."""
    L = s["lengths"]
    bp = b_pins(s, lay)
    circles = [((0.0, 0.0), s["rd"] + s["gap_disc"] / 2.0 + s["w"])]
    circles += [(b["at"], s["w"]) for b in bp]
    circles += [(p, s["standoff"] / 2.0 + s["w"] / 2.0)
                for p in s["standoffs"]]
    part = Part(d, f"plate {kind}", proc, mat)
    part.extrude(0.0, circle_hull(circles) + [
        circle((0, 0), s["rd"] + s["gap_disc"] / 2.0)], s["tp"],
        "new_body", "plate")
    if kind != "last":
        for b in bp:
            part.cylinders(s["tp"], [b["at"]], s["dp"], b["shoulder"], "add",
                           f"pivot {b['side']}")
            part.cylinders(s["tp"] + b["shoulder"], [b["at"]], s["ds"],
                           s["tc"], "add", f"pivot stub {b['side']}")
        part.cylinders(s["tp"], s["standoffs"], s["standoff"], s["slab"],
                       "add", "standoffs")
        part.cylinders(s["tp"] + s["slab"], s["standoffs"], s["ds"],
                       s["socket"] - s["gz"], "add", "standoff stubs")
    if kind != "first":
        part.extrude(0.0, [circle(p, s["key_bore"] / 2.0)
                           for p in s["standoffs"]], s["socket"], "cut",
                     "sockets")
    return part.finish()


def disc_angle(s, step_deg):
    """Where a middle disc's socket is, from its own crank pin: one phase
    back, where the last throw's pin stands."""
    a = math.radians(-step_deg)
    return (s["lengths"]["m"] * math.cos(a), s["lengths"]["m"] * math.sin(a))


def d_round(c, r, toward):
    """A round of radius r about c with a flat r/sqrt 2 off its centre on
    the side at angle `toward` (radians): the D a crank pin prints lying on
    (no face under it past 45 degrees), and the socket that keys it."""
    a0 = toward + math.pi / 4.0
    a1 = toward - math.pi / 4.0 + 2.0 * math.pi
    p0 = (c[0] + r * math.cos(a0), c[1] + r * math.sin(a0))
    p1 = (c[0] + r * math.cos(a1), c[1] + r * math.sin(a1))
    return [{"type": "arc", "center": [float(c[0]), float(c[1])],
             "radius": float(r), "start_angle": a0, "end_angle": a1},
            line(p1, p0)]


def pin_socket(s, angle):
    """A crank pin's socket at the crank radius at `angle` in a disc's own
    frame: the pin's D, its flat toward the crank axis."""
    m = s["lengths"]["m"]
    c = (m * math.cos(angle), m * math.sin(angle))
    return d_round(c, s["pin_bore"] / 2.0, angle + math.pi)


def build_disc(d, s, kind, step_deg, proc, mat):
    """A crankshaft segment, printed flat: a disc in its plate with a
    D-socket on its top face at angle 0 for its own throw's crank pin, and
    (a middle disc) one on its underside one phase back for the last
    throw's. The crank pins are parts of their own, printed lying down
    (build_crank_pin): no pin stands across its layers in the torque's
    path. `first` stands on the hand crank's flange and arm; `last` is the
    end disc, its one socket and its flange, printed flange down. Returns
    the z of the disc's plate-side face in the part (where it meets its
    plate's bottom)."""
    part = Part(d, f"disc {kind}", proc, mat)
    if kind in ("first", "last"):
        part.cylinders(0.0, [(0, 0)], 2 * s["rf"], s["tf"], "new_body",
                       "flange")
        if kind == "first":
            part.extrude(0.0, stadium((0.0, 0.0), (-s["arm"], 0.0),
                                      s["w"] / 2.0 + s["wall"]),
                         s["tf"], "add", "arm")
            part.extrude(0.0, [circle((-s["arm"], 0.0),
                                      s["pin_bore"] / 2.0)],
                         s["tf"], "cut", "handle bore")
        z = s["tf"]
        part.cylinders(z, [(0, 0)], 2 * s["rd"], s["gz"] + s["tp"], "add",
                       "disc")
        top = z + s["gz"] + s["tp"]
    else:
        part.cylinders(0.0, [(0, 0)], 2 * s["rd"], s["tp"], "new_body",
                       "disc")
        part.extrude(0.0, pin_socket(s, math.radians(-step_deg)),
                     s["socket"], "cut", "socket below")
        top = s["tp"]
    # The socket its own throw's pin stands in (the last disc's is its one
    # socket, from its top, the face it turns over onto).
    part.extrude(top, pin_socket(s, 0.0), s["socket"], "cut",
                 "socket" if kind == "last" else "socket above",
                 reversed_=True)
    part.finish()
    return top


def build_crank_pin(d, s, proc, mat):
    """A crank pin, printed lying on its flat (its axis along x, the D's
    flat on the bed), so the torque that bends it between two discs bends
    it ALONG its layers. It spans its slab and reaches into the discs'
    sockets on both sides, a gap's play short of each socket's floor."""
    length = s["slab"] + 2.0 * (s["socket"] - s["gz"])
    r = s["dp"] / 2.0
    h = r / math.sqrt(2.0)  # the axis over the flat
    s["crank_pin_len"] = length
    s["crank_pin_h"] = h
    part = Part(d, "crank pin", proc, mat)
    sk = check(d.call("add_feature", feature={
        "type": "Sketch", "name": "crank pin section",
        "plane": {"origin": [0.0, 0.0, 0.0], "normal": [1, 0, 0],
                  "uAxis": [0, 1, 0]},
        "entities": d_round((0.0, h), r, -math.pi / 2.0)}),
        "crank pin")["id"]
    check(d.call("add_feature", feature={
        "type": "Extrude", "name": "crank pin", "sketch_id": sk,
        "distance": length, "mode": "new_body"}), "crank pin")
    return part.finish()


# ---- strength ---------------------------------------------------------------

# The friction between the feet and the floor (mu, static): 0.5, a STATED
# upper value for dry printed PLA on a table. Published measurements of
# printed PLA are against steel and vary with the load (Stoimenov et al.,
# "Static and Kinetic Friction of 3D Printed Polymers and Composites",
# Tribology in Industry 46(1), 2024); none is on a wooden table. To
# confirm: tilt the walker on its floor until it slides, mu = tan(angle).
FOOT_MU = 0.5
# The walk's own torque is doubled for what the statics leaves out -- the
# joints' friction, a rough floor -- before it is set against the most
# the feet let a hand put through (the slip case).
WALK_MARGIN = 2.0
# Every pin studied must hold at least this factor, across its layers and
# against the yield, or the run stops: the hook's design factor.
PIN_NEED = 2.0


def walker_weight(d, s, lay, frames, throws):
    """The walker's weight, N: every printed piece's mass_properties, the
    printed estimate where its settings give one, else solid (more, so the
    loads err high)."""
    grams, how = 0.0, set()
    seen = {}
    for it in print_items(s, lay, frames, throws):
        part = it["part"]
        if part not in seen:
            mp = d.call("mass_properties", part=part)["total"]
            pr = (mp.get("printed") or {}).get("mass_g")
            seen[part] = (pr, "printed") if pr else (mp["mass_g"], "solid")
        g, kind = seen[part]
        grams += g
        how.add(kind)
    return grams / 1000.0 * 9.81, grams, sorted(how)


def band_z(z0, z1):
    return {"normal": [0.0, 0.0, 1.0], "from_mm": z0, "to_mm": z1}


def strength(d, s, lay, frames, throws, step_deg, hand_n, mu):
    """Every pin in a load path, studied as it prints (_lib.pin_strength)
    under the walker's own loads (linkage.walking_loads): the weight on the
    feet that stand, and the most friction the floor gives. The crank's
    design torque is the larger of the walk's (doubled, WALK_MARGIN) and
    the slip's -- or `hand_n` on the knob, when stated; the joints' loads
    likewise. Studied: a crank pin carrying that torque between two discs
    and its crank bars' pull, the handle under the push that makes it, each
    joint pin of a leg pair, and the frame pivots. Under PIN_NEED either
    way the run stops (pin_strength)."""
    L = s["lengths"]
    m = L["m"]
    weight, grams, how = walker_weight(d, s, lay, frames, throws)
    lo = lk.walking_loads(L, throws, math.radians(step_deg), weight, mu)
    t_walk = lo["walk"]["torque_nmm"]
    t_slip = lo["slip"]["torque_nmm"]
    if hand_n is not None:
        t_d, governs = hand_n * s["arm"], "the stated hand push"
    elif WALK_MARGIN * t_walk >= t_slip:
        t_d, governs = WALK_MARGIN * t_walk, "the walk, doubled"
    else:
        t_d, governs = t_slip, "the slip: the most the feet let a hand put through"
    # The joints at the same case; a stated push beyond the slip scales
    # them with it (the legs then carry it, the walker held).
    scale = max(1.0, t_d / t_slip)
    pins_n = {j: scale * max(WALK_MARGIN * lo["walk"]["pins_n"][j],
                             lo["slip"]["pins_n"][j])
              for j in lo["slip"]["pins_n"]}
    out = {"weight_n": weight, "mass_g": grams, "mass_from": how,
           "mu": mu, "mu_source": "stated (FOOT_MU)",
           "walk_margin": WALK_MARGIN, "torque_walk_nmm": t_walk,
           "torque_slip_nmm": t_slip, "crank_height_mm": lo["height_mm"],
           "design_torque_nmm": t_d, "governs": governs,
           "hand_n": hand_n, "pin_loads_n": pins_n, "need": PIN_NEED,
           "studies": {}}
    print(f"  loads: walker {grams:.0f} g ({'/'.join(how)}), mu {mu:g}: "
          f"walking {t_walk:.1f} N mm, slipping {t_slip:.1f} N mm; design "
          f"{t_d:.1f} N mm ({governs})")

    def faces(part):
        d.call("set_active_part", part=part)
        return d.call("list_elements")["bodies"][0]["faces"]

    def rounds(fs, r, near=None):
        return [f["name"] for f in fs if f["kind"] == "cylinder" and
                abs(f.get("radius", 0.0) - r) < 1e-6 and
                (near is None or math.hypot(f["centroid"][0] - near[0],
                                            f["centroid"][1] - near[1])
                 < 1e-3)]

    st = out["studies"]
    # The crank pin, lying along x: torque between its discs. The pin is
    # held in both sockets, which turn together, so it bends as a beam
    # fixed at both ends and offset at one -- two cantilevers meeting at
    # mid-span. One of them: held in a socket, the force at the middle.
    Lp = s["crank_pin_len"]
    xs = s["socket"] - s["gz"]
    fs = faces("crank pin")
    pin = rounds(fs, s["dp"] / 2.0)
    if len(pin) != 1:
        raise SystemExit(f"strandbeest: the crank pin's round: {pin}")
    bx = lambda x0, x1: {"normal": [1.0, 0.0, 0.0],  # noqa: E731
                         "from_mm": x0, "to_mm": x1}
    mid = bx(Lp / 2.0 - s["t"] / 2.0, Lp / 2.0 + s["t"] / 2.0)
    st["crank pin, torque"] = pin_strength(
        d, "crank pin", pin[0], t_d / m, [0.0, 1.0, 0.0],
        seat={"face": pin[0], "region": bx(0.0, xs)},
        layer_normal=[0.0, 0.0, 1.0], region=mid,
        name="crank pin: the design torque", need=PIN_NEED)
    # Its crank bars' pull, in the slab, held in both sockets.
    st["crank pin, bars"] = pin_strength(
        d, "crank pin", pin[0], pins_n["A"], [0.0, 0.0, -1.0],
        seat=[{"face": pin[0], "region": bx(0.0, xs)},
              {"face": pin[0], "region": bx(Lp - xs, Lp)}],
        layer_normal=[0.0, 0.0, 1.0], region=mid,
        name="crank pin: its bars", need=PIN_NEED)
    # The handle, standing on its knob: the push that makes the torque, on
    # the knob, its stub held in the arm.
    fs = faces("handle")
    stub, knob = rounds(fs, s["dp"] / 2.0), rounds(fs, s["handle_d"] / 2.0)
    if len(stub) != 1 or len(knob) != 1:
        raise SystemExit(f"strandbeest: the handle's faces: {stub} {knob}")
    st["handle"] = pin_strength(
        d, "handle", knob[0], t_d / s["arm"], [1.0, 0.0, 0.0], seat=stub[0],
        layer_normal=[0.0, 0.0, 1.0], name="handle: the push",
        need=PIN_NEED)
    # Each joint pin, standing on its head: the band of its top body
    # loaded, the other bodies' bands holding it.
    for side in SIDES:
        for j in lk.PINS:
            part = f"pin {side} {j}"
            info = pin_span(lay, side, j)
            z0 = z_bottom(s, info[0]) - s["th"]
            bodies = sorted(lk.JOINTS[j],
                            key=lambda b: layer_of(lay, side, b))
            bands = {b: band_z(z_bottom(s, layer_of(lay, side, b)) - z0,
                               z_bottom(s, layer_of(lay, side, b)) +
                               s["t"] - z0) for b in bodies}
            shaft = rounds(faces(part), s["dp"] / 2.0)
            if len(shaft) != 1:
                raise SystemExit(f"strandbeest: {part}'s shaft: {shaft}")
            st[part] = pin_strength(
                d, part, shaft[0], pins_n[j], [1.0, 0.0, 0.0],
                seat=[{"face": shaft[0], "region": bands[b]}
                      for b in bodies[:-1]],
                layer_normal=[0.0, 0.0, 1.0], region=bands[bodies[-1]],
                name=f"{part}: its legs' load", need=PIN_NEED)
    # The frame pivots, printed on the first plate: held by the plate, the
    # load on the band of their higher body.
    fs = faces("plate first")
    base = [f["name"] for f in fs if f["kind"] == "plane" and
            abs(f["centroid"][2]) < 1e-6]
    for b in b_pins(s, lay):
        side = b["side"]
        piv = rounds(fs, s["dp"] / 2.0, near=b["at"])
        if len(piv) != 1 or not base:
            raise SystemExit(f"strandbeest: the {side} pivot's faces: "
                             f"{piv} {base}")
        top = max(layer_of(lay, side, x) for x in lk.JOINTS["B"])
        z = s["tp"] + z_bottom(s, top)
        st[f"pivot {side}"] = pin_strength(
            d, "plate first", piv[0], pins_n["B"], [1.0, 0.0, 0.0],
            seat=base, layer_normal=[0.0, 0.0, 1.0],
            region=band_z(z, z + s["t"]),
            name=f"{side} pivot: its legs' load", need=PIN_NEED)
    out["least_factor"] = min(
        min(v["interlayer_factor_of_safety"], v["factor_of_safety"])
        for v in st.values())
    print(f"  strength: {len(st)} pin studies, the least factor "
          f"{out['least_factor']:.2f} (at least {PIN_NEED:g})")
    return out


def build_handle(d, s, proc, mat):
    part = Part(d, "handle", proc, mat)
    part.cylinders(0.0, [(0, 0)], s["handle_d"], s["handle_h"], "new_body",
                   "knob")
    # Its stub is a crank pin's size, not a cap's: it carries the hand's
    # push across its layers (strength(), below).
    part.cylinders(s["handle_h"], [(0, 0)], s["dp"], s["tf"], "add", "stub")
    return part.finish()


# ---- assembled parts --------------------------------------------------------

def q_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def q_z(a):
    return (0.0, 0.0, math.sin(a / 2.0), math.cos(a / 2.0))


Q_X180 = (1.0, 0.0, 0.0, 0.0)
# The walker stands up: its legs move in the world's XZ plane, the crank
# axis along -Y, the world Z-up -- part (x, y, z) at world (x, -z, y).
Q_STAND = (math.sin(math.pi / 4.0), 0.0, 0.0, math.cos(math.pi / 4.0))


def stand(p):
    return (p[0], -p[2], p[1])


def derived(d, label, source, position, orientation=(0.0, 0.0, 0.0, 1.0)):
    check(d.call("add_feature", feature={
        "type": "Derived", "name": label, "source_part": part_id(d, source),
        "reference": False,
        "placement": {"position": [float(v) for v in position],
                      "orientation": [float(v) for v in orientation]}}),
        f"derived {label}")


_PART_IDS = {}


def part_id(d, name):
    if name not in _PART_IDS:
        for p in d.call("list_parts")["parts"]:
            if p["type"] == "part":
                _PART_IDS[p["name"]] = p["id"]
    if name not in _PART_IDS:
        raise SystemExit(f"strandbeest: no part '{name}'")
    return _PART_IDS[name]


def mat_quat(rows):
    """The quaternion (x, y, z, w) of the rotation whose matrix has these
    ROWS (each the world x, y or z of the frame's three axes)."""
    (m00, m01, m02), (m10, m11, m12), (m20, m21, m22) = rows
    tr = m00 + m11 + m22
    if tr > 0.0:
        k = 0.5 / math.sqrt(tr + 1.0)
        return ((m21 - m12) * k, (m02 - m20) * k, (m10 - m01) * k,
                0.25 / k)
    if m00 > m11 and m00 > m22:
        k = 2.0 * math.sqrt(1.0 + m00 - m11 - m22)
        return (0.25 * k, (m01 + m10) / k, (m02 + m20) / k, (m21 - m12) / k)
    if m11 > m22:
        k = 2.0 * math.sqrt(1.0 + m11 - m00 - m22)
        return ((m01 + m10) / k, 0.25 * k, (m12 + m21) / k, (m02 - m20) / k)
    k = 2.0 * math.sqrt(1.0 + m22 - m00 - m11)
    return ((m02 + m20) / k, (m12 + m21) / k, 0.25 * k, (m10 - m01) / k)


def throw_angle(p, step_deg):
    return math.radians(p * step_deg)


def build_frame(d, s, lay, throws, proc, mat):
    """The frame as assembled: its plates where they stand. The pivots'
    caps are occurrences of their own, fastened on, their press a printed
    press the exact clash check relieves
    (docs/tolerances/README.md#printed-press-fits)."""
    name = f"frame x{throws}"
    d.call("add_part", name=name)
    d.call("set_print_process", **proc)
    for p in range(throws + 1):
        kind = "first" if p == 0 else "last" if p == throws else "middle"
        derived(d, f"plate {p}", f"plate {kind}",
                (0.0, 0.0, slab_base(s, p) - s["tp"]))
    d.call("add_feature", feature={
        "type": "Frame", "name": "O", "kind": "explicit",
        "origin": [0, 0, 0], "z_axis": [0, 0, 1], "x_axis": [1, 0, 0]})
    for p in range(throws):
        for b in b_pins(s, lay):
            d.call("add_feature", feature={
                "type": "Frame", "name": f"B {b['side']} {p}",
                "kind": "explicit",
                "origin": [b["at"][0], b["at"][1], slab_base(s, p)],
                "z_axis": [0, 0, 1], "x_axis": [1, 0, 0]})
            d.call("add_feature", feature={
                "type": "Frame", "name": f"cap {b['side']} {p}",
                "kind": "explicit",
                "origin": [b["at"][0], b["at"][1],
                           slab_base(s, p) + b["shoulder"]],
                "z_axis": [0, 0, 1], "x_axis": [1, 0, 0]})
    # The pivots' stubs: a Derived copy carries no tolerance of its source,
    # so the frame marks its own.
    printed_press(d, name, s["ds"], [b["at"] for b in b_pins(s, lay)])
    return name


def build_crankshaft(d, s, throws, step_deg, tops, proc):
    """The crankshaft as assembled: the first disc on its hand crank, a
    middle disc per further throw turned to its phase, the end disc turned
    over, the handle pressed into the arm."""
    name = f"crankshaft x{throws}"
    d.call("add_part", name=name)
    d.call("set_print_process", **proc)
    derived(d, "disc 0", "disc first",
            (0.0, 0.0, -s["tp"] - s["gz"] - s["tf"]), q_z(0.0))
    for p in range(1, throws):
        derived(d, f"disc {p}", "disc middle",
                (0.0, 0.0, slab_base(s, p) - s["tp"]),
                q_z(throw_angle(p, step_deg)))
    derived(d, f"disc {throws}", "disc last",
            (0.0, 0.0, slab_base(s, throws) + s["gz"] + s["tf"]),
            q_mul(q_z(throw_angle(throws - 1, step_deg)), Q_X180))
    # The crank pins, each standing in its slab between two discs' sockets,
    # its flat toward the crank axis as the sockets' D is.
    m = s["lengths"]["m"]
    h = s["crank_pin_h"]
    for p in range(throws):
        a = throw_angle(p, step_deg)
        c, sn = math.cos(a), math.sin(a)
        q = mat_quat(((0.0, sn, c), (0.0, -c, sn), (1.0, 0.0, 0.0)))
        derived(d, f"crank pin {p}", "crank pin",
                ((m - h) * c, (m - h) * sn,
                 slab_base(s, p) - (s["socket"] - s["gz"])), q)
    derived(d, "handle", "handle",
            (-s["arm"], 0.0, -s["tp"] - s["gz"] - s["tf"] - s["handle_h"]))
    d.call("add_feature", feature={
        "type": "Frame", "name": "O", "kind": "explicit",
        "origin": [0, 0, 0], "z_axis": [0, 0, 1], "x_axis": [1, 0, 0]})
    for p in range(throws):
        a = throw_angle(p, step_deg)
        d.call("add_feature", feature={
            "type": "Frame", "name": f"A {p}", "kind": "explicit",
            "origin": [m * math.cos(a), m * math.sin(a), slab_base(s, p)],
            "z_axis": [0, 0, 1], "x_axis": [1, 0, 0]})
    return name


# ---- the assembly -----------------------------------------------------------

def place(d, part, name, pos, angle, fixed=False):
    """An occurrence at walker position `pos` turned `angle` about the crank
    axis, the walker stood up (Q_STAND)."""
    q = q_mul(Q_STAND, q_z(angle))
    return d.call("add_occurrence", part=part, name=name, fixed=fixed,
                  transform={"position": list(stand(pos)),
                             "orientation": list(q)})["id"]


def wrap_deg(a):
    a = math.degrees(a) % 360.0
    return a - 360.0 if a > 180.0 else a


def build_assembly(d, s, lay, throws, step_deg, name, parts, L=None,
                   frames=None, layers=None):
    """The walker of `throws` throws: the frame fixed, the crankshaft on a
    revolute at O, and per throw a front and a back leg on its crank pin,
    each pin fastened to the bar under its head. Every occurrence is
    placed where the leg's own kinematics put it at crank angle 0, and
    every revolute starts at the angle it is placed at, so the loops start
    closed on the built branch. `L`, `frames` and `layers` override the
    leg (the negative controls). Returns the ids a study and its checks
    need."""
    L = L or s["lengths"]
    d.call("add_assembly", name=name)
    ids = {"bars": {}, "pins": {}, "caps": {}, "throws": throws}
    ids["frame"] = place(d, parts["frame"], "frame", (0, 0, 0), 0.0,
                         fixed=True)
    ids["crankshaft"] = place(d, parts["crankshaft"], "crankshaft",
                              (0, 0, 0), 0.0)
    mates = []
    mates.append(("revolute", ids["frame"], "O", ids["crankshaft"], "O",
                  0.0, "crank"))
    for p in range(throws):
        theta = throw_angle(p, step_deg)
        zb = slab_base(s, p)
        for side in SIDES:
            mirror = side == "back"
            bodies, joints = world_pose(L, theta, mirror)
            occ = {}
            for bar in lk.BARS:
                ell = (layers or {}).get((side, bar),
                                         layer_of(lay, side, bar))
                (ox, oy), ang = bodies[bar]
                occ[bar] = place(d, parts["bars"][(side, bar)],
                                 f"{side} {bar} {p}",
                                 (ox, oy, zb + z_bottom(s, ell)), ang)
                ids["bars"][(p, side, bar)] = occ[bar]

            def rev(a, ja, b, jb, what):
                mates.append(("revolute", occ[a] if a in occ else a, ja,
                              occ[b], jb,
                              wrap_deg(bodies[b][1] - (bodies[a][1]
                                                       if a in occ else
                                                       theta)), what))
            # The crank pin, the frame pivot, and the joints between bars.
            for bar in ("j", "k"):
                mates.append(("revolute", ids["crankshaft"], f"A {p}",
                              occ[bar], "A", wrap_deg(bodies[bar][1] - 0.0),
                              f"A {side} {bar} {p}"))
            for bar in ("bcd", "c"):
                mates.append(("revolute", ids["frame"], f"B {side} {p}",
                              occ[bar], "B", wrap_deg(bodies[bar][1]),
                              f"B {side} {bar} {p}"))
            rev("j", "C", "bcd", "C", f"C {side} {p}")
            rev("bcd", "D", "f", "D", f"D {side} {p}")
            rev("c", "E", "efg", "E", f"E {side} c {p}")
            rev("k", "E", "c", "E", f"E {side} k {p}")
            rev("f", "F", "efg", "F", f"F {side} {p}")
            for joint in lk.PINS:
                lo, _ = pin_span(lay, side, joint)
                head = min(lk.JOINTS[joint],
                           key=lambda b: layer_of(lay, side, b))
                jx, jy = joints[joint]
                pin = place(d, parts["pins"][(side, joint)],
                            f"pin {side} {joint} {p}",
                            (jx, jy, zb + z_bottom(s, lo) - s["th"]),
                            bodies[head][1])
                ids["pins"][(p, side, joint)] = pin
                mates.append(("fastened", occ[head], joint, pin, "J", 0.0,
                              f"pin {side} {joint} {p}"))
                # Its cap, pressed on: an occurrence of its own on a
                # fastened mate, the press relieved as a printed press.
                info = parts["pin_info"][(side, joint)]
                cap = place(d, "cap", f"cap {side} {joint} {p}",
                            (jx, jy, zb + z_bottom(s, lo) - s["th"] +
                             info["shoulder"] - info["z0"]),
                            bodies[head][1])
                ids["caps"][(p, side, joint)] = cap
                mates.append(("fastened", pin, "cap", cap, "seat", 0.0,
                              f"cap {side} {joint} {p}"))
        for b in b_pins(s, lay):
            cap = place(d, "cap", f"cap {b['side']} B {p}",
                        (b["at"][0], b["at"][1], zb + b["shoulder"]), 0.0)
            ids["caps"][(p, b["side"], "B")] = cap
            mates.append(("fastened", ids["frame"], f"cap {b['side']} {p}",
                          cap, "seat", 0.0, f"cap {b['side']} B {p}"))
    t0 = time.perf_counter()
    calls = []
    for kind, a, fa, b, fb, rot, what in mates:
        calls.append({"method": "add_mate", "params": {
            "type": kind, "name": what, "flipped": False,
            "rotation_deg": rot,
            "a": {"occurrence": a, "frame": fa},
            "b": {"occurrence": b, "frame": fb}}})
    r = d.request("batch", {"calls": calls}, timeout=3600)
    if r.get("failed"):
        raise SystemExit(f"strandbeest: {name}: a mate was refused: "
                         f"{r.get('failed')}")
    # The crank is the first mate: the batch says the id it created.
    ids["crank"] = r["results"][0]["id"]
    ids["mates"] = len(mates)
    ids["revolutes"] = sum(1 for m in mates if m[0] == "revolute")
    rep = d.request("solve_assembly", {}, timeout=3600)
    ids["solve"] = {k: rep.get(k) for k in ("dof", "errored_mates",
                                            "loops_closed", "residual_mm",
                                            "loop_mates")}
    ids["mate_seconds"] = time.perf_counter() - t0
    if rep.get("errored_mates") or not rep.get("loops_closed", True):
        bad = [m for m in rep.get("mates", []) if m["status"] == "error"]
        raise SystemExit(f"strandbeest: {name} does not assemble: "
                         f"{ids['solve']} {bad[:4]}")
    return ids


# ---- the study and what it must show ---------------------------------------

def foot_local(s, frames, side):
    """The foot's point on its triangle, mid-thickness: the vertex G, whose
    rounded corner (radius w/2) is what touches the ground."""
    g = frames[side]["efg"]["G"]
    return [g[0], g[1], s["t"] / 2.0]


def joint_traces(s, ids, frames, throws):
    """The points the checks read: each leg's foot, and the joints its knees
    are judged by, each on a body that carries it."""
    tr = []
    for p in range(throws):
        for side in SIDES:
            f = frames[side]

            def on(bar, joint, label):
                x, y = f[bar][joint]
                tr.append({"name": f"{label} {side} {p}",
                           "occurrence": ids["bars"][(p, side, bar)],
                           "point_mm": [x, y, s["t"] / 2.0]})
            on("j", "A", "A")
            on("j", "C", "C")
            on("bcd", "B", "B")
            on("f", "D", "D")
            on("k", "E", "E")
            on("f", "F", "F")
            tr.append({"name": f"G {side} {p}",
                       "occurrence": ids["bars"][(p, side, "efg")],
                       "point_mm": foot_local(s, frames, side)})
    return tr


def run_study(d, s, ids, frames, samples, clash, name, extra=()):
    """The turn, its clash check EXACT: the boolean, which relieves each
    cap's printed press on its pin and lists it in `press_fits`, where a
    mesh calls it the overlap it is."""
    st = d.call("add_motion_study", driver=ids["crank"], dof="rotation",
                **{"from": 0.0}, to=360.0, samples=samples,
                check_clash=clash, clash_mode="exact", name=name)
    if st.get("why_not"):
        raise SystemExit(f"strandbeest: {name}: {st['why_not']}")
    trace = joint_traces(s, ids, frames, ids["throws"]) + list(extra)
    t0 = time.perf_counter()
    r = d.request("run_motion_study", {"study": st["id"], "trace": trace},
                  timeout=3600)
    r["wall_seconds"] = time.perf_counter() - t0
    r["study"] = st["id"]
    r["paths"] = {t["name"]: t["points_mm"] for t in r["trace"]}
    return r


def leg_xy(pt):
    """A traced world point back in the leg's frame: x along, y up."""
    return (pt[0], pt[2])


def check_branch(r, throws):
    """Every sample solved, and every knee of every leg on the side it was
    built on (linkage.KNEES)."""
    bad = [smp["index"] for smp in r["samples"] if not smp["solved"]]
    if bad:
        raise SystemExit(f"strandbeest: unsolved at samples {bad[:10]}")
    worst = {}
    for p in range(throws):
        for side in SIDES:
            paths = {j: r["paths"][f"{j} {side} {p}"] for j in "ABCDEF"}
            built = None
            for i in range(len(r["samples"])):
                q = {j: leg_xy(paths[j][i]) for j in paths}
                signs = lk.knee_signs(q)
                built = built or signs
                if signs != built:
                    raise SystemExit(
                        f"strandbeest: the {side} leg of throw {p} left its "
                        f"branch at sample {i} ({r['samples'][i]['value']:.1f}"
                        f" deg): knees {signs}, built {built}")
            worst[f"{side} {p}"] = built
    return {"samples": len(r["samples"]), "every_sample_solved": True,
            "knee_signs": worst}


def windowed_flatness(curve, span):
    """The flattest the curve gets over any run of samples spanning `span`
    along x: how far a foot rises over a stride that long."""
    n = len(curve)
    best = None
    for start in range(n):
        xs, ys = [], []
        for k in range(n):
            x, y = curve[(start + k) % n]
            xs.append(x)
            ys.append(y)
            if max(xs) - min(xs) >= span:
                v = max(ys) - min(ys)
                best = v if best is None else min(best, v)
                break
    return best if best is not None else float("inf")


def foot_curves(r, s, throws):
    """Each foot's ground point (G less the corner's radius) per sample."""
    out = {}
    for p in range(throws):
        for side in SIDES:
            pts = r["paths"][f"G {side} {p}"]
            out[(p, side)] = [(x, z - s["w"] / 2.0) for x, _, z in pts]
    return out


def reference_curve(s, samples, side, phase):
    """The scaled Jansen curve the foot must walk: the leg's own kinematics
    (linkage.pose), its ground point the vertex less the corner's radius."""
    L = s["lengths"]
    out = []
    for i in range(samples):
        th = phase + 2.0 * math.pi * i / (samples - 1)
        x, y = lk.pose(L, th, side == "back")["G"]
        out.append((x, y - s["w"] / 2.0))
    return out


def check_foot(r, s, throws, step_deg, ref_L=None):
    """The foot against the scaled Jansen curve: every sample within a
    hundredth of the crank radius of it, and the stride (the flat run's
    length, the foot within the band of its lowest), its lift and its
    flatness over a stride's length matching the curve's. The band is 2 %
    of the crank radius: a printed foot flexes more than that. Returns the
    numbers, and `ok`."""
    samples = len(r["samples"])
    band = 0.02 * s["size"]
    tol = 0.01 * s["size"]
    feet = foot_curves(r, s, throws)
    out = {"band_mm": band, "tolerance_mm": tol, "legs": {}}
    ok = True
    ref_s = dict(s)
    if ref_L:
        ref_s["lengths"] = ref_L
    for (p, side), curve in feet.items():
        ref = reference_curve(ref_s, samples, side,
                              throw_angle(p, step_deg))
        dev = max(math.hypot(a[0] - b[0], a[1] - b[1])
                  for a, b in zip(curve, ref))
        closed = curve[:-1]
        mine = lk.stride(closed, band)
        theirs = lk.stride(ref[:-1], band)
        span = theirs["stride_mm"] - tol
        flat = windowed_flatness(closed, span)
        flat_ref = windowed_flatness(ref[:-1], span)
        leg = {"deviation_mm": dev, "stride_mm": mine["stride_mm"],
               "stride_ref_mm": theirs["stride_mm"],
               "lift_mm": mine["lift_mm"], "lift_ref_mm": theirs["lift_mm"],
               "flatness_mm": flat, "flatness_ref_mm": flat_ref,
               "lowest_mm": mine["lowest_mm"],
               "on_ground": mine["fraction_on_ground"]}
        leg["ok"] = (dev <= tol
                     and abs(mine["stride_mm"] - theirs["stride_mm"])
                     <= 2 * tol + 1e-9
                     and abs(mine["lift_mm"] - theirs["lift_mm"]) <= tol
                     and flat <= flat_ref + band)
        ok = ok and leg["ok"]
        out["legs"][f"{side} {p}"] = leg
    out["ok"] = ok
    return out


def walk(r, s, throws):
    """What the walker does over a turn, from its feet: it stands on the
    lowest, which carry it, so it moves by the lowest foot's travel back
    along x at every sample -- summed over the turn, the distance walked per
    crank turn. The step it clears is the foot's lift."""
    feet = foot_curves(r, s, throws)
    n = len(r["samples"])
    dist = 0.0
    for i in range(n - 1):
        low = min(feet, key=lambda k: feet[k][i][1])
        dist += feet[low][i + 1][0] - feet[low][i][0]
    lifts = [max(y for _, y in c) - min(y for _, y in c)
             for c in feet.values()]
    return {"distance_per_turn_mm": abs(dist),
            "direction": "-x" if dist > 0 else "+x",
            "step_height_mm": min(lifts)}


def com_traces(d, ids_all, part_of):
    """A trace at each occurrence's own centre of mass (mass_properties of
    its part), and its mass: the walker's centre of mass at every sample is
    their mean, weighted."""
    props = {}
    tr, mass = [], {}
    for occ, part in part_of.items():
        if part not in props:
            mp = d.call("mass_properties", part=part)["total"]
            if mp.get("mass_g") is None:
                raise SystemExit(f"strandbeest: {part} has no mass (a "
                                 f"body without a material)")
            props[part] = (mp["mass_g"], mp["center_of_mass_mm"])
        g, c = props[part]
        tr.append({"name": f"com {occ}", "occurrence": occ, "point_mm": c})
        mass[f"com {occ}"] = g
    return tr, mass


def check_stability(r, s, throws, mass):
    """At every sample: the walker stands on the feet within the band of
    the lowest (2 % of the crank radius, as the stride's), and its centre
    of mass must lie inside the polygon they make on the ground, by a
    margin. Returns the worst margin and where."""
    band = 0.02 * s["size"]
    total = sum(mass.values())
    feet = {}
    for p in range(throws):
        for side in SIDES:
            feet[(p, side)] = r["paths"][f"G {side} {p}"]
    worst = None
    for i in range(len(r["samples"])):
        com = [sum(mass[n] * r["paths"][n][i][k] for n in mass) / total
               for k in range(3)]
        zs = {k: v[i][2] for k, v in feet.items()}
        low = min(zs.values())
        on = [(feet[k][i][0], feet[k][i][1]) for k in feet
              if zs[k] - low <= band]
        poly = hull(on)
        if len(poly) < 3:
            margin = -float("inf")
        else:
            margin = min(
                lk.cross(poly[j], poly[(j + 1) % len(poly)],
                         (com[0], com[1])) /
                math.hypot(poly[(j + 1) % len(poly)][0] - poly[j][0],
                           poly[(j + 1) % len(poly)][1] - poly[j][1])
                for j in range(len(poly)))
        if worst is None or margin < worst["margin_mm"]:
            worst = {"margin_mm": margin, "sample": i,
                     "value_deg": r["samples"][i]["value"],
                     "feet_on_ground": len(on), "com_mm": com,
                     "support": poly}
    worst["ok"] = worst["margin_mm"] > 0.0
    worst["mass_g"] = total
    worst["band_mm"] = band
    return worst


def adjacent_pairs(s, lay, theta):
    """Slab 0's pairs of bars in adjacent layers that overlap seen along the
    crank axis at crank angle theta: where the gap between two layers is
    the only air."""
    L = s["lengths"]
    out = []
    els = [(sd, b) for sd in SIDES for b in lk.BARS]
    poses = {sd: lk.pose(L, theta, sd == "back") for sd in SIDES}
    for i in range(len(els)):
        for j in range(i + 1, len(els)):
            a, b = els[i], els[j]
            if abs(layer_of(lay, *a) - layer_of(lay, *b)) != 1:
                continue
            ea = lk.edges([poses[a[0]][n] for n in lk.BARS[a[1]]])
            eb = lk.edges([poses[b[0]][n] for n in lk.BARS[b[1]]])
            if lk.skel_dist(ea, eb) < s["w"]:
                out.append((a, b))
    return out


def check_layer_clearance(d, s, lay, ids, r, step_deg, every):
    """The air between adjacent layers over the study: at every `every`th
    sample, slab 0's overlapping adjacent-layer pairs measured exactly with
    the assembly left there. Slab 0 stands for every throw: they are the
    same leg pair a phase apart, and the samples cover the turn."""
    worst = None
    n = 0
    for i in range(0, len(r["samples"]) - 1, every):
        d.call("run_motion_study", study=r["study"], leave_at=i)
        theta = math.radians(r["samples"][i]["value"])
        for a, b in adjacent_pairs(s, lay, theta):
            m = d.call("measure",
                       a={"occurrence": ids["bars"][(0,) + a]},
                       b={"occurrence": ids["bars"][(0,) + b]})
            dist = m.get("distance_mm", m.get("distance"))
            n += 1
            if worst is None or dist < worst["min_mm"]:
                worst = {"min_mm": dist, "sample": i,
                         "pair": [" ".join(a), " ".join(b)]}
    d.call("run_motion_study", study=r["study"], leave_at="start")
    worst = worst or {"min_mm": None}
    worst["pairs_measured"] = n
    worst["required_mm"] = s["gz"]
    worst["ok"] = worst["min_mm"] is not None and \
        worst["min_mm"] >= s["gz"] - 1e-6
    return worst


# ---- the bed ----------------------------------------------------------------

def footprint(s, lay, frames, part):
    """A print part's outline on the bed as circles ((x, y), r) in its own
    coordinates: their hull is the part seen from above."""
    kind, *rest = part
    if kind == "bar":
        side, bar = rest
        return [(frames[side][bar][j], s["w"] / 2.0)
                for j in lk.BARS[bar]]
    if kind in ("pin", "cap"):
        return [((0.0, 0.0), s["dh"] / 2.0)]
    if kind == "handle":
        return [((0.0, 0.0), s["handle_d"] / 2.0)]
    if kind == "crankpin":  # lying along x, its section dp wide
        r = s["dp"] / 2.0
        return [((r, 0.0), r), ((s["crank_pin_len"] - r, 0.0), r)]
    if kind == "plate":
        c = [((0.0, 0.0), s["rd"] + s["gap_disc"] / 2.0 + s["w"])]
        c += [(b["at"], s["w"]) for b in b_pins(s, lay)]
        c += [(p, s["standoff"] / 2.0 + s["w"] / 2.0)
              for p in s["standoffs"]]
        return c
    if kind == "disc":
        c = [((0.0, 0.0), s["rf"] if rest[0] in ("first", "last")
              else s["rd"])]
        if rest[0] == "first":
            c.append(((-s["arm"], 0.0), s["w"] / 2.0 + s["wall"]))
        return c
    if kind == "press":
        w, h, cap_y = rest
        return [((0.0, 0.0), 0.0), ((w, 0.0), 0.0), ((0.0, h), 0.0),
                ((w, h), 0.0), ((0.0, cap_y), s["dh"] / 2.0),
                ((w, cap_y), s["dh"] / 2.0)]
    raise ValueError(kind)


def turned_box(circles, a):
    c, sn = math.cos(a), math.sin(a)
    pts = [((x * c - y * sn), (x * sn + y * c), r) for (x, y), r in circles]
    return (min(x - r for x, _, r in pts), min(y - r for _, y, r in pts),
            max(x + r for x, _, r in pts), max(y + r for _, y, r in pts))


def best_turn(circles):
    """The turn about the bed's normal that boxes the part tightest."""
    best = None
    for k in range(0, 180, 3):
        a = math.radians(k)
        x0, y0, x1, y1 = turned_box(circles, a)
        area = (x1 - x0) * (y1 - y0)
        if best is None or area < best[0] - 1e-9:
            best = (area, a)
    return best[1]


def pack(items, bed, gap, zones=()):
    """MaxRects, best short side: each item {name, circles} onto beds of
    `bed` (x, y) mm, `gap` between boxes and from the edges, and from each
    of the bed's keep-out `zones` [x0, y0, x1, y1]. Returns beds of
    [(item, angle, x, y)] -- the part turned by angle, then moved by
    (x, y)."""
    def boxes(it):
        a = best_turn(it["circles"])
        out = []
        for extra in (0.0, math.pi / 2):
            x0, y0, x1, y1 = turned_box(it["circles"], a + extra)
            out.append((a + extra, x0, y0, x1 - x0 + gap, y1 - y0 + gap))
        return out
    size_x, size_y = bed[0] - gap, bed[1] - gap  # the far edges' gap is in
    # each box
    order = sorted(items, key=lambda it: -max(
        b[3] * b[4] for b in boxes(it)))
    beds = []

    def place(free, w, h):
        best = None
        for fi, (fx, fy, fw, fh) in enumerate(free):
            if w <= fw + 1e-9 and h <= fh + 1e-9:
                score = min(fw - w, fh - h)
                if best is None or score < best[0]:
                    best = (score, fi, fx, fy)
        return best

    def split(free, x, y, w, h):
        out = []
        for fx, fy, fw, fh in free:
            if x >= fx + fw or x + w <= fx or y >= fy + fh or y + h <= fy:
                out.append((fx, fy, fw, fh))
                continue
            if x > fx:
                out.append((fx, fy, x - fx, fh))
            if x + w < fx + fw:
                out.append((x + w, fy, fx + fw - x - w, fh))
            if y > fy:
                out.append((fx, fy, fw, y - fy))
            if y + h < fy + fh:
                out.append((fx, y + h, fw, fy + fh - y - h))
        keep = []
        for i, a in enumerate(out):
            inside = any(i != j and a[0] >= b[0] - 1e-9 and a[1] >= b[1] -
                         1e-9 and a[0] + a[2] <= b[0] + b[2] + 1e-9 and
                         a[1] + a[3] <= b[1] + b[3] + 1e-9 and
                         (a != b or j < i) for j, b in enumerate(out))
            if not inside:
                keep.append(a)
        return keep
    for it in order:
        done = False
        for b in beds:
            for a, x0, y0, w, h in boxes(it):
                spot = place(b["free"], w, h)
                if spot:
                    _, _, fx, fy = spot
                    b["items"].append((it, a, fx + gap - x0, fy + gap - y0))
                    b["free"] = split(b["free"], fx, fy, w, h)
                    done = True
                    break
            if done:
                break
        if not done:
            # A box at free-space x spans the bed's x + gap .. x + w, so a
            # zone taken out of the free space from x0 - gap to x1 keeps a
            # gap from it on either side.
            free = [(0.0, 0.0, size_x, size_y)]
            for z in zones:
                free = split(free, z[0] - gap, z[1] - gap,
                             z[2] - z[0] + gap, z[3] - z[1] + gap)
            b = {"free": free, "items": []}
            for a, x0, y0, w, h in boxes(it):
                spot = place(b["free"], w, h)
                if spot:
                    _, _, fx, fy = spot
                    b["items"].append((it, a, fx + gap - x0, fy + gap - y0))
                    b["free"] = split(b["free"], fx, fy, w, h)
                    break
            else:
                raise SystemExit(f"strandbeest: {it['name']} does not fit a "
                                 f"{bed[0]:g} x {bed[1]:g} mm bed")
            beds.append(b)
    return [b["items"] for b in beds]


def build_plate_part(d, name, proc, placed):
    """A print plate: a Derived copy of each piece where it prints."""
    d.call("add_part", name=name)
    d.call("set_print_process", **proc)
    for k, (it, a, x, y) in enumerate(placed):
        derived(d, f"{it['name']} #{k + 1}", it["part"], (x, y, 0.0),
                q_z(a))


def check_plate(d, name, proc, placed, out, bed, quiet=False):
    """check_geometry verified and check_print every item, every piece on
    the bed and inside it, then the STL."""
    d.call("set_active_part", part=name)
    geo = d.call("check_geometry")
    if geo["receipt"]["standing"] != "verified":
        raise SystemExit(f"strandbeest: {name} is not verified: "
                         f"{geo['receipt']['summary']}")
    pr = check_print(d, name, need=PRINT_ITEMS, quiet=quiet)
    want = sum(it["bodies"] for it, *_ in placed)
    if pr["shells"]["pieces"] != want:
        raise SystemExit(f"strandbeest: {name}: {pr['shells']['pieces']} "
                         f"pieces for the {want} placed")
    # check_print's bed_fit says the plate's box fits the profile's bed;
    # the packer also placed it ON the bed, from its corner.
    box = d.request("mass_properties", {}, timeout=600)["bbox_mm"]
    lo, hi = box["min"], box["max"]
    if min(lo[0], lo[1]) < -1e-6 or hi[0] > bed[0] + 1e-6 or \
            hi[1] > bed[1] + 1e-6:
        raise SystemExit(f"strandbeest: {name} runs off the {bed[0]:g} x "
                         f"{bed[1]:g} mm bed: {lo} .. {hi}")
    stl = out / f"{name.replace(' ', '_')}.stl"
    exp = d.call("export_stl", path=str(stl))
    return {"part": name, "stl": str(stl), "shells": exp.get("shells"),
            "deflection_mm": exp.get("deflection_mm"),
            "bbox_mm": [lo, hi], "print": print_receipt(pr),
            "pieces": sorted(f"{it['name']}" for it, *_ in placed)}


# ---- the run ----------------------------------------------------------------

def material_of(d, proc):
    if "material" in proc:
        return proc["material"]
    for p in d.call("list_print_profiles")["profiles"]:
        if p["name"] == proc["profile"]:
            return p["material"]
    raise SystemExit(f"strandbeest: no print profile '{proc['profile']}'")


def build_print_parts(d, s, lay, frames, proc, mat, step_deg, throws):
    """Every printed part, each in the attitude it prints in."""
    t0 = time.perf_counter()
    for side in SIDES:
        for bar in lk.BARS:
            build_bar(d, s, lay, frames[side], side, bar, proc, mat)
    pins = {(side, j): build_pin(d, s, lay, side, j, proc, mat)
            for side in SIDES for j in lk.PINS}
    build_cap(d, s, proc, mat)
    kinds = ["first", "last"] + (["middle"] if throws > 1 else [])
    for kind in kinds:
        build_plate(d, s, lay, kind, proc, mat)
    tops = {kind: build_disc(d, s, kind, step_deg, proc, mat)
            for kind in kinds}
    build_handle(d, s, proc, mat)
    build_crank_pin(d, s, proc, mat)
    print(f"  {12 + 8 + 1 + 2 * len(kinds) + 2} print parts "
          f"({time.perf_counter() - t0:.0f} s)")
    return pins, tops


def assembled(d, s, lay, pins, tops, throws, step_deg, proc):
    """The parts a walker of `throws` throws is assembled from: the bars,
    the pins and their caps, the frame and the crankshaft."""
    parts = {"bars": {(side, bar): f"{side} {bar}" for side in SIDES
                      for bar in lk.BARS},
             "pins": {k: f"pin {k[0]} {k[1]}" for k in pins},
             "pin_info": pins}
    parts["frame"] = build_frame(d, s, lay, throws, proc, None)
    parts["crankshaft"] = build_crankshaft(d, s, throws, step_deg, tops,
                                           proc)
    return parts


def part_names_of(ids, parts):
    """Each occurrence's part, for its mass."""
    out = {ids["frame"]: parts["frame"],
           ids["crankshaft"]: parts["crankshaft"]}
    for (p, side, bar), occ in ids["bars"].items():
        out[occ] = parts["bars"][(side, bar)]
    for (p, side, j), occ in ids["pins"].items():
        out[occ] = parts["pins"][(side, j)]
    for occ in ids["caps"].values():
        out[occ] = "cap"
    return out


def print_items(s, lay, frames, throws, with_press=None):
    """The pieces one walker of `throws` throws prints: per throw a leg
    pair's twelve bodies, eight pins, ten caps (two on the frame pivots)
    and a crank pin; a plate and a disc more than the throws; the
    handle."""
    items = []

    def add(name, part, fp, n=1, bodies=1):
        for k in range(n):
            items.append({"name": name, "part": part, "bodies": bodies,
                          "circles": footprint(s, lay, frames, fp)})
    for side in SIDES:
        for bar in lk.BARS:
            add(f"{side} {bar}", f"{side} {bar}", ("bar", side, bar), throws)
        for j in lk.PINS:
            add(f"pin {side} {j}", f"pin {side} {j}", ("pin",), throws)
    add("cap", "cap", ("cap",), 10 * throws)
    add("plate first", "plate first", ("plate",))
    add("plate last", "plate last", ("plate",))
    add("plate middle", "plate middle", ("plate",), throws - 1)
    add("disc first", "disc first", ("disc", "first"))
    add("disc last", "disc last", ("disc", "last"))
    add("disc middle", "disc middle", ("disc", "middle"), throws - 1)
    add("handle", "handle", ("handle",))
    add("crank pin", "crank pin", ("crankpin",), throws)
    if with_press:
        add("press row", with_press["part"],
            ("press",) + tuple(with_press["box"]),
            bodies=1 + len(with_press["caps"]))
    return items


def press_row_part(d, s, proc, mat):
    """The coupon's press row at this walker's stub and cap
    (print_coupon.press_row): a numbered pin per step and a notched cap for
    each, to find the press before twelve legs need it."""
    step = s["gap_pin"] / 6.0
    row = print_coupon.press_row(d, proc, s["ds"], 2, step,
                                 "press row", cap_d=s["dh"], cap_t=s["tc"])
    for b in d.call("list_elements")["bodies"]:
        d.call("set_material", feature_id=b["created_by"], material=mat)
    z = row["sizes"]
    n = len(row["caps"])
    row["box"] = (n * z["pitch"], z["pitch"],
                  -0.5 * z["pitch"] - z["cap_d"] / 2.0)
    return row


def control_long_bar(d, s, lay, frames, proc, mat, step_deg, samples, parts):
    """A negative control: the front leg's f bar 10 % long, on a leg pair of
    its own. It assembles and turns, and its foot must FAIL the curve."""
    L = dict(s["lengths"])
    L["f"] *= 1.1
    long_frames = {sd: dict(frames[sd]) for sd in SIDES}
    long_frames["front"]["f"] = body_frames(L)["f"]
    build_bar(d, s, lay, long_frames["front"], "front", "f", proc, mat,
              name="front f 10% long")
    ctl = dict(parts, bars=dict(parts["bars"]))
    ctl["bars"][("front", "f")] = "front f 10% long"
    ids = build_assembly(d, s, lay, 1, step_deg, "control: f 10% long", ctl,
                         L=L)
    r = run_study(d, s, ids, long_frames, samples, False, "control turn")
    foot = check_foot(r, s, 1, step_deg)
    leg = foot["legs"]["front 0"]
    if leg["ok"]:
        raise SystemExit(f"strandbeest: the 10 % long bar PASSES the foot "
                         f"check: {leg}")
    return {"bar": "front f", "length_mm": L["f"], "foot": leg,
            "failed": True,
            "why": (f"flatness {leg['flatness_mm']:.2f} mm over the "
                    f"{leg['stride_ref_mm']:.1f} mm stride against "
                    f"{leg['flatness_ref_mm']:.2f}, {leg['deviation_mm']:.1f}"
                    f" mm off the curve")}


def control_one_layer(d, s, lay, frames, proc, mat, step_deg, samples,
                      parts):
    """A negative control: the front k bar built into the front triangle
    BCD's layer (its frames there), two bars that cross over the turn. The
    study's clash check must find them."""
    bcd = layer_of(lay, "front", "bcd")
    build_bar(d, s, lay, frames["front"], "front", "k", proc, mat,
              name="front k on bcd's layer", layer=bcd)
    ctl = dict(parts, bars=dict(parts["bars"]))
    ctl["bars"][("front", "k")] = "front k on bcd's layer"
    ids = build_assembly(d, s, lay, 1, step_deg, "control: k on bcd's layer",
                         ctl, layers={("front", "k"): bcd})
    r = run_study(d, s, ids, frames, samples, True, "control clash")
    k, b = ids["bars"][(0, "front", "k")], ids["bars"][(0, "front", "bcd")]
    hits = [smp["value"] for smp in r["samples"]
            if any({c["a"], c["b"]} == {k, b} for c in smp["clashes"])]
    if not hits:
        raise SystemExit(f"strandbeest: k on bcd's layer is NOT a clash: "
                         f"{r['collision_status']}")
    return {"layer": bcd, "clash_status": r["collision_status"],
            "why": (f"the clash check finds front k against front bcd at "
                    f"{len(hits)} samples, from {hits[0]:.0f} deg"),
            "clashing_samples": len(hits),
            "first_deg": hits[0], "failed": True,
            "pair": ["front k 0", "front bcd 0"]}


def stage(d, s, lay, frames, parts, throws, step_deg, samples, name,
          stability):
    """One walker: assembled, studied, and every check."""
    ids = build_assembly(d, s, lay, throws, step_deg, name, parts)
    print(f"  {name}: {len(ids['bars']) + len(ids['pins']) + len(ids['caps']) + 2} occurrences,"
          f" {ids['mates']} mates, solve {ids['solve']} "
          f"({ids['mate_seconds']:.0f} s)")
    extra, mass = [], None
    if stability:
        extra, mass = com_traces(d, None, part_names_of(ids, parts))
    # What the SOLVE costs a sample, the clash check apart: the same turn
    # with no clash check (docs/assemblies/solver.md#coupled-loops).
    bare = run_study(d, s, ids, frames, samples, False, f"{name} solve only")
    solve_ms = 1000.0 * bare["seconds"] / len(bare["samples"])
    print(f"  {name}: the solve alone, {solve_ms:.1f} ms a sample "
          f"({bare['seconds']:.1f} s for {len(bare['samples'])})")
    r = run_study(d, s, ids, frames, samples, True, f"{name} turn", extra)
    n = len(r["samples"])
    per = r["seconds"] / n
    print(f"  {name}: exact study {n} samples, {r['collision_status']}, "
          f"{r['seconds']:.1f} s ({1000 * per:.0f} ms a sample), "
          f"{r['pairs_checked']} pair checks, {r['refined_samples']} "
          f"samples refined, {r['branch_jumps']} branch jumps")
    if r["collision_status"] != "passed":
        raise SystemExit(f"strandbeest: {name}: {r['collision_status']}\n"
                         f"{r['table'][-3000:]}")
    out = {"occurrences": len(ids["bars"]) + len(ids["pins"]) +
           len(ids["caps"]) + 2,
           "mates": ids["mates"], "solve": ids["solve"],
           "study": {"samples": n, "seconds": r["seconds"],
                     "ms_per_sample": 1000 * per,
                     "solve_ms_per_sample": solve_ms,
                     "revolute_columns": ids["revolutes"],
                     "collision_status": r["collision_status"],
                     "clash_mode": "exact",
                     "pairs_checked": r["pairs_checked"],
                     "refined_samples": r["refined_samples"],
                     "branch_jumps": r["branch_jumps"]}}
    # Every cap on its pin is a printed press the exact check relieved and
    # listed, once a pair: none of them is a clash, and none is missing.
    capped = {frozenset((ids["pins"][k] if k[2] != "B" else ids["frame"],
                         c)) for k, c in ids["caps"].items()}
    fits = {frozenset((f["a"], f["b"])): f for f in r["press_fits"]}
    missing = [k for k in capped if k not in fits]
    if missing:
        raise SystemExit(f"strandbeest: {name}: {len(missing)} of "
                         f"{len(capped)} caps not listed as a press fit")
    first = next(iter(fits.values()))
    out["press_fits"] = {
        "caps": len(capped), "listed": len(fits),
        "fit": first["fit"], "interference_mm": first["interference_mm"],
        "max_interference_mm": first["max_interference_mm"],
        "kinds": sorted({f["fit"] for f in fits.values()})}
    print(f"  {name}: {len(fits)} press fits relieved and listed, each cap "
          f"on its pin: {first['fit']} (up to "
          f"{first['max_interference_mm']:g} mm)")
    out["branch"] = check_branch(r, throws)
    foot = check_foot(r, s, throws, step_deg)
    if not foot["ok"]:
        raise SystemExit(f"strandbeest: {name}: the feet are off the "
                         f"curve: {json.dumps(foot['legs'], indent=1)}")
    out["foot"] = foot
    out["walk"] = walk(r, s, throws)
    if stability:
        st = check_stability(r, s, throws, mass)
        if not st["ok"]:
            raise SystemExit(f"strandbeest: {name} tips over at "
                             f"{st['value_deg']:.0f} deg: {st}")
        # The traced centre of mass, against the assembly's own at the start.
        mp = d.call("mass_properties")["total"]
        com0 = [sum(mass[k] * r["paths"][k][0][i] for k in mass) /
                sum(mass.values()) for i in range(3)]
        st["mass_properties_com_mm"] = mp["center_of_mass_mm"]
        st["traced_com_mm"] = com0
        st["com_agrees_mm"] = math.dist(com0, mp["center_of_mass_mm"])
        out["stability"] = st
    out["layers"] = check_layer_clearance(d, s, lay, ids, r, step_deg,
                                          max(1, (n - 1) // 12))
    if not out["layers"]["ok"]:
        raise SystemExit(f"strandbeest: {name}: adjacent layers too close: "
                         f"{out['layers']}")
    return ids, r, out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int)
    ap.add_argument("--size", type=float, default=lk.HOLY["m"],
                    help="the crank radius, mm (Jansen's own 15 by default;"
                         " every length scales with it)")
    ap.add_argument("--pairs", type=int, default=3,
                    help="crank phases, each with a front and a back leg on "
                         "each side of the frame")
    ap.add_argument("--layer", type=float, default=None,
                    help="the layer height, mm (default: the profile's, "
                         "else Bambu Studio's 0.20 mm preset)")
    ap.add_argument("--process", default="fdm")
    ap.add_argument("--material", default="pla")
    ap.add_argument("--profile", default=None,
                    help="one of your print profiles (list_print_profiles)")
    ap.add_argument("--samples", type=int, default=181,
                    help="study samples over the turn (181: 2 degrees)")
    ap.add_argument("--stage", choices=("1", "2", "all"), default="all")
    ap.add_argument("--no-controls", action="store_true")
    ap.add_argument("--hand-n", type=float, default=None,
                    help="a push on the crank's knob to design for, N, "
                         "instead of the walker's own loads (the larger of "
                         "its walk, doubled, and the most its feet let a "
                         "hand put through)")
    ap.add_argument("--mu", type=float, default=FOOT_MU,
                    help="the feet's static friction on the floor (FOOT_MU "
                         "says where 0.5 comes from)")
    ap.add_argument("--fresh", action="store_true",
                    help="start a new project first (an untitled scratch one is "
                         "discarded; a saved one with unsaved work refused)")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()
    if args.pairs < 1:
        raise SystemExit("strandbeest: --pairs is 1 or more")
    out = Path(args.out) if args.out else OUT
    t_all = time.perf_counter()

    d = connect(args.port)
    if args.fresh:
        fresh_project(d)
    proc = ({"profile": args.profile} if args.profile else
            {"process": args.process, "material": args.material})
    prof = next((p for p in d.call("list_print_profiles")["profiles"]
                 if p["name"] == args.profile), None) if args.profile else {}
    if args.profile and prof is None:
        raise SystemExit(f"strandbeest: no print profile '{args.profile}'")
    layer = args.layer or prof.get("layer_height_mm") or FDM_LAYER_MM
    # The bed is the printer's, so the profile's: nothing guesses one.
    if not prof or not prof.get("bed_mm"):
        raise SystemExit(
            "strandbeest: the walker is packed into the printer's bed, and "
            + (f"profile '{args.profile}' states no bed_mm" if prof else
               "the built-in table is no printer's")
            + ": --profile one that states it (print_coupon.py --seed gives "
              "the P1S profiles theirs; Settings, Print profiles, for "
              "yours)")
    bed = (float(prof["bed_mm"][0]), float(prof["bed_mm"][1]))

    allowances = {}

    def allow(kind, dia):
        r = d.call("print_allowance", kind=kind, size_mm=dia, **proc)
        allowances[f"{kind} Ø{dia:.3f}"] = {
            "allowance_mm": r["allowance_mm"], "source": r["source"],
            "from": r.get("from"), "note": r.get("note")}
        return r["allowance_mm"]
    s = solve(args.size, layer, allow)
    t0 = time.perf_counter()
    lay = layout(s, 90)
    step_deg = 360.0 / args.pairs
    throws = 2 * args.pairs
    L = s["lengths"]
    print(f"strandbeest: crank r{L['m']:g}, pins Ø{s['dp']:.2f} in "
          f"Ø{s['hole']:.2f} holes (gap {s['gap_pin'] / 2:g} a side), bars "
          f"{s['w']:.2f} x {s['t']:g}, {s['top'] + 1} layers of "
          f"{s['pitch']:.2f} a slab ({s['slab']:.2f} mm), caps on "
          f"Ø{s['ds']:.2f} stubs at a press of {s['press']:+g} "
          f"({time.perf_counter() - t0:.0f} s to lay out)")
    print(f"  layers: " + ", ".join(
        f"{sd} {b} {lay['layers'][(sd, b)]}" for sd in SIDES
        for b in lk.BARS))
    mat = material_of(d, proc)
    frames = {sd: body_frames(L, sd == "back") for sd in SIDES}
    pins, tops = build_print_parts(d, s, lay, frames, proc, mat, step_deg,
                                   throws)
    press = press_row_part(d, s, proc, mat)
    strong = strength(d, s, lay, frames, throws, step_deg, args.hand_n,
                      args.mu)
    receipt = {"size": args.size, "pairs": args.pairs, "process": proc,
               "strength": strong,
               "layer_mm": layer,
               "sizes": {k: v for k, v in s.items() if k != "lengths"},
               "lengths": L, "layers": {f"{a} {b}": v for (a, b), v in
                                        lay["layers"].items()},
               "pins": lay["pins"], "allowances": allowances,
               "press_row": {k: press[k] for k in
                             ("allowance_mm", "from", "note", "step_mm",
                              "caps")}}
    receipt["sizes"]["standoffs"] = s["standoffs"]

    receipt["bed_mm"] = prof["bed_mm"]
    if args.stage in ("1", "all"):
        one = assembled(d, s, lay, pins, tops, 1, step_deg, proc)
        _, r1, chk1 = stage(d, s, lay, frames, one, 1, step_deg,
                            args.samples, "leg pair", stability=False)
        receipt["stage1"] = chk1
        o1 = out / "stage1"
        o1.mkdir(parents=True, exist_ok=True)
        items = print_items(s, lay, frames, 1, with_press=press)
        beds = pack(items, bed, s["w"] / 2.0, bed_zones(prof))
        if len(beds) != 1:
            raise SystemExit(f"strandbeest: stage 1 needs {len(beds)} beds, "
                             f"not one")
        build_plate_part(d, "stage1 plate", proc, beds[0])
        receipt["stage1"]["plate"] = check_plate(d, "stage1 plate", proc,
                                                 beds[0], o1, bed)
        receipt["stage1"]["job"] = job_receipt(export_print_job(
            d, [("stage1 plate", "stage 1")],
            o1 / "strandbeest_stage1_job.3mf", "pla"))
        d.call("save_project", path=str(o1 / "strandbeest_stage1.dsgn"))
        print(f"  stage 1: one plate, {len(beds[0])} pieces -> "
              f"{receipt['stage1']['plate']['stl']}")
        if not args.no_controls:
            receipt["negative_controls"] = {
                "a bar 10 % long": control_long_bar(
                    d, s, lay, frames, proc, mat, step_deg, args.samples,
                    one),
                "two crossing bars on one layer": control_one_layer(
                    d, s, lay, frames, proc, mat, step_deg, args.samples,
                    one)}
            for k, v in receipt["negative_controls"].items():
                print(f"  negative control, {k}: FAILS as it must "
                      f"({v['why']})")

    if args.stage in ("2", "all"):
        full = assembled(d, s, lay, pins, tops, throws, step_deg, proc)
        _, r2, chk2 = stage(d, s, lay, frames, full, throws, step_deg,
                            args.samples, "walker", stability=True)
        receipt["stage2"] = chk2
        w = chk2["walk"]
        st = chk2["stability"]
        print(f"  walker: {w['distance_per_turn_mm']:.1f} mm a crank turn "
              f"(toward {w['direction']}), steps {w['step_height_mm']:.1f} "
              f"mm; centre of mass inside the feet by "
              f"{st['margin_mm']:.1f} mm at worst ({st['value_deg']:.0f} "
              f"deg, {st['feet_on_ground']} feet down)")
        o2 = out / "stage2"
        o2.mkdir(parents=True, exist_ok=True)
        beds = pack(print_items(s, lay, frames, throws), bed,
                    s["w"] / 2.0, bed_zones(prof))
        plates = []
        for k, plate in enumerate(beds):
            name = f"stage2 plate {k + 1}"
            build_plate_part(d, name, proc, plate)
            plates.append(check_plate(d, name, proc, plate, o2, bed,
                                      quiet=True))
            print(f"  {name}: {len(plate)} pieces, every check_print item "
                  f"passes -> {plates[-1]['stl']}")
        receipt["stage2"]["plates"] = plates
        receipt["stage2"]["job"] = job_receipt(export_print_job(
            d, [(f"stage2 plate {k + 1}", f"plate {k + 1}")
                for k in range(len(beds))],
            o2 / "strandbeest_job.3mf", "pla"))
        d.call("save_project", path=str(o2 / "strandbeest.dsgn"))

    receipt["seconds"] = time.perf_counter() - t_all
    out.mkdir(parents=True, exist_ok=True)
    (out / "strandbeest.json").write_text(json.dumps(receipt, indent=1,
                                                     default=str))
    print(f"strandbeest: done ({receipt['seconds']:.0f} s) -> "
          f"{out / 'strandbeest.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
