#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A print-in-place mechanical counter: number wheels on one axis, a Geneva
carry between each pair, a housing with a window per digit. One plate, off
the bed reading 0000 and counting, every piece of it printed from the bed.

    python3 generators/counter/build.py --port 8801 --profile "K5 PLA on P1S"
    python3 generators/counter/build.py --port 8801 --digits 3 --digit-height 10

Each wheel carries a pin disc, a pin and a lock disc under its band of
digits. The pin turns a three-slot Geneva on a transfer beside the stack
once a turn, 120 degrees, and the transfer's 9-tooth pinion turns the next
wheel's 30-tooth gear 36 degrees: one digit. Between carries the lock disc
rides in the Geneva's concave arc and the next wheel cannot turn.

Every piece starts on the bed (generators/counter/README.md#the-print): each
wheel stands on a tube that runs down inside the wheels under it, each
transfer on a column of its own beside the stack, and no piece prints over
another but through a 45-degree face with the profile's air under it. The
2026-10-15 print, whose wheels each started one layer over the one below,
fused solid; that layout is kept as a negative control (stacked.py).

Every size derives from `--digit-height`, the print process's gap
(`print_allowance` kind `gap`) and its air (the profile's `z_gap_mm`, else
three layers): nothing here is a literal in millimetres except what those
scale (README "Where every size comes from").

What it builds:
- parts `wheel 1` (units, the knob) to `wheel 1000`, `transfer 1` to
  `transfer 3` and `housing`; an assembly `counter` with a revolute per
  wheel and transfer, an `intermittent` (Geneva, 3 slots) relation wheel ->
  transfer and a `gear` (9:30) relation transfer -> next wheel, and motion
  studies;
- the part `counter print`: every body where it prints, as Derived copies
  at the pose that reads 0000 in the middle of the profile's bed, exported
  as ONE STL of one shell per body and as a print job;
- the controls: the carry put wrong, a transfer at half the gap, and the
  old stacked layout (`control: stacked`), each of which must fail.

Output (generated, untracked): scratch/counter/ -- counter.stl,
counter_job.3mf, counter.dsgn and counter.json, the receipt.
"""
import argparse
import json
import math
import os
import sys
import time
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
from _lib import (PRINT_ITEMS, arc_through, bed_zones, check,  # noqa: E402
                  check_print, circle, clear_x, connect, export_print_job,
                  fresh_project, job_receipt, line, print_receipt)
from gear import involute_profile  # noqa: E402
import stacked  # noqa: E402

ROOT = Path(HERE).resolve().parent.parent
OUT = Path(os.environ["LOBOLE_SCRATCH"]) / "counter" if \
    os.environ.get("LOBOLE_SCRATCH") else ROOT / "scratch" / "counter"

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
WALL_LINES = 4  # a tube's wall, and the housing's round a column
# A digit's glyph is about 0.6 of its height wide (a sans numeral) and the
# band leaves 0.15 of the height beyond it, either side, along the axis. A
# digit is wanted to fill 70 % of its cell round the band; the band is as
# large as the carry needs, and at least that.
DIGIT_FILL = 0.7
GLYPH_WIDTH = 0.6
FACE_MARGIN = 0.15
# The transfers stand round the +X side of the stack, the windows at -X.
TRANSFER_SIDE_DEG = 0.0


def lines(n):
    return n * NOZZLE_MM


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


def solve(h, digits, gap, layer, air):
    """Every size from the digit height, the gap a side, the layer and the
    air (the vertical space under a face over another piece)."""
    if digits < 2:
        raise SystemExit("counter: a counter of one digit carries nothing")
    g = gap / 2.0
    # The pin and each transfer's column.
    r_pin = lines(3)
    r_col = max(0.25 * h, lines(WALL_LINES))
    # The Geneva's centre distance. A transfer's column rises from the bed
    # past every wheel's pin disc, which reaches out to the pin circle and
    # the pin, R + r_pin = c sin 60 + r_pin; the column clears it by the gap,
    # so c (1 - sin 60) >= r_col + r_pin + g. The pin then comes within the
    # gap of the column at the slot's bottom, which is where a three-slot
    # Geneva's pin goes. The band is the pin disc's size, and at least what
    # the digits fill.
    sin_n = math.sin(math.pi / SLOTS)
    r_digits = DIGITS_PER_TURN * (h / DIGIT_FILL) / (2.0 * math.pi)
    c = max((r_col + r_pin + g) / (1.0 - sin_n),
            (r_digits - r_pin) / sin_n) + 0.01
    R = c * sin_n                        # the pin circle
    r_g = c * math.cos(math.pi / SLOTS)  # the Geneva's radius
    r_band = R + r_pin
    cell = 2.0 * math.pi * r_band / DIGITS_PER_TURN
    # The gear: the next wheel's 30 teeth and the transfer's 9 turn a
    # 120-degree Geneva step into one digit (120 * 9/30 = 36). The coarsest
    # pair that keeps a printable tip.
    best = None
    for k in (1, 2, 3):
        zw, zp = 30 * k, 9 * k
        m = 2.0 * c / (zw + zp)
        j = 2.0 * g / math.cos(math.radians(PRESSURE_DEG))
        if zp >= MIN_TEETH and tip_land(m, zp, j / 2.0) >= MIN_TIP_MM:
            best = (zw, zp, m, j)
            break
    if best is None:
        raise SystemExit(f"counter: no gear pair with a {MIN_TIP_MM:.2f} mm "
                         f"tip fits a {h:g} mm digit at {g:g} mm a side")
    zw, zp, m, j = best
    tip_c = max(0.25 * m, g)  # tip clearance at least the gap
    rp_p, rp_w = m * zp / 2.0, m * zw / 2.0
    tip_p, tip_w = rp_p + ADDENDUM * m, rp_w + ADDENDUM * m
    root_p = rp_p - (ADDENDUM * m + tip_c)
    if tip_w + g > c - r_col:
        raise SystemExit("counter: a wheel's gear reaches the columns")
    if root_p < r_col + lines(2):
        raise SystemExit("counter: the pinion's root is under two lines "
                         "outside its column")
    # The tubes, innermost the units': each wheel stands on a tube inside
    # the wheels under it, a wall thick, the gap from the next.
    wall = lines(WALL_LINES)
    r_tube = [r_col + k_ * (g + wall) for k_ in range(digits)]
    bore = [0.0] + [r_tube[k_ - 1] + g for k_ in range(1, digits)]
    # The lock disc as large as the Geneva's horns allow, and round the
    # tubes it is threaded on.
    w = r_pin + g
    lo, hi = bore[-1] + wall, R - r_pin - g
    if lo >= hi or horn_mm(c, r_g, w, lo + g) < MIN_HORN_MM:
        raise SystemExit("counter: the Geneva's horns are too thin at any "
                         "lock disc round the tubes")
    for _ in range(60):
        mid = (lo + hi) / 2.0
        if horn_mm(c, r_g, w, mid + g) >= MIN_HORN_MM:
            lo = mid
        else:
            hi = mid
    r_lock = lo
    # The window: a teardrop on its side, a digit high and six lines over
    # (the glyph's height runs round the band, its width, drawn out by the
    # engraving's lean, along the axis), its roof flat under the bridge
    # limit.
    win_d = h + lines(6)
    h_face = max(GLYPH_WIDTH * h + 2.0 * FACE_MARGIN * h, win_d + lines(2))
    s = {
        "digit_height": h, "digits": digits, "gap": gap, "g": g,
        "layer": layer, "air": air, "cell": cell, "fill": h / cell,
        "r_digits": r_digits, "r_band": r_band, "r_col": r_col, "c": c,
        "R": R, "r_geneva": r_g, "r_pin": r_pin, "r_lock": r_lock,
        "zw": zw, "zp": zp, "module": m, "backlash": j,
        "tip_clearance": tip_c, "rp_w": rp_w, "tip_w": tip_w,
        "rp_p": rp_p, "tip_p": tip_p, "root_p": root_p,
        "slot_bottom": c - R, "horn_mm": horn_mm(c, r_g, w, r_lock + g),
        "r_tube": r_tube, "bore": bore, "wall": wall,
        "h_lock": 0.4 * h,              # the Geneva's working depth
        "h_mesh": 0.4 * h,              # the gears' full-tooth face
        "h_face": h_face, "window_d": win_d,
        "r_knob": 0.7 * r_band, "h_knob": 0.75 * h,
        # Two lines deep, plus the band's fall-away across half a glyph.
        "engrave": 1.5 * NOZZLE_MM,
    }
    # The carry's window: centred where the driving wheel goes from 9 (36
    # degrees) to 0 (0 degrees), counting being a turn BACK 36 a digit.
    s["carry_centre_deg"] = 18.0
    s["window_deg"] = 180.0 - 360.0 / SLOTS
    s["start_deg"] = s["carry_centre_deg"] - s["window_deg"] / 2.0
    # The transfers round the stack, apart by what keeps one's Geneva clear
    # of the next one's column and the housing round it.
    reach = r_g + g + r_col + g + wall
    spread = 2.0 * math.degrees(math.asin(min(1.0, reach / (2.0 * c))))
    s["transfer_spread_deg"] = spread
    n_t = digits - 1
    s["transfer_az"] = [TRANSFER_SIDE_DEG + spread * (k_ - (n_t - 1) / 2.0)
                        for k_ in range(n_t)]
    # The band's underside starts inside the deepest the horns go into the
    # lock disc (the relief, c - r_g from the axis), two lines short.
    s["r_band_foot"] = c - r_g - g - lines(2)
    layout(s)
    return s


def layout(s):
    """Heights, bottom to top: the most significant wheel on the bed, the
    units on top under the knob. Every wheel and transfer starts on the bed;
    each wheel's working parts stand over the wheel below through 45-degree
    faces and the air."""
    a, g, n = s["air"], s["g"], s["digits"]
    rb, rl, rg, rc = s["r_band"], s["r_lock"], s["r_geneva"], s["r_col"]
    wheels = {}
    for j in range(n - 1, -1, -1):
        w = {}
        wheels[j] = w
        if j == n - 1:
            # The thousands' band stands on the bed.
            w["band"] = (0.0, s["h_face"])
            w["face"] = (0.0, s["h_face"])
        else:
            base = wheels[j + 1]["top"] + a
            # The pin disc: a 45-degree cone from the wheel's tube out to
            # the band's size, over the wheel below's gear.
            z_pd = base + (rb - s["r_tube"][j])
            w["pin_disc"] = (base, z_pd)
            # The transfer's Geneva over it: its cone from the column, the
            # air over the pin disc, then the working depth.
            z_s0 = z_pd + a + (rg - rc)
            z_s1 = z_s0 + s["h_lock"]
            w["geneva"] = (z_pd + a, z_s0, z_s1)
            w["pin"] = (z_pd, z_s1)
            # The band over the Geneva: its 45-degree underside from inside
            # the lock disc, short of where the Geneva's horns reach into
            # it through the relief, so it stands on the lock disc all the
            # way round, the relief too; the air over the Geneva.
            b0 = z_s1 + a - g
            w["lock"] = (z_pd, b0)
            f0 = b0 + (rb - s["r_band_foot"])
            w["band"] = (b0, f0 + s["h_face"])
            w["face"] = (f0, f0 + s["h_face"])
        z = w["band"][1]
        if j > 0:
            # The gear on the band. The pinion that turns it starts the air
            # over the band less the gap it stands off it by, and slopes up
            # from its column at 45 degrees to full teeth: the gear runs on
            # past that by the mesh, and by the play of both stacks.
            z_p0 = z + a - g
            full = z_p0 + (s["tip_p"] - rc)
            top = full + s["h_mesh"] + 2.0 * a
            w["gear"] = (z, top)
            w["pinion"] = (z_p0, full, top)
            w["top"] = top
    s["wheels"] = wheels
    # The lip over the units' band that keeps the stack in, at 45 degrees
    # from the wall in to the knob, the air over the band.
    u = wheels[0]
    z_l = u["band"][1] + a
    rise = rb - s["r_knob"]
    s["lip"] = (z_l, z_l + rise, z_l + rise + lines(4))
    u["knob"] = (u["band"][1], s["lip"][2] + s["h_knob"])
    u["top"] = u["knob"][1]
    s["top"] = s["lip"][2]
    s["height"] = u["top"]
    s["transfers"] = {}
    for j in range(n - 1):
        s["transfers"][j] = {
            "column": (0.0, wheels[j]["geneva"][2]),
            "pinion": wheels[j + 1]["pinion"],
            "geneva": wheels[j]["geneva"], "az": s["transfer_az"][j]}


def wheel_name(j):
    return f"wheel {10 ** j}"


def transfer_name(j):
    return f"transfer {j + 1}"


# ---- sketching --------------------------------------------------------------

def plane_z(z, origin=(0.0, 0.0), flip=False):
    return {"origin": [origin[0], origin[1], z],
            "normal": [0, 0, -1 if flip else 1], "uAxis": [1, 0, 0]}


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

    def cone_under(self, name, r_top, z_top, r_bottom):
        """A disc's 45-degree underside: the circle `r_top` at `z_top` on
        the axis, extruded down to `r_bottom`, its wall leaning in at 45
        degrees (a tapered extrude, docs/modeling/README.md#tapered-extrudes).
        A downward plane's u runs along -X... a circle on the axis does not
        mind."""
        return self.extrude(name, plane_z(z_top, flip=True),
                            [circle((0.0, 0.0), r_top)], r_top - r_bottom,
                            taper_deg=45.0)

    def revolve(self, name, profile, centre=(0.0, 0.0)):
        """A solid of revolution about the vertical axis through `centre`:
        `profile` is (r, z) points, r >= 0, a closed polygon."""
        cx, cy = centre
        sk = self.sketch(f"{name} profile",
                         {"origin": [cx, cy, 0.0], "normal": [0, -1, 0],
                          "uAxis": [1, 0, 0]},
                         polygon([(r, z) for r, z in profile]))
        axis = ({"kind": "z"} if centre == (0.0, 0.0) else
                {"kind": "custom", "origin": [cx, cy, 0.0],
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


# Seven segments, each lit for the digits that use it: a top, b top right,
# c bottom right, d bottom, e bottom left, f top left, g middle.
SEGMENTS = {0: "abcdef", 1: "bc", 2: "abged", 3: "abgcd", 4: "fgbc",
            5: "afgcd", 6: "afgedc", 7: "abc", 8: "abcdefg", 9: "abcdfg"}


def digit_segments(dgt, height):
    """A digit as seven-segment bars, `height` tall along the sketch's v and
    0.55 of it wide along u, each bar a hexagon with gaps at the corners:
    no glyph closes round a counter. A font's 0, 4, 6, 8 and 9 do, and
    their counters, engraved on a wall, are islands that start in the air
    in some layer -- a slicer's "floating regions"."""
    H, W = height, 0.55 * height
    t = max(0.14 * H, lines(3))   # a bar's thickness: three lines
    gap = 0.5 * t
    hw, hh = W / 2.0, H / 2.0

    def bar(p, q):
        # A hexagon from p to q (its ends pointed), t thick, shortened by
        # the gap at both ends.
        (x0, y0), (x1, y1) = p, q
        L = math.hypot(x1 - x0, y1 - y0)
        ux, uy = (x1 - x0) / L, (y1 - y0) / L
        nx, ny = -uy, ux
        a = (x0 + gap * ux, y0 + gap * uy)
        b = (x1 - gap * ux, y1 - gap * uy)
        h = t / 2.0
        pts = [a, (a[0] + h * ux + h * nx, a[1] + h * uy + h * ny),
               (b[0] - h * ux + h * nx, b[1] - h * uy + h * ny), b,
               (b[0] - h * ux - h * nx, b[1] - h * uy - h * ny),
               (a[0] + h * ux - h * nx, a[1] + h * uy - h * ny)]
        return polygon(pts)
    # In the sketch: u (x) across the digit, v (y) up it.
    corners = {"tl": (-hw, hh), "tr": (hw, hh), "ml": (-hw, 0.0),
               "mr": (hw, 0.0), "bl": (-hw, -hh), "br": (hw, -hh)}
    ends = {"a": ("tl", "tr"), "b": ("tr", "mr"), "c": ("mr", "br"),
            "d": ("bl", "br"), "e": ("ml", "bl"), "f": ("tl", "ml"),
            "g": ("ml", "mr")}
    ents = []
    for seg in SEGMENTS[dgt]:
        p, q = ends[seg]
        ents += bar(corners[p], corners[q])
    return ents


def digit_text(label, height):
    return {"type": "text", "text": label, "font": "sans", "height": height,
            "anchor": [0.0, 0.0], "angle": 0.0, "halign": "center",
            "valign": "middle"}


# ---- the parts ------------------------------------------------------------
# Each in its own frame: the axis is Z through the origin, z = 0 the bed. A
# wheel at rotation 0 shows 0 in the window (at -X); digit d is engraved at
# azimuth 180 + 36 d, so counting is a turn BACK 36 degrees, and the next
# digit comes up from below. A transfer's frame has its +X pointing away
# from the stack's axis, which is at its 180 degrees, c away. Wheel j's pin
# stands 18 degrees short of its transfer's azimuth, on the line to it when
# the wheel is at +18, half way from 9 to 0.

def build_wheel(b, s, j):
    n = s["digits"]
    w = s["wheels"][j]
    g, rb, rl = s["g"], s["r_band"], s["r_lock"]
    bodies = []
    if j < n - 1:
        az = s["transfer_az"][j]
        base, z_pd = w["pin_disc"]
        bodies.append(b.prism(f"{wheel_name(j)} tube",
                              [circle((0, 0), s["r_tube"][j])], 0.0, base))
        bodies.append(b.cone_under(f"{wheel_name(j)} pin disc", rb, z_pd,
                                   s["r_tube"][j]))
        l0, l1 = w["lock"]
        bodies.append(b.prism(f"{wheel_name(j)} lock disc",
                              [circle((0, 0), rl)], l0, l1))
        pin_c = polar(s["R"], az - s["carry_centre_deg"])
        p0, p1 = w["pin"]
        bodies.append(b.prism(f"{wheel_name(j)} pin",
                              [circle(pin_c, s["r_pin"])], p0, p1))
        f0 = w["face"][0]
        bodies.append(b.cone_under(f"{wheel_name(j)} band underside", rb, f0,
                                   s["r_band_foot"]))
        bodies.append(b.prism(f"{wheel_name(j)} band",
                              [circle((0, 0), rb)], f0, w["face"][1]))
    else:
        bodies.append(b.prism(f"{wheel_name(j)} band",
                              [circle((0, 0), rb)], *w["band"]))
    if "gear" in w:
        # The gear's teeth face the transfer that turns it: rotated to its
        # azimuth, as the pinion's phase expects.
        g0, g1 = w["gear"]
        bodies.append(b.prism(f"{wheel_name(j)} gear",
                              gear_ents(s, s["zw"], s["transfer_az"][j - 1]),
                              g0, g1))
    else:
        k0, k1 = w["knob"]
        # A fluted knob: 24 points alternating between two radii.
        r = s["r_knob"]
        pts = [polar(r if i % 2 == 0 else 0.9 * r, 360.0 * i / 24)
               for i in range(24)]
        bodies.append(b.prism(f"{wheel_name(j)} knob", polygon(pts), k0, k1))
    body = b.union(wheel_name(j), bodies)
    if j < n - 1:
        # The relief the Geneva's horns swing through at mid-carry: its
        # circle, where it stands when the pin is on the line of centres,
        # through the lock disc to the band. The band's underside, rising
        # at 45 degrees from inside it, roofs it: no part of the band
        # starts in the air over it.
        az = s["transfer_az"][j]
        centre = polar(s["c"], az - s["carry_centre_deg"])
        l0, l1 = w["lock"]
        body = b.extrude(f"{wheel_name(j)} relieved",
                         plane_z(l0), [circle(centre, s["r_geneva"] + g)],
                         l1 - l0, mode="cut")
    if j > 0:
        # The bore the tubes inside it turn in, through.
        body = b.prism(f"{wheel_name(j)} bore",
                       [circle((0, 0), s["bore"][j])], 0.0, w["top"],
                       mode="cut")
    # The digits, engraved on the band's face. Cut straight in, a glyph's
    # upper walls would be ceilings over its pocket; cut in and DOWN at 45
    # degrees, from a plane leaning back over the face, every wall of the
    # pocket is a 45-degree face or one that looks up. The lean draws the
    # glyph's width along the axis out by sqrt 2 (README "The digits").
    f0, f1 = w["face"]
    zmid = (f0 + f1) / 2.0
    out = 1.0
    sag = (s["digit_height"] / 2.0) ** 2 / (2.0 * rb)
    k = math.sqrt(0.5)
    for dgt in range(DIGITS_PER_TURN):
        az = 180.0 + 36.0 * dgt
        nx, ny = math.cos(math.radians(az)), math.sin(math.radians(az))
        rr = rb + out
        plane = {"origin": [rr * nx, rr * ny, zmid + out],
                 "normal": [k * nx, k * ny, k],
                 "uAxis": [-k * nx, -k * ny, k]}
        body = b.extrude(f"{wheel_name(j)} digit {dgt}", plane,
                         digit_segments(dgt, s["digit_height"]),
                         (out + s["engrave"] + sag) / k, mode="cut",
                         reversed=True)
    return body


def geneva_slots(s):
    """Local slot directions, degrees: at rest (rotation 0) the concave arc
    between two slots faces the wheel axis at 180, so the slots stand at
    180 +- 60 and 0."""
    return [180.0 + 360.0 / SLOTS * (k + 0.5) for k in range(SLOTS)]


def build_transfer(b, s, j, gap=None):
    """Transfer j, in its own frame: its column from the bed, its pinion
    (sloping up from the column at 45 degrees), its Geneva (on a 45-degree
    cone from the column), the slots and arcs cut through both."""
    g = s["g"] if gap is None else gap / 2.0
    t = s["transfers"][j]
    rc = s["r_col"]
    col0, col1 = t["column"]
    p0, _, p1 = t["pinion"]
    v0, v_s0, v1 = t["geneva"]
    col = b.prism(f"{transfer_name(j)} column", [circle((0, 0), rc)],
                  col0, col1)
    # The pinion meshes the next wheel's gear across the line to the stack,
    # at 180: half a pitch off it. Its teeth's undersides slope up from the
    # column at 45 degrees (a prism cut by a cone), so the pinion prints
    # over the band below through 45-degree faces.
    pin_phase = 180.0 + 180.0 / s["zp"]
    pin = b.prism(f"{transfer_name(j)} pinion",
                  gear_ents(s, s["zp"], pin_phase), p0, p1)
    big = s["tip_p"] + 1.0
    cone = b.revolve(f"{transfer_name(j)} pinion slope", [
        (0.0, p0), (rc, p0), (big, p0 + (big - rc)), (big, p1), (0.0, p1)])
    pin = b.boolean(f"{transfer_name(j)} pinion sloped", pin, cone,
                    "intersect")
    gen_cone = b.cone_under(f"{transfer_name(j)} geneva underside",
                            s["r_geneva"], v_s0, rc)
    gen = b.prism(f"{transfer_name(j)} geneva",
                  [circle((0, 0), s["r_geneva"])], v_s0, v1)
    body = b.union(transfer_name(j), [col, pin, gen_cone, gen])
    w = s["r_pin"] + g
    for k, az in enumerate(geneva_slots(s)):
        # A slot: from its rounded bottom (the pin's nearest approach) out
        # past the rim, w either side of its centre line, down through the
        # cone the Geneva stands on, so the pin rising from its disc below
        # passes through it.
        r0 = s["slot_bottom"]
        r1 = s["r_geneva"] + 2.0
        a1 = rot((r1, w), az)
        a0 = rot((r0, w), az)
        b0_ = rot((r0, -w), az)
        b1_ = rot((r1, -w), az)
        bottom = rot((r0 - w, 0.0), az)
        ents = [line(a0, a1), line(a1, b1_), line(b1_, b0_),
                arc_through(b0_, bottom, a0)]
        body = b.extrude(f"{transfer_name(j)} slot {k + 1}", plane_z(v0),
                         ents, v1 - v0, mode="cut")
    for k, az in enumerate(geneva_slots(s)):
        arc_az = az + 180.0 / SLOTS
        body = b.extrude(f"{transfer_name(j)} arc {k + 1}", plane_z(v0),
                         [circle(polar(s["c"], arc_az), s["r_lock"] + g)],
                         v1 - v0, mode="cut")
    return body


def envelope_wheels(s):
    """The stack's swept envelope as (r, z) points: the band's size from
    the bed up (the thousands' band stands on it), run up to the top."""
    g = s["g"]
    return [(0.0, 0.0), (s["r_band"] + g, 0.0),
            (s["r_band"] + g, s["top"] + 1.0), (0.0, s["top"] + 1.0)]


def envelope_transfer(s, j):
    """Transfer j's swept envelope, (r, z) about its axis: everything it
    sweeps and everything over it, the gap out and the air under. The
    housing is what is left, so it holds the column from the bed to under
    the pinion, and never stands over a moving part."""
    g, a, rc = s["g"], s["air"], s["r_col"]
    t = s["transfers"][j]
    p0 = t["pinion"][0]
    v0 = t["geneva"][0]
    tp, rg = s["tip_p"], s["r_geneva"]
    top = s["top"] + 1.0
    return [(0.0, 0.0), (rc + g, 0.0), (rc + g, p0 - a),
            (tp + g, p0 - a + (tp - rc)), (tp + g, v0 - a + (tp - rc)),
            (rg + g, v0 - a + (rg - rc)), (rg + g, top), (0.0, top)]


def housing_extent(s):
    """The housing block's [x0, y0, x1, y1]: a wall round the stack and
    every transfer's Geneva."""
    m = s["g"] + s["wall"]
    x0, y0 = -(s["r_band"] + m), -(s["r_band"] + m)
    x1, y1 = s["r_band"] + m, s["r_band"] + m
    for t in s["transfers"].values():
        cx, cy = polar(s["c"], t["az"])
        r = s["r_geneva"] + m
        x0, y0 = min(x0, cx - r), min(y0, cy - r)
        x1, y1 = max(x1, cx + r), max(y1, cy + r)
    return [x0, y0, x1, y1]


def build_housing(b, s):
    """A block minus every moving part's swept envelope, each run up to the
    top: what is left stands only beside and under the moving parts, never
    over them. Then the lip over the units' band, a window per digit and
    the frames the mates use."""
    g, n = s["g"], s["digits"]
    x0, y0, x1, y1 = housing_extent(s)
    body = b.prism("housing block", polygon([
        (x0, y0), (x1, y0), (x1, y1), (x0, y1)]), 0.0, s["top"])
    env = b.revolve("housing: the stack swept", envelope_wheels(s))
    body = b.boolean("housing - the stack", body, env, "subtract")
    for j in range(n - 1):
        env = b.revolve(f"housing: {transfer_name(j)} swept",
                        envelope_transfer(s, j),
                        centre=polar(s["c"], s["transfer_az"][j]))
        body = b.boolean(f"housing - {transfer_name(j)}", body, env,
                         "subtract")
    # The lip: a quarter ring at -X over the units' band, its underside
    # rising at 45 degrees from the wall in to the knob, the air over the
    # band; it keeps the stack from lifting out.
    z_l, z_r, z_t = s["lip"]
    ri, ro = s["r_knob"] + g, s["r_band"] + g
    # Its outside sinks half a wall into the housing: run out to the
    # block's face it would graze it at -X, a seam no solid holds.
    rw = ro + 0.5 * s["wall"]
    lip = b.revolve("lip ring", [(ri, z_r), (ro, z_l), (rw, z_l),
                                 (rw, z_t), (ri, z_t)])
    sector = b.prism("lip sector", polygon([
        (0.0, 0.0), polar(2.0 * ro, 135.0), (-2.0 * ro, 0.0),
        polar(2.0 * ro, 225.0)]), z_l - 1.0, z_t + 1.0)
    lip = b.boolean("lip", lip, sector, "intersect")
    body = b.boolean("housing + lip", body, lip, "union")
    # A window per digit, at -X: a teardrop on its side through the wall
    # (docs/modeling/holes-and-threads.md#printed-holes), its point up,
    # centred on the band's face. A Hole drills along its sketch plane's
    # -normal: the plane faces -X, its (u, v) = (y, -z).
    sk = b.sketch("windows", {"origin": [x0, 0.0, 0.0], "normal": [-1, 0, 0],
                              "uAxis": [0, 1, 0]},
                  [{"type": "point", "p": [0.0, -(f0 + f1) / 2.0]}
                   for f0, f1 in (s["wheels"][j]["face"] for j in range(n))])
    check(b.d.call("add_feature", feature={
        "type": "Hole", "name": "windows", "sketch_id": sk,
        "hole_kind": "simple", "diameter": s["window_d"],
        "depth": s["wall"] + g + 1.0, "printed": "teardrop_flat"}),
        "windows")
    for j in range(n):
        check(b.d.call("add_feature", feature={
            "type": "Frame", "name": wheel_name(j), "kind": "explicit",
            "origin": [0.0, 0.0, 0.0],
            "z_axis": [0, 0, 1], "x_axis": [1, 0, 0]}), "wheel frame")
    for j in range(n - 1):
        az = s["transfer_az"][j]
        cx, cy = polar(s["c"], az)
        check(b.d.call("add_feature", feature={
            "type": "Frame", "name": transfer_name(j), "kind": "explicit",
            "origin": [cx, cy, 0.0], "z_axis": [0, 0, 1],
            "x_axis": [math.cos(math.radians(az)),
                       math.sin(math.radians(az)), 0]}), "transfer frame")
    return body


# ---- the project ------------------------------------------------------------

def profile_of(d, proc):
    if "profile" not in proc:
        return {}
    for p in d.call("list_print_profiles")["profiles"]:
        if p["name"] == proc["profile"]:
            return p
    raise SystemExit(f"counter: no print profile '{proc['profile']}'")


def material_of(d, proc):
    if "material" in proc:
        return proc["material"]
    return profile_of(d, proc)["material"]


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
    for j in range(s["digits"] - 1):
        new_part(d, transfer_name(j), proc)
        build_transfer(b, s, j)
        weigh(d, mat)
    new_part(d, "housing", proc)
    build_housing(b, s)
    weigh(d, mat)
    print(f"  parts: {s['digits']} wheels, {s['digits'] - 1} transfers and "
          f"the housing in {time.perf_counter() - t0:.0f} s")


def print_pose(s, count=0):
    """Each wheel's and transfer's turn, degrees, with the counter at
    `count` -- the relations' own arithmetic (phases 0)."""
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


def bed_place(s, prof, gap):
    """Where the plate's axis stands on the bed: the housing's middle at
    the bed's, moved along x past its keep-out zones by the gap when it
    would sit on one. The origin without a bed (the built-in table)."""
    bed = prof.get("bed_mm")
    if not bed:
        return (0.0, 0.0)
    x0, y0, x1, y1 = housing_extent(s)
    w, h = x1 - x0, y1 - y0
    x = clear_x(bed[0] / 2.0 - w / 2.0, bed[1] / 2.0 - h / 2.0, w, h,
                bed_zones(prof), gap)
    return (x - x0, bed[1] / 2.0 - h / 2.0 - y0)


def build_print(d, s, proc, at, count=0):
    """The plate: a Derived copy of every part where it prints, turned to
    the pose that reads `count`, the stack's axis at `at` on the bed. A
    copy, not a link, so the plate is what was checked."""
    theta, beta = print_pose(s, count)
    ax, ay = at
    placed = [("housing", (ax, ay, 0.0), 0.0)]
    for j in range(s["digits"]):
        placed.append((wheel_name(j), (ax, ay, 0.0), theta[j]))
    for j in range(s["digits"] - 1):
        az = s["transfer_az"][j]
        cx, cy = polar(s["c"], az)
        placed.append((transfer_name(j), (ax + cx, ay + cy, 0.0),
                       az + beta[j]))
    ids = {part: part_id(d, part) for part, _, _ in placed}
    new_part(d, "counter print", proc)
    names = {}
    for part, pos, deg in placed:
        f = check(d.call("add_feature", feature={
            "type": "Derived", "name": part, "source_part": ids[part],
            "reference": False,
            "placement": {"position": list(pos), "orientation": quat_z(deg)}}),
            f"derived {part}")
        names[part] = f["id"]
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
        occ[transfer_name(j)] = d.call(
            "add_occurrence", part=transfer_name(j),
            name=transfer_name(j))["id"]
    mates, rels = {}, {}
    for name in occ:
        if name == "housing":
            continue
        mates[name] = d.call(
            "add_mate", type="revolute", name=f"{name} axis",
            a={"occurrence": occ["housing"], "frame": name},
            b={"occurrence": occ[name], "origin": True}, flipped=False)["id"]
    for j in range(n - 1):
        t = transfer_name(j)
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
    leaves out a pair already clashing at the first sample, which is how a
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


def hero(d, path, at=(0.0, 0.0)):
    """The plate cut through the stack's axis (at `at` on the bed), seen
    from the cut: the transfers' Genevas and pinions in orange beside the
    wheels, the housing dark. Colours are appearance only; the bodies stay
    the profile's PLA."""
    d.call("set_active_part", part="counter print")
    for b_ in d.call("list_elements")["bodies"]:
        name = b_["name"]
        colour = ([0.03, 0.03, 0.035] if name.startswith("housing") else
                  [0.80, 0.25, 0.02] if name.startswith("transfer") else
                  [0.70, 0.70, 0.68])
        d.call("set_appearance", feature_id=b_["created_by"], color=colour,
               roughness=0.5)
    d.call("set_section", base_plane="xz", offset=at[1])  # keeps y <= axis
    d.call("view", name="iso")
    d.call("set_camera", direction=[0.25, 1.0, 0.3])  # from the cut side
    d.call("render_still", path=str(path), width=1200, height=1300)
    d.call("set_section", enabled=False)


def fit_digits(s_of, digits, prof):
    """The most digits, up to `digits`, whose counter stands within the
    profile's bed (its bed_mm height and plan): (digits, sizes, why fewer)."""
    bed = prof.get("bed_mm")
    why = None
    for n in range(digits, 1, -1):
        s = s_of(n)
        if not bed:
            return n, s, why
        x0, y0, x1, y1 = housing_extent(s)
        too = []
        if len(bed) > 2 and s["height"] > bed[2]:
            too.append(f"{s['height']:.1f} mm tall against the bed's "
                       f"{bed[2]:g}")
        if x1 - x0 > bed[0] or y1 - y0 > bed[1]:
            too.append(f"{x1 - x0:.1f} x {y1 - y0:.1f} mm in plan against "
                       f"{bed[0]:g} x {bed[1]:g}")
        if not too:
            return n, s, why
        why = (why or "") + f"{n} digits: " + ", ".join(too) + "; "
    raise SystemExit(f"counter: not even two digits fit the bed: {why}")


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
                    help="start a new project first (an untitled scratch one is "
                         "discarded; a saved one with unsaved work refused)")
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
    # The layer and the air the profile states
    # (docs/tolerances/README.md#print-profiles): the vertical counterpart
    # of the gap, measured by print_coupon's air row; else three layers,
    # which is what check_print's stacking holds a part to without one.
    prof = profile_of(d, proc)
    layer = args.layer or prof.get("layer_height_mm") or 0.2
    z_gap = prof.get("z_gap_mm")
    if z_gap and args.layer and prof.get("layer_height_mm") and abs(
            args.layer - prof["layer_height_mm"]) > 1e-9:
        raise SystemExit(f"counter: --layer {args.layer:g} is not the "
                         f"{prof['layer_height_mm']:g} mm the profile's Z gap "
                         f"was measured at")
    air = z_gap or 3.0 * layer
    air_from = ("the profile's z_gap_mm" if z_gap else
                f"three layers of {layer:g}")
    digits, s, fewer = fit_digits(
        lambda n: solve(args.digit_height, n, gap["allowance_mm"], layer,
                        air), args.digits, prof)
    s["air_from"] = air_from
    if fewer:
        print(f"counter: {args.digits} digits do not fit "
              f"{args.profile}'s bed ({fewer.rstrip('; ')}): building "
              f"{digits}, the most that do")
    g = s["g"]
    print(f"counter: {digits} digits {args.digit_height:g} mm high, "
          f"band Ø{2 * s['r_band']:.1f} ({s['fill']:.0%} of a cell), Geneva "
          f"{SLOTS} slots at {s['c']:.2f}, gear {s['zp']}:{s['zw']} "
          f"m{s['module']:.3f}, gap {g:g} a side "
          f"({args.profile or args.material}), lock disc "
          f"r{s['r_lock']:.2f} (horns {s['horn_mm']:.2f} mm), air "
          f"{air:g} ({air_from}), {s['height']:.1f} mm tall")

    build_parts(d, s, proc)
    at = bed_place(s, prof, g)
    feats, theta, beta = build_print(d, s, proc, at)
    geo = d.call("check_geometry")
    if geo["receipt"]["standing"] != "verified":
        raise SystemExit(f"counter: the print part is not verified: {geo}")
    # The app's printability check (check_print), every item: on the bed,
    # one piece per body, nothing past 45 degrees, no ceiling past the
    # bridge limit, the gap a side between every pair, the chords a fifth of
    # it, every piece on the bed and no face within the air over another
    # piece, and the plate on the profile's bed.
    pr = check_print(d, "counter", need=PRINT_ITEMS)
    cl = {f"{p['a']} / {p['b']}": p["distance_mm"]
          for p in pr["clearance"]["pairs"]}
    stl = out / "counter.stl"
    exp = d.call("export_stl", path=str(stl))
    job = ({"skipped": "no print profile with a bed: the built-in table is "
                       "no printer to place the plate on"}
           if not prof.get("bed_mm") else
           job_receipt(export_print_job(d, [("counter print", "counter")],
                                        out / "counter_job.3mf",
                                        material_of(d, proc))))
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
    gear1 = rel_ids[f"{transfer_name(0)} drives {wheel_name(1)}"]
    window = (s["start_deg"] - 6.0, s["start_deg"] + s["window_deg"] + 6.0)
    # Each must clash, and between the parts it put wrong.
    controls = (
        ("the Geneva 10 degrees late",
         {"id": carry1, "start_deg": s["start_deg"] + 10.0},
         {"id": carry1, "start_deg": s["start_deg"]},
         (wheel_name(0), transfer_name(0))),
        ("the gear half a tooth out",
         {"id": gear1, "phase": math.pi / s["zw"]},
         {"id": gear1, "phase": 0.0},
         (transfer_name(0), wheel_name(1))))
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
    top = 10 ** digits
    runs = (("ten digits and a carry", 0, 11),
            ("the triple carry" if digits >= 4 else "the carries through",
             top // 10 * 10 - 2 if digits >= 4 else top - 2,
             top // 10 * 10 + 1 if digits >= 4 else top + 1))
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
        print(f"  motion, {name} ({c0:0{digits}d} -> {c1 % top:0{digits}d}): "
              f"{res['collision_status']}; reads {read[0]:0{digits}d} -> "
              f"{read[1]:0{digits}d}; worst off a digit "
              + ", ".join(f"{k} {v:.2f}" for k, v in worst.items()))
        if not res["clean"]:
            raise SystemExit(f"counter: {name} clashes: "
                             f"{res.get('table', res)}")
        if read != [c0 % top, c1 % top]:
            raise SystemExit(f"counter: {name} reads {read}, not "
                             f"{[c0 % top, c1 % top]}")
        if max(worst.values()) > 1.5:
            raise SystemExit(f"counter: a wheel stands {worst} degrees off "
                             f"its digit at rest")

    # The locks: with the driving wheel between carries, the next wheel
    # turned 3 degrees either way must run its Geneva into the lock disc.
    for j in range(digits - 1):
        count = 5 * 10 ** j            # wheel j at 5: mid-dwell
        set_count(d, mates, count)
        nxt = mates[wheel_name(j + 1)]
        here = next(m for m in d.call("list_mates")["mates"]
                    if m["id"] == nxt)["rotation_deg"]
        verdicts = []
        # What holds it: the transfer's Geneva arc against the driving
        # wheel's lock disc. (The carry also reports itself not satisfied,
        # since nothing turned its input: the lock, said by the relation.)
        lock_pair = tuple(sorted((wheel_name(j), transfer_name(j))))
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
              f"{count:0{digits}d}: " + ", ".join(
                  f"{v['status']} ({' x '.join(lock_pair)})"
                  for v in verdicts))
    set_count(d, mates, 0)

    # A transfer modelled at half the gap must fail the clearance gate: the
    # units wheel copied beside one, at a count (5) where the Geneva is
    # locked and the transfer stands at its own zero.
    new_part(d, "control: half gap", proc)
    build_transfer(Builder(d), s, 0, gap=gap["allowance_mm"] / 2.0)
    weigh(d, material_of(d, proc))
    wheel = check(d.call("add_feature", feature={
        "type": "Derived", "name": wheel_name(0), "source_part":
        part_id(d, wheel_name(0)), "reference": False,
        "placement": {"position": [-s["c"], 0.0, 0.0],
                      "orientation": quat_z(-36.0 * 5
                                            - s["transfer_az"][0])}}),
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

    # The layout that fused, at two digits: its stacking must fail.
    control = stacked.build_control(d, proc, args.digit_height,
                                    gap["allowance_mm"], layer, z_gap)
    old = check_print(d, "control: stacked", need=(), quiet=True,
                      part=control)
    ost = old["stacking"]
    motion["negative_controls"]["the stacked layout"] = {
        "status": ost["status"], "lifted": len(ost["lifted"]),
        "pairs": len(ost["pairs"])}
    print(f"  negative control, the stacked layout: stacking "
          f"{ost['status']}, {len(ost['lifted'])} pieces off the bed, "
          f"{len(ost['pairs'])} pairs within the air"
          + (" -> fails, as it must" if ost["status"] == "fail" else ""))
    if ost["status"] != "fail":
        raise SystemExit("counter: the stacked layout passes stacking; the "
                         "check cannot see it")

    if not args.no_render:
        hero(d, out / "counter.png", at)

    receipt = {"digit_height": args.digit_height, "digits": digits,
               "digits_asked": args.digits, "digits_why": fewer,
               "process": proc, "sizes": {k: v for k, v in s.items()
                                          if k not in ("wheels",
                                                       "transfers")},
               "heights": {"wheels": s["wheels"],
                           "transfers": s["transfers"]},
               "plate_axis_mm": list(at),
               "print_pose_deg": {"wheels": theta, "transfers": beta},
               "clearances_mm": cl, "print": print_receipt(pr),
               "motion": motion, "stl": str(stl),
               "stl_shells": exp.get("shells"),
               "stl_deflection_mm": exp.get("deflection_mm"),
               "print_job": job, "confirm": gap.get("confirm")}
    (out / "counter.json").write_text(json.dumps(receipt, indent=1))
    d.call("save_project", path=str(out / "counter.dsgn"))
    print(f"counter: {exp.get('shells')} shells -> {stl}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
