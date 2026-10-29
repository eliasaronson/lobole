#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The woodcutter automaton: a hand-cranked camshaft in a printed box, one
chop per turn. The axe cam lifts the woodcutter's arms slowly through a rod
and a link and drops them onto the log; the bird's cam, phase-shifted on the
same shaft, makes the bird peck while the axe is rising. The box is printed
panels pegged together, the shaft turns in printed bushings, and a crank on
its end turns it. Phases 2 to 6 of docs/plans/woodcutter-automaton.md.

    python3 generators/print_coupon.py --port 8800 --seed
    python3 generators/woodcutter/build.py --port 8800 --profile "K5 PLA on P1S"
    python3 generators/woodcutter/build.py --port 8800 --profile ... --width 150

Every size derives from `--width` (the box's inside width), the print
profile's gap and press (`print_allowance` kinds `gap` and `press`), and,
for the crank end, a child's design lean against the material's strength;
the cams' base radii are searched against CamProfile's own pressure-angle report
(generators/woodcutter/README.md#where-every-size-comes-from).

What it builds and checks (the receipt holds every number):
- the parts, each verified by check_geometry and checked as it prints
  (check_print in its print direction), every press marked a printed press;
- the assembly: the panels and their pegs fixed, the camshaft on a revolute
  in the left wall with its cams, bushings and crank pressed on, the rods
  on sliders in the top's guides driven by `contact` relations, each
  figure's lever closed through a link (a mate loop), every pin fastened in
  the part it is pressed into;
- the assembly sequence, every step slid home along its way in;
- a study over one crank turn and one across the drop (mesh clash), each
  clean, and `check_followers` over both at twice the hand speed;
- requirements: liftoff and pressure angle per follower, the axe's
  drop_rpm, both studies clean, the sequence, the blade's clearance over
  the log;
- the camshaft, the crank, the handle's pin and the left bushing as static
  studies on pin supports: the camshaft at work, its loads read from
  check_followers, must hold with its factor; leaned on, the handle's pin
  must hold the lean with its factor and every part behind it the pin's
  breaking load with its own, none buckling; and every study's pins must
  answer its loads' force and moment (woodcutter_fem.py);
- negative controls: the axe's landing dwell shortened under its fall
  fails drop_rpm, the bird's cam on too small a base fails the pressure
  angle, and a guide tilted off the cam's plane or moved off its width is
  refused as a contact;
- the print plates on the profile's bed, each checked (every item), and the
  print job; with --slice, sliced by your Bambu Studio for its time and
  filament;
- the bill of materials and the drawings (woodcutter_drawings.py): the
  assembly with an exploded view and its balloons, a sheet per cam, each
  check_drawing clean;
- with --film (woodcutter_film.py), three films: the crank turned three
  times at the hand's speed with the camera on the figures, the same in
  section down the axe cam, and the assembly sequence going together;
  the film's drop set beside check_followers' at the hand's speed.

Output (generated, untracked): scratch/woodcutter/ -- woodcutter.lob,
woodcutter.json (the receipt), woodcutter_job.3mf, woodcutter_bom.csv, the
sheets (PDF and SVG) and the renders.
"""
import argparse
import json
import math
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import woodcutter_drawings as D  # noqa: E402
import woodcutter_parts as P  # noqa: E402
import woodcutter_fem as F  # noqa: E402
import woodcutter_film as M  # noqa: E402
import woodcutter_spec as S  # noqa: E402
from _lib import (PRINT_ITEMS, bed_zones, check, check_print,  # noqa: E402
                  connect, export_print_job, fresh_project, job_receipt,
                  pack, print_item_line, print_receipt, q_axis, q_up, qmul,
                  turned_box)

ROOT = Path(HERE).resolve().parent.parent
OUT = Path(os.environ["LOBOLE_SCRATCH"]) / "woodcutter" if \
    os.environ.get("LOBOLE_SCRATCH") else ROOT / "scratch" / "woodcutter"
ASM = "woodcutter automaton"
SPEED = S.MARGIN * S.HAND_RPM     # 120 rpm: twice the hand's turn
# What each part looks like in a render (linear RGB; looks only, PLA all):
# the filament colours a painted automaton might be printed in.
BOX = ("floor", "back", "left wall", "right wall", "top")
LOOKS = {**{k: (0.45, 0.25, 0.10) for k in BOX},
         "peg": (0.35, 0.18, 0.07), "camshaft": (0.6, 0.6, 0.6),
         "bushing": (0.3, 0.3, 0.3), "crank": (0.5, 0.05, 0.03),
         "grip": (0.75, 0.55, 0.05), "handle pin": (0.3, 0.3, 0.3),
         "rod pin": (0.3, 0.3, 0.3), "lever pin": (0.3, 0.3, 0.3),
         "pivot pin": (0.3, 0.3, 0.3),
         "axe cam": (0.75, 0.55, 0.05), "bird cam": (0.75, 0.55, 0.05),
         "axe rod": (0.6, 0.6, 0.6), "bird rod": (0.6, 0.6, 0.6),
         "axe link": (0.3, 0.3, 0.3), "bird link": (0.3, 0.3, 0.3),
         "arms": (0.5, 0.05, 0.03), "woodcutter": (0.05, 0.15, 0.45),
         "bird": (0.8, 0.6, 0.02), "perch": (0.12, 0.25, 0.06),
         "log": (0.25, 0.12, 0.04)}


def fail(msg):
    raise SystemExit(f"woodcutter: {msg}")


def slow(d, method, **kw):
    """A call that runs a study or measures over one: past the client's
    60 s default on a loaded machine (the requirements after the controls
    run both studies and five follower checks again)."""
    return d.request(method, kw, timeout=900.0)


# ---- parts -----------------------------------------------------------------

# Bambu Studio's "0.20mm Standard" on the P1S, for every part; what is
# loaded across its section -- the shaft, the pins, the pegs -- with four
# walls, which at their sizes leaves them nearly solid.
PLA_PRINT = {"walls": 2, "top_layers": 5, "bottom_layers": 3,
             "infill_pct": 15, "infill_pattern": "grid"}
SOLID_WALLS = ("camshaft", "peg", "rod pin", "lever pin", "pivot pin",
               "handle pin", "axe rod", "bird rod")


def new_part(d, name, proc, up=None):
    """A part, printed by the profile, with its slicer settings and the way
    up it prints (woodcutter_parts.PRINT_UP): its printed mass stacks
    layers along it and its printed holes point their roofs up it."""
    d.call("add_part", name=name)
    d.call("set_print_process", **proc)
    if up is not None:
        settings = dict(PLA_PRINT, direction=list(up))
        if name in SOLID_WALLS:
            settings["walls"] = 4
        d.call("set_print_settings", **settings)


def material_of(d, proc):
    if "material" in proc:
        return proc["material"]
    for p in d.call("list_print_profiles")["profiles"]:
        if p["name"] == proc["profile"]:
            return p["material"]
    fail(f"no print profile '{proc['profile']}'")


def finish_part(d, name, mat):
    """One body, verified, given its material."""
    bodies = d.call("list_elements")["bodies"]
    if len(bodies) != 1:
        fail(f"{name} is {len(bodies)} bodies, not one: "
             f"{[b['name'] for b in bodies]}")
    geo = d.call("check_geometry")
    if geo["receipt"]["standing"] != "verified":
        fail(f"{name} is not verified: {geo['receipt']}")
    d.call("set_material", feature_id=bodies[0]["created_by"], material=mat)
    if name in LOOKS:
        d.call("set_appearance", feature_id=bodies[0]["created_by"],
               color=list(LOOKS[name]), roughness=0.6)
    return bodies[0]["created_by"]


def press_sizes(s, name):
    """The round faces of a part that are pressed (the pin's, the peg's,
    the shaft's flat ends, and the holes they go in): each diameter."""
    pin, peg = (s["d_pin"], s["d_pin_press"]), (s["d_peg"], s["d_peg_bore"])
    shaft = (s["d_shaft"], s["d_shaft"] + s["press"])
    handle = (s["d_handle"], s["d_handle"] + s["press"])
    by = {"rod pin": pin[:1], "lever pin": pin[:1], "pivot pin": pin[:1],
          "axe rod": pin[1:], "bird rod": pin[1:], "arms": pin[1:],
          "bird": pin[1:], "woodcutter": pin[1:], "perch": pin[1:],
          "peg": peg[:1], "floor": peg[1:], "back": peg[1:],
          "left wall": peg[1:], "right wall": peg[1:], "top": peg[1:],
          "camshaft": shaft[:1], "axe cam": shaft[1:], "bird cam": shaft[1:],
          "bushing": shaft[1:], "crank": shaft[1:] + handle[1:],
          "handle pin": handle[:1]}
    return by.get(name, ())


def mark_press(d, name, dias):
    """Each round face of those diameters a PRINTED PRESS
    (set_tolerance printed: press, docs/tolerances/README.md#printed-press-fits):
    its band read off the profile's press row, so the motion study's exact
    rigid check relieves it and lists it among its press fits."""
    n = 0
    for b in d.call("list_elements")["bodies"]:
        for f in b.get("faces", []):
            if f.get("kind") != "cylinder" or not any(
                    abs(2.0 * f.get("radius", 0.0) - x) < 1e-6 for x in dias):
                continue
            check(d.call("set_tolerance", feature_id=b["created_by"],
                         element=f["name"], printed="press"),
                  f"{name}: printed press")
            n += 1
    if dias and n == 0:
        fail(f"{name} has no pressed face of {dias}")
    return n


def build_part(d, name, proc, mat, fn, *a):
    new_part(d, name, proc, P.PRINT_UP.get(name))
    out = fn(P.Builder(d), *a)
    body = finish_part(d, name, mat)
    mark_press(d, name, press_sizes(a[0], name) if a else ())
    return body, out


def settle_base(d, s, which, floor, proc):
    """The least base radius (to 0.05 mm, not under `floor`) whose cam
    CamProfile reports within the pressure-angle limit, searched in a
    scratch part that is deleted after. Returns (base, report)."""
    new_part(d, f"{which} base search", proc)
    b = P.Builder(d)
    fid = b.frame("cam", (0.0, 0.0, 0.0), P.X, P.Z)
    spec = P.cam_spec(s, which, floor, fid)
    feat = b.add(spec, f"{which} cam at {floor:g}")
    tried = []

    def at(base):
        spec["base_radius_mm"] = base
        r = check(d.call("modify_feature", feature_id=feat["id"],
                         feature=spec), f"{which} cam at {base:g}")
        rep = d.call("get_state")["features"]
        rep = next(f for f in rep if f.get("id") == feat["id"])["report"]
        tried.append((base, rep["max_pressure_angle_deg"]))
        return rep
    base = max(floor, math.floor(s[f"{which}_base_pressure"] * 20) / 20.0)
    rep = at(base)
    while rep["max_pressure_angle_deg"] > S.PRESSURE_LIMIT_DEG:
        base = round(base + 0.05, 2)
        rep = at(base)
    while base - 0.05 >= floor:
        lower = at(round(base - 0.05, 2))
        if lower["max_pressure_angle_deg"] > S.PRESSURE_LIMIT_DEG:
            break
        base, rep = round(base - 0.05, 2), lower
    d.call("delete_part", part=f"{which} base search")
    return base, rep, tried


# Every part: (name, builder, its arguments after the sizes).
PARTS = [("floor", P.build_floor), ("back", P.build_back),
         ("left wall", P.build_wall, "left"),
         ("right wall", P.build_wall, "right"), ("top", P.build_top),
         ("peg", P.build_peg), ("camshaft", P.build_shaft),
         ("bushing", P.build_bushing), ("crank", P.build_crank),
         ("grip", P.build_grip), ("handle pin", P.build_handle_pin),
         ("axe cam", P.build_cam, "axe"), ("bird cam", P.build_cam, "bird"),
         ("axe rod", P.build_rod, "axe"), ("bird rod", P.build_rod, "bird"),
         ("axe link", P.build_link, "axe"),
         ("bird link", P.build_link, "bird"),
         ("rod pin", P.build_pin, "rod"), ("lever pin", P.build_pin, "lever"),
         ("pivot pin", P.build_pin, "pivot"), ("arms", P.build_arms),
         ("woodcutter", P.build_woodcutter), ("bird", P.build_bird),
         ("perch", P.build_perch), ("log", P.build_log)]


# ---- the assembly --------------------------------------------------------------

def occ(d, part, name, fixed=False):
    return d.call("add_occurrence", part=part, name=name, fixed=fixed)["id"]


def mate(d, kind, a, fa, b, fb, name):
    r = d.call("add_mate", type=kind, name=name,
               a={"occurrence": a, "frame": fa},
               b={"occurrence": b, "frame": fb}, flipped=False)
    return r["id"]


def build_assembly(d, s):
    """The panels and their pegs fixed where they stand; the mechanism on
    its mates, each pin fastened in the part it is pressed into."""
    d.call("add_assembly", name=ASM)
    o = {}
    for n in BOX:
        o[n] = occ(d, n, n, fixed=True)
    o["pegs"] = []
    for i, (joint, c, axis) in enumerate(P.peg_joints(s)):
        tr = {"position": list(c), "orientation": [0.0, 0.0, 0.0, 1.0]}
        if axis == P.X:   # the peg's Z onto X: a quarter turn about Y
            h = math.sqrt(0.5)
            tr["orientation"] = [0.0, h, 0.0, h]
        o["pegs"].append(d.call("add_occurrence", part="peg",
                                name=f"peg {i + 1} ({joint})", fixed=True,
                                transform=tr)["id"])
    for n in ("camshaft", "axe cam", "bird cam", "axe rod", "bird rod",
              "axe link", "bird link", "arms", "woodcutter", "bird",
              "perch", "log", "crank", "grip", "handle pin"):
        o[n] = occ(d, n, n)
    for n in ("bushing L", "bushing R"):
        o[n] = occ(d, "bushing", n)
    for k in ("axe", "bird"):
        for pin in ("rod", "lever", "pivot"):
            o[f"{k} {pin} pin"] = occ(d, f"{pin} pin", f"{k} {pin} pin")
    m = {}
    m["crank"] = mate(d, "revolute", o["left wall"], "shaft", o["camshaft"],
                      "shaft", "camshaft")
    for n in ("bushing L", "bushing R"):
        mate(d, "fastened", o["camshaft"], n, o[n], "bushing",
             f"{n} pressed")
    mate(d, "fastened", o["camshaft"], "crank", o["crank"], "crank",
         "crank pressed")
    mate(d, "fastened", o["crank"], "handle", o["handle pin"], "handle",
         "handle pin pressed")
    m["handle"] = mate(d, "revolute", o["crank"], "handle", o["grip"],
                       "handle", "handle")
    for k in ("axe", "bird"):
        mate(d, "fastened", o["camshaft"], f"cam {k}", o[f"{k} cam"],
             f"cam {k}", f"{k} cam key")
        m[f"{k} slide"] = mate(d, "slider", o["top"], f"guide {k}",
                               o[f"{k} rod"], f"guide {k}", f"{k} guide")
        lever = "arms" if k == "axe" else "bird"
        body = "woodcutter" if k == "axe" else "perch"
        mate(d, "revolute", o[f"{k} rod"], f"rod pin {k}", o[f"{k} link"],
             f"rod pin {k}", f"{k} rod pin")
        mate(d, "revolute", o[f"{k} link"], f"lever pin {k}", o[lever],
             f"lever pin {k}", f"{k} lever pin")
        m[f"{k} pivot"] = mate(d, "revolute", o[body], f"pivot {k}",
                               o[lever], f"pivot {k}",
                               "shoulder" if k == "axe" else "bird's feet")
        mate(d, "fastened", o["top"], f"stand {k}", o[body], f"stand {k}",
             f"{body} stands")
        mate(d, "fastened", o[f"{k} rod"], f"rod pin {k}",
             o[f"{k} rod pin"], "rod pin axe", f"{k} rod pin pressed")
        mate(d, "fastened", o[lever], f"lever pin {k}",
             o[f"{k} lever pin"], "lever pin axe", f"{k} lever pin pressed")
        mate(d, "fastened", o[body], f"pivot {k}", o[f"{k} pivot pin"],
             "pivot axe", f"{k} pivot pin pressed")
    mate(d, "fastened", o["top"], "log", o["log"], "log", "log rests")
    sol = d.call("solve_assembly")
    if sol["errored_mates"] or not sol.get("loops_closed", False):
        fail(f"the assembly does not solve: {sol}")
    rel = {}
    for k in ("axe", "bird"):
        r = d.call("add_relation", type="contact", a=m["crank"],
                   b=m[f"{k} slide"], cam=o[f"{k} cam"],
                   follower=o[f"{k} rod"], name=f"{k} follower")
        if r.get("error") or r.get("toward") != "negative":
            fail(f"the {k} rod's contact: {r}")
        rel[k] = r["id"]
    sol = d.call("solve_assembly")
    if sol["errored_mates"] or not sol.get("loops_closed", False):
        fail(f"the assembly with its followers does not solve: {sol}")
    return o, m, rel, sol


def asm_id(d):
    return next(p["id"] for p in d.call("list_parts")["parts"]
                if p["name"] == ASM)


def add_study(d, m, name, lo, hi, samples, clash):
    st = d.call("add_motion_study", driver=m["crank"], dof="rotation",
                **{"from": lo}, to=hi, samples=samples, clash_mode="mesh",
                clash_deflection_mm=clash, name=name)
    if st.get("why_not"):
        fail(f"motion study {name}: {st['why_not']}")
    return st.get("id", st.get("study"))


def run_study(d, sid, o, s):
    """Run it, tracing the blade's low corner and the bird's beak."""
    arm = s["arm"]
    low = s["blade_rest"]
    x_arm = sum(s["axe_slabs"]["lever"]) / 2.0
    x_bird = sum(s["bird_slabs"]["lever"]) / 2.0
    tip = P.beak_tip(s)
    beak = (x_bird, tip[0], tip[1])
    r = slow(d, "run_motion_study", study=sid, include_poses=True, trace=[
        {"name": "blade", "occurrence": o["arms"],
         "point_mm": [x_arm, low[0], low[1]]},
        {"name": "shoulder", "occurrence": o["arms"],
         "point_mm": [x_arm, arm["P"][0], arm["P"][1]]},
        {"name": "beak", "occurrence": o["bird"], "point_mm": list(beak)}])
    return r.get("result", r)


def clash_summary(res):
    bad = [{"value": smp["value"],
            "pairs": [(c["a_name"], c["b_name"]) for c in smp["clashes"]]}
           for smp in res["samples"] if smp.get("clashes")]
    unsolved = [smp["value"] for smp in res["samples"]
                if not smp.get("solved", True)]
    touching = sorted({tuple(sorted((c["a_name"], c["b_name"])))
                       for smp in res["samples"]
                       for c in smp.get("touching", [])})
    return {"samples": len(res["samples"]), "clean": res.get("clean"),
            "status": res.get("collision_status"), "clashes": bad,
            "unsolved": unsolved, "touching": touching,
            "branch_jumps": res.get("branch_jumps", 0),
            "press_fits": [(p["a_name"], p["b_name"], p.get("fit"))
                           for p in res.get("press_fits", [])],
            "deflection_mm": res.get("deflection_mm"),
            "pairs_checked": res.get("pairs_checked")}


def followers(d, sid, rel):
    out = slow(d, "check_followers", study=sid, speed_rpm=SPEED,
                 max_pressure_angle_deg=S.PRESSURE_LIMIT_DEG)
    by = {r["relation"] if "relation" in r else r.get("id"): r
          for r in out["relations"]}
    return {k: by[v] for k, v in rel.items()}, out


def follower_numbers(r):
    g = r["gravity"]
    return {"touching": r["touching"],
            "clash_mm": r.get("clash_mm"), "gap_mm": r.get("gap_mm"),
            "max_pressure_deg": r["pressure_angle"]["max_deg"],
            "pressure_at_cam_deg": r["pressure_angle"].get("at_cam_deg"),
            "pressure_ok": r["pressure_angle"].get("ok"),
            "deviation_mm": r.get("deviation_mm"),
            "fit_deviation_mm": r.get("fit_deviation_mm"),
            "mate_zero_mm": r.get("mate_zero_mm"),
            "liftoff_rpm": g.get("liftoff_rpm"), "held": g.get("held"),
            "not_held_deg": g.get("not_held_deg"),
            "drops": g.get("drops", []),
            "samples_in_drops": r.get("samples_in_drops"),
            "holds_at_speed": r.get("holds_at_speed"),
            "curvature": r.get("curvature"),
            "failures": r.get("failures", [])[:4]}


def requirements(d, s, o, rel, turn, chop):
    doc = asm_id(d)
    ids = {}

    def req(name, measure, bound, source):
        ids[name] = d.call("add_requirement", name=name,
                           measure={"document": doc, **measure},
                           bound=bound, source=source)["id"]
    for k in ("axe", "bird"):
        req(f"{k} stays on at twice a hand's turn",
            {"kind": "follower", "relation": rel[k], "study": turn,
             "quantity": "liftoff_rpm"},
            {"kind": "at_least", "value": SPEED},
            "a hand turns the crank at about 60 rpm; the follower must "
            "hold at twice that")
        req(f"{k} cam's pressure angle",
            {"kind": "follower", "relation": rel[k], "study": turn,
             "quantity": "pressure_angle_deg"},
            {"kind": "at_most", "value": S.PRESSURE_LIMIT_DEG},
            "a translating follower jams in its guide past about 30 degrees")
    req("the axe lands within its dwell at twice a hand's turn",
        {"kind": "follower", "relation": rel["axe"], "study": chop,
         "quantity": "drop_rpm"},
        {"kind": "at_least", "value": SPEED},
        "the drop must land before the cam lifts the rod again")
    req("a crank turn is clean", {"kind": "motion_clean", "study": turn},
        {"kind": "pass"}, "nothing clashes over one chop")
    req("the chop is clean", {"kind": "motion_clean", "study": chop},
        {"kind": "pass"}, "nothing clashes across the drop")
    req("it goes together in its order",
        {"kind": "assembly_sequence"}, {"kind": "pass"},
        "every part slides home along its way in, nothing forced past "
        "another")
    req("the blade stops over the log",
        {"kind": "clearance", "a": o["arms"], "b": o["log"],
         "mode": "exact", "motion_study": turn},
        {"kind": "at_least", "value": round(0.9 * s["blade_gap"], 4)},
        "the blade meets the log without striking it")
    return ids


def check_reqs(d, ids):
    r = slow(d, "check_requirements", ids=list(ids.values()))
    rows = {x["id"]: x for x in r["requirements"]}
    return {n: {"state": rows[i]["state"], "value": rows[i].get("value"),
                "unit": rows[i].get("unit")} for n, i in ids.items()}


# ---- printing -----------------------------------------------------------------------

def counts(s):
    """How many of each part the automaton takes."""
    n = {name: 1 for name, *_ in PARTS}
    n["peg"] = len(P.peg_joints(s))
    for k in ("bushing", "rod pin", "lever pin", "pivot pin"):
        n[k] = 2
    return n


PART_ITEMS = tuple(k for k in PRINT_ITEMS if k not in ("bed", "bed_fit"))


def check_parts(d, s):
    """Every part on its own, the way it prints (PRINT_UP): every
    check_print item but the two the plates answer. A part is modelled
    where it stands in the box, not on a bed, so `bed` (on the bed at its
    lowest point) and `bed_fit` (where it lies on the bed, with keep-out
    zones) are each plate's; every other item judges from the part's own
    lowest point."""
    out = {}
    for name, *_ in PARTS:
        up = P.PRINT_UP[name]
        r = check_print(d, name, need=PART_ITEMS, quiet=True, part=name,
                        direction=list(up))
        mp = d.call("mass_properties", part=name)["total"]
        out[name] = {"up": up, "orientation": P.ORIENT[name],
                     "overhang_deg": r["overhang"].get("worst_deg"),
                     "slender": (r["slender"].get("pieces") or
                                 [{}])[0].get("ratio"),
                     "mass_g": mp.get("mass_g"),
                     "printed_g": (mp.get("printed") or {}).get("mass_g"),
                     "print": print_receipt(r)}
    return out


def bbox(d, part):
    r = d.call("mass_properties", part=part)["bbox_mm"]
    return r["min"], r["max"]


def make_plate(d, name, proc, rows, ids):
    """A part of Derived copies, each piece turned the way it prints and
    lying on the bed where the packer put it."""
    d.call("add_part", name=name)
    d.call("set_print_process", **proc)
    for p in rows:
        lo, hi = turned_box(*bbox(d, p["part"]), p["q_bed"])
        pos = [p["at"][0] - lo[0], p["at"][1] - lo[1], -lo[2]]
        check(d.call("add_feature", feature={
            "type": "Derived", "name": p["label"],
            "source_part": ids[p["part"]], "reference": False,
            "placement": {"position": pos, "orientation": p["q_bed"]}}),
            f"{name}: {p['label']}")


def print_plates(d, s, proc, prof, out):
    """Every piece the automaton takes on the profile's bed, packed into as
    few plates as hold them, each plate checked as it prints (every item,
    the bed's keep-out zones and stacking included)."""
    bed = (float(prof["bed_mm"][0]), float(prof["bed_mm"][1]))
    zones = bed_zones(prof)
    ids = {p["name"]: p["id"] for p in d.call("list_parts")["parts"]
           if p["type"] == "part"}
    pcs = []
    for name, n in counts(s).items():
        q = q_up(P.PRINT_UP[name])
        lo, hi = turned_box(*bbox(d, name), q)
        if hi[2] - lo[2] > prof["bed_mm"][2]:
            fail(f"{name} is {hi[2] - lo[2]:.0f} mm tall as it prints, over "
                 f"the {prof['bed_mm'][2]:g} mm the printer reaches")
        for k in range(n):
            pcs.append({"part": name, "label": name if n == 1 else
                        f"{name} {k + 1}", "q": q, "lo": lo, "hi": hi})
    items = [(i, p["hi"][0] - p["lo"][0], p["hi"][1] - p["lo"][1])
             for i, p in enumerate(pcs)]
    try:
        beds = pack(items, bed, s["spacing"], zones)
    except ValueError as e:
        # A panel over the bed would be cut by split_for_printing; at any
        # width this generator builds, none is (README).
        fail(f"a piece does not fit the bed: {e}")
    plates = {}
    for k, placed in enumerate(beds):
        rows = []
        for i, turned, x, y in placed:
            p = dict(pcs[i])
            p["q_bed"] = (qmul(q_axis((0, 0, 1), 90.0), p["q"]) if turned
                          else p["q"])
            p["at"] = (x, y)
            rows.append(p)
        name = f"plate {k + 1}"
        make_plate(d, name, proc, rows, ids)
        geo = d.call("check_geometry")
        if geo["receipt"]["standing"] != "verified":
            fail(f"{name} is not verified: {geo['receipt'].get('summary')}")
        pr = check_print(d, f"  {name}", need=PRINT_ITEMS, quiet=True)
        if pr["shells"]["pieces"] != len(rows):
            fail(f"{name}: {pr['shells']['pieces']} pieces for the "
                 f"{len(rows)} placed")
        mp = d.call("mass_properties")["total"]
        plates[name] = {
            "pieces": [{"piece": p["label"], "part": p["part"],
                        "orientation": P.ORIENT[p["part"]],
                        "at_mm": [round(v, 1) for v in p["at"]]}
                       for p in rows],
            "mass_g": mp.get("mass_g"),
            "printed_g": (mp.get("printed") or {}).get("mass_g"),
            "print": print_receipt(pr)}
        print(f"  {name}: {len(rows)} pieces, every check_print item passes "
              f"({print_item_line('stacking', pr['stacking'])}; "
              f"{print_item_line('bed_fit', pr['bed_fit'])})")
    job = export_print_job(d, [(n, n) for n in plates],
                           out / "woodcutter_job.3mf", material_of(d, proc),
                           name="woodcutter automaton")
    return plates, job


def slicer_setup(out):
    """With --slice and no LOBOLE_SLICER_CONFIGS: the P1S's PLA project
    config made from your Bambu Studio's presets by tools/bambu-config.py
    (the job's slicer_config), so the job slices on the P1S's bed. Returns
    why not, or None."""
    if os.environ.get("LOBOLE_SLICER_CONFIGS"):
        return None
    cfg = out / "slicer"
    cfg.mkdir(exist_ok=True)
    if not (cfg / "pla.json").exists():
        r = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "bambu-config.py"),
             "--printer", "Bambu Lab P1S 0.4 nozzle",
             "--process", "0.20mm Standard @BBL X1C",
             "--filament", "Bambu PLA Basic @BBL P1S 0.4 nozzle",
             "-o", str(cfg / "pla.json")], capture_output=True, text=True,
            timeout=600)
        if r.returncode != 0:
            return f"bambu-config: {r.stderr.strip()[-300:]}"
    os.environ["LOBOLE_SLICER_CONFIGS"] = str(cfg)
    return None


def slice_job(job, out):
    """Slice the job with your Bambu Studio's CLI (as tools/bambu-config.py
    finds it) and read the time and filament it reports: the slicer's
    numbers, not an estimate of the app's. None, saying why, without it."""
    sys.path.insert(0, str(ROOT / "tools"))
    import importlib
    bc = importlib.import_module("bambu-config")
    bambu = bc.find_bambu()
    if bambu is None or not job or job.get("skipped"):
        return {"skipped": "no Bambu Studio CLI" if bambu is None else
                "no job written"}
    sl = out / "sliced"
    sl.mkdir(exist_ok=True)
    r = subprocess.run([str(bambu), "--slice", "0", "--outputdir", str(sl),
                        job["path"]], capture_output=True, text=True,
                       timeout=1800)
    res = sl / "result.json"
    if not res.exists():
        return {"skipped": f"Bambu made no result ({r.returncode}): "
                           f"{r.stderr[-400:]}"}
    result = json.loads(res.read_text())
    for pl in result.get("sliced_plates", []):
        pl.pop("objects", None)   # every object's box: the job says them
    plates = []
    for g in sorted(sl.glob("plate_*.gcode")):
        head = {}
        with open(g, errors="replace") as f:
            for line in f:
                if not line.startswith(";"):
                    if head:
                        break
                    continue
                for key in ("model printing time", "total estimated time",
                            "total filament weight [g]",
                            "total filament length [mm]"):
                    if key in line:
                        head[key] = line.split(":", 1)[-1].strip() \
                            if ":" in line else line.split("=", 1)[-1].strip()
        plates.append({"gcode": g.name, **head})
    return {"return_code": result.get("return_code"),
            "sliced_plates": result.get("sliced_plates"), "plates": plates}


def sequence(d, s, o):
    """How it goes together, each step slid home along its way in
    (docs/assemblies/README.md#assembly-sequences): the panels and their
    pegs, the shaft through the left wall with its cams already pressed on,
    the right wall, the bushings and the crank; the rods dropped onto their
    cams before the top goes on over them; the figures in their slots, the
    levers, their pins, the links and theirs."""
    X, MX, MZ = [1.0, 0.0, 0.0], [-1.0, 0.0, 0.0], [0.0, 0.0, -1.0]
    joints = [j for j, _, _ in P.peg_joints(s)]

    def pegs(*names):
        return [o["pegs"][i] for i, j in enumerate(joints) if j in names]

    def step(occ, direction=None, note=None, distance=None):
        st = ({"occurrences": occ} if isinstance(occ, list) else
              {"occurrence": occ})
        if direction is not None:
            st["direction"] = direction
        if distance is not None:
            st["distance_mm"] = distance
        if note:
            st["note"] = note
        return st
    steps = [
        step(o["floor"], note="the floor, on the bench"),
        step(pegs("floor/back"), MZ, "the back's pegs into the floor"),
        step(o["back"], MZ, "the back onto its pegs"),
        step(pegs("floor/left wall", "back/left wall"), X,
             "the left wall's pegs into the floor's and the back's edges"),
        step(o["left wall"], X, "the left wall onto its pegs"),
        step([o["camshaft"], o["axe cam"], o["bird cam"]], MX,
             "the camshaft through the left wall's bore, its cams pressed "
             "on first, each from its nearer end"),
        step(pegs("floor/right wall", "back/right wall"), MX,
             "the right wall's pegs"),
        step(o["right wall"], MX, "the right wall onto its pegs and over the "
             "shaft's end"),
        step(o["bushing L"], X, "a bushing pressed on each end of the "
             "shaft, into its wall's bore"),
        step(o["bushing R"], MX),
        step(o["crank"], X, "the crank pressed on the shaft's end"),
        step(o["grip"], X, "the grip on, and its pin through it into the "
             "crank"),
        step(o["handle pin"], X),
        step(o["axe rod"], MZ, "each rod dropped onto its cam"),
        step(o["bird rod"], MZ),
        step(pegs("left wall/top", "right wall/top", "back/top"), MZ,
             "the top's pegs into the walls' and the back's top edges"),
        step(o["top"], MZ, "the top down over the rods and onto its pegs"),
        step(o["woodcutter"], MZ, "the figures and the log into their "
             "slots"),
        step(o["perch"], MZ), step(o["log"], MZ)]
    # A pin starts its own length out, by hand at the hole: from further it
    # would pass through the other figure, which no hand reaches past.
    sl = s["axe_slabs"]
    pin_len = {"rod": sl["link"][1] - sl["link"][0] + s["g"] + s["d_rod"],
               "lever": sl["link"][1] - sl["lever"][0],
               "pivot": sl["lever"][1] - sl["body"][0]}
    for k, lever in (("axe", "arms"), ("bird", "bird")):
        steps += [
            step(o[lever], MZ, f"the {lever} beside its "
                 f"{'woodcutter' if k == 'axe' else 'perch'}"),
            step(o[f"{k} pivot pin"], X, "its pivot pin pressed in from "
                 "outside", pin_len["pivot"] + s["gap"]),
            step(o[f"{k} link"], MZ, "the link down between the lever and "
                 "the rod"),
            step(o[f"{k} rod pin"], MX, "the rod's pin through the rod into "
                 "the link", pin_len["rod"] + s["gap"]),
            step(o[f"{k} lever pin"], MX, "the lever's pin through the link "
                 "into the lever", pin_len["lever"] + s["gap"])]
    d.call("set_assembly_sequence", steps=steps, allowance_mm=0.0)
    return steps


# ---- negative controls -------------------------------------------------------------

def control_short_dwell(d, s, cams, rel, chop, req_id, o):
    """The axe's landing dwell shortened under the fall's turn at 120 rpm:
    drop_rpm must fall under 120 and the requirement fail."""
    d.call("set_active_part", part="axe cam")
    rise_to, segs = S.axe_segments(S.SHORT_DWELL_DEG)
    spec = P.cam_spec(s, "axe", s["axe_base"], cams["axe"]["frame"],
                      S.with_rise(segs, s["h_axe"]), cams["axe"]["material"])
    check(d.call("modify_feature", feature_id=cams["axe"]["id"],
                 feature=spec), "short dwell")
    d.call("set_active_part", part=ASM)
    # The chop study straddles the drop, which moved to rise_to.
    d.call("set_motion_study", study=chop, **{"from": rise_to - 10.0},
           to=rise_to + 10.0)
    slow(d, "run_motion_study", study=chop)
    fr, _ = followers(d, chop, {"axe": rel["axe"]})
    drop = fr["axe"]["gravity"]["drops"][0]
    req = slow(d, "check_requirements", ids=[req_id])["requirements"][0]
    out = {"dwell_deg": S.SHORT_DWELL_DEG, "drop": drop,
           "requirement": req["state"], "value": req.get("value")}
    if drop["lands"] or drop["drop_rpm"] >= SPEED or req["state"] != "fail":
        fail(f"the short dwell passes: {out}")
    # Back to the design, and the chop study back over its drop.
    spec = P.cam_spec(s, "axe", s["axe_base"], cams["axe"]["frame"],
                      material=cams["axe"]["material"])
    d.call("set_active_part", part="axe cam")
    check(d.call("modify_feature", feature_id=cams["axe"]["id"],
                 feature=spec), "axe cam restored")
    d.call("set_active_part", part=ASM)
    d.call("set_motion_study", study=chop,
           **{"from": S.AXE_RISE_TO_DEG - 10.0},
           to=S.AXE_RISE_TO_DEG + 10.0)
    return out


def control_small_base(d, s, cams, rel, turn, req_id):
    """The bird's cam on the hub's floor radius, under what the pressure
    angle needs: check_followers' angle must be over the limit and the
    requirement fail. (A rounded knife on these motions does not undercut
    at any base: README.)"""
    base = s["hub_floor"]
    if base >= s["bird_base"]:
        fail("the bird's base is the hub's floor: no smaller base to try")
    d.call("set_active_part", part="bird cam")
    spec = P.cam_spec(s, "bird", base, cams["bird"]["frame"],
                      material=cams["bird"]["material"])
    r = check(d.call("modify_feature", feature_id=cams["bird"]["id"],
                     feature=spec), "small base")
    d.call("set_active_part", part=ASM)
    slow(d, "run_motion_study", study=turn)
    fr, _ = followers(d, turn, {"bird": rel["bird"]})
    pa = fr["bird"]["pressure_angle"]
    req = slow(d, "check_requirements", ids=[req_id])["requirements"][0]
    out = {"base_mm": base, "max_pressure_deg": pa["max_deg"],
           "ok": pa.get("ok"), "requirement": req["state"]}
    if pa.get("ok") is not False or req["state"] != "fail":
        fail(f"the small base passes: {out}")
    d.call("set_active_part", part="bird cam")
    check(d.call("modify_feature", feature_id=cams["bird"]["id"],
                 feature=P.cam_spec(s, "bird", s["bird_base"],
                                    cams["bird"]["frame"],
                                    material=cams["bird"]["material"])),
          "bird cam restored")
    d.call("set_active_part", part=ASM)
    return out


def control_guides(d, s, proc):
    """A guide tilted out of the plane square to the camshaft, and one
    moved along it off the cam's width: each must be refused as a contact.
    Its own assembly, so the mechanism's studies are not touched."""
    new_part(d, "control guides", proc)
    b = P.Builder(d)
    x, st = s["x_axe"], s["stage"]
    tilt = math.radians(10.0)
    b.frame("tilted", (x, 0.0, st), [math.sin(tilt), 0.0, math.cos(tilt)],
            P.Y)
    b.frame("off the width", (x + s["cam_w"] + s["d_rod"], 0.0, st), P.Z,
            P.Y)
    d.call("add_assembly", name="control: guides off the cam")
    box = occ(d, "left wall", "left wall", fixed=True)
    guides = occ(d, "control guides", "guides", fixed=True)
    shaft = occ(d, "camshaft", "camshaft")
    cam = occ(d, "axe cam", "axe cam")
    crank = mate(d, "revolute", box, "shaft", shaft, "shaft", "camshaft")
    mate(d, "fastened", shaft, "cam axe", cam, "cam axe", "key")
    out = {}
    for frame in ("tilted", "off the width"):
        rod = occ(d, "axe rod", f"rod {frame}")
        slide = mate(d, "slider", guides, frame, rod, "guide axe",
                     f"guide {frame}")
        d.call("solve_assembly")
        try:
            r = d.call("add_relation", type="contact", a=crank, b=slide,
                       cam=cam, follower=rod)
        except RuntimeError as e:
            out[frame] = {"refused": str(e)}
            continue
        if r.get("error"):
            out[frame] = {"refused": r["error"]}
            continue
        fail(f"a guide {frame} is accepted as a contact: {r}")
    return out


# ---- renders --------------------------------------------------------------------------

def renders(d, s, out, turn, offscreen):
    """Quick stills at three crank angles: at rest (axe on the log), the
    bird pecking, the axe at the top just before the drop."""
    if not offscreen:
        return {"skipped": "headless: no renderer"}
    W, st = s["width"], s["stage"]
    target = [W / 2.0, 0.0, st * 0.75]
    shots = {}
    for name, deg in (("rest", 0.0),
                      ("peck", S.BIRD_PECK_FROM_DEG + S.BIRD_PECK_DEG),
                      ("raised", S.AXE_RISE_TO_DEG - 7.5)):
        slow(d, "run_motion_study", study=turn, leave_at_deg=deg)
        d.call("set_camera", target=target,
               eye=[W * 1.35, W * 1.55, st * 1.6], up=[0, 0, 1])
        p = out / f"woodcutter_{name}.png"
        r = d.call("render_still", path=str(p), mode="quick", width=1600,
                   height=1000, fit=True)
        if r.get("ok") is False:
            return {"skipped": r}
        shots[name] = {"crank_deg": deg, "path": str(p)}
    # From the side, square to the shaft: both levers' swings read.
    slow(d, "run_motion_study", study=turn,
           leave_at_deg=S.AXE_RISE_TO_DEG - 7.5)
    d.call("set_camera", target=target, eye=[-W * 2.0, 0.0, st * 0.9],
           up=[0, 0, 1])
    p = out / "woodcutter_side_raised.png"
    d.call("render_still", path=str(p), mode="quick", width=1600,
           height=1000, fit=True)
    shots["side_raised"] = {"crank_deg": S.AXE_RISE_TO_DEG - 7.5,
                            "path": str(p)}
    slow(d, "run_motion_study", study=turn, leave_at="start")
    return shots


# ---- main ------------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int)
    ap.add_argument("--width", type=float, default=120.0,
                    help="the box's inside width along the camshaft, mm")
    ap.add_argument("--profile", required=True,
                    help="one of your print profiles (list_print_profiles), "
                         "stating its layer, line width and bed: "
                         "print_coupon.py --seed installs the P1S's")
    ap.add_argument("--samples", type=int, default=49,
                    help="samples over one crank turn (49: every 7.5 deg)")
    ap.add_argument("--no-render", action="store_true")
    ap.add_argument("--film", action="store_true",
                    help="film the chop, the chop in section and the "
                         "assembly sequence (woodcutter_film.py; needs a "
                         "renderer, adds about 30 s)")
    ap.add_argument("--slice", action="store_true",
                    help="slice the print job with your Bambu Studio CLI "
                         "for its time and filament (a profile with a bed)")
    ap.add_argument("--fresh", action="store_true",
                    help="start a new project first (an untitled scratch "
                         "one is discarded; a saved one with unsaved work "
                         "refused)")
    ap.add_argument("--converge", choices=("auto", "always", "never"),
                    help="how the static studies settle their peaks "
                         "(default: each study's own, auto); never while "
                         "iterating sizes")
    ap.add_argument("--fem-timeout", type=float, default=F.TIMEOUT_S,
                    help="seconds each static study may run")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()
    F.CONVERGE, F.TIMEOUT_S = args.converge, args.fem_timeout
    if args.film and args.no_render:
        fail("--film films with the renderer, which --no-render leaves out")
    out = Path(args.out) if args.out else OUT
    out.mkdir(parents=True, exist_ok=True)
    t0 = time.perf_counter()
    stages = {}
    last = [t0]

    def stage(name):
        now = time.perf_counter()
        stages[name] = round(now - last[0], 1)
        last[0] = now

    d = connect(args.port)
    if args.film and d.call("ping").get("mode") == "headless":
        fail("--film needs a renderer: a headless instance has none "
             "(tools/headless-preview.sh --start PORT)")
    if args.fresh:
        fresh_project(d)
    proc = {"profile": args.profile}
    mat = material_of(d, proc)
    prof = next((p for p in d.call("list_print_profiles")["profiles"]
                 if p["name"] == args.profile), None)
    # Printed, so the parts are weighed as they print (the followers'
    # liftoff depends on it) and their teardrops pointed: a profile that
    # says no layer or line width weighs nothing, and the built-in table
    # is no printer.
    if prof is None:
        fail(f"no print profile '{args.profile}'")
    missing = [k for k in ("layer_height_mm", "line_width_mm")
               if not prof.get(k)]
    if missing:
        fail(f"'{args.profile}' states no {' or '.join(missing)}: the "
             f"printed parts cannot be weighed (save_print_profile, or "
             f"print_coupon.py --seed for the P1S's)")
    # The sliding fits, at the rod's size: the gap two printed surfaces
    # need to come apart (docs/tolerances/README.md).
    # The material's strength, as the studies read it: the crank end is
    # sized from it for the lean (woodcutter_spec.lean_sizes).
    strength = d.call("describe")["materials"].get(mat)
    if not strength or not all(strength.get(k) for k in
                               ("yield_mpa", "interlayer_mpa")):
        fail(f"the app states no yield and interlayer strength for "
             f"'{mat}': the crank end cannot be sized for the lean")
    strength = {k: strength[k] for k in ("yield_mpa", "interlayer_mpa")}
    s0 = S.solve(args.width, 0.0, 0.0, strength)
    gap = d.call("print_allowance", kind="gap", size_mm=s0["d_rod"], **proc)
    # The presses (pins, pegs, the parts keyed to the shaft) at the pin's
    # size, the one the coupon's press row is printed at.
    press = d.call("print_allowance", kind="press", size_mm=s0["d_pin"],
                   **proc)
    s = S.solve(args.width, gap["allowance_mm"], press["allowance_mm"],
                strength)

    # The base radii, against CamProfile's report.
    search = {}
    for k in ("axe", "bird"):
        base, rep, tried = settle_base(d, s, k, s["hub_floor"], proc)
        search[k] = {"base_mm": base, "closed_form_base_mm":
                     s[f"{k}_base_pressure"], "hub_floor_mm": s["hub_floor"],
                     "report_pressure_deg": rep["max_pressure_angle_deg"],
                     "tried": tried}
    S.layout(s, search["axe"]["base_mm"], search["bird"]["base_mm"])
    print(f"woodcutter: box {s['width']:g} x {s['depth']:g} x {s['H']:.1f} "
          f"inside, walls {s['wall']:.2f}; shaft Ø{s['d_shaft']:g} at "
          f"z {s['zc']:.2f}; gap {gap['allowance_mm']:g} "
          f"({args.profile}); axe cam base "
          f"{s['axe_base']:g} (hub floor {s['hub_floor']:g}), bird cam "
          f"base {s['bird_base']:g} (closed form "
          f"{s['bird_base_pressure']:g}); arm swing "
          f"{s['arm']['down_deg']:g}..{s['arm']['up_deg']:.1f} deg, bird "
          f"{s['bird']['down_deg']:g}..{s['bird']['up_deg']:.1f}")

    # The parts.
    cams = {}
    for name, fn, *a in PARTS:
        if fn is P.build_cam:
            k = a[0]
            body, r = build_part(d, name, proc, mat, fn, s, k,
                                 s[f"{k}_base"])
            frame = next(f["id"] for f in d.call("get_state")["features"]
                         if f.get("name") == f"cam {k}")
            cams[k] = {"id": r["id"], "frame": frame, "report": r["report"],
                       "material": mat}
        else:
            build_part(d, name, proc, mat, fn, s, *a)
    for k in ("axe", "bird"):
        rep = cams[k]["report"]
        print(f"  {k} cam: base {rep['base_radius_mm']:g}, max radius "
              f"{rep['max_radius_mm']:.2f}, pressure "
              f"{rep['max_pressure_angle_deg']:.2f} deg at "
              f"{rep['max_pressure_at_deg']:g}, fit "
              f"{rep['fit_deviation_mm']:.2e} mm, min convex "
              f"{rep['min_convex_radius_mm']:.2f}")

    parts_print = check_parts(d, s)
    over = {n: r["overhang_deg"] for n, r in parts_print.items()}
    print(f"  every part checked as it prints ({len(parts_print)} parts, "
          f"{sum(counts(s).values())} pieces): every item passes; the worst "
          f"overhang {max(v or 0.0 for v in over.values()):.2f} deg")
    stage("base search and parts")
    # The assembly and its followers.
    o, m, rel, sol = build_assembly(d, s)
    sequence(d, s, o)
    seq = slow(d, "check_assembly_sequence")
    seq_rows = [{"step": i, "occurrence": st.get("occurrence",
                                                  st.get("occurrences")),
                 "pass": st.get("pass"), "press_mm": st.get("press_mm"),
                 "interfering_mm": st.get("interfering_mm"),
                 "worst_against": st.get("worst_against")}
                for i, st in enumerate(seq["steps"])]
    if not seq["pass"]:
        bad = [r for r in seq_rows if r["pass"] is False]
        fail(f"the assembly sequence fails: {bad[:4]} missing "
             f"{seq.get('missing')}")
    print(f"  assembly sequence: {len(seq['steps'])} steps, every one goes "
          f"home ({seq['seconds']:.1f} s)")
    stage("assembly, sequence and contact tables")
    clash = s["g"] / 5.0
    turn = add_study(d, m, "one crank turn", 0.0, 360.0, args.samples, clash)
    chop = add_study(d, m, "the chop", S.AXE_RISE_TO_DEG - 10.0,
                     S.AXE_RISE_TO_DEG + 10.0, 41, clash)
    t_study = time.perf_counter()
    res_turn = run_study(d, turn, o, s)
    res_chop = run_study(d, chop, o, s)
    t_study = time.perf_counter() - t_study
    stage("studies")
    motion = {"turn": clash_summary(res_turn), "chop": clash_summary(res_chop)}
    for n, c in motion.items():
        print(f"  study {n}: {c['samples']} samples, mesh at {clash:g}: "
              f"{'clean' if c['clean'] else 'NOT clean'}"
              f"{'' if not c['clashes'] else ' ' + str(c['clashes'][:3])}"
              f"; touching {c['touching']}")
    traces = {t["name"]: {k: t[k] for k in ("min_mm", "max_mm",
                                            "extent_mm")}
              for t in res_turn["trace"]}
    fr_turn, fr_turn_all = followers(d, turn, rel)
    fr_chop, _ = followers(d, chop, rel)
    nums = {"turn": {k: follower_numbers(v) for k, v in fr_turn.items()},
            "chop": {k: follower_numbers(v) for k, v in fr_chop.items()}}
    # Each follower stands on its cam at every sample of both studies, as
    # its cam was cut for it: the realised motion is the designed one to
    # within the contact tolerance, and gravity holds it at any speed up
    # to its liftoff.
    for st, fr in (("turn", fr_turn), ("chop", fr_chop)):
        for k, r in fr.items():
            n = nums[st][k]
            dev = n["deviation_mm"]
            tol = r.get("tolerance_mm", 1e-3)
            if not n["touching"] or not n["held"]:
                fail(f"the {k} rod over the {st}: touching {n['touching']}, "
                     f"held {n['held']}: {n['failures']}")
            if dev is not None and dev > n["fit_deviation_mm"] + tol:
                fail(f"the {k} rod over the {st} deviates {dev} mm from its "
                     f"cam's motion")
    for k in ("axe", "bird"):
        n = nums["turn"][k]
        print(f"  {k} follower over the turn: touching {n['touching']}, "
              f"pressure {n['max_pressure_deg']:.2f} deg at "
              f"{n['pressure_at_cam_deg']}, deviation "
              f"{n['deviation_mm']}, liftoff {n['liftoff_rpm']} rpm, held "
              f"{n['held']}")
    for dr in nums["chop"]["axe"]["drops"]:
        print(f"  axe drop at {dr.get('at_cam_deg')}: {dr.get('height_mm')} "
              f"mm, falls in {dr.get('fall_s', 0) * 1000:.1f} ms, dwell "
              f"{dr.get('dwell_deg')} deg, drop_rpm {dr.get('drop_rpm')}, "
              f"turns {dr.get('turn_deg')} deg at {SPEED:g} rpm, lands "
              f"{dr.get('lands')}")

    stage("check_followers")
    # Requirements.
    ids = requirements(d, s, o, rel, turn, chop)
    reqs = check_reqs(d, ids)
    for n, r in reqs.items():
        print(f"  requirement {n}: {r['state']} ({r['value']} {r['unit']})")
    bad = [n for n, r in reqs.items() if r["state"] != "pass"]
    if bad:
        fail(f"requirements fail: {bad}")
    d.call("save_project", path=str(out / "woodcutter.lob"))

    stage("requirements")
    # Negative controls.
    controls = {}
    controls["the axe's dwell shortened"] = control_short_dwell(
        d, s, cams, rel, chop,
        ids["the axe lands within its dwell at twice a hand's turn"], o)
    c = controls["the axe's dwell shortened"]
    print(f"  negative control, the axe's dwell at {c['dwell_deg']:g} deg: "
          f"drop_rpm {c['drop']['drop_rpm']:.1f}, lands "
          f"{c['drop']['lands']}, requirement {c['requirement']}")
    controls["the bird's cam on the hub's radius"] = control_small_base(
        d, s, cams, rel, turn, ids["bird cam's pressure angle"])
    c = controls["the bird's cam on the hub's radius"]
    print(f"  negative control, the bird's cam on base {c['base_mm']:g}: "
          f"pressure {c['max_pressure_deg']:.2f} deg, requirement "
          f"{c['requirement']}")
    controls["guides off the cam"] = control_guides(d, s, proc)
    for k, v in controls["guides off the cam"].items():
        print(f"  negative control, a guide {k}: refused: {v['refused']}")

    # The design again: every requirement passes as it lies.
    d.call("set_active_part", part=ASM)
    final = check_reqs(d, ids)
    bad = [n for n, r in final.items() if r["state"] != "pass"]
    if bad:
        fail(f"after the controls, requirements fail: {bad}")

    stage("negative controls")
    # The parts under load, on pins, every load from check_followers.
    fem = F.studies(d, s, fr_turn_all, rel)
    if not fem["camshaft"]["pass"]:
        fail(f"FEM: the camshaft does not hold its working load with "
             f"{S.WORK_NEED:g} to spare: {fem['camshaft']['factor']}")
    if fem["unbalanced"]:
        fail(f"FEM: the pins of {fem['unbalanced']} do not answer their "
             f"loads' statics within {100 * F.STATICS_TOL:g} %")
    if fem["lean_breaks"] or fem["buckles"]:
        fail(f"FEM: leaned on, {fem['lean_breaks']} do not hold and "
             f"{fem['buckles']} buckle (the pin must hold "
             f"{S.LEAN_N:g} N with {S.LEAN_NEED:g} to spare, every part "
             f"behind it the pin's breaking load with {S.OUTLAST:g})")
    pin = fem["handle_pin"]
    print(f"  FEM: the camshaft holds its working load, and every study's "
          f"pins answer its loads' force and moment within "
          f"{100 * F.STATICS_TOL:g} %; a child's lean of "
          f"{fem['lean']['push_n']:g} N: the handle's pin holds it with "
          f"{pin['factor']:.2f} and breaks at {fem['pin_breaks_n']:.0f} N, "
          f"which " + ", ".join(
              f"the {k.replace('_', ' ')} holds with "
              f"{fem[k]['at_pin_break']:.2f}"
              for k in F.LEANED if k != "handle_pin"))
    d.call("set_active_part", part=ASM)
    stage("FEM")
    offscreen = not d.call("ping").get("headless", False)
    shots = {} if args.no_render else renders(d, s, out, turn, offscreen)
    d.call("save_project", path=str(out / "woodcutter.lob"))
    stage("renders and save")
    plates, job, sliced = {}, None, None
    if prof.get("bed_mm"):
        why = slicer_setup(out) if args.slice else None
        if why:
            print(f"  slicing skipped: {why}")
        plates, job = print_plates(d, s, proc, prof, out)
        job = job_receipt(job)
        if shots and not shots.get("skipped"):
            for name in plates:
                d.call("set_active_part", part=name)
                b = d.call("mass_properties")["bbox_mm"]
                c = [(b["min"][i] + b["max"][i]) / 2.0 for i in range(3)]
                d.call("set_camera", target=c,
                       eye=[c[0], c[1] - 0.6 * prof["bed_mm"][1],
                            c[2] + 1.2 * prof["bed_mm"][1]], up=[0, 0, 1])
                p = out / f"woodcutter_{name.replace(' ', '_')}.png"
                d.call("render_still", path=str(p), mode="quick",
                       width=1600, height=1200, fit=True)
                shots[name] = {"path": str(p)}
        if args.slice:
            sliced = slice_job(job, out)
            for pl in sliced.get("plates", []):
                print(f"  sliced by Bambu Studio, {pl['gcode']}: "
                      f"{pl.get('total estimated time', '?')}, "
                      f"{pl.get('total filament weight [g]', '?')} g, "
                      f"{pl.get('total filament length [mm]', '?')} mm of "
                      f"filament")
            if sliced.get("skipped"):
                print(f"  not sliced: {sliced['skipped']}")
    else:
        job = {"skipped": "no print profile with a bed: the built-in table "
                          "is no printer to lay the plates on"}
    stage("plates")
    # The drawings and the bill (woodcutter_drawings.py), once the plates
    # are laid out: the bill names the plate each part prints on.
    dw = D.drawings(d, s, out, {k: v["report"] for k, v in cams.items()},
                    f"Printed {mat.upper()} on {args.profile}. Nothing "
                    f"bought.", bool(plates))
    b = dw["bom"]
    print(f"  bill of materials: {b['rows']} parts, {b['pieces']} pieces, "
          f"all printed in {'/'.join(b['materials'])}, nothing bought: "
          f"{b['printed_g']:.1f} g printed ({b['solid_g']:.1f} g solid) -> "
          f"{Path(b['path']).name}")
    a = dw["assembly"]
    print(f"  drawing assembly: front, side and exploded, {a['balloons']} "
          f"balloons laid out, none crossing; check_drawing clean")
    for k in ("axe", "bird"):
        c = dw[f"{k} cam"]
        print(f"  drawing {k} cam: its motion and lift table; the drawn "
              f"outline within {c['outline_vs_table_mm']:.3f} mm of the "
              f"table; check_drawing clean")
    d.call("set_active_part", part=ASM)
    d.call("save_project", path=str(out / "woodcutter.lob"))
    stage("drawings")
    film = None
    if args.film:
        # The film's study is saved with the project: the Motion window
        # plays it at the hand's speed.
        film = M.films(d, s, out)
        M.report(film)
        d.call("set_active_part", part=ASM)
        d.call("save_project", path=str(out / "woodcutter.lob"))
        stage("films")
    elapsed = time.perf_counter() - t0
    receipt = {"width": args.width, "process": proc,
               "gap": {"allowance_mm": gap["allowance_mm"],
                       "source": gap.get("source")},
               "sizes": {k: v for k, v in s.items()},
               "base_search": search,
               "cams": {k: v["report"] for k, v in cams.items()},
               "closed_forms": {
                   "bird_rod_only_liftoff_rpm": S.liftoff_rod_rpm(
                       s["h_bird"], math.radians(S.BIRD_PECK_DEG)),
                   "axe_pressure_deg": S.max_pressure_deg(
                       s["h_axe"], math.radians(S.AXE_RISE_TO_DEG),
                       s["axe_base"] + s["tip"]),
                   "bird_pressure_deg": S.max_pressure_deg(
                       s["h_bird"], math.radians(S.BIRD_PECK_DEG),
                       s["bird_base"] + s["tip"]),
                   "axe_free_fall_s": math.sqrt(2 * s["h_axe"] / 1000.0 /
                                                S.G)},
               "solve": {k: sol.get(k) for k in ("dof", "loops_closed",
                                                  "residual_mm",
                                                  "loop_mates")},
               "motion": motion, "traces": traces, "followers": nums,
               "speed_rpm": SPEED, "requirements": final,
               "negative_controls": controls, "fem": fem, "renders": shots,
               "parts": parts_print, "counts": counts(s),
               "sequence": {"pass": seq["pass"], "steps": seq_rows,
                            "seconds": seq.get("seconds")},
               "plates": plates, "job": job, "sliced": sliced,
               "drawings": dw, "film": film,
               "study_seconds": t_study, "stage_seconds": stages,
               "seconds": elapsed}
    (out / "woodcutter.json").write_text(json.dumps(receipt, indent=1,
                                                    default=str))
    print(f"  seconds by stage: {stages}")
    print(f"woodcutter: {len(counts(s))} parts, "
          f"{sum(counts(s).values())} pieces, {len(plates)} plates")
    print(f"woodcutter: every check passed -> "
          f"{out / 'woodcutter.lob'} ({elapsed:.0f} s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
