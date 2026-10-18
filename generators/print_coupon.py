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
        --profile "K5 PLA on P1S" --layer 0.2 --z-gap-layers 3 \\
        --post-gap-side 0.1
    python3 generators/print_coupon.py --port 8800 --press-only \\
        --profile "K5 PLA on P1S" --size 3.2 --step 0.05
    python3 generators/print_coupon.py --port 8800 --record \\
        --profile "K5 PLA on P1S" --press-side 0.025

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
- two rows of rings captured on posts, every ring standing on the bed in a
  pocket through the plate, round a post of the plate (Ø half the size)
  that a rail over the pocket ties to the plate. A 45-degree head on the
  post flares out over the ring's inner edge -- the one face over another
  part, as a print-in-place design's should be. The AIR row puts N whole
  layers of vertical air under the head there (1-4 by default), the ring
  clear of its post by twice the gap a side so only the air varies: the
  vertical air a face needs over another part to come away from it. The
  SMALL-GAP row has the head at the air row's largest air and steps the
  bore's gap a side round the table's (one --step under, three over): the
  sideways gap at the post's size on the first layers, which a Ø`size`
  disc does not measure.

Print it with the settings the parts will get, then push a real Ø`size` pin
(a drill shank, a dowel) into the bores and the pins into a reamed Ø`size`
hole, and screw a real bolt and nut onto the threads. Push each loose disc out
of its hole: the tightest that comes free by hand is the gap print-in-place
parts want. Turn each ring on its post (push its top round with a
fingertip): in the air row the fewest layers that turn cleanly are the air,
in the small-gap row the tightest gap that turns is the post's gap. The bore
and the pin that fit the way you want are the model sizes the table should
give.

`--profile NAME` builds the coupon against one of your print profiles
(docs/tolerances/README.md#print-profiles) and exports it at the profile's
chord deviation. `--record` keeps the answer: it reads the winners off the
command line (`--gap-side`, `--bore`, `--pin`, `--thread-went`,
`--z-gap-layers`, `--post-gap-side`), turns them into rows on top of the
profile (or, for a new one, the built-in table of `--process`/`--material`)
-- the air into the profile's `z_gap_mm` and `layer_height_mm`, the post's
gap into a `gap` row below Ø`size` -- and saves it with
`save_print_profile`, each row's `source` naming the coupon. `--seed`
installs the two profiles measured on a Bambu Lab P1S (K5 PLA at 0.1 mm a
side, a plain PLA at 0.15, both at the preset's 0.2 mm layers, on the
P1S's 256 x 256 x 256 mm bed) and a TPU 95A one on the same printer from
the built-in TPU table, skipping any name the store already has except to
give one that states no bed the P1S's.

`--press-only` prints the PRESS row (docs/tolerances/README.md#press-allowance):
pins of Ø`size` on shoulders on a plate, and a loose cap for each on the bed
in front of it, its bore stepped `--press-step` (0.1 mm at least) on the
diameter either side of the table's `press` allowance, TWO caps at every
step: a step larger than a print's scatter between caps, and the scatter
shown by the pair. The pins are numbered on the plate, 1 the tightest, and
each cap carries as many notches in its rim. Press each cap down its pin onto the
shoulder: the smallest interference that holds by hand and does not split
is the press, which `--record --press-side MM` keeps (the interference a
side, as modelled) as the profile's `press` row.

`--gap-only` prints the disc row alone and `--z-gap-only` the air and
small-gap rows, for a profile whose other rows are already measured. The
airs are whole layers of the layer the parts are sliced at: the profile's
`layer_height_mm`, else `--layer`; with neither the coupon refuses rather
than guess one.

Every size derives from `--size`: the pitch between features, the plate's
thickness (a bore as deep as it is wide; a quarter of the size under the
ring rows), the rings, posts and rail, the label height and the engraving
depth. The numbers come from the app (`print_allowance`), never from here, so
the coupon always tests the table the build carries.

Output (generated, untracked): scratch/print_coupon/<name>.stl, the project,
and <name>.json listing every feature, its modelled diameter and its label.
"""
import argparse
import datetime
import json
import math
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lobole.sdk import Plane  # noqa: E402
from _lib import (PRINT_ITEMS, check, check_print, circle,  # noqa: E402
                  connect, export_print_job, fresh_project, job_receipt,
                  line, print_receipt)

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
# The P1S's build volume, mm (x, y, z): Bambu Lab's published 256 x 256 x
# 256, but 250 high in Bambu Studio's P1S preset (`printable_height`), the
# slicer's limit and so the one a plate must fit. The printer's, not the
# filament's, so both profiles carry it.
P1S_BED_MM = [256.0, 256.0, 250.0]
# Its keep-out zone, from Bambu Studio's P1S machine preset
# (`bed_exclude_area` 0x0, 18x0, 18x28, 0x28): the front-left corner its
# nozzle wiper sweeps, which the slicer refuses a part on.
P1S_BED_EXCLUDE_MM = [[0.0, 0.0, 18.0, 28.0]]
# The wall line width of Bambu Studio's "0.20mm Standard" process on a
# 0.4 mm nozzle (`inner_wall_line_width`): what a printed part's mass takes
# a wall loop as.
P1S_LINE_WIDTH_MM = 0.45
# What seeding gives a profile of that name that lacks it: the printer's
# own numbers, never a filament's measurement.
P1S_PRINTER = {"bed_mm": P1S_BED_MM, "bed_exclude_mm": P1S_BED_EXCLUDE_MM,
               "line_width_mm": P1S_LINE_WIDTH_MM}


# The P1S's TPU profile: the built-in TPU 95A table, every row to confirm
# with a coupon, on the same printer (docs/tolerances/README.md#tpu). The
# bed is the printer's, not the filament's: a TPU plate fits the P1S as a
# PLA one does. Not measured: its rows keep the built-in sources.
TPU_SEED = {"name": "TPU 95A on P1S", "layer": 0.2,
            "conditions": "Bambu Lab P1S, 0.4 mm nozzle, Bambu Studio 0.20 "
                          "mm, Bambu Lab TPU 95A HF, X-Y hole and contour "
                          "compensation 0; the built-in TPU table, not yet "
                          "measured on this printer"}


def seed(d):
    """Install the P1S profiles -- the two PLA ones measured, and the TPU
    one from the built-in table -- leaving any name already stored, but
    giving one that lacks the P1S's bed, keep-out zone or line width
    those, the printer's numbers it is missing."""
    have = {p["name"]: p for p in d.call("list_print_profiles")["profiles"]}
    if TPU_SEED["name"] not in have:
        p = d.call("save_print_profile", profile={
            "name": TPU_SEED["name"], "process": "fdm", "material": "tpu",
            "conditions": TPU_SEED["conditions"],
            "layer_height_mm": TPU_SEED["layer"], **P1S_PRINTER,
            "rows": builtin_rows(d, "fdm", "tpu")})
        print(f"print_coupon: saved '{p['name']}' (the built-in TPU table, "
              "to confirm with a coupon)")
    for s in SEED:
        if s["name"] in have:
            mine = have[s["name"]]
            missing = {k: v for k, v in P1S_PRINTER.items()
                       if not mine.get(k)}
            if not missing:
                print(f"print_coupon: '{s['name']}' is already one of your "
                      f"profiles; left as it is")
                continue
            keep = {k: mine[k] for k in (
                "name", "process", "material", "conditions", "rows",
                "layer_height_mm", "z_gap_mm", "bed_mm", "bed_exclude_mm",
                "line_width_mm") if k in mine}
            if mine.get("stl_deflection_stated"):
                keep["stl_deflection_mm"] = mine["stl_deflection_mm"]
            keep.update(missing)
            d.call("save_print_profile", profile=keep)
            print(f"print_coupon: '{s['name']}' is already one of your "
                  f"profiles; gave it the P1S's "
                  f"{', '.join(sorted(missing))} and left the rest as it is")
            continue
        rows = [r for r in builtin_rows(d, "fdm", "pla")
                if r["kind"] != "gap"]
        rows.append({"kind": "gap", "from_mm": 0,
                     "allowance_mm": round(2.0 * s["gap_side"], 4),
                     "source": s["source"]})
        p = d.call("save_print_profile", profile={
            "name": s["name"], "process": "fdm", "material": "pla",
            "conditions": P1S_CONDITIONS, "stl_deflection_mm": 0.01,
            "layer_height_mm": s["layer"], **P1S_PRINTER,
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
    if args.post_gap_side is not None:
        # The small-gap row measured the gap at the post's size, Ø half the
        # coupon's; the disc row at the coupon's size. So the post's answer
        # is a gap row of its own below the coupon's size, and the
        # profile's gap carries on from there.
        if args.post_gap_side <= 0.0:
            raise SystemExit("print_coupon --record: --post-gap-side is the "
                             "gap a side, more than 0 mm")
        post = 0.5 * args.size
        gaps = sorted((r for r in rows if r["kind"] == "gap"),
                      key=lambda r: r["from_mm"])
        above = []
        for r in gaps:
            if r.get("to_mm") is not None and r["to_mm"] <= args.size:
                continue
            if r["from_mm"] < args.size:
                r = dict(r, from_mm=args.size)
            above.append(r)
        if not above:
            raise SystemExit("print_coupon --record: the profile has no gap "
                             f"row from Ø{fmt(args.size)} up to keep above "
                             "the post's -- record the disc row's "
                             "(--gap-side) too")
        rows = [r for r in rows if r["kind"] != "gap"]
        rows.append({"kind": "gap", "from_mm": 0, "to_mm": args.size,
                     "allowance_mm": round(2.0 * args.post_gap_side, 4),
                     "source": f"{tag}: the tightest small-gap ring that "
                               f"turned on its Ø{fmt(post)} post was "
                               f"{fmt(args.post_gap_side)} a side"})
        rows += above
    if args.press_side is not None:
        # The interference a side as modelled; the row is the cap's bore
        # less its pin on the diameter, so its negative, doubled.
        replace("press", round(-2.0 * args.press_side, 4),
                f"the smallest cap that held by hand and did not split was "
                f"{fmt(args.press_side)} a side of interference")
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
                         "--gap-side, --bore, --pin, --thread-went, "
                         "--z-gap-layers, --post-gap-side or --press-side")
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
    for k in ("bed_mm", "bed_exclude_mm", "line_width_mm"):
        if prof.get(k):
            out[k] = prof[k]  # the printer's, kept as it was
    saved = d.call("save_print_profile", profile=out)
    z = (f", Z gap {saved['z_gap_mm']:g} mm" if saved.get("z_gap_mm")
         else "")
    print(f"print_coupon: recorded {len(rows)} rows in '{saved['name']}' "
          f"(STL at {saved['stl_deflection_mm']:g} mm{z})")
    return 0


def z_row(d, args, gap):
    """The air row's layer height and its airs in whole layers, and the
    small-gap row's gaps a side.

    The layer is the profile's `layer_height_mm`, else `--layer`; with
    neither, refused -- an air in the wrong layers measures nothing. The
    airs are `--z-layers`, else 1-4 layers, or four round the profile's
    measured air (`z_gap_mm`). The small-gap row steps the bore's gap a
    side round the profile's, one `--step` below it and three above."""
    prof = None
    if args.profile:
        prof = next((p for p in d.call("list_print_profiles")["profiles"]
                     if p["name"] == args.profile), None)
    layer, source = args.layer, "--layer"
    if layer is None and prof and prof.get("layer_height_mm"):
        layer, source = prof["layer_height_mm"], "the profile"
    if layer is None or layer <= 0:
        raise SystemExit("print_coupon: the air row is whole layers of the "
                         "layer you slice at -- pass --layer (Bambu "
                         "Studio's 0.20 mm preset is 0.2) or state "
                         "layer_height_mm in the profile")
    if args.z_layers:
        try:
            layers = sorted({int(n) for n in args.z_layers.split(",")})
        except ValueError:
            raise SystemExit(f"print_coupon: --z-layers is whole layers, "
                             f"e.g. 1,2,3,4, not {args.z_layers!r}")
        if layers[0] < 1:
            raise SystemExit("print_coupon: --z-layers starts at 1 layer")
    elif prof and prof.get("z_gap_mm"):
        k = round(prof["z_gap_mm"] / layer)
        first = max(1, k - 1)
        layers = list(range(first, first + 4))
    else:
        layers = [1, 2, 3, 4]
    g = gap["allowance_mm"] / 2.0
    sides = [round(g + k * args.step, 4) for k in (-1, 0, 1, 2, 3)]
    if sides[0] <= 0.0:
        raise SystemExit(
            f"print_coupon: a step of {fmt(args.step)} below the gap of "
            f"{fmt(g)} a side closes it -- a smaller --step")
    return {"layer_mm": layer, "layer_from": source, "layers": layers,
            "gap_side_mm": g, "small_gaps_mm": sides}


def ring_sizes(size):
    """The ring and post every Z-row feature shares, from the size: a post
    a quarter of the size in radius (Ø half the size), a ring 1.6 sizes across and a quarter of one thick, standing on
    the bed in a pocket 0.15 sizes clear of it all round, a 45-degree head
    overlapping the ring's hole by 0.15 sizes, and the rail that ties the
    posts to the plate a quarter of a size thick and a little wider than a
    post (0.6 sizes), so its sides never touch a post's."""
    return {"r_post": 0.25 * size, "r_ring": 0.8 * size,
            "thick": 0.25 * size, "overlap": 0.15 * size,
            "r_pocket": 0.95 * size, "rail": 0.25 * size,
            "r_rail": 0.3 * size}


def z_features(zrow, size, xs_air, xs_small, y_air, y_small):
    """Each ring on the bed round its post, in millimetres from the size.

    The air row: the ring clears its post by twice the gap a side, so only
    the air varies, and the head's 45-degree underside comes down to N
    layers over the ring's inner edge. The small-gap row: the head at the
    air row's largest air, and the bore's gap a side stepped round the
    profile's."""
    z = ring_sizes(size)
    L = zrow["layer_mm"]
    t = z["thick"]
    rings = []
    for x, n in zip(xs_air, zrow["layers"]):
        air = round(n * L, 6)
        rings.append({"row": "air", "x": x, "y": y_air, "layers": n,
                      "air": air, "side": 2.0 * zrow["gap_side_mm"],
                      "label": fmt(air)})
    top_air = round(max(zrow["layers"]) * L, 6)
    for x, side in zip(xs_small, zrow["small_gaps_mm"]):
        rings.append({"row": "small_gap", "x": x, "y": y_small,
                      "layers": max(zrow["layers"]), "air": top_air,
                      "side": side, "label": fmt(side)})
    for r in rings:
        r_hole = z["r_post"] + r["side"]
        r_head = r_hole + z["overlap"]
        # The head's underside is 45 degrees: `air` over the ring's inner
        # edge, more further out.
        cone = t + r["air"] - r["side"]
        r.update({"thick": t, "r_post": z["r_post"], "r_hole": r_hole,
                  "r_ring": z["r_ring"], "r_head": r_head, "cone": cone,
                  "head_top": cone + (r_head - z["r_post"]),
                  "label_at": (r["x"], r["y"] - z["r_pocket"] - 0.05 * size)})
    rails = {}
    for r in rings:
        rails[r["row"]] = max(rails.get(r["row"], 0.0), r["head_top"])
    for r in rings:
        r["rail"] = rails[r["row"]]
        r["report"] = {"kind": "air" if r["row"] == "air" else "small_gap",
                       "layers": r["layers"], "air_mm": r["air"],
                       "gap_per_side_mm": r["side"],
                       "ring_mm": 2.0 * r["r_ring"],
                       "post_mm": 2.0 * r["r_post"],
                       "hole_mm": 2.0 * r["r_hole"],
                       "head_mm": 2.0 * r["r_head"], "thick_mm": t,
                       "at": [r["x"], r["y"]]}
    return rings, rails


def build_post(d, r, rail_top):
    """The post and its 45-degree head, revolved about the ring's axis from
    the bed up into the rail, joined to the plate."""
    cone, rp, rh = r["cone"], r["r_post"], r["r_head"]
    ht = r["head_top"]
    pts = [(0.0, 0.0), (rp, 0.0), (rp, cone), (rh, ht)]
    if rail_top - ht > 1e-9:
        pts += [(rp, ht)]
    pts += [(rp, rail_top), (0.0, rail_top)]
    ents = [line(pts[i], pts[(i + 1) % len(pts)]) for i in range(len(pts))]
    name = f"Post {r['row']} {r['label']}"
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
    """The ring: its own body, standing on the bed in its pocket."""
    name = f"Ring {r['row']} {r['label']}"
    sk = check(d.call("add_feature", feature={
        "type": "Sketch", "name": f"{name} sketch",
        "plane": Plane.xy(0.0).json(),
        "entities": [circle((r["x"], r["y"]), r["r_ring"]),
                     circle((r["x"], r["y"]), r["r_hole"])]}),
        f"{name} sketch")
    check(d.call("add_feature", feature={
        "type": "Extrude", "name": name, "mode": "new_body",
        "sketch_id": sk["id"], "distance": r["thick"]}), name)


def rect(x0, y0, x1, y1):
    return [line((x0, y0), (x1, y0)), line((x1, y0), (x1, y1)),
            line((x1, y1), (x0, y1)), line((x0, y1), (x0, y0))]


def build_z_rows(d, rings, rails, size, width, top):
    """The plate's side of the Z rows: a rail along each row, a post's width
    across, standing on the plate between the pockets and bridging each
    pocket at the height of its row's highest head, so every post is part
    of the plate while its ring stands on the bed. The pockets go through
    the plate; the rail's underside over a ring is a bridge between the
    plate's own pillars and the post, its air the head's height over the
    ring."""
    z = ring_sizes(size)
    rows = sorted({r["row"] for r in rings})
    ys = {r["row"]: r["y"] for r in rings}
    rail_top = {row: rails[row] + z["rail"] for row in rows}
    for row in rows:
        y = ys[row]
        check(d.call("add_feature", feature={
            "type": "Sketch", "name": f"Rail {row}",
            "plane": Plane.xy(0.0).json(),
            "entities": rect(0.0, y - z["r_rail"], width,
                             y + z["r_rail"])}), f"rail {row} sketch")
        check(d.call("add_feature", feature={
            "type": "Extrude", "name": f"Rail {row} up", "mode": "add",
            "sketch_id": feature_id(d, f"Rail {row}"),
            "distance": rail_top[row]}), f"rail {row}")
    high = max([top] + list(rail_top.values())) + 1.0
    check(d.call("add_feature", feature={
        "type": "Sketch", "name": "Pockets", "plane": Plane.xy(high).json(),
        "entities": [circle((r["x"], r["y"]), z["r_pocket"])
                     for r in rings]}), "pocket sketch")
    check(d.call("add_feature", feature={
        "type": "Extrude", "name": "Pockets down", "mode": "cut",
        "sketch_id": feature_id(d, "Pockets"), "distance": high,
        "end_mode": "through_all", "reversed": True}), "pockets")
    for row in rows:
        y = ys[row]
        check(d.call("add_feature", feature={
            "type": "Sketch", "name": f"Bridge {row}",
            "plane": Plane.xy(rails[row]).json(),
            "entities": rect(0.0, y - z["r_rail"], width,
                             y + z["r_rail"])}), f"bridge {row} sketch")
        check(d.call("add_feature", feature={
            "type": "Extrude", "name": f"Bridge {row} over", "mode": "add",
            "sketch_id": feature_id(d, f"Bridge {row}"),
            "distance": z["rail"]}), f"bridge {row}")
    for r in rings:
        build_post(d, r, rail_top[r["row"]])


def press_sizes(size, cap_d=None, cap_t=None):
    """The press row's pin and cap from the pin's diameter (`size`): a
    shoulder Ø1.4 sizes and half a size tall on the plate, the pin standing
    on it as long as the cap is thick, the cap Ø2.6 sizes and 0.9 of one
    thick -- or the cap a part's own (`cap_d`, `cap_t`), so the row tests
    the cap the part presses on. A position every 3.5 sizes (more where the
    cap is wider), the plate 0.6 of a size thick, the caps on the bed in
    front of it, half a pitch clear."""
    cap_d = cap_d or 2.6 * size
    cap_t = cap_t or 0.9 * size
    pitch = max(3.5 * size, 1.4 * cap_d)
    return {"pin": size, "shoulder_d": 1.4 * size, "shoulder_h": 0.5 * size,
            "cap_d": cap_d, "cap_t": cap_t, "pitch": pitch,
            "plate_t": 0.6 * size, "notch": 0.12 * cap_d,
            "digit": 0.45 * pitch, "engrave": 0.1 * size}


# A press row's step and its repeats. The Strandbeest's first row stepped
# 0.05 mm on a Ø3.22 stub, and its caps came out of order: the tightest as
# modelled went on, the loosest would not. The scatter between two printed
# caps is the printer's, about as large in mm at any size, so a larger pin
# does not shrink it -- and the press a part needs is the one at ITS size,
# so the row stays at the size it is for. What reads is a step past the
# scatter, and two caps at each step, so the scatter itself shows.
PRESS_MIN_STEP = 0.1
PRESS_PER_STEP = 2


def press_row(d, proc, size, steps, step, name, cap_d=None, cap_t=None,
              one_row=False):
    """A part of its own: 2 * steps + 1 steps, each a PAIR of pins on a
    plate and a cap for each, loose on the bed in front of it, the caps'
    bores stepped `step` (PRESS_MIN_STEP at least) on the diameter either
    side of the table's press allowance (print_allowance kind `press`).
    Pin and cap are numbered 1.. from the left, 1 the tightest: the digit
    engraved on the plate behind the pin, as many notches in the cap's rim.
    Push each cap down its pin onto the shoulder: the step whose two caps
    both hold by hand and do not split is the press (`--record
    --press-side`, the interference a side of that step). Returns the row's
    report."""
    if step < PRESS_MIN_STEP - 1e-9:
        raise SystemExit(f"print_coupon: a press step of {fmt(step)} mm is "
                         f"under a print's scatter between caps; "
                         f"{fmt(PRESS_MIN_STEP)} mm at least")
    press = d.call("print_allowance", kind="press", size_mm=size, **proc)
    z = press_sizes(size, cap_d, cap_t)
    n = (2 * steps + 1) * PRESS_PER_STEP
    pitch = z["pitch"]
    # A column a step, its two pins one behind the other: the first's cap
    # on the bed in front of the plate, the second's behind it. Or, with
    # `one_row`, every pin in one row and every cap in front: the shape a
    # packed plate takes best.
    rows = 1 if one_row else PRESS_PER_STEP
    cols = n // rows
    width, depth = cols * pitch, rows * pitch
    d.call("add_part", name=name)
    d.call("set_print_process", **proc)
    check(d.call("add_feature", feature={
        "type": "Box", "name": "Plate", "on": "Top", "centered": False,
        "offset": [0, 0], "width": width, "depth": depth,
        "height": z["plate_t"]}), "press plate")
    at = [(pitch * (i // rows + 0.5), pitch * (i % rows + 0.4))
          for i in range(n)]
    cap_ys = [-0.5 * pitch - z["cap_d"] / 2.0,
              depth + 0.5 * pitch + z["cap_d"] / 2.0][:rows]
    top = Plane.xy(z["plate_t"]).json()
    check(d.call("add_feature", feature={
        "type": "Sketch", "name": "Shoulders", "plane": top,
        "entities": [circle(p, z["shoulder_d"] / 2.0) for p in at]}),
        "shoulder sketch")
    check(d.call("add_feature", feature={
        "type": "Extrude", "name": "Shoulders up", "mode": "add",
        "sketch_id": feature_id(d, "Shoulders"),
        "distance": z["shoulder_h"]}), "shoulders")
    check(d.call("add_feature", feature={
        "type": "Sketch", "name": "Press pins",
        "plane": Plane.xy(z["plate_t"] + z["shoulder_h"]).json(),
        "entities": [circle(p, size / 2.0) for p in at]}),
        "press pin sketch")
    check(d.call("add_feature", feature={
        "type": "Extrude", "name": "Press pins up", "mode": "add",
        "sketch_id": feature_id(d, "Press pins"),
        "distance": z["cap_t"]}), "press pins")
    check(d.call("add_feature", feature={
        "type": "Sketch", "name": "Press digits", "plane": top,
        "entities": [text(str(i + 1), (x, y + 0.55 * pitch), z["digit"])
                     for i, (x, y) in enumerate(at)]}),
        "digit sketch")
    check(d.call("add_feature", feature={
        "type": "Extrude", "name": "Engrave press digits", "mode": "cut",
        "sketch_id": feature_id(d, "Press digits"),
        "distance": z["engrave"], "reversed": True}), "engrave digits")
    caps = []
    report = []
    for i, (x, y_pin) in enumerate(at):
        y_cap = cap_ys[i % rows]
        k = i // PRESS_PER_STEP - steps
        allow = round(press["allowance_mm"] + k * step, 4)
        bore = round(size + allow, 4)
        if bore <= 0.0:
            raise SystemExit(f"print_coupon: a press step of {fmt(step)} "
                             f"closes the Ø{fmt(size)} cap's bore -- fewer "
                             f"--steps or a smaller --step")
        r = z["cap_d"] / 2.0
        ents = [circle((x, y_cap), r), circle((x, y_cap), bore / 2.0)]
        # i + 1 notches in the rim, every 30 degrees from the back: which
        # pin it goes on, once it is off the bed.
        nw = z["notch"]
        for j in range(i + 1):
            a = math.radians(90.0 - 30.0 * j)
            cx, cy = x + r * math.cos(a), y_cap + r * math.sin(a)
            ents.append(circle((cx, cy), nw / 2.0))
        caps.append(ents)
        report.append({"index": i + 1, "step": k, "pin_mm": size,
                       "bore_mm": bore, "allowance_mm": allow,
                       "interference_side_mm": round(-allow / 2.0, 4) + 0.0,
                       "offset_mm": round(k * step, 4), "table": k == 0,
                       "at": [x, y_pin], "cap_at": [x, y_cap]})
    for i, ents in enumerate(caps):
        cap = f"Cap {i + 1}"
        check(d.call("add_feature", feature={
            "type": "Sketch", "name": f"{cap} sketch",
            "plane": Plane.xy(0).json(), "entities": ents[:2]}),
            f"{cap} sketch")
        check(d.call("add_feature", feature={
            "type": "Extrude", "name": cap, "mode": "new_body",
            "sketch_id": feature_id(d, f"{cap} sketch"),
            "distance": z["cap_t"]}), cap)
        check(d.call("add_feature", feature={
            "type": "Sketch", "name": f"{cap} notches",
            "plane": Plane.xy(z["cap_t"]).json(), "entities": ents[2:]}),
            f"{cap} notches")
        check(d.call("add_feature", feature={
            "type": "Extrude", "name": f"{cap} notched", "mode": "cut",
            "sketch_id": feature_id(d, f"{cap} notches"),
            "distance": z["cap_t"], "reversed": True}), f"{cap} notched")
    return {"kind": "press", "sizes": z, "allowance_mm": press["allowance_mm"],
            "from": press.get("from"), "source": press["source"],
            "note": press.get("note"), "step_mm": step, "caps": report,
            "per_step": PRESS_PER_STEP, "width_mm": width, "depth_mm": depth,
            "cap_y_mm": cap_ys, "part": name}


# The coupon's own part stands at the origin, in the bed's keep-out corner
# on a P1S: its check leaves bed_fit to the plate the job prints it on.
COUPON_ITEMS = tuple(k for k in PRINT_ITEMS if k != "bed_fit")


def coupon_job(d, args, proc, name, out):
    """The coupon's print job: a plate part with a Derived copy of it in
    the middle of the profile's bed (clear of its keep-out zones), every
    check_print item on it, then the job. Only with a profile that states
    a bed and a layer: the built-in table is no printer, and a job slices
    every object at the profile's layer."""
    prof = (next((p for p in d.call("list_print_profiles")["profiles"]
                  if p["name"] == args.profile), None)
            if args.profile else None)
    if not prof or not prof.get("bed_mm") or not prof.get("layer_height_mm"):
        return {"skipped": "no print profile with a bed and a layer"}
    box = d.call("mass_properties", part=name)["bbox_mm"]
    bed = prof["bed_mm"]
    at = [bed[k] / 2.0 - (box["min"][k] + box["max"][k]) / 2.0
          for k in range(2)] + [0.0]
    ids = {p["name"]: p["id"] for p in d.call("list_parts")["parts"]}
    plate = f"{name} plate"
    d.call("add_part", name=plate)
    d.call("set_print_process", **proc)
    check(d.call("add_feature", feature={
        "type": "Derived", "name": name, "source_part": ids[name],
        "reference": False,
        "placement": {"position": at, "orientation": [0, 0, 0, 1]}}),
        "the coupon on its plate")
    check_print(d, f"{name} on the bed", need=("bed", "shells", "bed_fit"),
                quiet=True)
    r = job_receipt(export_print_job(d, [(plate, name)],
                                     out / f"{name}_job.3mf",
                                     prof["material"]))
    d.call("set_active_part", part=name)
    return r


def press_coupon(d, args, proc):
    """--press-only: the press row on its own plate, checked, exported."""
    name = args.name or ("coupon_" + (
        args.profile.lower().replace(" ", "_") if args.profile else
        f"{args.process}_{args.material}") + f"_{fmt(args.size)}_press")
    row = press_row(d, proc, args.size, args.steps, args.press_step, name)
    gap = d.call("print_allowance", kind="gap", size_mm=args.size, **proc)
    # Every piece on the bed, nothing closer than the caps' half pitch to
    # the plate, the notches and digits clean: every item.
    params = {"min_gap_mm": gap["allowance_mm"] / 2.0}
    if args.deflection is not None:
        params["stl_deflection_mm"] = args.deflection
    pr = check_print(d, "press coupon", quiet=True, need=COUPON_ITEMS,
                     **params)
    if pr["shells"]["expected"] != 1 + len(row["caps"]):
        raise SystemExit(f"print_coupon: {pr['shells']['expected']} solids, "
                         f"not the plate and {len(row['caps'])} caps")
    row["print"] = print_receipt(pr)
    out = Path(args.out) if args.out else OUT
    out.mkdir(parents=True, exist_ok=True)
    stl = out / f"{name}.stl"
    exp = {"path": str(stl)}
    if args.deflection is not None:
        exp["stl_deflection_mm"] = args.deflection
    r = d.call("export_stl", **exp)
    row["stl_deflection_mm"] = r.get("deflection_mm")
    row["job"] = coupon_job(d, args, proc, name, out)
    d.call("save_project", path=str(out / f"{name}.dsgn"))
    (out / f"{name}.json").write_text(json.dumps(row, indent=1))
    print(f"print_coupon: press row of {len(row['caps'])} -> {stl}")
    for c in row["caps"]:
        print(f"  {c['index']}: bore Ø{fmt(c['bore_mm'])} on a "
              f"Ø{fmt(c['pin_mm'])} pin, {c['interference_side_mm']:+g} a "
              f"side" + ("  (the table's)" if c["table"] else ""))
    print(f"  {PRESS_PER_STEP} caps a step of {fmt(row['step_mm'])}: press "
          "each onto its pin; --record --press-side MM the smallest step "
          "whose caps both hold by hand and do not split")
    if row.get("note"):
        print(f"  {row['note']}")
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
    ap.add_argument("--z-gap-layers", type=int, default=None,
                    help="--record: the fewest layers of air whose ring "
                         "turned cleanly (the air row)")
    ap.add_argument("--press-step", type=float, default=PRESS_MIN_STEP,
                    help="mm on the diameter between the press row's steps "
                         f"(two caps a step; {PRESS_MIN_STEP:g} at least)")
    ap.add_argument("--press-side", type=float, default=None,
                    help="--record: the smallest interference a side, as "
                         "modelled, whose cap held by hand and did not "
                         "split (the press row), mm")
    ap.add_argument("--press-only", action="store_true",
                    help="the press row alone: pins of Ø--size on a plate "
                         "and a cap for each, stepped in interference")
    ap.add_argument("--post-gap-side", type=float, default=None,
                    help="--record: the tightest gap a side whose ring "
                         "turned cleanly on its post (the small-gap row), "
                         "mm")
    ap.add_argument("--layer", type=float, default=None,
                    help="the layer height the coupon is sliced at, mm "
                         "(default: the profile's layer_height_mm)")
    ap.add_argument("--z-layers", default=None,
                    help="the air row's airs, in whole layers, e.g. 1,2,3,4 "
                         "(default: 1-4, or four round the profile's "
                         "measured z_gap_mm)")
    ap.add_argument("--z-gap-only", action="store_true",
                    help="only the two rows of rings on posts (the air and "
                         "the small gap), on a thin plate")
    ap.add_argument("--conditions", default=None,
                    help="--record: printer, nozzle, slicer preset")
    ap.add_argument("--seed", action="store_true",
                    help="install the two profiles measured on a P1S and its TPU 95A one")
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
    ap.add_argument("--out", default=None,
                    help="output directory (default: scratch/print_coupon)")
    ap.add_argument("--fresh", action="store_true",
                    help="start a new project first (an untitled scratch one is "
                         "discarded; a saved one with unsaved work refused)")
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
    if args.press_only:
        if args.gap_only or args.z_gap_only:
            raise SystemExit("print_coupon: --press-only, --gap-only or "
                             "--z-gap-only, one of them")
        return press_coupon(d, args, proc)

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

    # The Z rows: whole layers of the layer the parts are sliced at.
    zrow = None if args.gap_only else z_row(d, args, gap)

    # Everything from the size: a feature every 2.5 diameters, a plate a
    # diameter thick (a quarter of one under the Z rows, where the rings
    # stand on the bed in pockets through it), labels under each feature.
    pitch = 2.5 * size
    count = 2 * args.steps + 1
    label_h = min(0.3 * size, 4.0)
    engrave = 0.05 * size
    zthick = 0.25 * size
    thick = zthick if args.z_gap_only else size
    # Top to bottom: bores, pins, threads, gaps, air, small gap -- the ones
    # built.
    zkinds = ["air", "small_gap"]
    kinds = (["gap"] if args.gap_only else zkinds if args.z_gap_only else
             ["bore", "pin"] + (["thread"] if nut else []) + ["gap"]
             + zkinds)
    row_of = {k: i for i, k in enumerate(kinds)}
    rows = len(kinds)
    cols = max([0 if args.z_gap_only else count]
               + ([len(zrow["layers"]), len(zrow["small_gaps_mm"])]
                  if zrow else []))
    width = cols * pitch
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
    if zrow and thick > zthick:
        # The Z rows' end of the plate, down to a quarter of the size, so a
        # finger reaches the rings.
        z_edge = depth - pitch * row_of["air"]
        check(d.call("add_feature", feature={
            "type": "Sketch", "name": "Z step", "plane": Plane.xy(thick).json(),
            "entities": rect(0.0, 0.0, width, z_edge)}), "z step sketch")
        check(d.call("add_feature", feature={
            "type": "Extrude", "name": "Z step down", "mode": "cut",
            "sketch_id": feature_id(d, "Z step"),
            "distance": thick - zthick, "reversed": True}), "z step")

    def x_at(i):
        return pitch * (i + 0.5)

    def centred(n):
        first = (cols - n) // 2
        return [x_at(first + i) for i in range(n)]

    def y_at(row):
        return depth - pitch * (row + 0.45)

    report = {"process": proc, "size": size, "step": args.step,
              "gap_center_from": ("table" if args.gap_center is None
                                  else "--gap-center"),
              "confirm": hole["confirm"], "features": []}
    bores, pins, discs, labels, disc_gaps = [], [], [], [], []
    for i in range(0 if args.z_gap_only else count):
        k = i - args.steps
        x = centred(count)[i]
        for row, what, base in (() if args.gap_only else
                                ((row_of["bore"], "bore", hole),
                                 (row_of["pin"], "pin", shaft))):
            dia = round(base["model_mm"] + k * args.step, 4)
            (bores if what == "bore" else pins).append(
                circle((x, y_at(row)), dia / 2.0))
            labels.append(text(fmt(dia), (x, y_at(row) - 0.62 * size),
                               label_h))
            report["features"].append(
                {"kind": what, "modelled_mm": dia, "offset_mm": k * args.step,
                 "table": k == 0, "at": [x, y_at(row)]})
        # A gap is the whole diametral clearance, so its steps are twice the
        # bore's: the same step on each side of the disc. The label is the
        # gap a side, which is how print-in-place clearance is quoted.
        g = round(gap_mid + k * gap_step, 4)
        disc_gaps.append(g / 2.0)
        at = (x, y_at(row_of["gap"]))
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
        xi, xe, yt = centred(count)[0], centred(count)[-1], \
            y_at(row_of["thread"])
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

    rings, rails, zlabels = [], {}, []
    if zrow:
        # Centred under the rows above.
        rings, rails = z_features(
            zrow, size, centred(len(zrow["layers"])),
            centred(len(zrow["small_gaps_mm"])), y_at(row_of["air"]),
            y_at(row_of["small_gap"]))
        for r in rings:
            zlabels.append(text(r["label"], r["label_at"], label_h))
            report["features"].append(r["report"])
        report["z_rows"] = {k: zrow[k] for k in (
            "layer_mm", "layer_from", "layers", "gap_side_mm",
            "small_gaps_mm")}

    top = Plane.xy(thick).json()
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
    for sk_name, plane, ents in (("Labels", top, labels),
                                 ("Z labels", Plane.xy(zthick).json(),
                                  zlabels)):
        if not ents:
            continue
        check(d.call("add_feature", feature={
            "type": "Sketch", "name": sk_name, "plane": plane,
            "entities": ents}), f"{sk_name} sketch")
        check(d.call("add_feature", feature={
            "type": "Extrude", "name": f"Engrave {sk_name}", "mode": "cut",
            "sketch_id": feature_id(d, sk_name), "distance": engrave,
            "reversed": True}), f"engrave {sk_name}")
    # The rails, the pockets and the posts with their heads, part of the
    # plate.
    if rings:
        build_z_rows(d, rings, rails, size, width, thick)
    # The loose discs: their own bodies, standing on the bed in their holes.
    if discs:
        check(d.call("add_feature", feature={
            "type": "Sketch", "name": "Discs", "plane": Plane.xy(0).json(),
            "entities": discs}), "disc sketch")
        check(d.call("add_feature", feature={
            "type": "Extrude", "name": "Discs up", "mode": "new_body",
            "sketch_id": feature_id(d, "Discs"), "distance": thick}),
            "discs")
    # The rings: their own bodies, each on the bed round its post.
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

    # Will it print: every item, with the clearance and the air down to the
    # least the coupon holds on purpose -- the discs and the small-gap rings
    # stand at gaps under the table's, the air row's rings under the
    # stacking limit, which is what they measure (and a head's 45-degree
    # underside comes within its air / sqrt 2 of its ring's inner edge). So
    # it checks that nothing is closer than the coupon means it to be, that
    # every piece (the plate, each disc, each ring) starts on the bed, and
    # that every face over another piece is a 45-degree head or a bridge.
    # The threads' 60-degree flanks overhang past 45 by design, as every
    # printed metric thread's do, so with a thread the overhang item is
    # reported, not required.
    least_gap = min(disc_gaps
                    + [min(r["side"], r["air"] / math.sqrt(2.0))
                       for r in rings])
    params = {"min_gap_mm": least_gap}
    if rings:
        params["min_air_mm"] = min(r["air"] for r in rings)
    if args.deflection is not None:
        params["stl_deflection_mm"] = args.deflection
    need = tuple(k for k in COUPON_ITEMS if not (nut and k == "overhang"))
    pr = check_print(d, "print_coupon", quiet=True, need=need, **params)
    if pr["shells"]["expected"] != 1 + len(discs) + len(rings):
        raise SystemExit(f"print_coupon: {pr['shells']['expected']} solids, "
                         f"not the plate, {len(discs)} loose discs and "
                         f"{len(rings)} rings")
    report["print"] = print_receipt(pr)
    out = Path(args.out) if args.out else OUT
    out.mkdir(parents=True, exist_ok=True)
    stl = out / f"{name}.stl"
    exp = {"path": str(stl)}
    if args.deflection is not None:
        exp["stl_deflection_mm"] = args.deflection
    r = d.call("export_stl", **exp)
    report["stl_deflection_mm"] = r.get("deflection_mm")
    report["job"] = coupon_job(d, args, proc, name, out)
    d.call("save_project", path=str(out / f"{name}.dsgn"))
    (out / f"{name}.json").write_text(json.dumps(report, indent=1))
    print(f"print_coupon: {len(report['features'])} features -> {stl}")
    if zrow:
        air = [r for r in rings if r["row"] == "air"]
        small = [r for r in rings if r["row"] == "small_gap"]
        print(f"  air row: {', '.join(r['label'] for r in air)} mm under "
              f"the heads ({', '.join(str(n) for n in zrow['layers'])} "
              f"layers of {fmt(zrow['layer_mm'])}, from "
              f"{zrow['layer_from']}); turn each ring, and --record "
              f"--z-gap-layers N the fewest that turned cleanly")
        print(f"  small-gap row: {', '.join(r['label'] for r in small)} mm "
              f"a side round a Ø{fmt(2 * small[0]['r_post'])} post; "
              f"--record --post-gap-side MM the tightest that turned")
    print(f"  {hole['confirm']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
