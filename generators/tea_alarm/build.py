#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The tea-ready alarm: it clips on a mug's rim and rings when the tea has
cooled enough to drink. A straight KANTHAL 94S strip in the tea straightens
as it cools and lifts a probe; the probe tips a balanced ramp past its
detent at the set temperature, a steel ball rolls off it onto a chime and
into a dish. An M3 screw under its knob sets the temperature.

    python3 generators/print_coupon.py --port 8801 --seed
    python3 generators/tea_alarm/build.py --port 8801 --profile "K5 PLA on P1S" \\
        --strip-mm 5 0.3 --rod-mm 3 --vessel-mm 80 95 3 --ready-c 55 \\
        --room-c 22 [--no-render] [--fresh] [--out DIR]

The strip, the rod (frame, probe, pivot and chime are cut from it), the
vessel and the room are required: what was ordered and what it is for,
read off the order and the mug. Every size derives from them
(generators/tea_alarm/README.md#where-every-size-comes-from).

What it builds and checks (the receipt holds every number):
- the strip check: a steel/brass strip bonded in an assembly, heated, its
  tip against Timoshenko's, in-house and on CalculiX, and its control;
- the parts and the assembly, the ramp's statics from the app's masses,
  the ball, the probe's force and what the strip leaves (the K per mN);
- the trigger: tea_c swept from the pour down, at --ready-c and across the
  knob's range;
- the heat path: the frame rod as a fin at both ends of Incropera's
  free-convection range, on CalculiX too, and the printed block's margin,
  which picks its material;
- the clip: its grip's deflection and stress, and the clip on the vessel
  (contact);
- the chime's first mode against Euler-Bernoulli;
- the side cases (ta_cases.py): a snap-through latch and a plastic bracket;
- check_print for each printed part and the plate, the print job, the
  bill, and the drawings: the clip's flat pattern with its bend table, the
  rod's bend table and the knob's dial.

Output (generated, untracked): scratch/tea_alarm/.
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
import ta_cases as C  # noqa: E402
import ta_fem as F  # noqa: E402
import ta_mechanism as M  # noqa: E402
import ta_spec as S  # noqa: E402
from _lib import (PRINT_ITEMS, bed_zones, check_print, connect,  # noqa: E402
                  export_print_job, fresh_project, job_receipt, pack,
                  print_receipt, q_axis, q_up, qmul, turned_box)

ROOT = Path(HERE).resolve().parent.parent
OUT = Path(os.environ["LOBOLE_SCRATCH"]) / "tea_alarm" if \
    os.environ.get("LOBOLE_SCRATCH") else ROOT / "scratch" / "tea_alarm"
AUTHOR = "Loböle tea_alarm/build.py"
PART_ITEMS = tuple(k for k in PRINT_ITEMS if k not in ("bed", "bed_fit"))


def fail(msg):
    raise SystemExit(f"tea alarm: {msg}")


def library_matches(describe):
    """The app's KANTHAL 94S and 1.4301 rows say what ta_spec cites."""
    mats = describe["materials"]
    bi = mats.get("bimetal_kanthal_94s") or fail(
        "this build has no bimetal_kanthal_94s row")
    want = {"specific_deflection_per_k": S.K94S_A,
            "specific_curvature_per_k": S.K94S_K,
            "specific_curvature_tolerance": S.K94S_K_TOL}
    got = bi.get("bimetal", {})
    for k, v in want.items():
        if got.get(k) != v:
            fail(f"the library's 94S {k} is {got.get(k)}, the handbook's {v}")
    if bi.get("young_gpa") * 1000.0 != S.K94S_E or "poisson" in bi:
        fail(f"the library's 94S stiffness row moved: {bi}")
    ss = mats["stainless_steel"]
    for k, v in (("young_gpa", S.SS_E / 1000.0), ("poisson", S.SS_NU),
                 ("yield_mpa", S.SS_FY), ("conductivity_w_mk", S.SS_K),
                 ("density_kg_m3", S.SS_RHO)):
        if abs(ss.get(k, 0.0) - v) > 1e-9 * v:
            fail(f"the library's 1.4301 {k} is {ss.get(k)}, SCI P413's {v}")


def bimetal_refused(d):
    """A structural study of a body of the bimetal is refused by name: its
    row has no Poisson's ratio."""
    d.call("set_active_part", part="strip")
    sid = d.call("add_study", name="the strip on its own",
                 type="static")["id"]
    face = next(f["name"] for f in F.faces_where(d, "strip", lambda f: True)
                if f["kind"] == "plane" and f["normal"][0] < -0.99)
    d.call("set_active_part", part="strip")
    d.call("add_load", study=sid, type="fixed", face=face)
    try:
        d.call("run_study", id=sid)
    except Exception as e:  # noqa: BLE001 -- the refusal is the answer
        msg = str(e)
    else:
        fail("a structural study of the bimetal strip ran")
    finally:
        d.call("remove_study", id=sid)
    if "Poisson" not in msg:
        fail(f"the bimetal's refusal does not name its Poisson's ratio: {msg}")
    return msg


# ---- printing ---------------------------------------------------------------------

def check_parts(d):
    out = {}
    for name in M.PRINTED:
        r = check_print(d, name, need=PART_ITEMS, quiet=True, part=name,
                        direction=M.PRINT_UP[name])
        mp = d.call("mass_properties", part=name)["total"]
        out[name] = {"up": M.PRINT_UP[name], "mass_g": mp.get("mass_g"),
                     "printed_g": (mp.get("printed") or {}).get("mass_g"),
                     "print": print_receipt(r)}
    return out


def bbox(d, part):
    r = d.call("mass_properties", part=part)["bbox_mm"]
    return r["min"], r["max"]


def plate(d, proc, prof, out):
    """The four printed parts on one plate, each the way it prints."""
    bed = (float(prof["bed_mm"][0]), float(prof["bed_mm"][1]))
    gap = 4.0 * float(prof.get("line_width_mm") or 0.45)
    ids = {p["name"]: p["id"] for p in d.call("list_parts")["parts"]
           if p["type"] == "part"}
    pcs = []
    for name in M.PRINTED:
        q = q_up(M.PRINT_UP[name])
        lo, hi = turned_box(*bbox(d, name), q)
        pcs.append({"part": name, "q": q, "lo": lo, "hi": hi})
    beds = pack([(i, p["hi"][0] - p["lo"][0], p["hi"][1] - p["lo"][1])
                 for i, p in enumerate(pcs)], bed, gap, bed_zones(prof))
    if len(beds) != 1:
        fail(f"the printed parts need {len(beds)} plates, not one")
    d.call("add_part", name="plate")
    d.call("set_print_process", **proc)
    for i, turned, x, y in beds[0]:
        p = pcs[i]
        qb = qmul(q_axis((0, 0, 1), 90.0), p["q"]) if turned else p["q"]
        lo, _ = turned_box(*bbox(d, p["part"]), qb)
        d.call("add_feature", feature={
            "type": "Derived", "name": p["part"],
            "source_part": ids[p["part"]], "reference": False,
            "placement": {"position": [x - lo[0], y - lo[1], -lo[2]],
                          "orientation": qb}})
    d.call("set_active_part", part="plate")
    pr = check_print(d, "plate", need=PRINT_ITEMS, quiet=True)
    if pr["shells"]["pieces"] != len(pcs):
        fail(f"the plate has {pr['shells']['pieces']} pieces, not "
             f"{len(pcs)}")
    job = export_print_job(d, [("plate", "plate")], out / "tea_alarm.3mf",
                           M.proc_material(d, proc), name="tea alarm")
    return {"print": print_receipt(pr), "job": job_receipt(job)}


# ---- the bill and the drawings ---------------------------------------------------------

def bill(d, out):
    import csv
    d.call("set_active_part", part=M.ASM)
    p = out / "tea_alarm_bom.csv"
    d.call("export_bom", path=str(p))
    with open(p, newline="") as f:
        rows = [r for r in csv.DictReader(f) if r["part"] != "total"]
    printed = sorted(r["part"] for r in rows if r["printed_unit_mass_g"])
    bought = sorted(r["part"] for r in rows if not r["printed_unit_mass_g"])
    if printed != sorted(M.PRINTED):
        fail(f"the bill's printed parts are {printed}, not {M.PRINTED}")
    return {"path": str(p), "printed": printed, "bought": bought,
            "rows": [{k: r[k] for k in ("part", "quantity", "material",
                                        "standard")} for r in rows]}


def export(d, out, name):
    paths = {}
    for ext in ("pdf", "svg"):
        path = out / f"{name}.{ext}"
        d.call("export_drawing", path=str(path))
        paths[ext] = str(path)
    chk = d.call("check_drawing")
    if not chk["ok"]:
        fail(f"{name}: check_drawing is not clean: {chk['issues'][:6]}")
    return {"paths": paths, "issues": chk["issues"]}


def clip_sheet(d, s, flat_id, out):
    """The clip as bought sheet is bent by hand: its flat pattern with its
    bend lines, and the bend table (each bend's direction, angle, radius,
    K-factor and allowance), beside the clip as bent."""
    d.call("set_active_part", part="clip")
    have = [c for c in d.call("list_configurations")["configurations"]
            if c["name"] == "Flat"]
    cfg = have[0]["id"] if have else \
        d.call("add_configuration", name="Flat")["id"]
    d.call("set_configuration", id=cfg, unsuppressed_features=[flat_id])
    # The blank to cut: the flat pattern's extent.
    d.call("set_active_configuration", id=cfg)
    size = sorted(d.call("mass_properties", part="clip")["bbox_mm"]["size"])
    d.call("set_active_configuration", id=0)
    d.call("create_drawing", name="clip sheet", sheet={
        "size": "a4", "landscape": True,
        "title_block": {"project": "Tea-ready alarm", "document": "Clip",
                        "author": AUTHOR, "material": "1.4301, 0.5 MM",
                        "notes": "BEND BY HAND FROM BOUGHT SHEET"}})
    d.call("add_drawing_view", source="clip", orientation="front",
           name="as bent")
    flat = d.call("add_drawing_view", source="clip", orientation="top",
                  configuration="Flat", name="flat pattern")["id"]
    d.call("update_drawing")
    views = {v["id"]: v for v in d.call("list_drawing_views")["views"]}
    lines = views[flat].get("bend_lines")
    if not lines:
        fail("the clip's flat pattern draws no bend lines")
    d.call("add_drawing_note", table="bends", view=flat,
           position=[75.0, 40.0], text="CLIP - BEND TABLE")
    d.call("add_drawing_note", position=[75.0, 60.0],
           text=f"BLANK {size[2]:.1f} x {size[1]:.1f} mm, "
                f"{size[0]:.1f} mm SHEET")
    rec = export(d, out, "tea_alarm_clip")
    rec["blank_mm"] = size
    rec["bend_lines"] = lines if isinstance(lines, int) else len(lines)
    return rec


def rod_sheet(d, s, out):
    """The frame rod, bent by hand from bought rod: a bend table of its
    straights, its bends (angle, former) and the order to bend them in,
    read from the app's tube schedule."""
    sch = d.request("tube_schedule", {"part": "frame rod"})["tubes"][0]
    d.call("export_bend_schedule", part="frame rod",
           path=str(out / "tea_alarm_rod_bends.csv"))
    d.call("set_active_part", part="frame rod")
    rows = ["FRAME ROD - BEND TABLE (bend in this order)",
            "STEP\tSTRAIGHT mm\tBEND\tFORMER R mm"]
    for k, bend in enumerate(sch["bends"], 1):
        rows.append(f"{k}\t{bend['straight_before_mm']:.1f}\t"
                    f"{bend['angle_deg']:.0f} DEG\t{bend['radius_mm']:.1f}")
    used = sum(b["straight_before_mm"] + math.radians(b["angle_deg"]) *
               b["radius_mm"] for b in sch["bends"])
    rows.append(f"END\t{sch['cut_length_mm'] - used:.1f}\t\t")
    rows.append(f"CUT\t{sch['cut_length_mm']:.1f} mm of "
                f"{s['d']:g} mm 1.4301 rod")
    d.call("create_drawing", name="frame rod sheet", sheet={
        "size": "a3", "landscape": True,
        "title_block": {"project": "Tea-ready alarm",
                        "document": "Frame rod", "author": AUTHOR,
                        "material": f"1.4301, {s['d']:g} MM ROD",
                        "notes": "BEND BY HAND FROM BOUGHT ROD"}})
    # A3 is 420 x 297: the rod at 1:1 down the left, the table to its
    # right, clear of the title block.
    d.call("add_drawing_view", source="frame rod", orientation="front",
           name="front", scale=1.0, position=[120.0, 140.0])
    d.call("update_drawing")
    d.call("add_drawing_note", text="\n".join(rows), position=[300.0, 220.0])
    rec = export(d, out, "tea_alarm_rod")
    rec["bends"] = sch["bends"]
    rec["cut_length_mm"] = sch["cut_length_mm"]
    return rec


def knob_sheet(d, s, marks, out):
    """The knob's dial: a mark every 5 C, its angle clockwise (from above)
    from --ready-c's. Clockwise drives the right-hand screw down, out of
    the arm: the probe touches it sooner, hotter."""
    rows = ["KNOB DIAL", f"DEG CLOCKWISE FROM {s['ready']:g} C",
            "(CLOCKWISE = HOTTER)", "MARK C\tDEG"] + \
        [f"{m['c']}\t{m['deg']:.0f}" for m in marks]
    d.call("create_drawing", name="knob sheet", sheet={
        "size": "a4", "landscape": True,
        "title_block": {"project": "Tea-ready alarm", "document": "Knob",
                        "author": AUTHOR, "material": "PRINTED"}})
    # A4 landscape, 297 x 210: the knob at 2:1 on the right, the dial's
    # table on the left.
    d.call("add_drawing_view", source="knob", orientation="top", name="top",
           scale=2.0, position=[220.0, 140.0])
    d.call("update_drawing")
    d.call("add_drawing_note", text="\n".join(rows), position=[95.0, 130.0])
    return export(d, out, "tea_alarm_knob")


# ---- the run -------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int)
    ap.add_argument("--profile", required=True,
                    help="your print profile for the printed parts "
                         "(print_coupon.py --seed installs the P1S's)")
    ap.add_argument("--strip-mm", type=float, nargs=2, required=True,
                    metavar=("WIDTH", "THICKNESS"),
                    help="the KANTHAL 94S strip as ordered")
    ap.add_argument("--rod-mm", type=float, required=True,
                    help="the 1.4301 rod as ordered (frame, probe, pivot, "
                         "chime)")
    ap.add_argument("--vessel-mm", type=float, nargs=3, required=True,
                    metavar=("DIAMETER", "DEPTH", "WALL"),
                    help="the mug: inside diameter, inside depth below "
                         "the rim, and the wall's thickness at the rim")
    ap.add_argument("--ready-c", type=float, required=True,
                    help="the temperature to ring at, degrees C")
    ap.add_argument("--room-c", type=float, required=True,
                    help="the room's air, degrees C: what the dry rod and "
                         "the block convect to")
    ap.add_argument("--no-render", action="store_true")
    ap.add_argument("--fresh", action="store_true")
    ap.add_argument("--fem-timeout", type=float, default=F.TIMEOUT_S)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()
    F.TIMEOUT_S = args.fem_timeout
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
    if args.fresh:
        fresh_project(d)
    proc = {"profile": args.profile}
    prof = next((p for p in d.call("list_print_profiles")["profiles"]
                 if p["name"] == args.profile), None) or fail(
        f"no print profile '{args.profile}'")
    if not prof.get("bed_mm") or not prof.get("layer_height_mm"):
        fail(f"'{args.profile}' states no bed or layer: the printed parts "
             "cannot be weighed or laid out")
    describe = d.call("describe")
    library_matches(describe)
    mat = prof["material"]
    service = describe["materials"][mat].get("service_max_c") or fail(
        f"'{mat}' has no service temperature: the block's margin is not "
        "known")
    try:
        s = S.solve(*args.strip_mm, args.rod_mm, *args.vessel_mm,
                    args.ready_c, args.room_c)
    except ValueError as e:
        fail(str(e))
    rec = {"inputs": vars(args), "stages": stages}

    # 1. The strip check, in its own project.
    rec["strip_check"] = F.strip_check(d, describe, out)
    stage("strip check")

    # 2. The heat path sizes the riser: the dry rod a fin long enough for
    # the profile's material to keep its margin at the still-air film, the
    # fin check's own tolerance taken off.
    hot = S.FILM_W_M2K[0]
    tip_max = service - S.MARGIN_K - 0.01 * (S.T_POUR - args.room_c)
    S.riser(s, tip_max, hot, args.room_c)
    d.call("new_project")
    mech = M.build(d, s, proc, describe)
    stat = mech["statics"]
    budget = M.strip_budget(s, stat["probe_force_n"])
    rec["statics"] = stat
    rec["strip"] = {"L_mm": s["L"], "tip_per_k_mm": s["A"],
                    "k_per_n": s["k_per_n"], **budget}
    print(f"strip: KANTHAL 94S {s['b']:g} x {s['s']:g}, {s['L']:.1f} mm "
          f"free, tip travel {s['A']:.4f} mm/K "
          f"({s['A'] * (S.T_POUR - args.ready_c):.2f} mm from {S.T_POUR:g} to {args.ready_c:g} C); "
          f"the probe's {1000 * stat['probe_force_n']:.2f} mN costs "
          f"{budget['offset_k']:.2f} K (an offset the screw takes out, "
          f"{100 * budget['force_share']:.1f} % of the travel, under "
          f"Kanthal's half); friction costs {budget['k_per_mn']:.3f} K per "
          f"mN; strip stress {budget['stress_mpa']:.1f} MPa against "
          f"{budget['stress_bound_mpa']:g}")
    if budget["force_share"] > S.MAX_FORCE_SHARE:
        fail("the probe's force takes more than half the strip's travel")
    print(f"ball: the largest the ramp still rests on the probe with is "
          f"{stat['largest_ball_mm']:.2f} mm; this build uses "
          f"{stat['ball_mm']:g} mm ({stat['ball_g']:.2f} g, to be weighed "
          f"on arrival)")
    rec["bimetal_refused"] = bimetal_refused(d)
    print("negative control, a structural study of the bimetal strip: "
          "refused, naming its Poisson's ratio")
    stage("parts and assembly")

    # 3. The trigger.
    tr = M.trigger(d, s, mech["occurrences"], mech["mates"],
                   stat["probe_force_n"])
    scatter = S.K94S_K_TOL * (tr["ready_c"] - S.T_FLAT)
    tr["scatter_k_from_strip"] = scatter
    rec["trigger"] = tr
    lo, hi = S.TRIGGER_RANGE
    if not (abs(tr["ready_c"] - args.ready_c) < 0.05 and
            lo - 0.05 <= tr["range_c"][0] and tr["range_c"][1] <= hi + 0.05):
        fail(f"the trigger is not where it was set: {tr}")
    print(f"trigger: rings at {tr['ready_c']:.2f} C at the knob's mark "
          f"({args.ready_c:g} C), +/- {scatter:.1f} K from the strip's "
          f"+/- 4 % and {budget['k_per_mn']:.3f} K per mN of friction; the "
          f"knob's {tr['turns']:.2f} turns span {tr['range_c'][0]:.1f} to "
          f"{tr['range_c'][1]:.1f} C")
    if tr["contact_refused"]:
        print(f"gap, a contact relation driven by a sliding pusher: "
              f"refused ({tr['contact_refused']}); the ramp is posed by "
              f"hand")
    stage("trigger")

    # 4. The heat path.
    fins = [F.fin(d, s, args.room_c, h) for h in S.FILM_W_M2K]
    ccx = F.fin(d, s, args.room_c, hot, solver="calculix")
    rec["fin"] = {"films": fins, "calculix": ccx,
                  "agreement": abs(ccx["tip_c"] / fins[0]["tip_c"] - 1)}
    for f in fins:
        print(f"fin at {f['film_w_m2k']:g} W/(m2 K): the rod's end in the "
              f"block at {f['tip_c']:.2f} C, the adiabatic-tip fin "
              f"{f['fin_c']:.2f} C ({100 * f['excess_off']:+.2f} % of the "
              f"rise over the room)")
        if abs(f["excess_off"]) > 0.01:
            fail("the fin is not within 1 % of its closed form")
    print(f"fin on CalculiX: {ccx['tip_c']:.4f} C, within "
          f"{rec['fin']['agreement']:.1e} of in-house")
    if rec["fin"]["agreement"] > 1e-5:
        fail("CalculiX's fin is not the in-house one")
    t_mount = fins[0]["tip_c"]
    cands = [mat] + [m for m in ("pla", "petg") if m != mat]
    sid, req, margins = F.block_heat(d, s, args.room_c, hot, t_mount, cands)
    chosen = next((m for m in cands if margins[m]["requirement"] == "pass"),
                  None) or fail(f"no printed material keeps its margin: "
                                f"{margins}")
    d.call("set_active_part", part="block")
    body = d.call("list_elements")["bodies"][0]["created_by"]
    d.call("set_material", feature_id=body, material=chosen)
    rec["block_heat"] = {"study": sid, "requirement": req, "margins": margins,
                         "chosen": chosen}
    others = ", ".join(f"{m} {margins[m]['margin_k']:.1f} K" for m in cands)
    print(f"block: its mount at {t_mount:.1f} C (still air, the rod's end), "
          f"margins {others}: printed in {chosen}, the profile's material, "
          f"which keeps {S.MARGIN_K:g} K; conduction only, no steam")
    stage("heat path")

    # 5. The clip.
    lin = F.clip_linear(d, s)
    d.call("add_assembly", name="clip on the rim")
    oc = d.call("add_occurrence", part="clip", name="clip", fixed=True)["id"]
    ov = d.call("add_occurrence", part="vessel", name="vessel",
                fixed=True)["id"]
    con = F.clip_contact(d, s, oc, ov)
    rec["clip"] = {"linear": lin, "contact": con}
    print(f"clip: its {lin['grip_n']:.3f} N grip opens the leaf by "
          f"{lin['tip_moved_mm']:.3f} mm against the wall's "
          f"{lin['delta_mm']:.3f} ({100 * lin['off']:+.2f} %), peak "
          f"{lin['peak_mpa']:.0f} MPa against 230 (beam {s['leaf_stress']:.0f}"
          f" at the root); on the vessel the jaw carries "
          f"{abs(con['force_n'][0]):.4f} N along the grip "
          f"({100 * con['along_off']:.2f} % off; the rod's hold on the inner "
          f"leaf the rest), {100 * con['tangential_share']:.1f} % of it "
          f"across, its line {con['line_z_mm'] - con['tip_z_mm']:.2f} mm up "
          f"a {s['jaw']:g} mm jaw, closed over {con['closed_area_mm2']:.3f} "
          f"mm2; balanced to {con['imbalance']:.1e}")
    if abs(lin["off"]) > 0.02 or lin["peak_mpa"] >= S.SS_FY:
        fail("the clip's leaf is not as its beam theory says")
    if con["along_off"] > 0.01 or not con["on_jaw"] or \
            con["imbalance"] > 1e-3:
        fail("the vessel does not carry the clip's grip at its jaw")
    stage("clip")

    # 6. The chime.
    ch = F.chime_modal(d, s)
    rec["chime"] = ch
    print(f"chime: f1 {ch['f1_hz']:.0f} Hz against Euler-Bernoulli's "
          f"{ch['euler_bernoulli_hz']:.0f} ({100 * ch['off']:+.2f} %); "
          f"{'in' if ch['heard'] else 'outside'} the {S.HEARD_HZ[0]:g}-"
          f"{S.HEARD_HZ[1]:g} Hz band chosen to be heard")
    if abs(ch["off"]) > 0.02:
        fail("the chime is not within 2 % of Euler-Bernoulli")
    stage("chime")

    # 7. Print, bill, drawings, saved.
    rec["parts"] = check_parts(d)
    print(f"every printed part checked as it prints ({len(M.PRINTED)} "
          f"parts): every item passes")
    rec["plate"] = plate(d, proc, prof, out)
    print(f"plate: {len(M.PRINTED)} pieces, every check_print item passes")
    rec["bill"] = bill(d, out)
    print(f"bill of materials: {len(rec['bill']['printed'])} printed, "
          f"{len(rec['bill']['bought'])} bought "
          f"({', '.join(rec['bill']['bought'])})")
    rec["drawings"] = {"clip": clip_sheet(d, s, mech["clip"][1], out),
                       "rod": rod_sheet(d, s, out),
                       "knob": knob_sheet(d, s, tr["marks"], out)}
    print("drawings: the clip's flat pattern with its bend lines and bend "
          "table, the rod's bend table, the knob's dial; check_drawing "
          "clean")
    d.call("save_project", path=str(out / "tea_alarm.lob"))
    stage("print, bill and drawings")

    # 8. The side cases.
    snap, ok = C.snap_case(d, out, F.TIMEOUT_S)
    rec["snap"] = snap
    print(f"snap-through: the PETG latch at eps {snap['eps']:g} peaks at "
          f"{snap['peak_factor']:.4f} of Timoshenko and Gere's limit "
          f"({100 * snap['off']:+.2f} %, {snap['peak_at']}); "
          + (f"first yield at {snap['first_yield_factor']:.3f}: it "
             f"{'yields before it snaps' if snap['yields_first'] else 'snaps before it yields'}"
             if snap["first_yield_factor"] is not None else
             "it does not yield on the way: it snaps before it yields"))
    print(f"negative control, the latch at eps 1.5: "
          f"{snap['control']['peak_at']}")
    if not ok:
        fail(f"the snap-through case missed: {snap}")
    pl = C.plastic_case(d, out, F.TIMEOUT_S)
    rec["plastic"] = pl
    off = pl["off"]
    print(f"plasticity: the bracket at 1.25 x its M_p load reaches it; the "
          f"span's outer fibre {pl['outer_plastic_strain_max']:.5f} "
          f"plastic against the bilinear section's "
          f"{pl['outer_plastic_strain_closed']:.5f} "
          f"({100 * off['plastic_strain']:+.1f} %); left turned "
          f"{pl['residual_turn_rad']:.5f} rad against "
          f"{pl['residual_turn_closed_rad']:.5f} "
          f"({100 * off['residual_turn']:+.1f} %); first yield at "
          f"{pl['first_yield_factor']:.3f} of the load against the span's "
          f"{pl['first_yield_factor_closed']:.3f}; with large deformation "
          f"{'a collapse at ' + format(pl['large_deformation']['collapse_factor'], '.3f') if pl['large_deformation']['collapse_factor'] else 'no collapse reported'}")
    print(f"negative control, the bracket under its yield: plastic strain "
          f"{pl['control']['max_plastic_strain']:.1e}, left "
          f"{pl['control']['max_residual_mm']:.1e} mm")
    if not (pl["reached_load"] and abs(off["plastic_strain"]) < 0.05 and
            abs(off["residual_turn"]) < 0.05 and
            pl["control"]["max_plastic_strain"] == 0.0):
        fail(f"the plastic case missed: {pl}")
    stage("side cases")

    rec["seconds"] = round(time.perf_counter() - t0, 1)
    with open(out / "tea_alarm.json", "w") as f:
        json.dump(rec, f, indent=1, default=str)
    print(f"tea_alarm: every check passed ({rec['seconds']:.0f} s)")


if __name__ == "__main__":
    main()
