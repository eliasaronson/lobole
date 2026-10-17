#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A print-in-place mechanical counter: number wheels on one axle, a Geneva
carry between each pair, a housing with a window per digit. One plate, off
the bed reading 0000 and counting.

    python3 generators/counter/build.py --port 8801 --profile "K5 PLA on P1S"
    python3 generators/counter/build.py --port 8801 --digits 3 --digit-height 10

Each wheel carries a pin and a lock disc under its band of digits. The pin
turns a three-slot Geneva on a transfer beside the stack once a turn, 120
degrees, and the transfer's 9-tooth pinion turns the next wheel's 30-tooth
gear 36 degrees: one digit. Between carries the lock disc rides in the
Geneva's concave arc and the next wheel cannot turn.

Every size derives from `--digit-height` and the print process's gap
(`print_allowance` kind `gap`), plus the layer height the profile was
measured at: nothing here is a literal in millimetres except what those
scale (generators/counter/README.md#where-every-size-comes-from).

What it builds:
- parts `wheel 1` (units, the knob) to `wheel 1000`, `transfer` and
  `housing`; an assembly `counter` with a revolute per wheel and transfer,
  an `intermittent` (Geneva, 3 slots) relation wheel -> transfer and a
  `gear` (9:30) relation transfer -> next wheel, and motion studies;
- the part `counter print`: every body where it prints, as Derived copies
  at the pose that reads 0000, exported as ONE STL of one shell per body.

Output (generated, untracked): scratch/counter/ — counter.stl,
counter.dsgn and counter.json, the receipt.
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
from _lib import (arc_through, check, check_print, circle, connect,  # noqa: E402
                  fresh_project, line, print_receipt)
from gear import involute_profile  # noqa: E402

ROOT = Path(HERE).resolve().parent.parent
OUT = ROOT / "scratch" / "counter"

SLOTS = 3  # three, not four: a 60 degree window, so the next wheel stands
#            a degree off its digit at 9 and 0 instead of 4.5 (README)
DIGITS_PER_TURN = 10
# Stub teeth at 25 degrees, as the planetary's: a 9-tooth involute is not
# undercut at 25 (2 a / sin^2(25) = 9 with a = 0.8), and its tip is wide
# enough to print at the module a digit wheel leaves room for.
PRESSURE_DEG = 25.0
ADDENDUM = 0.8
MIN_TEETH = 9
NOZZLE_MM = 0.4  # the P1S's stock nozzle, which the allowance table assumes
MIN_TIP_MM = 1.5 * NOZZLE_MM
MIN_HORN_MM = 4 * NOZZLE_MM  # a Geneva's tip between slot and arc
# A digit fills 70 % of its cell around the band; its glyph is about 0.6 of
# its height wide (a sans numeral), and the band leaves 0.15 of the height
# above and below it.
DIGIT_FILL = 0.7
GLYPH_WIDTH = 0.6
FACE_MARGIN = 0.15
# The window, measured around the band, as a fraction of a digit's cell.
WINDOW_FRACTION = 0.9


# ---- the carry's motion, as the app's relation computes it ---------------
# (docs/assemblies/relations.md#intermittent; IntermittentValue in
# src/core/Occurrence.cpp). Used to place the parts at the print pose and to
# say where each wheel should stand; the assembly's own solve is what the
# studies check.

def geneva_law(t, slots=SLOTS):
    t = min(max(t, 0.0), 1.0)
    half = math.pi / slots
    k = math.sin(half)
    alpha = (t - 0.5) * (math.pi - 2.0 * half)
    beta = math.atan2(k * math.sin(alpha), 1.0 - k * math.cos(alpha))
    return (beta + half) / (2.0 * half)


def carry_deg(a_deg, start_deg, slots=SLOTS):
    """The transfer's turn, degrees, for its wheel at `a_deg` (phase 0)."""
    step = -360.0 / slots
    duration = 180.0 - 360.0 / slots
    x = (a_deg - start_deg) / 360.0
    n = math.floor(x)
    u = (x - n) * 360.0
    s = geneva_law(u / duration, slots) if u < duration else 1.0
    return step * (n + s)


# ---- sizes ----------------------------------------------------------------

def tip_land(m, z, backlash):
    alpha = math.radians(PRESSURE_DEG)
    rp = m * z / 2.0
    rb, ra = rp * math.cos(alpha), rp + ADDENDUM * m

    def inv(a):
        return math.tan(a) - a

    half = (math.pi / (2 * z) - backlash / (2.0 * rp) + inv(alpha)
            - inv(math.acos(rb / ra)))
    return 2.0 * half * ra


def horn_mm(c, r_g, w, r_lock_gap):
    """Width of a Geneva's tip between a slot's side and the concave arc
    that rides the lock disc, at the wheel's rim."""
    delta = math.asin(w / r_g)
    cos_phi = (r_g ** 2 + c ** 2 - r_lock_gap ** 2) / (2.0 * r_g * c)
    phi = math.acos(max(-1.0, min(1.0, cos_phi)))
    return r_g * (math.pi / SLOTS - delta - phi)


def solve(h, digits, gap, layer, z_gap=None):
    """Every size from the digit height, the gap a side and the layer, and
    the profile's measured Z gap when it has one."""
    if digits < 2:
        raise SystemExit("counter: a counter of one digit carries nothing")
    g = gap / 2.0
    cell = h / DIGIT_FILL                       # a digit's cell, around
    r_band = DIGITS_PER_TURN * cell / (2.0 * math.pi)
    # The transfer turns on a post of the housing's that rises past the
    # next wheel's band, so its axis stands a post's radius outside it.
    r_post = 0.25 * h
    c = r_band + g + r_post
    # The gear: the next wheel's 30 teeth and the transfer's 9 turn a
    # 120-degree Geneva step into one digit (120 * 9/30 = 36). The coarsest
    # pair that keeps a printable tip.
    best = None
    for k in (3, 4, 5):
        zw, zp = 10 * k, 3 * k
        m = 2.0 * c / (zw + zp)
        j = 2.0 * g / math.cos(math.radians(PRESSURE_DEG))
        if zp >= MIN_TEETH and tip_land(m, zp, j / 2.0) >= MIN_TIP_MM:
            best = (zw, zp, m, j)
            break
    if best is None:
        raise SystemExit(f"counter: no gear pair with a {MIN_TIP_MM:.2f} mm "
                         f"tip fits a {h:g} mm digit at {g:g} mm a side — a "
                         f"larger --digit-height, or the gap your printer "
                         f"measured (--profile)")
    zw, zp, m, j = best
    tip_c = max(0.25 * m, g)  # tip clearance at least the gap
    # The Geneva: the pin enters radially at R = C sin(60), the wheel's
    # radius is C cos(60).
    r_pin_circle = c * math.sin(math.pi / SLOTS)
    r_g = c * math.cos(math.pi / SLOTS)
    r_pin = max(3 * NOZZLE_MM, 0.06 * c)
    w = r_pin + g
    # The lock disc as large as the Geneva's horns allow.
    lo, hi = 0.3 * c, r_pin_circle - r_pin - g
    if horn_mm(c, r_g, w, lo + g) < MIN_HORN_MM:
        raise SystemExit("counter: the Geneva's horns are too thin at any "
                         "lock disc — a larger --digit-height")
    for _ in range(60):
        mid = (lo + hi) / 2.0
        if horn_mm(c, r_g, w, mid + g) >= MIN_HORN_MM:
            lo = mid
        else:
            hi = mid
    r_lock = lo
    slot_bottom = c - r_pin_circle  # the pin's nearest approach to O2
    rp_p = m * zp / 2.0
    root_p = rp_p - (ADDENDUM * m + tip_c)
    tip_p = rp_p + ADDENDUM * m
    rp_w = m * zw / 2.0
    tip_w = rp_w + ADDENDUM * m
    if root_p - (r_post + g) < 2 * NOZZLE_MM:
        raise SystemExit(f"counter: the pinion's wall round its post is "
                         f"{root_p - r_post - g:.2f} mm — a larger "
                         f"--digit-height")
    if tip_w + g >= r_band:
        raise SystemExit("counter: the wheel's gear is wider than its band")
    s = {
        "digit_height": h, "digits": digits, "gap": gap, "g": g,
        "layer": layer, "cell": cell, "r_band": r_band, "r_post": r_post,
        "c": c, "zw": zw, "zp": zp, "module": m, "backlash": j,
        "tip_clearance": tip_c, "rp_w": rp_w, "tip_w": tip_w,
        "rp_p": rp_p, "tip_p": tip_p, "root_p": root_p,
        "r_pin_circle": r_pin_circle, "r_geneva": r_g, "r_pin": r_pin,
        "r_lock": r_lock, "slot_bottom": slot_bottom,
        "horn_mm": horn_mm(c, r_g, w, r_lock + g),
        "r_axle": 0.25 * h,
        "h_lock": 0.4 * h,                    # the Geneva's working depth
        "chamfer": r_band - r_lock,           # the band's 45-degree underside
        "h_face": GLYPH_WIDTH * h + 2 * FACE_MARGIN * h,
        "h_gear": 0.4 * h,
        "t_base": 0.25 * h, "t_ceiling": 0.25 * h, "wall": 0.25 * h,
        "r_knob": 0.5 * r_band, "h_knob": 0.75 * h,
        # Two lines deep, plus the band's fall-away across half a glyph.
        "engrave": 1.5 * NOZZLE_MM,
    }
    s["h_band"] = s["chamfer"] + s["h_face"]
    # Where one part sits over another the space is whole layers: the
    # profile's z_gap_mm (print_coupon's air row), else at least the gap --
    # one layer at the P1S's 0.1 a side, two at 0.25. The 2026-10-15 print
    # fused at these interfaces and check_print's stacking fails them
    # (docs/tolerances/README.md#one-part-printed-over-another).
    if z_gap:
        s["zgap"], s["zgap_from"] = z_gap, "the profile's z_gap_mm"
    else:
        s["zgap"] = layer * math.ceil(g / layer - 1e-9)
        s["zgap_from"] = "whole layers at least the gap a side"
    # The carry's window: centred where the driving wheel goes from 9 (36
    # degrees) to 0 (0 degrees), counting being a turn BACK 36 a digit.
    s["carry_centre_deg"] = 18.0
    s["window_deg"] = 180.0 - 360.0 / SLOTS
    s["start_deg"] = s["carry_centre_deg"] - s["window_deg"] / 2.0
    s["pin_azimuth_deg"] = -s["carry_centre_deg"]
    layout(s)
    return s


def layout(s):
    """Heights, bottom to top: the most significant wheel on the base, the
    units on top under the knob. Wheel j (0 = units): its lock disc and pin
    (j < digits-1), its band, its gear (j > 0) — the band over the pin's
    zone is what the 45-degree chamfer is for."""
    L = s["zgap"]
    n = s["digits"]
    z = s["t_base"] + L
    wheels = {}
    for j in range(n - 1, -1, -1):
        w = {"z0": z}
        if j < n - 1:
            # The Geneva works over `lock`; the wheel's lock disc and pin
            # rise a layer past its top to meet the band, which clears the
            # Geneva by that layer.
            w["lock"] = (z, z + s["h_lock"])
            z += s["h_lock"] + L
        w["band"] = (z, z + s["h_band"])
        z += s["h_band"]
        if j > 0:
            w["gear"] = (z, z + s["h_gear"])
            z += s["h_gear"]
        else:
            w["knob"] = (z, z + L + s["t_ceiling"] + s["h_knob"])
            z = w["knob"][1]
        w["z1"] = z
        wheels[j] = w
        if j > 0:
            z += L
    s["wheels"] = wheels
    s["top"] = wheels[0]["knob"][0] + L + s["t_ceiling"]
    # Transfer j sits beside wheel j+1's gear and wheel j's lock zone: its
    # pinion a layer clear of wheel j+1's band, one layer of it joining the
    # Geneva above the gear's top, the Geneva in the lock zone.
    s["transfers"] = {}
    for j in range(n - 1):
        g0, g1 = wheels[j + 1]["gear"]
        l0, l1 = wheels[j]["lock"]
        s["transfers"][j] = {"z0": g0, "pinion": (g0 + L, g1),
                             "join": (g1, l0), "geneva": (l0, l1)}


def wheel_name(j):
    return f"wheel {10 ** j}"


# ---- sketching --------------------------------------------------------------

def plane_z(z, origin=(0.0, 0.0)):
    return {"origin": [origin[0], origin[1], z], "normal": [0, 0, 1],
            "uAxis": [1, 0, 0]}


def plane_xz(x0=0.0):
    """The XZ half-plane through (x0, 0): u = x - x0, v = z (V is +Z because
    the normal is -Y, docs/../_lib.py's V-axis note)."""
    return {"origin": [x0, 0.0, 0.0], "normal": [0, -1, 0],
            "uAxis": [1, 0, 0]}


def rot(p, deg):
    a = math.radians(deg)
    return (p[0] * math.cos(a) - p[1] * math.sin(a),
            p[0] * math.sin(a) + p[1] * math.cos(a))


def polar(r, deg):
    a = math.radians(deg)
    return (r * math.cos(a), r * math.sin(a))


def polygon(pts):
    return [line(pts[i], pts[(i + 1) % len(pts)]) for i in range(len(pts))]


class Builder:
    """Features into the active part, each body by the id that made it."""

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

    def prism(self, name, ents, z0, z1, mode="new_body", origin=(0.0, 0.0)):
        return self.extrude(name, plane_z(z0, origin), ents, z1 - z0, mode)

    def revolve(self, name, profile, x0=0.0):
        """A solid of revolution about the vertical axis through (x0, 0):
        `profile` is (r, z) points, r >= 0, a closed polygon."""
        sk = self.sketch(f"{name} profile", plane_xz(x0),
                         polygon([(r, z) for r, z in profile]))
        axis = ({"kind": "z"} if x0 == 0.0 else
                {"kind": "custom", "origin": [x0, 0.0, 0.0],
                 "direction": [0.0, 0.0, 1.0]})
        return check(self.d.call("add_feature", feature={
            "type": "Revolve", "name": name, "sketch_id": sk, "axis": axis,
            "angle_deg": 360.0, "mode": "new_body"}), name)["id"]

    def boolean(self, name, target, tool, op):
        return check(self.d.call("add_feature", feature={
            "type": "Boolean", "name": name, "target_id": target,
            "tool_id": tool, "op": op}), name)["id"]

    def union(self, name, ids):
        out = ids[0]
        for i, t in enumerate(ids[1:]):
            out = self.boolean(f"{name} +{i + 1}", out, t, "union")
        return out


def gear_ents(s, z, phase_deg, centre=(0.0, 0.0)):
    m = s["module"]
    ents, _ = involute_profile(m, z, PRESSURE_DEG, backlash=s["backlash"] / 2,
                               phase_deg=phase_deg, addendum=ADDENDUM,
                               dedendum=ADDENDUM + s["tip_clearance"] / m)
    if centre != (0.0, 0.0):
        for e in ents:
            for k in ("a", "b", "center"):
                if k in e:
                    e[k] = [e[k][0] + centre[0], e[k][1] + centre[1]]
    return ents


def digit_text(label, height):
    return {"type": "text", "text": label, "font": "sans", "height": height,
            "anchor": [0.0, 0.0], "angle": 0.0, "halign": "center",
            "valign": "middle"}


# ---- the parts ------------------------------------------------------------
# Each in its own frame: the axis is Z through the origin, z = 0 its bottom.
# A wheel at rotation 0 shows 0 in the window (at -X); digit d is engraved
# at azimuth 180 + 36 d, so counting is a turn BACK 36 degrees, and the
# next digit comes up from below. The pin stands at -18 degrees, on the line
# to the transfer (+X) when the wheel is at +18, half way from 9 to 0.

def build_wheel(b, s, j):
    n = s["digits"]
    w = s["wheels"][j]
    z0 = w["z0"]

    def zz(v):
        return v - z0

    bodies = []
    L = s["zgap"]
    if "lock" in w:
        l0, l1 = (zz(v) for v in w["lock"])
        l1 += L  # up through the layer the Geneva clears the band by
        disc = b.prism(f"{wheel_name(j)} lock disc",
                       [circle((0, 0), s["r_lock"])], l0, l1)
        # The relief the Geneva swings through at mid-carry: its circle,
        # where it stands when the pin is on the line of centres.
        relief = b.prism(f"{wheel_name(j)} relief",
                         [circle(polar(s["c"], s["pin_azimuth_deg"]),
                                 s["r_geneva"] + s["g"])], l0, l1)
        bodies.append(b.boolean(f"{wheel_name(j)} lock", disc, relief,
                                "subtract"))
        pin_c = polar(s["r_pin_circle"], s["pin_azimuth_deg"])
        bodies.append(b.prism(f"{wheel_name(j)} pin",
                              [circle(pin_c, s["r_pin"])], l0, l1))
        # Up into the band's chamfer, and no further out than the band.
        up = b.prism(f"{wheel_name(j)} pin root",
                     [circle(pin_c, s["r_pin"])], l1, l1 + s["chamfer"])
        clip = b.prism(f"{wheel_name(j)} pin clip",
                       [circle((0, 0), s["r_band"])], l1, l1 + s["chamfer"])
        bodies.append(b.boolean(f"{wheel_name(j)} pin root clipped", up,
                                clip, "intersect"))
    b0, b1 = (zz(v) for v in w["band"])
    bodies.append(b.revolve(f"{wheel_name(j)} band", [
        (0.0, b0), (s["r_lock"], b0), (s["r_band"], b0 + s["chamfer"]),
        (s["r_band"], b1), (0.0, b1)]))
    if "gear" in w:
        g0, g1 = (zz(v) for v in w["gear"])
        bodies.append(b.prism(f"{wheel_name(j)} gear",
                              gear_ents(s, s["zw"], 0.0), g0, g1))
    else:
        k0, k1 = (zz(v) for v in w["knob"])
        # A fluted knob: 24 points alternating between two radii.
        r = s["r_knob"]
        pts = [polar(r if i % 2 == 0 else 0.9 * r, 360.0 * i / 24)
               for i in range(24)]
        bodies.append(b.prism(f"{wheel_name(j)} knob", polygon(pts), k0, k1))
    body = b.union(wheel_name(j), bodies)
    # The axle's bore: through, or up into the knob's foot on the units.
    bore_top = zz(w["z1"]) if "gear" in w else b1
    body = b.prism(f"{wheel_name(j)} bore",
                   [circle((0, 0), s["r_axle"] + s["g"])], 0.0, bore_top,
                   mode="cut")
    # The digits, engraved on the band's face from a plane just outside it.
    f0 = b0 + s["chamfer"]
    zmid = (f0 + b1) / 2.0
    out = 1.0
    sag = (s["digit_height"] / 2.0) ** 2 / (2.0 * s["r_band"])
    for dgt in range(DIGITS_PER_TURN):
        az = 180.0 + 36.0 * dgt
        nx, ny = math.cos(math.radians(az)), math.sin(math.radians(az))
        rr = s["r_band"] + out
        plane = {"origin": [rr * nx, rr * ny, zmid], "normal": [nx, ny, 0.0],
                 "uAxis": [0.0, 0.0, 1.0]}
        body = b.extrude(f"{wheel_name(j)} digit {dgt}", plane,
                         [digit_text(str(dgt), s["digit_height"])],
                         out + s["engrave"] + sag, mode="cut", reversed=True)
    return body


def geneva_slots(s):
    """Local slot directions, degrees: at rest (rotation 0) the concave arc
    between two slots faces the wheel axis at 180, so the slots stand at
    180 +- 60 and 0."""
    return [180.0 + 360.0 / SLOTS * (k + 0.5) for k in range(SLOTS)]


def build_transfer(b, s, gap=None):
    """The transfer, frame at its bottom on its axis: pinion, one layer
    joining it to the Geneva inside the pinion's root, the Geneva."""
    g = s["g"] if gap is None else gap / 2.0
    t = s["transfers"][0]
    z0 = t["z0"]
    p0, p1 = (v - z0 for v in t["pinion"])
    j1 = t["join"][1] - z0
    v0, v1 = (v - z0 for v in t["geneva"])
    # The pinion meshes wheel j+1's gear (tooth 0 at +X there) across the
    # line from O2 to the wheel axis, at 180: half a pitch off it.
    pin_phase = 180.0 + 180.0 / s["zp"]
    pin = b.prism("transfer pinion", gear_ents(s, s["zp"], pin_phase), p0, p1)
    join = b.prism("transfer join", [circle((0, 0), s["root_p"])], p1, j1)
    gen = b.prism("transfer geneva", [circle((0, 0), s["r_geneva"])], v0, v1)
    body = b.union("transfer", [pin, join, gen])
    w = s["r_pin"] + g
    for k, az in enumerate(geneva_slots(s)):
        # A slot: from its rounded bottom (the pin's nearest approach) out
        # past the rim, w either side of its centre line.
        r0 = s["slot_bottom"]
        r1 = s["r_geneva"] + 2.0
        c0 = polar(r0, az)
        a1 = rot((r1, w), az)
        a0 = rot((r0, w), az)
        b0_ = rot((r0, -w), az)
        b1_ = rot((r1, -w), az)
        bottom = rot((r0 - w, 0.0), az)
        ents = [line(a0, a1), line(a1, b1_), line(b1_, b0_),
                arc_through(b0_, bottom, a0)]
        del c0
        body = b.extrude(f"transfer slot {k + 1}", plane_z(p1), ents,
                         v1 - p1, mode="cut")
    for k, az in enumerate(geneva_slots(s)):
        arc_az = az + 180.0 / SLOTS
        body = b.extrude(f"transfer arc {k + 1}", plane_z(v0),
                         [circle(polar(s["c"], arc_az), s["r_lock"] + g)],
                         v1 - v0, mode="cut")
    body = b.prism("transfer bore", [circle((0, 0), s["r_post"] + g)],
                   p0, p1, mode="cut")
    return body


def envelope(intervals):
    """The union of (r, z0, z1) cylinders as one stepped (r, z) profile."""
    zs = sorted({z for _, z0, z1 in intervals for z in (z0, z1)})
    steps = []
    for za, zb in zip(zs, zs[1:]):
        rmax = max((r for r, z0, z1 in intervals if z0 <= za and zb <= z1),
                   default=0.0)
        if rmax > 0.0:
            steps.append((rmax, za, zb))
    # Merge equal neighbours and require one connected stack.
    merged = []
    for r, za, zb in steps:
        if merged and abs(merged[-1][0] - r) < 1e-9 and \
                abs(merged[-1][2] - za) < 1e-9:
            merged[-1] = (r, merged[-1][1], zb)
        else:
            merged.append((r, za, zb))
    for (_, _, zb), (_, za, _) in zip(merged, merged[1:]):
        if abs(zb - za) > 1e-9:
            raise SystemExit("counter: an envelope with a hole in it")
    prof = [(0.0, merged[0][1])]
    for r, za, zb in merged:
        prof += [(r, za), (r, zb)]
    prof.append((0.0, merged[-1][2]))
    # Drop repeated points where neighbouring steps share a corner.
    out = []
    for p in prof:
        if not out or abs(out[-1][0] - p[0]) > 1e-9 or \
                abs(out[-1][1] - p[1]) > 1e-9:
            out.append(p)
    return out


def build_housing(b, s):
    """A block minus every moving part's swept envelope grown by the gap
    (sideways) and the layer (up and down): what is left is the base, the
    walls, the posts, the fins that hold each part at its height, and the
    ceiling. Then a window per digit and the frames the mates use."""
    g, L, n = s["g"], s["zgap"], s["digits"]
    r_side = max(s["r_band"], s["r_pin_circle"] + s["r_pin"]) + g + s["wall"]
    x1 = s["c"] + s["r_geneva"] + g + s["wall"]
    block = b.prism("housing block", polygon([
        (-r_side, -r_side), (x1, -r_side), (x1, r_side), (-r_side, r_side)]),
        0.0, s["top"])
    body = block
    for j in range(n):
        w = s["wheels"][j]
        iv = []
        if "lock" in w:
            iv.append((s["r_pin_circle"] + s["r_pin"] + g,
                       w["lock"][0] - L, w["lock"][1] + 2 * L))
        iv.append((s["r_band"] + g, w["band"][0] - L, w["band"][1] + L))
        if "gear" in w:
            iv.append((s["tip_w"] + g, w["gear"][0] - L, w["gear"][1] + L))
        else:
            iv.append((s["r_knob"] + g, w["knob"][0] - L, s["top"] + 1.0))
        env = b.revolve(f"housing: {wheel_name(j)} swept",
                        envelope(iv))
        body = b.boolean(f"housing - {wheel_name(j)}", body, env, "subtract")
    for j in range(n - 1):
        t = s["transfers"][j]
        iv = [(s["tip_p"] + g, t["pinion"][0] - L, t["join"][1]),
              (s["r_geneva"] + g, t["geneva"][0] - L, t["geneva"][1] + L)]
        env = b.revolve(f"housing: transfer {j + 1} swept", envelope(iv),
                        x0=s["c"])
        body = b.boolean(f"housing - transfer {j + 1}", body, env,
                         "subtract")
    # The posts: the wheels' axle from the base up to under the knob, and
    # each transfer's stub into its pinion.
    axle = b.prism("axle", [circle((0, 0), s["r_axle"])], s["t_base"] - 0.5,
                   s["wheels"][0]["band"][1] - L)
    body = b.boolean("housing + axle", body, axle, "union")
    for j in range(n - 1):
        t = s["transfers"][j]
        post = b.prism(f"transfer {j + 1} post",
                       [circle((s["c"], 0.0), s["r_post"])],
                       t["z0"] - 1.0, t["pinion"][1] - L)
        body = b.boolean(f"housing + post {j + 1}", body, post, "union")
    # A window per digit, at -X: the cell's height around the band, the
    # face's width along the axle.
    half = WINDOW_FRACTION * s["cell"] / 2.0
    for j in range(n):
        b0, b1 = s["wheels"][j]["band"]
        f0 = b0 + s["chamfer"] + 0.5 * FACE_MARGIN * s["digit_height"]
        f1 = b1 - 0.5 * FACE_MARGIN * s["digit_height"]
        body = b.extrude(f"window {wheel_name(j)}", {
            "origin": [-r_side - 1.0, 0.0, 0.0], "normal": [1, 0, 0],
            "uAxis": [0, 1, 0]},
            polygon([(-half, f0), (half, f0), (half, f1), (-half, f1)]),
            # through the wall and on into the band's own clearance, short
            # of the axle
            r_side + 1.0 - 0.5 * s["r_band"], mode="cut")
    for j in range(n):
        check(b.d.call("add_feature", feature={
            "type": "Frame", "name": wheel_name(j), "kind": "explicit",
            "origin": [0.0, 0.0, s["wheels"][j]["z0"]],
            "z_axis": [0, 0, 1], "x_axis": [1, 0, 0]}), "wheel frame")
    for j in range(n - 1):
        check(b.d.call("add_feature", feature={
            "type": "Frame", "name": f"transfer {j + 1}", "kind": "explicit",
            "origin": [s["c"], 0.0, s["transfers"][j]["z0"]],
            "z_axis": [0, 0, 1], "x_axis": [1, 0, 0]}), "transfer frame")
    return body


# ---- the project ------------------------------------------------------------

def material_of(d, proc):
    if "material" in proc:
        return proc["material"]
    for p in d.call("list_print_profiles")["profiles"]:
        if p["name"] == proc["profile"]:
            return p["material"]
    raise SystemExit(f"counter: no print profile '{proc['profile']}'")


def new_part(d, name, proc):
    d.call("add_part", name=name)
    d.call("set_print_process", **proc)


def weigh(d, mat):
    """The material on the active part's one body, by the feature that
    CREATED it: set_material on a later feature (the cut a builder returns)
    is refused, since it would set nothing."""
    bodies = d.call("list_elements")["bodies"]
    if len(bodies) != 1:
        raise SystemExit(f"counter: a part of {len(bodies)} bodies")
    d.call("set_material", feature_id=bodies[0]["created_by"], material=mat)


def build_parts(d, s, proc):
    b = Builder(d)
    mat = material_of(d, proc)
    t0 = time.perf_counter()
    for j in range(s["digits"]):
        new_part(d, wheel_name(j), proc)
        build_wheel(b, s, j)
        weigh(d, mat)
    new_part(d, "transfer", proc)
    build_transfer(b, s)
    weigh(d, mat)
    new_part(d, "housing", proc)
    build_housing(b, s)
    weigh(d, mat)
    print(f"  parts: {s['digits']} wheels, a transfer and the housing in "
          f"{time.perf_counter() - t0:.0f} s")


def print_pose(s, count=0):
    """Each wheel's and transfer's turn, degrees, with the counter at
    `count` — the relations' own arithmetic (phases 0)."""
    theta = {0: -36.0 * count}
    beta = {}
    for j in range(s["digits"] - 1):
        beta[j] = carry_deg(theta[j], s["start_deg"])
        theta[j + 1] = -(s["zp"] / s["zw"]) * beta[j]
    return theta, beta


def quat_z(deg):
    a = math.radians(deg) / 2.0
    return [0.0, 0.0, math.sin(a), math.cos(a)]


def part_id(d, name):
    """A part's slot id by its tab name (a Derived names its source so)."""
    for p in d.call("list_parts")["parts"]:
        if p["name"] == name and p["type"] == "part":
            return p["id"]
    raise SystemExit(f"counter: no part '{name}'")


def build_print(d, s, proc, count=0):
    """The plate: a Derived copy of every part where it prints, turned to
    the pose that reads `count`. A copy, not a link, so the plate is what
    was checked."""
    theta, beta = print_pose(s, count)
    placed = [("housing", (0.0, 0.0, 0.0), 0.0)]
    for j in range(s["digits"]):
        placed.append((wheel_name(j), (0.0, 0.0, s["wheels"][j]["z0"]),
                       theta[j]))
    for j in range(s["digits"] - 1):
        placed.append(("transfer", (s["c"], 0.0, s["transfers"][j]["z0"]),
                       beta[j]))
    ids = {part: part_id(d, part) for part, _, _ in placed}
    new_part(d, "counter print", proc)
    names = {}
    for k, (part, pos, deg) in enumerate(placed):
        label = part if part != "transfer" else f"transfer {k - s['digits']}"
        f = check(d.call("add_feature", feature={
            "type": "Derived", "name": label, "source_part": ids[part],
            "reference": False,
            "placement": {"position": list(pos), "orientation": quat_z(deg)}}),
            f"derived {label}")
        names[label] = f["id"]
    return names, theta, beta


def build_assembly(d, s):
    """The proof that the plate counts: every wheel and transfer on a
    revolute to the housing's frames, a Geneva carry from each wheel to its
    transfer and a 9:30 gear from the transfer to the next wheel."""
    n = s["digits"]
    d.call("add_assembly", name="counter")
    occ = {"housing": d.call("add_occurrence", part="housing", name="housing",
                             fixed=True)["id"]}
    for j in range(n):
        occ[wheel_name(j)] = d.call("add_occurrence", part=wheel_name(j),
                                    name=wheel_name(j))["id"]
    for j in range(n - 1):
        occ[f"transfer {j + 1}"] = d.call(
            "add_occurrence", part="transfer", name=f"transfer {j + 1}")["id"]
    mates, rels = {}, {}
    for name in occ:
        if name == "housing":
            continue
        mates[name] = d.call(
            "add_mate", type="revolute", name=f"{name} axis",
            a={"occurrence": occ["housing"], "frame": name},
            b={"occurrence": occ[name], "origin": True}, flipped=False)["id"]
    for j in range(n - 1):
        t = f"transfer {j + 1}"
        rels[f"carry {j + 1}"] = check(d.call(
            "add_relation", type="intermittent", name=f"carry {j + 1}",
            a=mates[wheel_name(j)], b=mates[t], motion="geneva",
            slots=SLOTS, start_deg=s["start_deg"], phase=0.0),
            f"carry {j + 1}")
        rels[f"gear {j + 1}"] = check(d.call(
            "add_relation", type="gear", name=f"{t} drives "
            f"{wheel_name(j + 1)}", a=mates[t], b=mates[wheel_name(j + 1)],
            teeth_a=s["zp"], teeth_b=s["zw"], phase=0.0), f"gear {j + 1}")
    for name, r in rels.items():
        if r.get("error"):
            raise SystemExit(f"counter: relation {name}: {r['error']}")
    return occ, mates, rels


def set_count(d, mates, count):
    """Stand the units at `count` and let the relations place the rest."""
    d.call("set_mate", id=mates[wheel_name(0)],
           rotation_deg=-36.0 * count)
    d.call("solve_assembly")


def run_motion(d, name, driver, from_deg, to_deg, samples, clash, poses=False):
    st = d.call("add_motion_study", driver=driver, dof="rotation",
                **{"from": from_deg}, to=to_deg, samples=samples,
                clash_mode="mesh", clash_deflection_mm=clash, name=name)
    if st.get("why_not"):
        raise SystemExit(f"counter: motion study {name}: {st['why_not']}")
    sid = st.get("id", st.get("study"))
    if poses:
        return d.call("run_motion_study", study=sid, include_poses=True)
    d.call("run_motion_study", study=sid, **{"async": True})
    while True:
        r = d.call("motion_status", study=sid)
        if not r.get("running") and not r.get("job"):
            return r["result"]
        time.sleep(1.0)


def appeared(result):
    """Every pair a study found clashing at any sample. (Its `appeared`
    leaves out a pair already touching at the first sample, which is how a
    gear put half a tooth out starts.)"""
    return {tuple(sorted((c["a_name"], c["b_name"])))
            for smp in result.get("samples", [])
            for c in smp.get("clashes", [])}


def readability(s, result):
    """At every sample where the units stand on a digit, how far each wheel
    stands off its nearest digit, degrees, from the study's own poses."""
    worst = {}
    for smp in result.get("samples", []):
        mates = {m["name"]: m["rotation_deg"] for m in smp.get("mates", [])}
        u = mates.get(f"{wheel_name(0)} axis")
        if u is None or abs(u / 36.0 - round(u / 36.0)) > 1e-6:
            continue
        for j in range(s["digits"]):
            v = mates.get(f"{wheel_name(j)} axis")
            if v is None:
                continue
            off = abs(v - 36.0 * round(v / 36.0))
            worst[wheel_name(j)] = max(worst.get(wheel_name(j), 0.0), off)
    return worst


def counts(s, result):
    """The reading at the first and last sample, from the wheels' turns."""
    out = []
    for smp in (result["samples"][0], result["samples"][-1]):
        mates = {m["name"]: m["rotation_deg"] for m in smp.get("mates", [])}
        v = 0
        for j in range(s["digits"]):
            deg = mates.get(f"{wheel_name(j)} axis", 0.0)
            v += (round(-deg / 36.0) % 10) * 10 ** j
        out.append(v)
    return out


def hero(d, path):
    """The plate cut through both axes, seen from the cut: the transfers'
    Genevas and pinions in orange beside the wheels, the housing dark.
    Colours are appearance only; the bodies stay the profile's PLA."""
    d.call("set_active_part", part="counter print")
    for b_ in d.call("list_elements")["bodies"]:
        name = b_["name"]
        colour = ([0.03, 0.03, 0.035] if name.startswith("housing") else
                  [0.80, 0.25, 0.02] if name.startswith("transfer") else
                  [0.70, 0.70, 0.68])
        d.call("set_appearance", feature_id=b_["created_by"], color=colour,
               roughness=0.5)
    d.call("set_section", base_plane="xz", offset=0.0)  # keeps y <= 0
    d.call("view", name="iso")
    d.call("set_camera", direction=[0.25, 1.0, 0.3])  # from the cut side
    d.call("render_still", path=str(path), width=1200, height=1300)
    d.call("set_section", enabled=False)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int)
    ap.add_argument("--digit-height", type=float, default=8.0,
                    help="a digit's height around the band, mm")
    ap.add_argument("--digits", type=int, default=4)
    ap.add_argument("--layer", type=float, default=None,
                    help="the layer height the parts are sliced at, mm "
                         "(default: the profile's layer_height_mm, else "
                         "0.2, Bambu Studio's 0.20 mm preset)")
    ap.add_argument("--process", default="fdm")
    ap.add_argument("--material", default="pla")
    ap.add_argument("--profile", default=None,
                    help="one of your print profiles (list_print_profiles)")
    ap.add_argument("--fresh", action="store_true",
                    help="start a new project first (refused on unsaved work)")
    ap.add_argument("--out", default=None)
    ap.add_argument("--no-render", action="store_true",
                    help="skip the still (a --headless instance has no "
                         "renderer)")
    args = ap.parse_args()
    out = Path(args.out) if args.out else OUT
    out.mkdir(parents=True, exist_ok=True)

    d = connect(args.port)
    if args.fresh:
        fresh_project(d)
    proc = ({"profile": args.profile} if args.profile else
            {"process": args.process, "material": args.material})
    gap = d.call("print_allowance", kind="gap",
                 size_mm=args.digit_height, **proc)
    # The layer and the Z gap the profile states
    # (docs/tolerances/README.md#print-profiles): the vertical counterpart of
    # the gap, measured by print_coupon's air row.
    prof = next((p for p in d.call("list_print_profiles")["profiles"]
                 if p["name"] == args.profile), {}) if args.profile else {}
    layer = args.layer or prof.get("layer_height_mm") or 0.2
    z_gap = prof.get("z_gap_mm")
    if z_gap and args.layer and prof.get("layer_height_mm") and abs(
            args.layer - prof["layer_height_mm"]) > 1e-9:
        raise SystemExit(f"counter: --layer {args.layer:g} is not the "
                         f"{prof['layer_height_mm']:g} mm the profile's Z gap "
                         f"was measured at")
    s = solve(args.digit_height, args.digits, gap["allowance_mm"], layer,
              z_gap)
    g = s["g"]
    print(f"counter: {args.digits} digits {args.digit_height:g} mm high, "
          f"band Ø{2 * s['r_band']:.1f}, Geneva {SLOTS} slots, gear "
          f"{s['zp']}:{s['zw']} m{s['module']:.3f}, gap {g:g} a side "
          f"({args.profile or args.material}), lock disc "
          f"r{s['r_lock']:.2f} (horns {s['horn_mm']:.2f} mm), Z gap "
          f"{s['zgap']:g} ({s['zgap_from']})")

    build_parts(d, s, proc)
    feats, theta, beta = build_print(d, s, proc)
    geo = d.call("check_geometry")
    if geo["receipt"]["standing"] != "verified":
        raise SystemExit(f"counter: the print part is not verified: {geo}")
    # The app's printability check (check_print): on the bed, one piece per
    # body (housing, wheels and transfers), every pair of bodies at least
    # the gap a side apart on the exact B-rep, and the STL's chords a fifth
    # of the gap at most. Overhangs and bridges are reported: the bands'
    # undersides are 45 degree chamfers by design, and the layer gaps print
    # as ceilings over the part below.
    pr = check_print(d, "counter",
                     need=("bed", "shells", "clearance", "chords"))
    cl = {f"{p['a']} / {p['b']}": p["distance_mm"]
          for p in pr["clearance"]["pairs"]}
    stl = out / "counter.stl"
    exp = d.call("export_stl", path=str(stl))
    d.call("save_project", path=str(out / "counter.dsgn"))

    occ, mates, rels = build_assembly(d, s)
    clash = g / 5.0
    motion = {"clash_deflection_mm": clash, "negative_controls": {},
              "locks": {}}
    units = mates[wheel_name(0)]
    step = 2.0  # degrees of the units a sample, through every window

    # First, that the check can fail.
    rel_ids = {r["name"]: r["id"] for r in
               d.call("list_relations")["relations"]}
    carry1 = rel_ids["carry 1"]
    gear1 = rel_ids[f"transfer 1 drives {wheel_name(1)}"]
    window = (s["start_deg"] - 6.0, s["start_deg"] + s["window_deg"] + 6.0)
    # Each must clash, and between the parts it put wrong.
    controls = (
        ("the Geneva 10 degrees late",
         {"id": carry1, "start_deg": s["start_deg"] + 10.0},
         {"id": carry1, "start_deg": s["start_deg"]},
         (wheel_name(0), "transfer 1")),
        ("the gear half a tooth out",
         {"id": gear1, "phase": math.pi / s["zw"]},
         {"id": gear1, "phase": 0.0},
         ("transfer 1", wheel_name(1))))
    for label, bad_p, good_p, pair in controls:
        d.call("set_relation", **bad_p)
        r = run_motion(d, f"control: {label}", units, window[1], window[0],
                       int((window[1] - window[0]) / step) + 1, clash)
        d.call("set_relation", **good_p)
        seen = appeared(r)
        motion["negative_controls"][label] = {
            "status": r["collision_status"], "clashing": sorted(seen)}
        print(f"  negative control, {label}: {r['collision_status']} "
              f"({', '.join(' x '.join(p) for p in sorted(seen))})")
        if r["clean"] or tuple(sorted(pair)) not in seen:
            raise SystemExit(f"counter: {label} does not clash "
                             f"{pair[0]} with {pair[1]}: {seen}; the clash "
                             f"check cannot see it")

    # The counting.
    runs = (("ten digits and a carry", 0, 11),
            ("the triple carry", 998, 1001))
    for name, c0, c1 in runs:
        a0, a1 = -36.0 * c0, -36.0 * c1
        r = run_motion(d, name, units, a0, a1,
                       int(abs(a1 - a0) / step) + 1, clash, poses=True)
        res = r.get("result", r)
        read = counts(s, res)
        worst = readability(s, res)
        motion[name] = {"from_count": c0, "to_count": c1,
                        "status": res["collision_status"],
                        "reads": read, "off_digit_deg": worst}
        print(f"  motion, {name} ({c0:04d} -> {c1:04d}): "
              f"{res['collision_status']}; reads {read[0]:04d} -> "
              f"{read[1]:04d}; worst off a digit "
              + ", ".join(f"{k} {v:.2f}" for k, v in worst.items()))
        if not res["clean"]:
            raise SystemExit(f"counter: {name} clashes: "
                             f"{res.get('table', res)}")
        if read != [c0 % 10 ** s["digits"], c1 % 10 ** s["digits"]]:
            raise SystemExit(f"counter: {name} reads {read}, not "
                             f"{[c0, c1]}")
        if max(worst.values()) > 1.5:
            raise SystemExit(f"counter: a wheel stands {worst} degrees off "
                             f"its digit at rest")

    # The locks: with the driving wheel between carries, the next wheel
    # turned 3 degrees either way must run its Geneva into the lock disc.
    for j in range(s["digits"] - 1):
        count = 5 * 10 ** j            # wheel j at 5: mid-dwell
        set_count(d, mates, count)
        nxt = mates[wheel_name(j + 1)]
        here = next(m for m in d.call("list_mates")["mates"]
                    if m["id"] == nxt)["rotation_deg"]
        verdicts = []
        # What holds it: the transfer's Geneva arc against the driving
        # wheel's lock disc. (The carry also reports itself not satisfied,
        # since nothing turned its input: the lock, said by the relation.)
        lock_pair = tuple(sorted((wheel_name(j), f"transfer {j + 1}")))
        for sign in (1, -1):
            r = run_motion(d, f"lock {j + 1} {'+' if sign > 0 else '-'}",
                           nxt, here, here + 3.0 * sign, 7, clash)
            first = next((smp["value"] - here
                          for smp in r.get("samples", [])
                          if smp.get("clashes")), None)
            verdicts.append({"status": r["collision_status"],
                             "first_clash_deg": first,
                             "clashing": sorted(appeared(r))})
            if r["clean"] or lock_pair not in appeared(r):
                raise SystemExit(f"counter: {wheel_name(j + 1)} turns "
                                 f"freely with {wheel_name(j)} at rest: "
                                 f"{appeared(r)}")
        motion["locks"][wheel_name(j + 1)] = verdicts
        print(f"  lock, {wheel_name(j + 1)} turned 3 deg each way at "
              f"{count:04d}: " + ", ".join(
                  f"{v['status']} ({' x '.join(lock_pair)})"
                  for v in verdicts))
    set_count(d, mates, 0)

    # A transfer modelled at half the gap must fail the clearance gate: the
    # units wheel copied beside one, at a count (5) where the Geneva is
    # locked and the transfer stands at its own zero.
    new_part(d, "control: half gap", proc)
    build_transfer(Builder(d), s, gap=gap["allowance_mm"] / 2.0)
    weigh(d, material_of(d, proc))
    t0 = s["transfers"][0]["z0"]
    wheel = check(d.call("add_feature", feature={
        "type": "Derived", "name": wheel_name(0), "source_part":
        part_id(d, wheel_name(0)), "reference": False,
        "placement": {"position": [-s["c"], 0.0, s["wheels"][0]["z0"] - t0],
                      "orientation": quat_z(-36.0 * 5)}}),
        "control wheel")["id"]
    others = [b_["created_by"] for b_ in d.call("list_elements")["bodies"]
              if b_["created_by"] != wheel]
    if len(others) != 1:
        raise SystemExit(f"counter: the half-gap control has {others}")
    ctl = check_print(d, "control: half gap", need=(), quiet=True)
    near = ctl["clearance"]
    motion["negative_controls"]["a transfer at half the gap"] = {
        "wheel / transfer": near.get("min_mm"), "status": near["status"]}
    print(f"  negative control, a transfer at half the gap: "
          f"{near.get('min_mm', 0.0):.3f} mm, clearance {near['status']}")
    if near["status"] != "fail":
        raise SystemExit("counter: a transfer at half the gap passes the "
                         "clearance check")

    if not args.no_render:
        hero(d, out / "counter.png")

    receipt = {"digit_height": args.digit_height, "digits": args.digits,
               "process": proc, "sizes": {k: v for k, v in s.items()
                                          if k not in ("wheels",
                                                       "transfers")},
               "print_pose_deg": {"wheels": theta, "transfers": beta},
               "clearances_mm": cl, "print": print_receipt(pr),
               "motion": motion, "stl": str(stl),
               "stl_shells": exp.get("shells"),
               "stl_deflection_mm": exp.get("deflection_mm"),
               "confirm": gap.get("confirm")}
    (out / "counter.json").write_text(json.dumps(receipt, indent=1))
    d.call("save_project", path=str(out / "counter.dsgn"))
    print(f"counter: {exp.get('shells')} shells -> {stl}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
