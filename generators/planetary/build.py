#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A print-in-place herringbone planetary gear set: one plate, five bodies,
off the bed already assembled and turning.

    python3 generators/planetary/build.py --port 8801 --fresh
    python3 generators/planetary/build.py --port 8801 --diameter 90

A sun, three planets and an internal ring, all with herringbone teeth: the V
holds every gear at its height, so nothing needs a carrier, a pin or a
support. Rolling bearers at the pitch circles, above the teeth, keep the
centre distances to within the gap, so a push on the sun cannot open a
mesh. Turn the hex knob on the sun with the ring held and the planets
orbit; the ratio is engraved on the ring.

Every size derives from `--diameter` (the ring's outside) and the print
process's gap (`print_allowance` kind `gap`): the module, the tooth counts,
the face width, the ring's wall, the knob and the lettering. Nothing here is
a literal in millimetres except what the diameter scales.

What it builds (generators/planetary/README.md):
- the part `planetary print`: the five bodies where they print, exported as
  ONE STL whose shells must number five (two bodies that touch would fuse);
- the parts `sun`, `planet` and `ring` and an assembly `planetary` with a
  kinematic carrier, the gear ratios as relations, and a motion study;
- a static study on the sun's teeth under a firm pinch on the knob;
- the push check: the worst push on the sun, against the engagement left on
  the mesh it opens, with the bearers and (the control) without.

Output (generated, untracked): scratch/planetary/ — the STL, the project and
planetary.json, the receipt (counts, module, ratio, gap, clearances, factor
of safety).
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
from _lib import (check, check_print, circle, connect, fresh_project,  # noqa: E402
                  line, print_receipt)
from gear import involute_profile, wedge  # noqa: E402

ROOT = Path(HERE).resolve().parent.parent
OUT = ROOT / "scratch" / "planetary"

PLANETS = 3
# Stub teeth at 25 degrees. A 70 mm set wants suns and planets of 12-15
# teeth, and the printed gap thins every tooth by a fixed amount, which a
# small tooth notices:
# - At 25 degrees an involute is not undercut down to 2 a / sin^2(25) = 9
#   teeth (a = the addendum below), so the flanks are true involutes to the
#   root and an internal ring can keep its whole tooth. At 20 the ring tooth
#   had to shrink to half a module to clear a 13-tooth planet's radial
#   (undercut) flank.
# - A stub addendum of 0.8 module (AGMA's stub form) leaves a printable tip:
#   about 0.7 mm on a 14-tooth sun, against 0.29 mm at a full module.
PRESSURE_DEG = 25.0
ADDENDUM = 0.8
# 30 degrees is the usual herringbone helix: steep enough that the V holds
# the gears axially, shallow enough that a printed flank (which overhangs by
# the helix angle) needs no support.
HELIX_DEG = 30.0
# The fewest teeth whose involute is not undercut at PRESSURE_DEG (gear.py
# draws no profile shift, and draws an undercut flank radially).
MIN_TEETH = 12
# The force a firm pinch puts on a knob: adult lateral (key) pinch strength
# is 60-100 N (Mathiowetz et al. 1985, grip and pinch norms); the low end,
# since a fidget is turned, not wrenched.
PINCH_N = 60.0
NOZZLE_MM = 0.4  # the P1S's stock nozzle, which the allowance table assumes
# The narrowest tip worth printing: a line and a half, which a variable-width
# slicer (Bambu Studio's Arachne) fills with one widened line.
MIN_TIP_MM = 1.5 * NOZZLE_MM
# The layer the README slices at (Bambu Studio's 0.20 mm preset on the P1S).
# A gap measured on the profile is between WALLS; where one gear's material
# crosses over another's horizontal face, the slicer needs an empty layer.
LAYER_MM = 0.2
# The worst push may leave no less than this share of the working depth
# engaged at the mesh it opens: half, one addendum, so each tooth still
# reaches the other gear's pitch circle and they bear flank on flank. The
# print without bearers had 40 % and was felt to be near a skip.
MIN_ENGAGED = 0.5


def plane(cx, cy, z):
    return {"origin": [cx, cy, z], "normal": [0, 0, 1], "uAxis": [1, 0, 0]}


def frac(x):
    return x - math.floor(x)


def solve(diameter, gap):
    """Module, tooth counts and every derived size, from the ring's outside
    diameter and the gap a side. Refuses what will not print."""
    g = gap / 2.0
    # A module that scales with the part: 50 modules across the ring, to the
    # nearest 0.05 — 1.4 at 70 mm, where a tooth tip is several lines wide.
    m = max(0.05, round(diameter / 50.0 / 0.05) * 0.05)
    # And teeth deep enough for the gap. The bearers let each mesh close by
    # the gap, so the worst push on the sun opens the far mesh by
    # g (2 / cos(pi/N) + 1) (push_check, which measures it); at least
    # MIN_ENGAGED of the working depth must stay. A coarse process wants
    # bigger teeth: at the built-in table's 0.25 a side, 1.6 against 1.4.
    m_push = (g * (2.0 / math.cos(math.pi / PLANETS) + 1.0)
              / (2.0 * ADDENDUM * (1.0 - MIN_ENGAGED)))
    why = ""
    if m_push > m:
        m = math.ceil(m_push / 0.05 - 1e-9) * 0.05
        why = (f" (the module the {g:g} mm gap needs, so the worst push "
               f"leaves {MIN_ENGAGED:.0%} of the teeth engaged)")
    # Tip clearance at least the gap: a printed tip on a printed root fuses
    # as readily as a flank on a flank.
    c = max(0.25 * m, g)
    wall = 0.07 * diameter
    zr_max = int(2.0 * (diameter / 2.0 - wall - (ADDENDUM * m + c)) / m)
    best = None
    for zr in range(zr_max, zr_max - 4, -1):
        for zs in range(MIN_TEETH, zr):
            zp2 = zr - zs
            if zp2 % 2:
                continue
            zp = zp2 // 2
            if zp < MIN_TEETH or (zs + zr) % PLANETS:
                continue
            # Neighbouring planets must clear each other tip to tip.
            a = m * (zs + zp) / 2.0
            chord = 2.0 * a * math.sin(math.pi / PLANETS)
            if chord - 2.0 * (m * zp / 2.0 + ADDENDUM * m) < 2.0 * g:
                continue
            score = abs(zs - zp) + 2 * (zr_max - zr)
            if best is None or score < best[0]:
                best = (score, zs, zp, zr)
    if best is None:
        raise SystemExit(f"planetary: no sun/planet/ring with at least "
                         f"{MIN_TEETH} teeth fits a Ø{diameter} ring at "
                         f"module {m:g}{why} — a larger --diameter"
                         + (", or a --profile measured on your printer "
                            "(a smaller gap)" if why else ""))
    _, zs, zp, zr = best
    alpha = math.radians(PRESSURE_DEG)
    beta = math.radians(HELIX_DEG)
    beta_b = math.atan(math.tan(beta) * math.cos(alpha))
    # Backlash on the pitch circle that leaves `g` between flanks along their
    # NORMAL: the transverse gap times cos(alpha) is the gap in the
    # transverse plane, and the helix tilts the flank by beta_b out of it.
    j = 2.0 * g / (math.cos(alpha) * math.cos(beta_b))
    # The ring's tooth is shortened so its tip only ever meets the planet's
    # involute. Below the planet's base circle gear.py draws the flank
    # radially (no undercut), and a full-height internal tooth reaches it:
    # the tip must stay outside the point where the line of action touches
    # the planet's base circle, plus the gap.
    rr, rpp = m * zr / 2.0, m * zp / 2.0
    tx = rr - rpp * math.sin(alpha) ** 2
    ty = rpp * math.sin(alpha) * math.cos(alpha)
    ring_addendum = min(ADDENDUM, (rr - math.hypot(tx, ty) - g) / m)
    if ring_addendum < 0.5:
        raise SystemExit(f"planetary: the ring's teeth would be "
                         f"{ring_addendum:.2f} modules tall to clear the "
                         f"planets — more planet teeth (a larger --diameter)")
    height = 0.3 * diameter
    # Rolling bearers above the teeth: a band at each pitch circle, half the
    # gap inside it on the sun and planets and outside it on the ring, so two
    # bands clear by the gap and roll at the pitch point
    # (generators/planetary/README.md#bearers). The neighbour's teeth reach
    # an addendum under every band, so the band stands on a 45 degree cone
    # that starts the gap plus an empty layer and the gap inside that reach:
    # it clears the tip by the gap in plan through the first layer, and
    # crosses over it a layer up. Two modules of band: fourteen layers.
    reach = ADDENDUM * m + g + (LAYER_MM + g)
    band = 2.0 * m
    cone = reach - g / 2.0
    teeth = height - cone - band
    if teeth < 0.5 * height:
        raise SystemExit(f"planetary: the bearers ({cone + band:.2f} mm) "
                         f"leave {teeth:.2f} mm of teeth in {height:.2f} — "
                         f"a larger --diameter")
    for z, b in ((zs, j / 2.0), (zp, j / 2.0)):
        land = tip_land(m, z, b)
        if land < MIN_TIP_MM:
            raise SystemExit(f"planetary: a {z}-tooth gear's tip is "
                             f"{land:.2f} mm wide, under the {MIN_TIP_MM} "
                             f"mm a {NOZZLE_MM} mm nozzle draws — a larger "
                             f"--diameter")
    return {
        "module": m, "zs": zs, "zp": zp, "zr": zr, "tip_clearance": c,
        "ring_addendum": ring_addendum,
        "backlash": j, "wall": wall, "height": height,
        "teeth_height": teeth, "band_height": band, "cone_height": cone,
        # From each gear's pitch radius: the band (+ inward on the ring) and
        # the cone's foot, both signed so `pitch - x` is an external gear's.
        "band_inset": g / 2.0, "cone_inset": reach,
        "centre": m * (zs + zp) / 2.0,
        "ratio": 1.0 + zr / zs,
        "knob_height": 0.3 * height,
        # Three or four layers at the P1S's 0.2 mm: deep enough to read.
        "engrave": 0.01 * diameter,
        "gap": gap,
    }


def tip_land(m, z, backlash):
    """Width of a tooth's tip, mm: what the nozzle has to draw there."""
    alpha = math.radians(PRESSURE_DEG)
    rp = m * z / 2.0
    rb, ra = rp * math.cos(alpha), rp + ADDENDUM * m

    def inv(a):
        return math.tan(a) - a

    half = (math.pi / (2 * z) - backlash / (2.0 * rp) + inv(alpha)
            - inv(math.acos(rb / ra)))
    return 2.0 * half * ra


def phases(s):
    """Where each gear's tooth 0 points so the set meshes, in radians: the
    sun's at 0, each planet's about its own centre, and the ring cutter's.

    A planet at angle th faces the sun across the line th + pi. If the sun's
    nearest tooth sits a fraction `f` of a pitch counter-clockwise of th, the
    planet's must sit half a pitch less `f` of its own counter-clockwise of
    th + pi (the two pitch circles run opposite ways through the contact).
    The ring meets the planet at th, where both run the same way, so the
    cutter's tooth (the ring's space) goes where the planet's tooth is."""
    zs, zp, zr = s["zs"], s["zp"], s["zr"]
    out = {"sun": 0.0, "planets": [], "ring": None}
    ring_fracs = []
    for i in range(PLANETS):
        th = 2.0 * math.pi * i / PLANETS
        f = frac(-th * zs / (2.0 * math.pi))
        phi = th + math.pi + (0.5 - f) * 2.0 * math.pi / zp
        out["planets"].append((th, phi))
        q = frac((th - phi) * zp / (2.0 * math.pi))
        ring_fracs.append(frac((th - q * 2.0 * math.pi / zr)
                               * zr / (2.0 * math.pi)))
    # (zs + zr) % PLANETS == 0 is what makes every planet ask for the same
    # ring phase; said again here, since a wrong ring is the one mistake
    # that prints and never turns.
    for rf in ring_fracs[1:]:
        dd = abs(rf - ring_fracs[0])
        if min(dd, 1.0 - dd) > 1e-9:
            raise SystemExit(f"planetary: the planets disagree on the ring's "
                             f"phase: {ring_fracs}")
    out["ring"] = ring_fracs[0] * 2.0 * math.pi / zr
    return out


def herringbone(d, name, s, z, centre, phase, twist_sign, backlash,
                addendum=ADDENDUM, dedendum=1.25):
    """A herringbone gear (or ring cutter) as one body: the tooth outline
    extruded with a twist up to the middle and back (Extrude `twist_deg` and
    `herringbone`, docs/modeling/README.md#twisted-extrudes). The flanks are
    exact helicoids about the sketch plane's origin, which is the gear's
    centre; the twist over the full height makes a helix of HELIX_DEG at the
    pitch circle, and the herringbone climbs half of it to the apex."""
    m, h = s["module"], s["teeth_height"]
    rp = m * z / 2.0
    k = twist_sign * math.degrees(math.tan(math.radians(HELIX_DEG)) / rp)
    ents, _ = involute_profile(m, z, PRESSURE_DEG, backlash=backlash,
                               phase_deg=math.degrees(phase),
                               addendum=addendum, dedendum=dedendum)
    sk = check(d.call("add_feature", feature={
        "type": "Sketch", "name": f"{name} outline",
        "plane": plane(centre[0], centre[1], 0.0), "entities": ents}),
        f"{name} outline")
    return check(d.call("add_feature", feature={
        "type": "Extrude", "name": name, "sketch_id": sk["id"],
        "distance": h, "mode": "new_body", "twist_deg": k * h,
        "herringbone": True}), f"{name} extrude")["id"]


def bearer(d, name, s, z, centre, internal=False):
    """The rolling bearer above a gear's teeth, as a revolved body: a band
    at the pitch circle (half the gap inside it, or outside it on the ring)
    on a 45 degree cone that rises from the top of the teeth inside the
    neighbour's tips. On the ring it is the TOOL that cuts the bore: the
    same profile, run up past the top so the cut leaves no skin."""
    m, ht, H = s["module"], s["teeth_height"], s["height"]
    rp = m * z / 2.0
    sign = -1.0 if internal else 1.0
    r_band = rp - sign * s["band_inset"]
    r_foot = rp - sign * s["cone_inset"]
    top = H + (m if internal else 0.0)
    ents = [line((0.0, ht), (r_foot, ht)),
            line((r_foot, ht), (r_band, ht + s["cone_height"])),
            line((r_band, ht + s["cone_height"]), (r_band, top)),
            line((r_band, top), (0.0, top)),
            line((0.0, top), (0.0, ht))]
    sk = check(d.call("add_feature", feature={
        "type": "Sketch", "name": f"{name} bearer profile",
        "plane": {"origin": [centre[0], centre[1], 0.0],
                  "normal": [0, -1, 0], "uAxis": [1, 0, 0]},
        "entities": ents}), f"{name} bearer profile")
    return check(d.call("add_feature", feature={
        "type": "Revolve", "name": f"{name} bearer", "sketch_id": sk["id"],
        "axis": {"kind": "custom", "origin": [centre[0], centre[1], 0.0],
                 "direction": [0.0, 0.0, 1.0]},
        "angle_deg": 360.0, "mode": "new_body"}), f"{name} bearer")["id"]


def with_bearer(d, name, s, z, centre, body):
    return check(d.call("add_feature", feature={
        "type": "Boolean", "name": name, "target_id": body,
        "tool_id": bearer(d, name, s, z, centre), "op": "union"}),
        f"{name} + bearer")["id"]


def text(label, at, height, angle=0.0):
    return {"type": "text", "text": label, "font": "sans", "height": height,
            "anchor": [float(at[0]), float(at[1])], "angle": angle,
            "halign": "center", "valign": "middle"}


def sun(d, s, ph, centre=(0.0, 0.0)):
    """The sun with its bearer and its hex knob on top. The knob stays
    inside the root circle, so it stands straight up off the bearer and
    overhangs nothing."""
    m = s["module"]
    c = s["tip_clearance"]
    body = herringbone(d, "sun teeth", s, s["zs"], centre, ph["sun"], +1,
                       s["backlash"] / 2.0, dedendum=ADDENDUM + c / m)
    body = with_bearer(d, "sun bearing", s, s["zs"], centre, body)
    rf = m * s["zs"] / 2.0 - (ADDENDUM * m + c)
    r_hex = 0.9 * rf
    pts = [(centre[0] + r_hex * math.cos(math.pi / 3 * i),
            centre[1] + r_hex * math.sin(math.pi / 3 * i)) for i in range(6)]
    sk = check(d.call("add_feature", feature={
        "type": "Sketch", "name": "knob", "plane": plane(0, 0, s["height"]),
        "entities": [line(pts[i], pts[(i + 1) % 6]) for i in range(6)]}),
        "knob sketch")
    knob = check(d.call("add_feature", feature={
        "type": "Extrude", "name": "knob", "sketch_id": sk["id"],
        "distance": s["knob_height"], "mode": "new_body"}), "knob")
    return check(d.call("add_feature", feature={
        "type": "Boolean", "name": "sun", "target_id": body,
        "tool_id": knob["id"], "op": "union"}), "sun + knob")["id"]


def planet(d, s, phase, centre=(0.0, 0.0), label=None):
    m, c = s["module"], s["tip_clearance"]
    name = f"planet {label or ''}".strip()
    body = herringbone(d, f"{name} teeth", s, s["zp"], centre, phase, -1,
                       s["backlash"] / 2.0, dedendum=ADDENDUM + c / m)
    body = with_bearer(d, name, s, s["zp"], centre, body)
    if label:
        # A number on each, so the orbit can be counted against the ratio.
        rf = m * s["zp"] / 2.0 - (ADDENDUM * m + c)
        sk = check(d.call("add_feature", feature={
            "type": "Sketch", "name": f"planet {label} number",
            "plane": plane(0, 0, s["height"]),
            "entities": [text(label, centre, 0.9 * rf)]}), "number sketch")
        body = check(d.call("add_feature", feature={
            "type": "Extrude", "name": f"planet {label} engrave",
            "sketch_id": sk["id"], "distance": s["engrave"],
            "reversed": True, "mode": "cut"}), "number")["id"]
    return body


def ring(d, s, ph, diameter, engrave=True, bearers=True, sector_deg=0.0):
    """A blank cut by an external gear of the ring's tooth count: the
    cutter's teeth are the ring's spaces, so the roles of addendum and
    dedendum swap and its backlash goes negative (fatter cutter teeth,
    wider spaces). `sector_deg`: only a pie of that half-angle about +X,
    the teeth planet 1 meets, for the push check (an exact distance to the
    whole ring takes seconds)."""
    m, c = s["module"], s["tip_clearance"]
    h = s["height"] if bearers else s["teeth_height"]
    if sector_deg > 0.0:
        blank = wedge(d, diameter / 2.0, sector_deg, h, "ring")
    else:
        sk = check(d.call("add_feature", feature={
            "type": "Sketch", "name": "ring blank", "plane": plane(0, 0, 0),
            "entities": [circle((0, 0), diameter / 2.0)]}), "blank sketch")
        blank = check(d.call("add_feature", feature={
            "type": "Extrude", "name": "ring blank", "sketch_id": sk["id"],
            "distance": h, "mode": "new_body"}), "blank")["id"]
    cutter = herringbone(d, "ring cutter", s, s["zr"], (0, 0), ph["ring"], -1,
                         -s["backlash"] / 2.0, addendum=ADDENDUM + c / m,
                         dedendum=s["ring_addendum"])
    body = check(d.call("add_feature", feature={
        "type": "Boolean", "name": "ring teeth", "target_id": blank,
        "tool_id": cutter, "op": "subtract"}), "ring cut")["id"]
    if bearers:
        body = check(d.call("add_feature", feature={
            "type": "Boolean", "name": "ring", "target_id": body,
            "tool_id": bearer(d, "ring", s, s["zr"], (0, 0), internal=True),
            "op": "subtract"}), "ring bore")["id"]
    if engrave:
        r_root = m * s["zr"] / 2.0 + ADDENDUM * m + c
        mid = (r_root + diameter / 2.0) / 2.0
        th = 0.45 * (diameter / 2.0 - r_root)
        sk = check(d.call("add_feature", feature={
            "type": "Sketch", "name": "ratio", "plane": plane(0, 0,
                                                             s["height"]),
            "entities": [text(f"{s['ratio']:.2f} : 1", (0, -mid), th)]}),
            "ratio sketch")
        body = check(d.call("add_feature", feature={
            "type": "Extrude", "name": "ratio engrave", "sketch_id": sk["id"],
            "distance": s["engrave"], "reversed": True, "mode": "cut"}),
            "ratio")["id"]
    return body


def bodies_by_name(d):
    """Body name -> the feature that made it, off the active part."""
    return {b["name"]: b["created_by"]
            for b in d.call("list_elements")["bodies"]}


def build_print(d, s, ph, diameter, proc):
    d.call("add_part", name="planetary print")
    d.call("set_print_process", **proc)
    t0 = time.perf_counter()
    ids = {"sun": sun(d, s, ph), "ring": ring(d, s, ph, diameter)}
    for i, (th, phi) in enumerate(ph["planets"]):
        centre = (s["centre"] * math.cos(th), s["centre"] * math.sin(th))
        ids[f"planet {i + 1}"] = planet(d, s, phi, centre, str(i + 1))
    ids = bodies_by_name(d)
    for fid in ids.values():
        d.call("set_material", feature_id=fid, material=material_of(d, proc))
    print(f"  print part: {len(ids)} bodies in "
          f"{time.perf_counter() - t0:.0f} s")
    return ids


def build_assembly(d, s, ph, diameter, proc):
    """The gears as parts, placed and coupled: the proof that the print
    TURNS. A kinematic carrier (frames only, nothing printed) holds the
    planets; the ring is fixed; the sun drives the carrier and each planet
    through relations whose ratios come from the tooth counts."""
    zs, zp, zr = s["zs"], s["zp"], s["zr"]
    for name, make in (("sun", lambda: sun(d, s, ph)),
                       ("planet", lambda: planet(d, s, 0.0)),
                       ("ring", lambda: ring(d, s, ph, diameter))):
        d.call("add_part", name=name)
        d.call("set_print_process", **proc)
        make()
        for fid in bodies_by_name(d).values():
            d.call("set_material", feature_id=fid,
                   material=material_of(d, proc))
    d.call("add_part", name="carrier")
    for i, (th, _) in enumerate(ph["planets"]):
        check(d.call("add_feature", feature={
            "type": "Frame", "name": f"planet {i + 1}", "kind": "explicit",
            "origin": [s["centre"] * math.cos(th), s["centre"] * math.sin(th),
                       0.0],
            "z_axis": [0, 0, 1], "x_axis": [1, 0, 0]}), "planet frame")

    d.call("add_assembly", name="planetary")
    # A ground to hang every axis on: the frames-only carrier part, placed
    # fixed. The ring is no longer the anchor, because the epicyclic
    # relation needs the ring on an axis of its own (it is the member the
    # solve holds while the sun is driven).
    occ = {"ground": d.call("add_occurrence", part="carrier", name="ground",
                            fixed=True)["id"],
           "ring": d.call("add_occurrence", part="ring")["id"],
           "sun": d.call("add_occurrence", part="sun")["id"],
           "carrier": d.call("add_occurrence", part="carrier")["id"]}
    for i in range(PLANETS):
        occ[f"planet {i + 1}"] = d.call("add_occurrence", part="planet",
                                        name=f"planet {i + 1}")["id"]

    def origin(o):
        return {"occurrence": occ[o], "origin": True}

    mates = {k: d.call("add_mate", type="revolute", name=f"{k} axis",
                       a=origin("ground"), b=origin(k),
                       flipped=False)["id"]
             for k in ("sun", "ring", "carrier")}
    for i, (_, phi) in enumerate(ph["planets"]):
        o = f"planet {i + 1}"
        mates[o] = d.call("add_mate", type="revolute", name=f"{o} axis",
                          a={"occurrence": occ["carrier"], "frame": o},
                          b=origin(o), flipped=False, rotation_deg=math.degrees(phi))["id"]
    # The train as written on paper: sun, ring and carrier by Willis, and
    # each planet's turn on the carrier, from the tooth counts
    # (docs/assemblies/relations.md#relations-over-several-mates). The
    # constants are where the parts stand, so each planet keeps its phase.
    check(d.call("add_relation", type="epicyclic", name="planetary",
                 sun=mates["sun"], ring=mates["ring"],
                 carrier=mates["carrier"], teeth_sun=zs, teeth_ring=zr,
                 planets=[{"mate": mates[f"planet {i + 1}"], "teeth": zp}
                          for i in range(PLANETS)]), "epicyclic relation")
    # What the train should do with the ring held, for the receipt.
    return occ, mates, {"carrier": zs / (zs + zr),
                        "planet": -(zs / zp) * (zr / (zs + zr))}


# The ring the push check measures planet 1 against: the teeth within this
# half-angle of it. Planet 1's tips subtend asin(ra / centre) = 33 degrees
# of the ring at 70 mm; 45 leaves a tooth and more either side.
RING_SECTOR_DEG = 45.0


def bare_parts(d, s, ph, diameter):
    """The control: the sun, a planet and the ring as teeth alone, no
    bearers (what the generator printed before them); and both rings as
    the sector planet 1 meets."""
    m, c = s["module"], s["tip_clearance"]
    d.call("add_part", name="sun, no bearer")
    herringbone(d, "sun teeth", s, s["zs"], (0, 0), ph["sun"], +1,
                s["backlash"] / 2.0, dedendum=ADDENDUM + c / m)
    d.call("add_part", name="planet, no bearer")
    herringbone(d, "planet teeth", s, s["zp"], (0, 0), 0.0, -1,
                s["backlash"] / 2.0, dedendum=ADDENDUM + c / m)
    for suffix, bearers in (("", True), (", no bearer", False)):
        d.call("add_part", name="ring sector" + suffix)
        ring(d, s, ph, diameter, engrave=False, bearers=bearers,
             sector_deg=RING_SECTOR_DEG)


def closure(d, moving, still, at, rot_deg, tol=1e-3):
    """How far `moving` travels along +X from `at` before it touches
    `still`, on the exact solids: the centre distance a mesh can close.

    Two bounds close in on the answer. A body moved by d closes no gap by
    more than d, so `lo + gap` is never past the contact (a safe step). The
    gap closes at the rate the last two clear steps measured until another
    pair of faces takes over, which only ever closes faster, so a step of
    `gap / rate` lands at the contact or past it. The steps alternate: a
    jump that lands in contact is followed by a safe step."""
    def gap(t):
        d.call("set_occurrence_transform", occurrence=moving, transform={
            "position": [at[0] + t, at[1], 0.0],
            "rotate": {"axis": [0, 0, 1], "angle_deg": rot_deg}})
        return d.call("measure", a={"occurrence": moving},
                      b={"occurrence": still})["distance"]
    lo, glo, hi, rate, n = 0.0, gap(0.0), None, None, 1
    if glo <= tol:
        raise SystemExit(f"planetary: push check: the pair touches as "
                         f"placed ({glo:.4f} mm)")
    safe = True
    while True:
        low = lo + glo  # no contact is nearer than this
        if hi is not None and hi - low < 2.0 * tol:
            answer = 0.5 * (low + hi)
            break
        if n > 40:
            raise SystemExit("planetary: push check: no contact found")
        t = low
        if not safe and rate:
            jump = lo + glo / rate
            if hi is not None:
                jump = min(jump, hi - tol)
            if jump > low + tol:
                t = jump
        g, n = gap(t), n + 1
        if g > tol:
            rate = (glo - g) / (t - lo) if glo > g else rate
            lo, glo, safe = t, g, False
        elif t == low:
            answer = low  # the contact is within `tol` of a safe step
            break
        else:
            hi, safe = t, True
    d.call("set_occurrence_transform", occurrence=moving, transform={
        "position": [at[0], at[1], 0.0],
        "rotate": {"axis": [0, 0, 1], "angle_deg": rot_deg}})
    return answer, n


def push_check(d, s, ph, diameter):
    """The worst push on the sun, toward the space between two planets,
    against the remaining engagement of the opposite planet's mesh.

    MEASURED: how far each mesh's centre distance closes before the solids
    touch (`closure`, sun toward planet 1 and planet 1 toward the ring).
    The phases centre every tooth in its space, so a translation closes both
    flanks together, as the free gears' play does. COMPOSED from those, by
    the train's geometry (no contact solver says it): the sun closes the two
    planets' meshes in series along lines pi/N off the push, so it travels
    (t_sp + t_pr) / cos(pi/N), and the opposite planet's sun mesh opens by
    that plus the planet's own drift out to the ring, t_pr.

    The tilt case, a push on the knob above the bearers: the sun pivots on
    them and its BOTTOM swings toward the opposite planet, held there by the
    teeth alone, so the two pushed-toward meshes open at the bottom by
    (t_sp + t_pr) cos(pi/N) of the TEETH's closures plus their own drift.

    Run on the bare gears too, as the negative control: that must fail."""
    t0 = time.perf_counter()
    bare_parts(d, s, ph, diameter)
    t_bare = time.perf_counter() - t0
    d.call("add_assembly", name="push check")
    a = s["centre"]
    th, phi = ph["planets"][0]
    at_planet = (a * math.cos(th), a * math.sin(th))
    rot = math.degrees(phi)
    depth = 2.0 * ADDENDUM * s["module"]
    cos_n = math.cos(math.pi / PLANETS)
    out = {}
    for label, suffix in (("bearers", ""), ("bare", ", no bearer")):
        occ = {k: d.call("add_occurrence", part=p + suffix,
                         name=f"{p}{suffix}")["id"]
               for k, p in (("sun", "sun"), ("planet", "planet"),
                            ("ring", "ring sector"))}
        d.call("set_occurrence_transform", occurrence=occ["planet"],
               transform={"position": [at_planet[0], at_planet[1], 0.0],
                          "rotate": {"axis": [0, 0, 1], "angle_deg": rot}})
        # Sun toward planet 1, then planet 1 toward the ring: +X both.
        t_sp, n_sp = closure(d, occ["sun"], occ["planet"], (0.0, 0.0), 0.0)
        t_pr, n_pr = closure(d, occ["planet"], occ["ring"], at_planet, rot)
        for k in occ.values():
            d.call("set_occurrence_visible", occurrence=k, visible=False)
        push = (t_sp + t_pr) / cos_n
        opened = push + t_pr
        out[label] = {"closure_sun_planet_mm": t_sp,
                      "closure_planet_ring_mm": t_pr,
                      "measurements": n_sp + n_pr,
                      "sun_travel_mm": push, "mesh_opened_mm": opened,
                      "engaged_mm": depth - opened,
                      "engaged_share": (depth - opened) / depth}
    # The bottom of a sun pushed at the knob: the bare closures.
    tilt = out["bare"]
    opened = ((tilt["closure_sun_planet_mm"] + tilt["closure_planet_ring_mm"])
              * cos_n + tilt["closure_planet_ring_mm"])
    out["tilt"] = {"mesh_opened_mm": opened, "engaged_mm": depth - opened,
                   "engaged_share": (depth - opened) / depth}
    out["working_depth_mm"] = depth
    out["min_engaged_share"] = MIN_ENGAGED
    out["seconds"] = time.perf_counter() - t0
    out["seconds_bare_parts"] = t_bare
    return out


def material_of(d, proc):
    """The material slug of a print process: given, or its profile's."""
    if "material" in proc:
        return proc["material"]
    for p in d.call("list_print_profiles")["profiles"]:
        if p["name"] == proc["profile"]:
            return p["material"]
    raise SystemExit(f"planetary: no print profile '{proc['profile']}'")


def run_motion(d, mates, s, name, to_deg, samples, mode, deflection=None):
    extra = {} if deflection is None else {"clash_deflection_mm": deflection}
    st = d.call("add_motion_study", driver=mates["sun"], dof="rotation",
                **{"from": 0.0}, to=to_deg, samples=samples,
                clash_mode=mode, name=name, **extra)
    if st.get("why_not"):
        raise SystemExit(f"planetary: motion study {name}: {st['why_not']}")
    sid = st.get("id", st.get("study"))
    d.call("run_motion_study", study=sid, **{"async": True})
    while True:
        r = d.call("motion_status", study=sid)
        if not r.get("running") and not r.get("job"):
            return r
        time.sleep(2.0)


def sun_study(d, s, ph, material, mesh_modules=0.4):
    """The sun's teeth under a firm pinch on its knob: the pinch's torque on
    the knob's flats (a `torque` load about the sun's axis), held at each
    planet by the one flank that drives it. The sun is the weakest gear: the
    smallest, and it carries every planet's mesh.

    Reports two stresses, and says which is which: the global peak, which
    sits in the sharp inside corner where the knob meets the gear and grows
    as the mesh is refined, and the tooth ROOTS' (probe_study on every root
    face), which is what would break a tooth."""
    m, h, ht = s["module"], s["height"], s["teeth_height"]
    alpha = math.radians(PRESSURE_DEG)
    rs = m * s["zs"] / 2.0
    rb, ra = rs * math.cos(alpha), rs + ADDENDUM * m
    r_hex = 0.9 * (rs - (ADDENDUM * m + s["tip_clearance"]))
    torque = PINCH_N * r_hex * math.cos(math.pi / 6)  # N mm, on the flats
    f_mesh = torque / rs / PLANETS                    # N tangential a mesh
    rf = rs - (ADDENDUM * m + s["tip_clearance"])
    d.call("set_active_part", part="sun")
    faces = d.call("list_elements")["bodies"][0]["faces"]
    flats, loaded, roots = [], {i: [] for i in range(PLANETS)}, []
    pitch = 2.0 * math.pi / s["zs"]
    for f in faces:
        x, y, z = f["centroid"]
        n = f["normal"]
        if f["kind"] == "plane" and z > h + 1e-6 and abs(n[2]) < 1e-6:
            flats.append(f["name"])
            continue
        r = math.hypot(x, y)
        # A root arc's face: near the root circle and facing out from the
        # axis. A twisted face's centroid lies inside the surface (by about
        # 2 % over half the sun's twist), so radius alone also takes the
        # short radial flanks below the base circle; those face sideways.
        if (f["kind"] != "plane" and z < ht and r < rf + 0.2 * m
                and (n[0] * x + n[1] * y) / max(r, 1e-9) > 0.7):
            roots.append(f["name"])
            continue
        # The bearer's band and cone stand above the teeth.
        if f["kind"] == "plane" or not rb <= r <= ra or z > ht:
            continue
        ang = math.atan2(y, x)
        # Which tooth: its angle less the helix's twist at this height.
        zh = z if z <= ht / 2.0 else ht - z
        k = round((ang - math.tan(math.radians(HELIX_DEG)) * zh / rs) / pitch)
        t = (-math.sin(ang), math.cos(ang))
        for i, (th, _) in enumerate(ph["planets"]):
            # The tooth nearest the planet, and its flank that faces the way
            # the sun turns (+): the one that pushes the planet. Two arcs
            # draw a flank, in each half of the V.
            if (abs(math.remainder(k * pitch - th, 2.0 * math.pi))
                    <= pitch / 2.0 and n[0] * t[0] + n[1] * t[1] > 0.3):
                loaded[i].append(f["name"])
    if (len(flats) != 6 or any(len(v) != 4 for v in loaded.values())
            or len(roots) != 2 * s["zs"]):
        raise SystemExit(f"planetary: the sun's study found {len(flats)} knob "
                         f"flats, {len(roots)} root faces (two a tooth "
                         f"expected) and "
                         f"flats and flanks {loaded} — six flats, and a "
                         f"planet's tooth's flank as two arcs in each half "
                         f"of the V, were expected")
    st = d.call("add_study", type="static", name="sun under a pinch")
    sid = st.get("id", st.get("study"))
    # Four quadratic elements across a tooth's root (about 1.6 modules
    # thick): the stress there is the answer.
    d.call("set_study", study=sid, mesh_size=mesh_modules * m)
    # The hand's torque, a sixth on each flat: a flat is not round, so the
    # axis is stated. The six together carry no net force.
    for name in flats:
        d.call("add_load", study=sid, type="torque", face=name,
               torque_nmm=torque / len(flats),
               axis_origin_mm=[0.0, 0.0, h], axis_dir=[0.0, 0.0, 1.0])
    for i in range(PLANETS):
        for name in loaded[i]:
            d.call("add_load", study=sid, type="fixed", face=name)
    t0 = time.perf_counter()
    d.call("run_study", study=sid, **{"async": True})
    while d.call("study_status", study=sid).get("running"):
        time.sleep(1.0)
    r = d.call("study_results", study=sid)
    status = d.call("study_status", study=sid)
    probes = [d.call("probe_study", study=sid, face=name) for name in roots]
    worst = max(probes, key=lambda p: p["von_mises_max_mpa"])
    return {"torque_nmm": torque, "force_per_mesh_n": f_mesh,
            "root_von_mises_max_mpa": worst["von_mises_max_mpa"],
            "root_von_mises_max_at_mm": worst["von_mises_max_at_mm"],
            "root_von_mises_mean_mpa": max(p["von_mises_mean_mpa"]
                                           for p in probes),
            "material": material, "mesh_size_mm": mesh_modules * m,
            "seconds": time.perf_counter() - t0,
            "solver": status.get("solver"), "results": r}


def hero(d, path):
    """The print plate from above and to one side, each kind of gear in its
    own colour so the eye can follow the train. Colours are appearance only:
    the bodies stay PLA for mass and the study."""
    d.call("set_active_part", part="planetary print")
    colours = {"sun": [0.80, 0.25, 0.02], "ring": [0.03, 0.03, 0.035]}
    for name, fid in bodies_by_name(d).items():
        d.call("set_appearance", feature_id=fid,
               color=colours.get(name, [0.70, 0.70, 0.68]), roughness=0.45)
    d.call("view", name="iso")
    d.call("set_camera", direction=[0.45, -0.75, 0.85])
    d.call("render_still", path=str(path), width=1600, height=1200)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int)
    ap.add_argument("--diameter", type=float, default=70.0,
                    help="the ring's outside diameter, mm")
    ap.add_argument("--process", default="fdm")
    ap.add_argument("--material", default="pla")
    ap.add_argument("--profile", default=None,
                    help="one of your print profiles (list_print_profiles), "
                         "measured on your printer; without it the built-in "
                         "--process/--material table, the community's "
                         "numbers")
    ap.add_argument("--fresh", action="store_true",
                    help="start a new project first (refused on unsaved work)")
    ap.add_argument("--out", default=None)
    ap.add_argument("--study", action="store_true",
                    help="also solve the sun's static study (half a minute on a GPU, two on the CPU)")
    ap.add_argument("--no-render", action="store_true",
                    help="skip the hero still (a --headless instance has "
                         "no renderer)")
    args = ap.parse_args()
    out = Path(args.out) if args.out else OUT

    d = connect(args.port)
    if args.fresh:
        fresh_project(d)
    proc = ({"profile": args.profile} if args.profile else
            {"process": args.process, "material": args.material})
    gap = d.call("print_allowance", kind="gap", size_mm=args.diameter,
                 **proc)
    s = solve(args.diameter, gap["allowance_mm"])
    ph = phases(s)
    print(f"planetary: Ø{args.diameter:g}, module {s['module']:g}, "
          f"sun {s['zs']} / planets {s['zp']} / ring {s['zr']}, "
          f"ratio {s['ratio']:.3f}:1, gap {gap['allowance_mm'] / 2:g} a side "
          f"({args.profile or args.material})")

    ids = build_print(d, s, ph, args.diameter, proc)
    geo = d.call("check_geometry")
    if geo["receipt"]["standing"] != "verified":
        raise SystemExit(f"planetary: the print part is not verified: {geo}")
    ids = bodies_by_name(d)
    # The app's printability check (check_print): on the bed, five separate
    # pieces, the gap between every pair of bodies on the exact B-rep, and
    # the STL's chords a fifth of the gap at most. The closest point of a
    # mesh comes out about 5 um short of the gap -- gear.py draws each flank
    # as two arcs through involute points -- which the check's tolerance (a
    # tenth of the gap) holds. And nothing overhangs past 45 degrees or
    # bridges: the helix, the bearers' cone and the knob are 45 or less by
    # design, and every item must pass.
    pr = check_print(d, "planetary")
    cl = {f"{p['a']} / {p['b']}": p["distance_mm"]
          for p in pr["clearance"]["pairs"]}

    out.mkdir(parents=True, exist_ok=True)
    stl = out / "planetary.stl"
    # A printed part exports at its profile's chord deviation (else a tenth
    # of its smallest gap), the deviation check_print judged. The reply
    # says what it used.
    exp = d.call("export_stl", path=str(stl))
    # The print part is saved as built; the hero still is a separate look.
    d.call("save_project", path=str(out / "planetary.dsgn"))

    occ, mates, ratios = build_assembly(d, s, ph, args.diameter, proc)
    # One mesh cycle finely (every tooth pair passes through every phase of
    # its mesh in it), then a whole orbit of the planets coarsely.
    cycle = 360.0 * (s["zs"] + s["zr"]) / (s["zs"] * s["zr"])
    # The mesh check's triangles a fifth of the gap: at its derived 0.2 mm
    # they were coarser than the 0.1 mm the teeth clear by.
    clash = gap["allowance_mm"] / 2.0 / 5.0
    motion = {"clash_deflection_mm": clash, "negative_controls": {}}
    # First, that this check can fail: planet 1 put half a tooth out, and
    # then only 0.4 mm along its pitch circle, must each clash.
    # Planet 1's own relation (kind `planet`): moving its constant by the
    # planet's coefficient times an angle turns that planet by the angle.
    r1, coef = None, None
    for r in d.call("list_relations")["relations"]:
        for t in r.get("terms", []):
            if r.get("kind") == "planet" and \
                    t["mate"] == mates["planet 1"]:
                r1, coef = r, t["coefficient"]
    if r1 is None:
        raise SystemExit("planetary: no planet relation for planet 1")
    rp_planet = s["module"] * s["zp"] / 2.0
    for label, off in (("half a tooth", math.pi / s["zp"]),
                       ("0.4 mm", 0.4 / rp_planet)):
        d.call("set_relation", id=r1["id"],
               constant=r1["constant"] + coef * off)
        r = run_motion(d, mates, s, f"control: {label}", cycle, 24, "mesh",
                       clash)["result"]
        motion["negative_controls"][label] = r["collision_status"]
        print(f"  negative control, planet 1 {label} out: "
              f"{r['collision_status']}")
        if r["clean"]:
            raise SystemExit(f"planetary: planet 1 {label} out of phase "
                             f"reads clean; the clash check cannot see "
                             f"this gap")
    d.call("set_relation", id=r1["id"], constant=r1["constant"])
    for name, to, n in (("one mesh cycle", cycle, 24),
                        ("one orbit", 360.0 * s["ratio"], 240)):
        r = run_motion(d, mates, s, name, to, n, "mesh", clash)["result"]
        motion[name] = {"to_deg": to, "samples": n,
                        "status": r["collision_status"],
                        "summary": r["receipt"]["summary"]}
        print(f"  motion, {name} ({to:.1f} deg of the sun): "
              f"{r['collision_status']}")
        if not r["clean"]:
            raise SystemExit(f"planetary: {name} clashes: "
                             f"{r.get('table', r)}")

    push = push_check(d, s, ph, args.diameter)
    for label in ("bearers", "bare"):
        p = push[label]
        print(f"  push, {label}: meshes close {p['closure_sun_planet_mm']:.3f}"
              f" / {p['closure_planet_ring_mm']:.3f} mm, the sun travels "
              f"{p['sun_travel_mm']:.2f} mm, the opposite mesh opens "
              f"{p['mesh_opened_mm']:.2f}: {p['engaged_mm']:.2f} of "
              f"{push['working_depth_mm']:.2f} mm engaged "
              f"({100 * p['engaged_share']:.0f} %)")
    print(f"  push at the knob, the bottom: {push['tilt']['engaged_mm']:.2f} "
          f"mm engaged ({100 * push['tilt']['engaged_share']:.0f} %); "
          f"{push['bearers']['measurements'] + push['bare']['measurements']}"
          f" measurements, {push['seconds']:.0f} s")
    for label in ("bearers", "tilt"):
        if push[label]["engaged_share"] < MIN_ENGAGED:
            raise SystemExit(f"planetary: the worst push ({label}) leaves "
                             f"{push[label]['engaged_mm']:.2f} mm engaged, "
                             f"under {MIN_ENGAGED:.0%} of the working depth")
    if push["bare"]["engaged_share"] >= MIN_ENGAGED:
        raise SystemExit("planetary: the push check passes the gears WITHOUT "
                         "bearers too; it cannot see the fault it is for")
    print(f"  push check: passed; without bearers it fails, as it must")

    study = None
    if args.study:
        study = sun_study(d, s, ph, material_of(d, proc))
        res = study["results"]
        vm = res["max_von_mises_mpa"]
        # Both peaks sit in a re-entrant corner, whose value grows as the
        # mesh is refined: say which one.
        z = vm["at"][2]
        where = ("the knob's inside corner" if z > s["height"] - 1e-3 else
                 "the teeth's top, at the foot of the bearer's cone"
                 if abs(z - s["teeth_height"]) < s["module"] else
                 "in the teeth")
        print(f"  sun under a {PINCH_N:g} N pinch "
              f"({study['torque_nmm'] / 1000:.2f} N m): tooth roots "
              f"{study['root_von_mises_max_mpa']:.2f} MPa at worst, "
              f"{study['root_von_mises_mean_mpa']:.2f} on average; the "
              f"global peak {vm['value']:.2f} MPa at "
              f"{[round(v, 1) for v in vm['at']]} ({where}), factor of "
              f"safety {res.get('factor_of_safety', 0):.1f} on it")

    if not args.no_render:
        hero(d, out / "planetary.png")

    receipt = {"diameter": args.diameter, "process": proc, **s,
               "ratios": ratios, "motion": motion, "push": push,
               "study": study,
               "phases": {"ring": ph["ring"],
                          "planets": [list(p) for p in ph["planets"]]},
               "clearances_mm": cl, "print": print_receipt(pr),
               "stl": str(stl), "stl_deflection_mm": exp.get("deflection_mm"),
               "stl_shells": exp.get("shells"), "confirm": gap["confirm"]}
    (out / "planetary.json").write_text(json.dumps(receipt, indent=1))
    d.call("save_project", path=str(out / "planetary.dsgn"))
    print(f"planetary: {exp.get('shells')} shells -> {stl}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
