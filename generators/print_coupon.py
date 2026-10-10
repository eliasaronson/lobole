#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A printable test coupon that calibrates a print profile.

    python3 generators/print_coupon.py --port 8800
    python3 generators/print_coupon.py --port 8800 --size 10 --steps 3 \\
        --thread M10 --process fdm --material pla
    python3 generators/print_coupon.py --port 8800 --profile "K5 PLA on P1S"
    python3 generators/print_coupon.py --port 8800 --record \\
        --profile "K5 PLA on P1S" --process fdm --material pla --size 8 \\
        --gap-side 0.1 --bore 8.2 --pin 7.9 --thread-went yes
    python3 generators/print_coupon.py --port 8800 --seed

The allowance table (docs/tolerances/README.md#printed-parts) is community
practice and CAD-tool defaults, "to confirm with a test coupon". This is the
coupon. One plate, three rows, every feature engraved with the diameter it is
MODELLED at:

- bores at the table's model size for a Ø`size` hole, and `steps` either side
  of it `step` mm apart;
- pins the same way for a Ø`size` shaft;
- discs printed loose in holes at the table's print-in-place `gap` round a
  Ø`size` disc, and `steps` either side (the clearance a print-in-place
  joint or gear needs so its parts do not fuse);
- a printed internal and external `thread` (default the metric coarse thread
  of `size`, when there is one) at the table's thread allowance, modelled as
  the custom threads print_allowance and a Thread's report advise.

Print it with the settings the parts will get, then push a real Ø`size` pin
(a drill shank, a dowel) into the bores and the pins into a reamed Ø`size`
hole, and screw a real bolt and nut onto the threads. Push each loose disc out
of its hole: the tightest that comes free by hand is the gap print-in-place
parts want. The bore and the pin that fit the way you want are the model
sizes the table should give.

`--profile NAME` builds the coupon against one of your print profiles
(docs/tolerances/README.md#print-profiles) and exports it at the profile's
chord deviation. `--record` keeps the answer: it reads the winners off the
command line (`--gap-side`, `--bore`, `--pin`, `--thread-went`), turns them
into rows on top of the profile (or, for a new one, the built-in table of
`--process`/`--material`) and saves it with `save_print_profile`, each row's
`source` naming the coupon. `--seed` installs the two profiles measured on a
Bambu Lab P1S (K5 PLA at 0.1 mm a side, a plain PLA at 0.15), skipping any
name the store already has.

Every size derives from `--size`: the pitch between features, the plate's
thickness (a bore as deep as it is wide), the label height and the engraving
depth. The numbers come from the app (`print_allowance`), never from here, so
the coupon always tests the table the build carries.

Output (generated, untracked): scratch/print_coupon/<name>.stl, the project,
and <name>.json listing every feature, its modelled diameter and its label.
"""
import argparse
import datetime
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _lib import check, circle, connect, fresh_project, plane_at_z  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "scratch" / "print_coupon"


def fmt(v):
    """8.15 -> "8.15", 8.2 -> "8.2": the label a person reads off the plate."""
    s = f"{v:.3f}".rstrip("0").rstrip(".")
    return s


def text(label, at, height):
    return {"type": "text", "text": label, "font": "sans", "height": height,
            "anchor": [float(at[0]), float(at[1])], "halign": "center",
            "valign": "top"}


def feature_id(d, name):
    return {f["name"]: f["id"] for f in d.call("get_state")["features"]}[name]


def round_face(d, diameter, x, y):
    """The cylindrical face of `diameter` centred at (x, y), as a ref."""
    for b in d.call("list_elements")["bodies"]:
        for f in b.get("faces", []):
            if f.get("kind") != "cylinder":
                continue
            c = f["centroid"]
            if (abs(2.0 * f["radius"] - diameter) < 1e-3
                    and abs(c[0] - x) < 1e-3 and abs(c[1] - y) < 1e-3):
                return {"feature_id": b["created_by"], "element": f["name"]}
    raise SystemExit(f"no Ø{diameter} face at ({x}, {y})")


# Measured on one Bambu Lab P1S (Bambu Studio 0.20 mm, X-Y hole and contour
# compensation 0, STL at 0.01 mm chord deviation), print_coupon --gap-only:
# the first two profiles (docs/tolerances/README.md#from-a-measurement).
P1S_CONDITIONS = ("Bambu Lab P1S, 0.4 mm nozzle, Bambu Studio 0.20 mm, X-Y "
                  "hole and contour compensation 0, the STL at 0.01 mm chord "
                  "deviation")
SEED = [
    {"name": "K5 PLA on P1S", "gap_side": 0.1,
     "source": "MEASURED, print_coupon --gap-only on a Bambu Lab P1S in K5 "
               "PLA (2026-10-09): 0.075 mm a side freed only under force and "
               "rubbed, 0.1 slid freely, and a gearbox modelled at 0.25 was "
               "loose"},
    {"name": "PLA on P1S", "gap_side": 0.15,
     "source": "MEASURED, print_coupon --gap-only on a Bambu Lab P1S in an "
               "ordinary PLA: 0.15 mm a side slid freely"},
]


def seed(d):
    """Install the two P1S profiles, leaving any name already stored."""
    have = {p["name"] for p in d.call("list_print_profiles")["profiles"]}
    for s in SEED:
        if s["name"] in have:
            print(f"print_coupon: '{s['name']}' is already one of your "
                  f"profiles; left as it is")
            continue
        rows = [r for r in builtin_rows(d, "fdm", "pla")
                if r["kind"] != "gap"]
        rows.append({"kind": "gap", "from_mm": 0,
                     "allowance_mm": round(2.0 * s["gap_side"], 4),
                     "source": s["source"]})
        p = d.call("save_print_profile", profile={
            "name": s["name"], "process": "fdm", "material": "pla",
            "conditions": P1S_CONDITIONS, "stl_deflection_mm": 0.01,
            "rows": rows})
        print(f"print_coupon: saved '{p['name']}' (gap {s['gap_side']:g} mm "
              f"a side)")
    return 0


def builtin_rows(d, process, material):
    """The built-in table's rows of a pair -- the defaults a profile starts
    from -- as save_print_profile takes them."""
    for t in d.call("list_print_profiles")["defaults"]:
        if t["process"] == process and t["material"] == material:
            rows = []
            for r in t["rows"]:
                r = {k: v for k, v in r.items() if v is not None}
                rows.append(r)
            return rows
    raise SystemExit(f"print_coupon: no built-in table for {process} "
                     f"{material}")


def record(d, args):
    """The coupon's winners, as rows of a profile, saved."""
    if not args.profile:
        raise SystemExit("print_coupon --record needs --profile NAME: the "
                         "profile the answer is kept in")
    mine = {p["name"]: p for p in d.call("list_print_profiles")["profiles"]}
    if args.profile in mine:
        prof = mine[args.profile]
    else:
        prof = {"name": args.profile, "process": args.process,
                "material": args.material,
                "rows": builtin_rows(d, args.process, args.material)}
    when = datetime.date.today().isoformat()
    tag = f"print_coupon {when}, Ø{fmt(args.size)}"
    rows = [dict(r) for r in prof["rows"]]

    def replace(kind, allowance, what):
        nonlocal rows
        rows = [r for r in rows if r["kind"] != kind]
        rows.append({"kind": kind, "from_mm": 0, "allowance_mm": allowance,
                     "source": f"{tag}: {what}"})

    if args.bore is not None:
        replace("hole", round(args.bore - args.size, 4),
                f"the bore modelled Ø{fmt(args.bore)} fit a Ø{fmt(args.size)} "
                f"pin")
    if args.pin is not None:
        replace("shaft", round(args.pin - args.size, 4),
                f"the pin modelled Ø{fmt(args.pin)} fit a Ø{fmt(args.size)} "
                f"hole")
    if args.gap_side is not None:
        replace("gap", round(2.0 * args.gap_side, 4),
                f"the tightest disc that slid was {fmt(args.gap_side)} a side")
    if args.thread_went is not None:
        for r in rows:
            if r["kind"] in ("internal_thread", "external_thread"):
                r["source"] = (f"{tag}: the thread went at this allowance"
                               if args.thread_went == "yes" else
                               f"{tag}: the thread did NOT go at this "
                               f"allowance; open it")
    if len(rows) == len(prof["rows"]) and all(
            a == b for a, b in zip(rows, prof["rows"])):
        raise SystemExit("print_coupon --record: nothing to record — pass "
                         "--gap-side, --bore, --pin or --thread-went")
    # The deflection the coupon was exported at: the one asked for, else
    # the profile's own, else the coupon's default.
    deflection = (args.deflection if args.deflection is not None else
                  prof["stl_deflection_mm"] if prof.get("stl_deflection_stated")
                  else 0.01)
    out = {"name": prof["name"], "process": prof["process"],
           "material": prof["material"], "rows": rows,
           "conditions": args.conditions or prof.get("conditions", ""),
           "stl_deflection_mm": deflection}
    saved = d.call("save_print_profile", profile=out)
    print(f"print_coupon: recorded {len(rows)} rows in '{saved['name']}' "
          f"(STL at {saved['stl_deflection_mm']:g} mm)")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int)
    ap.add_argument("--process", default="fdm")
    ap.add_argument("--material", default="pla")
    ap.add_argument("--profile", default=None,
                    help="one of your print profiles: build against it, or "
                         "with --record the one the answer is kept in")
    ap.add_argument("--record", action="store_true",
                    help="build nothing: save the winners below as rows of "
                         "--profile")
    ap.add_argument("--gap-side", type=float, default=None,
                    help="--record: the tightest gap disc that slid, mm a "
                         "side")
    ap.add_argument("--bore", type=float, default=None,
                    help="--record: the bore (as modelled) that fit a "
                         "Ø--size pin, mm")
    ap.add_argument("--pin", type=float, default=None,
                    help="--record: the pin (as modelled) that fit a "
                         "Ø--size hole, mm")
    ap.add_argument("--thread-went", choices=("yes", "no"), default=None,
                    help="--record: whether the bolt and nut went on")
    ap.add_argument("--conditions", default=None,
                    help="--record: printer, nozzle, slicer preset")
    ap.add_argument("--seed", action="store_true",
                    help="install the two profiles measured on a P1S")
    ap.add_argument("--size", type=float, default=8.0,
                    help="the diameter the fit is at, mm (the printed size)")
    ap.add_argument("--steps", type=int, default=2,
                    help="variants either side of the table's size")
    ap.add_argument("--step", type=float, default=0.05,
                    help="mm on the diameter between variants")
    ap.add_argument("--thread", default=None,
                    help="thread designation (default: the metric coarse "
                         "M<size>, if the tables carry it; 'none' for none)")
    # Fine: at export_stl's own 0.1 mm the chords took 0.03 mm a side off
    # every gap and the slicer closed the two narrowest; a coupon that
    # measures the chords measures nothing about the printer.
    ap.add_argument("--deflection", type=float, default=None,
                    help="STL chord deviation, mm (default: the profile's, "
                         "else 0.01; export the parts it calibrates at the "
                         "same)")
    ap.add_argument("--gap-only", action="store_true",
                    help="only the row of loose discs (a second coupon, "
                         "once the first has said which way to look)")
    ap.add_argument("--gap-center", type=float, default=None,
                    help="mm A SIDE the gap row centres on (default: the "
                         "table's), for a printer the first coupon showed "
                         "to sit outside its steps")
    ap.add_argument("--name", default=None)
    ap.add_argument("--fresh", action="store_true",
                    help="start a new project first (refused on unsaved work)")
    args = ap.parse_args()

    d = connect(args.port)
    if args.seed:
        return seed(d)
    if args.record:
        return record(d, args)
    if args.fresh:
        fresh_project(d)
    proc = ({"profile": args.profile} if args.profile else
            {"process": args.process, "material": args.material})
    if args.deflection is None and not args.profile:
        args.deflection = 0.01
    size = args.size

    def allowance(kind, **kw):
        return d.call("print_allowance", kind=kind, **proc, **kw)

    hole = allowance("hole", size_mm=size)
    shaft = allowance("shaft", size_mm=size)
    gap = allowance("gap", size_mm=size)
    gap_step = 2.0 * args.step
    # On the diameter, as the table states it.
    gap_mid = (gap["allowance_mm"] if args.gap_center is None
               else 2.0 * args.gap_center)
    if gap_mid - args.steps * gap_step <= 0.0:
        raise SystemExit(
            f"print_coupon: {args.steps} steps of {fmt(gap_step)} below a "
            f"gap of {fmt(gap_mid)} on the diameter close it — fewer "
            f"--steps or a smaller --step")
    thread = "none" if args.gap_only else args.thread
    if thread is None:
        guess = f"M{fmt(size)}"
        try:
            allowance("internal_thread", designation=guess)
            thread = guess
        except RuntimeError:
            thread = "none"
    nut = screw = None
    if thread != "none":
        nut = allowance("internal_thread", designation=thread)
        screw = allowance("external_thread", designation=thread)

    # Everything from the size: a feature every 2.5 diameters, a plate a
    # diameter thick, labels under each feature.
    pitch = 2.5 * size
    count = 2 * args.steps + 1
    label_h = min(0.3 * size, 4.0)
    engrave = 0.05 * size
    thick = size
    rows = 1 if args.gap_only else 4 if nut else 3
    width = count * pitch
    depth = rows * pitch
    what = (args.profile.lower().replace(" ", "_") if args.profile else
            f"{args.process}_{args.material}")
    name = args.name or (f"coupon_{what}_{fmt(size)}"
                         + ("_gap" if args.gap_only else ""))

    d.call("add_part", name=name)
    d.call("set_print_process", **proc)
    # The plate stands on Top with its low corner at the origin.
    check(d.call("add_feature", feature={
        "type": "Box", "name": "Plate", "on": "Top", "centered": False,
        "offset": [0, 0], "width": width, "depth": depth, "height": thick}),
        "plate")

    def x_at(i):
        return pitch * (i + 0.5)

    def y_at(row):
        return depth - pitch * (row + 0.45)

    report = {"process": proc, "size": size, "step": args.step,
              "gap_center_from": ("table" if args.gap_center is None
                                  else "--gap-center"),
              "confirm": hole["confirm"], "features": []}
    bores, pins, discs, labels = [], [], [], []
    for i in range(count):
        k = i - args.steps
        for row, what, base in (() if args.gap_only else
                                ((0, "bore", hole), (1, "pin", shaft))):
            dia = round(base["model_mm"] + k * args.step, 4)
            (bores if what == "bore" else pins).append(
                circle((x_at(i), y_at(row)), dia / 2.0))
            labels.append(text(fmt(dia), (x_at(i), y_at(row) - 0.62 * size),
                               label_h))
            report["features"].append(
                {"kind": what, "modelled_mm": dia, "offset_mm": k * args.step,
                 "table": k == 0, "at": [x_at(i), y_at(row)]})
        # A gap is the whole diametral clearance, so its steps are twice the
        # bore's: the same step on each side of the disc. The label is the
        # gap a side, which is how print-in-place clearance is quoted.
        g = round(gap_mid + k * gap_step, 4)
        at = (x_at(i), y_at(rows - 1))
        bores.append(circle(at, (size + g) / 2.0))
        discs.append(circle(at, size / 2.0))
        labels.append(text(fmt(g / 2.0), (at[0], at[1] - 0.62 * size),
                           label_h))
        report["features"].append(
            {"kind": "gap", "disc_mm": size, "hole_mm": round(size + g, 4),
             "gap_per_side_mm": g / 2.0, "offset_mm": k * gap_step,
             "table": k == 0, "at": list(at)})

    thread_faces = []
    if nut:
        # The advised form: a custom thread on the face that gives the
        # printed major (docs/tolerances/README.md#printed-parts). An
        # internal thread's bore is its basic minor; a boss is its major.
        p = nut["pitch"]
        nut_face = round(nut["model_mm"] - 1.0825 * p, 4)
        screw_face = round(screw["model_mm"], 4)
        xi, xe = x_at(0), x_at(count - 1)
        bores.append(circle((xi, y_at(2)), nut_face / 2.0))
        pins.append(circle((xe, y_at(2)), screw_face / 2.0))
        labels.append(text(f"{thread} {nut['allowance_mm']:+g}",
                           (xi, y_at(2) - 0.62 * size), label_h))
        labels.append(text(f"{thread} {screw['allowance_mm']:+g}",
                           (xe, y_at(2) - 0.62 * size), label_h))
        thread_faces = [("nut", nut_face, xi, nut), ("screw", screw_face, xe,
                                                     screw)]
        report["features"] += [
            {"kind": "internal_thread", "designation": thread,
             "modelled_face_mm": nut_face, "major_mm": nut["model_mm"],
             "pitch": p},
            {"kind": "external_thread", "designation": thread,
             "modelled_face_mm": screw_face, "major_mm": screw["model_mm"],
             "pitch": p}]

    top = plane_at_z(thick)
    if pins:
        check(d.call("add_feature", feature={
            "type": "Sketch", "name": "Pins", "plane": top,
            "entities": pins}), "pin sketch")
        check(d.call("add_feature", feature={
            "type": "Extrude", "name": "Pins up", "mode": "add",
            "sketch_id": feature_id(d, "Pins"), "distance": size}), "pins")
    check(d.call("add_feature", feature={
        "type": "Sketch", "name": "Bores", "plane": top, "entities": bores}),
        "bore sketch")
    check(d.call("add_feature", feature={
        "type": "Extrude", "name": "Bores down", "mode": "cut",
        "sketch_id": feature_id(d, "Bores"), "distance": thick,
        "end_mode": "through_all", "reversed": True}), "bores")
    check(d.call("add_feature", feature={
        "type": "Sketch", "name": "Labels", "plane": top,
        "entities": labels}), "label sketch")
    check(d.call("add_feature", feature={
        "type": "Extrude", "name": "Engrave", "mode": "cut",
        "sketch_id": feature_id(d, "Labels"), "distance": engrave,
        "reversed": True}), "engrave")
    # The loose discs: their own bodies, standing on the bed in their holes.
    check(d.call("add_feature", feature={
        "type": "Sketch", "name": "Discs", "plane": plane_at_z(0),
        "entities": discs}), "disc sketch")
    check(d.call("add_feature", feature={
        "type": "Extrude", "name": "Discs up", "mode": "new_body",
        "sketch_id": feature_id(d, "Discs"), "distance": thick}), "discs")

    for what, dia, x, info in thread_faces:
        r = check(d.call("add_feature", feature={
            "type": "Thread", "name": f"Coupon {what}",
            "designation": f"coupon {what} thread", "pitch": info["pitch"],
            "modeled": True,
            "face": round_face(d, dia, x, y_at(2))}), f"{what} thread")
        if "print" in r:
            print(f"  {r['print']['sentence']}")

    OUT.mkdir(parents=True, exist_ok=True)
    stl = OUT / f"{name}.stl"
    exp = {"path": str(stl)}
    if args.deflection is not None:
        exp["deflection"] = args.deflection
    r = d.call("export_stl", **exp)
    report["stl_deflection_mm"] = r.get("deflection_mm")
    if r.get("shells") is not None and r["shells"] != 1 + count:
        raise SystemExit(f"print_coupon: the STL has {r['shells']} shells, "
                         f"not the plate and {count} loose discs: {r}")
    d.call("save_project", path=str(OUT / f"{name}.dsgn"))
    (OUT / f"{name}.json").write_text(json.dumps(report, indent=1))
    print(f"print_coupon: {len(report['features'])} features -> {stl}")
    print(f"  {hole['confirm']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
