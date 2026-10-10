#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A print-in-place herringbone planetary gear set: one plate, five bodies,
off the bed already assembled and turning.

    python3 generators/planetary/build.py --port 8801 --fresh
    python3 generators/planetary/build.py --port 8801 --diameter 90

A sun, three planets and an internal ring, all with herringbone teeth: the V
holds every gear at its height, so nothing needs a carrier, a pin or a
support. Turn the hex knob on the sun with the ring held and the planets
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
- a static study on the sun's teeth under a firm pinch on the knob.

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
from _lib import check, circle, connect, fresh_project, line  # noqa: E402
from gear import involute_profile  # noqa: E402

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
                         f"module {m} — a larger --diameter")
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
    m, h = s["module"], s["height"]
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


def text(label, at, height, angle=0.0):
    return {"type": "text", "text": label, "font": "sans", "height": height,
            "anchor": [float(at[0]), float(at[1])], "angle": angle,
            "halign": "center", "valign": "middle"}


def sun(d, s, ph, centre=(0.0, 0.0)):
    """The sun with its hex knob on top. The knob stays inside the root
    circle, so it stands straight up off the teeth and overhangs nothing."""
    m = s["module"]
    c = s["tip_clearance"]
    body = herringbone(d, "sun", s, s["zs"], centre, ph["sun"], +1,
                       s["backlash"] / 2.0, dedendum=ADDENDUM + c / m)
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
    body = herringbone(d, f"planet {label or ''}".strip(), s, s["zp"], centre,
                       phase, -1, s["backlash"] / 2.0,
                       dedendum=ADDENDUM + c / m)
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


def ring(d, s, ph, diameter, engrave=True):
    """A blank cut by an external gear of the ring's tooth count: the
    cutter's teeth are the ring's spaces, so the roles of addendum and
    dedendum swap and its backlash goes negative (fatter cutter teeth,
    wider spaces)."""
    m, c = s["module"], s["tip_clearance"]
    sk = check(d.call("add_feature", feature={
        "type": "Sketch", "name": "ring blank", "plane": plane(0, 0, 0),
        "entities": [circle((0, 0), diameter / 2.0)]}), "blank sketch")
    blank = check(d.call("add_feature", feature={
        "type": "Extrude", "name": "ring blank", "sketch_id": sk["id"],
        "distance": s["height"], "mode": "new_body"}), "blank")["id"]
    cutter = herringbone(d, "ring cutter", s, s["zr"], (0, 0), ph["ring"], -1,
                         -s["backlash"] / 2.0, addendum=ADDENDUM + c / m,
                         dedendum=s["ring_addendum"])
    body = check(d.call("add_feature", feature={
        "type": "Boolean", "name": "ring", "target_id": blank,
        "tool_id": cutter, "op": "subtract"}), "ring cut")["id"]
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
                          b=origin(o), flipped=False, rotation=phi)["id"]
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
    m, h = s["module"], s["height"]
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
        if (f["kind"] != "plane" and z < h and r < rf + 0.2 * m
                and (n[0] * x + n[1] * y) / max(r, 1e-9) > 0.7):
            roots.append(f["name"])
            continue
        if f["kind"] == "plane" or not rb <= r <= ra:
            continue
        ang = math.atan2(y, x)
        # Which tooth: its angle less the helix's twist at this height.
        zh = z if z <= h / 2.0 else h - z
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


def clearances(d, ids):
    """The smallest gap between every pair that meshes, measured on the
    exact B-rep. The pairs that never meet (planet to planet, sun to ring)
    are left to the STL's shell count."""
    out = {}
    pairs = [("sun", f"planet {i + 1}") for i in range(PLANETS)]
    pairs += [(f"planet {i + 1}", "ring") for i in range(PLANETS)]
    for a, b in pairs:
        r = d.call("measure", a={"feature_id": ids[a]},
                   b={"feature_id": ids[b]})
        out[f"{a} / {b}"] = r.get("distance", r.get("min_distance"))
    return out


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
    cl = clearances(d, ids)
    worst = min(cl.values())
    for k, v in cl.items():
        print(f"  {k}: {v:.3f} mm")
    # The closest point comes out about 5 um short of the gap, lofted or
    # extruded alike: gear.py draws each flank as two arcs through involute
    # points, not the involute itself. So the allowance is absolute, a
    # hundredth of a millimetre, far under what a 0.4 mm nozzle resolves.
    if worst < gap["allowance_mm"] / 2.0 - 0.01:
        raise SystemExit(f"planetary: the closest mesh is {worst:.3f} mm, "
                         f"under the {gap['allowance_mm'] / 2:g} mm the "
                         f"table wants a side")

    out.mkdir(parents=True, exist_ok=True)
    stl = out / "planetary.stl"
    # A printed part exports at its profile's chord deviation (else a tenth
    # of its smallest gap): at the old 0.1 mm the chords took 0.03 mm a side
    # off every gap. The reply says what it used.
    exp = d.call("export_stl", path=str(stl))
    if exp.get("deflection_mm", 1.0) > gap["allowance_mm"] / 2.0 / 5.0:
        raise SystemExit(f"planetary: the STL's chords ({exp}) can take a "
                         f"fifth of the gap")
    if exp.get("shells") != len(ids) or not exp.get("watertight", True):
        raise SystemExit(f"planetary: the STL is not {len(ids)} separate "
                         f"watertight shells: {exp}")
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

    study = None
    if args.study:
        study = sun_study(d, s, ph, material_of(d, proc))
        res = study["results"]
        vm = res["max_von_mises_mpa"]
        print(f"  sun under a {PINCH_N:g} N pinch "
              f"({study['torque_nmm'] / 1000:.2f} N m): tooth roots "
              f"{study['root_von_mises_max_mpa']:.2f} MPa at worst; the "
              f"global peak {vm['value']:.2f} MPa at "
              f"{[round(v, 1) for v in vm['at']]} (the knob's inside "
              f"corner), factor of safety {res.get('factor_of_safety', 0):.1f}"
              f" on it")

    if not args.no_render:
        hero(d, out / "planetary.png")

    receipt = {"diameter": args.diameter, "process": proc, **s,
               "ratios": ratios, "motion": motion, "study": study,
               "phases": {"ring": ph["ring"],
                          "planets": [list(p) for p in ph["planets"]]},
               "clearances_mm": cl, "stl": str(stl),
               "stl_shells": exp.get("shells"), "confirm": gap["confirm"]}
    (out / "planetary.json").write_text(json.dumps(receipt, indent=1))
    d.call("save_project", path=str(out / "planetary.dsgn"))
    print(f"planetary: {exp.get('shells')} shells -> {stl}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
