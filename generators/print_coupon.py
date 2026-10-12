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
    python3 generators/print_coupon.py --port 8800 --z-gap-only \\
        --profile "K5 PLA on P1S" --layer 0.2
    python3 generators/print_coupon.py --port 8800 --record \\
        --profile "K5 PLA on P1S" --layer 0.2 --z-gap-layers 1

The allowance table (docs/tolerances/README.md#printed-parts) is community
practice and CAD-tool defaults, "to confirm with a test coupon". This is the
coupon. One plate, a row of each, every feature engraved with the size it is
MODELLED at:

- bores at the table's model size for a Ø`size` hole, and `steps` either side
  of it `step` mm apart;
- pins the same way for a Ø`size` shaft;
- discs printed loose in holes at the table's print-in-place `gap` round a
  Ø`size` disc, and `steps` either side (the clearance a print-in-place
  joint or gear needs so its parts do not fuse);
- a printed internal and external `thread` (default the metric coarse thread
  of `size`, when there is one) at the table's thread allowance, modelled as
  the custom threads print_allowance and a Thread's report advise;
- rings captured on posts, each printed N whole layers over the plate (1, 2
  and 3 by default): the vertical gap a print-in-place part stacked on
  another needs, where the disc row measures the sideways one. The ring
  clears its post by the table's gap a side, so the Z gap is the only thing
  that varies, and a 45-degree head on the post keeps it, well clear above.

Print it with the settings the parts will get, then push a real Ø`size` pin
(a drill shank, a dowel) into the bores and the pins into a reamed Ø`size`
hole, and screw a real bolt and nut onto the threads. Push each loose disc out
of its hole: the tightest that comes free by hand is the gap print-in-place
parts want. Twist each ring on its post: the fewest layers that turn cleanly
are the Z gap. The bore and the pin that fit the way you want are the model
sizes the table should give.

`--profile NAME` builds the coupon against one of your print profiles
(docs/tolerances/README.md#print-profiles) and exports it at the profile's
chord deviation. `--record` keeps the answer: it reads the winners off the
command line (`--gap-side`, `--bore`, `--pin`, `--thread-went`,
`--z-gap-layers`), turns them into rows on top of the profile (or, for a new
one, the built-in table of `--process`/`--material`) -- the Z gap into the
profile's `z_gap_mm` and `layer_height_mm` -- and saves it with
`save_print_profile`, each row's `source` naming the coupon. `--seed`
installs the two profiles measured on a Bambu Lab P1S (K5 PLA at 0.1 mm a
side, a plain PLA at 0.15, both at the preset's 0.2 mm layers), skipping any
name the store already has.

`--gap-only` and `--z-gap-only` print one row alone, for a profile whose
other rows are already measured. The Z gaps are whole layers of the layer
the parts are sliced at: the profile's `layer_height_mm`, else `--layer`;
with neither the coupon refuses rather than guess one.

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
from _lib import (check, circle, connect, fresh_project, line,  # noqa: E402
                  plane_at_z)

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
    {"name": "K5 PLA on P1S", "gap_side": 0.1, "layer": 0.2,
     "source": "MEASURED, print_coupon --gap-only on a Bambu Lab P1S in K5 "
               "PLA (2026-10-09): 0.075 mm a side freed only under force and "
               "rubbed, 0.1 slid freely, and a gearbox modelled at 0.25 was "
               "loose"},
    {"name": "PLA on P1S", "gap_side": 0.15, "layer": 0.2,
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
            "layer_height_mm": s["layer"], "rows": rows})
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
    layer = prof.get("layer_height_mm")
    z_gap = prof.get("z_gap_mm")
    if args.layer is not None:
        layer = args.layer
    if args.z_gap_layers is not None:
        if args.z_gap_layers < 1:
            raise SystemExit("print_coupon --record: --z-gap-layers counts "
                             "whole layers, 1 or more")
        if not layer:
            raise SystemExit("print_coupon --record: a Z gap is whole layers "
                             "-- pass --layer, the layer height the coupon "
                             "was sliced at (the profile states none)")
        z_gap = round(args.z_gap_layers * layer, 6)
    unchanged = (len(rows) == len(prof["rows"]) and all(
        a == b for a, b in zip(rows, prof["rows"])))
    if (unchanged and z_gap == prof.get("z_gap_mm")
            and layer == prof.get("layer_height_mm")):
        raise SystemExit("print_coupon --record: nothing to record — pass "
                         "--gap-side, --bore, --pin, --thread-went or "
                         "--z-gap-layers")
    # The deflection the coupon was exported at: the one asked for, else
    # the profile's own, else the coupon's default.
    deflection = (args.deflection if args.deflection is not None else
                  prof["stl_deflection_mm"] if prof.get("stl_deflection_stated")
                  else 0.01)
    out = {"name": prof["name"], "process": prof["process"],
           "material": prof["material"], "rows": rows,
           "conditions": args.conditions or prof.get("conditions", ""),
           "stl_deflection_mm": deflection}
    if layer:
        out["layer_height_mm"] = layer
    if z_gap:
        out["z_gap_mm"] = z_gap
    saved = d.call("save_print_profile", profile=out)
    z = (f", Z gap {saved['z_gap_mm']:g} mm" if saved.get("z_gap_mm")
         else "")
    print(f"print_coupon: recorded {len(rows)} rows in '{saved['name']}' "
          f"(STL at {saved['stl_deflection_mm']:g} mm{z})")
    return 0


def z_row(d, args, gap):
    """The Z-gap row's layer height and its gaps in whole layers.

    The layer is the profile's `layer_height_mm`, else `--layer`; with
    neither, refused -- a Z gap in the wrong layers measures nothing. The
    gaps are `--z-layers`, else 1-3 layers, or either side of the profile's
    measured Z gap."""
    prof = None
    if args.profile:
        prof = next((p for p in d.call("list_print_profiles")["profiles"]
                     if p["name"] == args.profile), None)
    layer, source = args.layer, "--layer"
    if layer is None and prof and prof.get("layer_height_mm"):
        layer, source = prof["layer_height_mm"], "the profile"
    if layer is None or layer <= 0:
        raise SystemExit("print_coupon: the Z-gap row is whole layers of the "
                         "layer you slice at -- pass --layer (Bambu "
                         "Studio's 0.20 mm preset is 0.2) or state "
                         "layer_height_mm in the profile")
    if args.z_layers:
        try:
            layers = sorted({int(n) for n in args.z_layers.split(",")})
        except ValueError:
            raise SystemExit(f"print_coupon: --z-layers is whole layers, "
                             f"e.g. 1,2,3, not {args.z_layers!r}")
        if layers[0] < 1:
            raise SystemExit("print_coupon: --z-layers starts at 1 layer")
    elif prof and prof.get("z_gap_mm"):
        k = round(prof["z_gap_mm"] / layer)
        layers = [k - 1, k, k + 1] if k > 1 else [1, 2, 3]
    else:
        layers = [1, 2, 3]
    return {"layer_mm": layer, "layer_from": source, "layers": layers,
            "gap_side_mm": gap["allowance_mm"] / 2.0}


def z_features(zrow, size, plate_top, pitch, xs, y):
    """Each ring on its post, in millimetres from the size: a post a
    quarter of the size in radius, a ring 1.6 sizes across and a quarter of
    one thick, clear of the post by the gap a side, and a 45-degree head that
    overlaps the ring's hole by 0.15 sizes, two layers clear above the
    largest Z gap's ring so only the ring's underside is under test."""
    L, g = zrow["layer_mm"], zrow["gap_side_mm"]
    r_post = 0.25 * size
    r_hole = r_post + g
    r_ring = 0.8 * size
    t = 0.25 * size
    r_head = r_hole + 0.15 * size
    clear = L * (max(zrow["layers"]) + 2)
    out = []
    for x, n in zip(xs, zrow["layers"]):
        z_gap = n * L
        bottom = plate_top + z_gap
        cone = bottom + t + clear  # where the head starts to flare
        label = fmt(round(z_gap, 6))
        out.append({
            "x": x, "y": y, "layers": n, "z_gap": z_gap, "label": label,
            "plate_top": plate_top, "bottom": bottom, "thick": t,
            "r_post": r_post, "r_hole": r_hole, "r_ring": r_ring,
            "r_head": r_head, "cone": cone,
            "label_at": (x, y - r_ring - 0.1 * size),
            "report": {"kind": "z_gap", "layers": n, "z_gap_mm": z_gap,
                       "ring_mm": 2.0 * r_ring, "post_mm": 2.0 * r_post,
                       "hole_mm": 2.0 * r_hole, "thick_mm": t,
                       "head_clear_mm": clear, "at": [x, y]}})
    return out


def build_post(d, r):
    """The post and its 45-degree head, revolved about the ring's axis and
    joined to the plate."""
    top, cone = r["plate_top"], r["cone"]
    rp, rh = r["r_post"], r["r_head"]
    head_top = cone + (rh - rp)  # 45 degrees: as high as it is wide
    ents = [line((0.0, top), (rp, top)), line((rp, top), (rp, cone)),
            line((rp, cone), (rh, head_top)),
            line((rh, head_top), (0.0, head_top)),
            line((0.0, head_top), (0.0, top))]
    name = f"Post {r['label']}"
    sk = check(d.call("add_feature", feature={
        "type": "Sketch", "name": f"{name} profile",
        "plane": {"origin": [r["x"], r["y"], 0.0], "normal": [0, -1, 0],
                  "uAxis": [1, 0, 0]},
        "entities": ents}), f"{name} profile")
    check(d.call("add_feature", feature={
        "type": "Revolve", "name": name, "sketch_id": sk["id"],
        "axis": {"kind": "custom", "origin": [r["x"], r["y"], 0.0],
                 "direction": [0.0, 0.0, 1.0]},
        "angle_deg": 360.0, "mode": "add"}), name)


def build_ring(d, r):
    """The ring: its own body, its underside `layers` layers over the
    plate."""
    name = f"Ring {r['label']}"
    sk = check(d.call("add_feature", feature={
        "type": "Sketch", "name": f"{name} sketch",
        "plane": plane_at_z(r["bottom"]),
        "entities": [circle((r["x"], r["y"]), r["r_ring"]),
                     circle((r["x"], r["y"]), r["r_hole"])]}),
        f"{name} sketch")
    check(d.call("add_feature", feature={
        "type": "Extrude", "name": name, "mode": "new_body",
        "sketch_id": sk["id"], "distance": r["thick"]}), name)


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
    ap.add_argument("--z-gap-layers", type=int, default=None,
                    help="--record: the fewest layers whose ring turned "
                         "cleanly")
    ap.add_argument("--layer", type=float, default=None,
                    help="the layer height the coupon is sliced at, mm "
                         "(default: the profile's layer_height_mm)")
    ap.add_argument("--z-layers", default=None,
                    help="the Z-gap row's gaps, in whole layers, e.g. 1,2,3 "
                         "(default: 1,2,3, or either side of the profile's "
                         "measured Z gap)")
    ap.add_argument("--z-gap-only", action="store_true",
                    help="only the row of rings on posts (the vertical gap), "
                         "on a thin plate")
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
    if not args.z_gap_only and gap_mid - args.steps * gap_step <= 0.0:
        raise SystemExit(
            f"print_coupon: {args.steps} steps of {fmt(gap_step)} below a "
            f"gap of {fmt(gap_mid)} on the diameter close it — fewer "
            f"--steps or a smaller --step")
    if args.gap_only and args.z_gap_only:
        raise SystemExit("print_coupon: --gap-only or --z-gap-only, not "
                         "both (without either, the coupon has every row)")
    one_row = args.gap_only or args.z_gap_only
    thread = "none" if one_row else args.thread
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

    # The Z-gap row: whole layers of the layer the parts are sliced at.
    zrow = None if args.gap_only else z_row(d, args, gap)

    # Everything from the size: a feature every 2.5 diameters, a plate a
    # diameter thick (a quarter of one under the Z-gap row alone), labels
    # under each feature.
    pitch = 2.5 * size
    count = 2 * args.steps + 1
    label_h = min(0.3 * size, 4.0)
    engrave = 0.05 * size
    thick = 0.25 * size if args.z_gap_only else size
    # Top to bottom: bores, pins, threads, gaps, Z gaps -- the ones built.
    kinds = (["gap"] if args.gap_only else ["z"] if args.z_gap_only else
             ["bore", "pin"] + (["thread"] if nut else []) + ["gap", "z"])
    row_of = {k: i for i, k in enumerate(kinds)}
    rows = len(kinds)
    width = (len(zrow["layers"]) if args.z_gap_only else count) * pitch
    depth = rows * pitch
    what = (args.profile.lower().replace(" ", "_") if args.profile else
            f"{args.process}_{args.material}")
    name = args.name or (f"coupon_{what}_{fmt(size)}"
                         + ("_gap" if args.gap_only else
                            "_zgap" if args.z_gap_only else ""))

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
    for i in range(0 if args.z_gap_only else count):
        k = i - args.steps
        for row, what, base in (() if args.gap_only else
                                ((row_of["bore"], "bore", hole),
                                 (row_of["pin"], "pin", shaft))):
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
        at = (x_at(i), y_at(row_of["gap"]))
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
        xi, xe, yt = x_at(0), x_at(count - 1), y_at(row_of["thread"])
        bores.append(circle((xi, yt), nut_face / 2.0))
        pins.append(circle((xe, yt), screw_face / 2.0))
        labels.append(text(f"{thread} {nut['allowance_mm']:+g}",
                           (xi, yt - 0.62 * size), label_h))
        labels.append(text(f"{thread} {screw['allowance_mm']:+g}",
                           (xe, yt - 0.62 * size), label_h))
        thread_faces = [("nut", nut_face, xi, nut), ("screw", screw_face, xe,
                                                     screw)]
        report["features"] += [
            {"kind": "internal_thread", "designation": thread,
             "modelled_face_mm": nut_face, "major_mm": nut["model_mm"],
             "pitch": p},
            {"kind": "external_thread", "designation": thread,
             "modelled_face_mm": screw_face, "major_mm": screw["model_mm"],
             "pitch": p}]

    rings = []
    if zrow:
        # Centred under the rows above, which are `count` features wide.
        first = 0 if args.z_gap_only else (count - len(zrow["layers"])) // 2
        rings = z_features(zrow, size, thick, pitch,
                           [x_at(first + i) for i in
                            range(len(zrow["layers"]))],
                           y_at(row_of["z"]))
        for r in rings:
            labels.append(text(r["label"], r["label_at"], label_h))
            report["features"].append(r["report"])
        report["z_gap"] = {k: zrow[k] for k in ("layer_mm", "layer_from",
                                                "layers", "gap_side_mm")}

    top = plane_at_z(thick)
    if pins:
        check(d.call("add_feature", feature={
            "type": "Sketch", "name": "Pins", "plane": top,
            "entities": pins}), "pin sketch")
        check(d.call("add_feature", feature={
            "type": "Extrude", "name": "Pins up", "mode": "add",
            "sketch_id": feature_id(d, "Pins"), "distance": size}), "pins")
    if bores:
        check(d.call("add_feature", feature={
            "type": "Sketch", "name": "Bores", "plane": top,
            "entities": bores}), "bore sketch")
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
    # The posts and their heads, part of the plate.
    for r in rings:
        build_post(d, r)
    # The loose discs: their own bodies, standing on the bed in their holes.
    if discs:
        check(d.call("add_feature", feature={
            "type": "Sketch", "name": "Discs", "plane": plane_at_z(0),
            "entities": discs}), "disc sketch")
        check(d.call("add_feature", feature={
            "type": "Extrude", "name": "Discs up", "mode": "new_body",
            "sketch_id": feature_id(d, "Discs"), "distance": thick}),
            "discs")
    # The rings: their own bodies, each its whole layers over the plate.
    for r in rings:
        build_ring(d, r)

    for what, dia, x, info in thread_faces:
        r = check(d.call("add_feature", feature={
            "type": "Thread", "name": f"Coupon {what}",
            "designation": f"coupon {what} thread", "pitch": info["pitch"],
            "modeled": True,
            "face": round_face(d, dia, x, y_at(row_of["thread"]))}),
            f"{what} thread")
        if "print" in r:
            print(f"  {r['print']['sentence']}")

    OUT.mkdir(parents=True, exist_ok=True)
    stl = OUT / f"{name}.stl"
    exp = {"path": str(stl)}
    if args.deflection is not None:
        exp["deflection"] = args.deflection
    r = d.call("export_stl", **exp)
    report["stl_deflection_mm"] = r.get("deflection_mm")
    shells = 1 + len(discs) + len(rings)
    if r.get("shells") is not None and r["shells"] != shells:
        raise SystemExit(f"print_coupon: the STL has {r['shells']} shells, "
                         f"not the plate, {len(discs)} loose discs and "
                         f"{len(rings)} rings: {r}")
    d.call("save_project", path=str(OUT / f"{name}.dsgn"))
    (OUT / f"{name}.json").write_text(json.dumps(report, indent=1))
    print(f"print_coupon: {len(report['features'])} features -> {stl}")
    if zrow:
        print(f"  Z gaps: {', '.join(r['label'] for r in rings)} mm "
              f"({', '.join(str(n) for n in zrow['layers'])} layers of "
              f"{fmt(zrow['layer_mm'])}, from {zrow['layer_from']}); twist "
              f"each ring, and --record --z-gap-layers N the fewest that "
              f"turned cleanly")
    print(f"  {hole['confirm']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
