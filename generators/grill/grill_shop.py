# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""What a shop gets: a DXF per folded part, a drawing with the blank beside
the part and a bend table, and the checks that say the numbers are right.

The blank is checked against the bend-allowance arithmetic done here, from
the documented formula and the document's own K — not against what the
unfolder happened to produce. A flat pattern that agrees with the feature
that made it proves nothing.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from grill_lib import sheet_ok  # noqa: E402

import grill_spec as S  # noqa: E402

# Every part that is folded from one blank, and so has a flat pattern.
SHEET_PARTS = ["Firebox", "Hood", "Heat tent", "Grease tray", "Control panel",
               "Cart side left", "Cart side right", "Cart back", "Cart shelf",
               "Cart door left", "Cart door right", "Side shelf",
               "Grate ledge", "Tent ledge", "Rack ledge", "Tray rail"]


def bend_allowance(angle_deg, radius, k, t):
    """BA = angle * (R + K*t), the neutral arc through the bend."""
    return math.radians(angle_deg) * (radius + k * t)


def flat_config_of(d, part):
    for c in d.call("list_configurations")["configurations"]:
        if c["name"] == "Flat":
            return c["id"]
    raise SystemExit(f"{part} has no Flat configuration")


def blanks(d, out_dir):
    """Every sheet part, laid flat, checked, and written out as a DXF."""
    os.makedirs(out_dir, exist_ok=True)
    made = []
    for part in SHEET_PARTS:
        d.call("set_active_part", part=part)
        cfg = flat_config_of(d, part)
        d.call("set_active_configuration", id=cfg)
        body = d.call("get_state")["bodies"][0]
        rules = d.call("get_sheet_metal_rules")["rules"]
        t = rules["thickness_mm"]
        span = [body["bbox_max"][i] - body["bbox_min"][i] for i in range(3)]
        thin = min(span)
        if abs(thin - t) > 1e-4:
            raise SystemExit(f"{part}: the blank is {thin:.4f} thick, not {t} "
                             "— a bend did not lay flat")
        path = os.path.join(out_dir, part.lower().replace(" ", "-") + ".dxf")
        d.call("export_dxf", path=os.path.abspath(path))
        text = open(path).read()
        bends = text.count("LINE\n8\nBEND_UP") + text.count("LINE\n8\nBEND_DOWN")
        made.append((part, sorted(span)[1], sorted(span)[2], bends,
                     os.path.basename(path)))
        d.call("set_active_configuration", id=0)
    return made


def firebox_blank_is_right(d):
    """The firebox blank against the arithmetic, not against the unfolder.

    Across the box: the base between its two setbacks, then per side a bend
    allowance, the wall (itself less a setback, because the rim is folded
    off its end the same way), another allowance and the rim.

    A `material_inside` flange of length L reaches L + R past the face its
    edge is on, and lays flat as L: the panel it folds from loses a SETBACK
    of R + t, and the flange itself contributes its length plus the bend
    allowance. Both numbers are calibrated against the app on a single
    100 mm coupon (a blank, one 20 mm flange): base 97 + BA 3.322 + flange
    20 = 120.322, which is what the flat pattern measures.
    """
    d.call("set_active_part", part="Firebox")
    rules = d.call("get_sheet_metal_rules")["rules"]
    t = rules["thickness_mm"]
    r = rules["bend_radius_mm"]
    k = rules["k_factor"]
    ba = bend_allowance(90.0, r, k, t)
    setback = r + t
    per_side = ba + (S.BOX_H - setback) + ba + S.BOX_RIM
    want_x = S.BOX_X - 2 * setback + 2 * per_side
    want_y = S.BOX_Y - 2 * setback + 2 * per_side

    cfg = flat_config_of(d, "Firebox")
    d.call("set_active_configuration", id=cfg)
    body = d.call("get_state")["bodies"][0]
    got_x = body["bbox_max"][0] - body["bbox_min"][0]
    got_y = body["bbox_max"][1] - body["bbox_min"][1]
    d.call("set_active_configuration", id=0)
    for axis, want, got in (("across", want_x, got_x), ("deep", want_y, got_y)):
        if abs(want - got) > 0.05:
            raise SystemExit(f"firebox blank {axis}: {got:.2f} mm, the bend "
                             f"allowance says {want:.2f} mm")
    return want_x, want_y, ba


def drawing(d, out_dir):
    """A sheet with the firebox folded, its blank, and a bend table."""
    d.call("create_drawing", name="Firebox drawing",
           sheet={"size": "a2", "landscape": True,
                  "title_block": {"project": "Three-burner gas grill",
                                  "document": "Firebox — fold and blank",
                                  "author": "generators/grill"}})
    folded = d.call("add_drawing_view", source="Firebox", orientation="top",
                    scale=0.25, position=[150, 320])["id"]
    flat = d.call("add_drawing_view", source="Firebox", orientation="top",
                  scale=0.25, position=[430, 300], configuration="Flat")["id"]
    d.call("update_drawing")
    views = {v["id"]: v for v in d.call("list_drawing_views")["views"]}
    if views[flat].get("bend_lines", 0) < 4:
        raise SystemExit(f"the blank's view has {views[flat].get('bend_lines')} "
                         "bend lines; the firebox has eight bends")
    table = d.call("add_drawing_note", table="bends", view=flat,
                   position=[430, 120], text="FIREBOX — BEND TABLE")
    pdf = os.path.join(out_dir, "firebox.pdf")
    d.call("export_drawing", path=os.path.abspath(pdf))
    svg = os.path.join(out_dir, "firebox.svg")
    d.call("export_drawing", path=os.path.abspath(svg))
    return views[folded], views[flat], table


def tube_schedule(d, parts=("Lid handle",)):
    """The other half of what a shop gets: the bent tube's own program.

    A sheet part hands over a blank and a bend table; a bent tube hands over
    a CUT LENGTH and a schedule — per bend the angle, the former, the free
    straight before it and the roll. Read from the model, not restated here.
    """
    out = []
    for part in parts:
        for tube in d.request("tube_schedule", {"part": part})["tubes"]:
            print(f"  {part}: cut {tube['length_mm']:.1f} mm of "
                  f"{tube.get('outer_diameter_mm', 0):.0f} x "
                  f"{tube.get('wall_mm', 0):.1f} tube, "
                  f"{len(tube['bends'])} bends")
            for b in tube["bends"]:
                tight = " TIGHT" if b.get("tight") else ""
                print(f"    bend {b['index']}: {b['angle_deg']:5.1f} deg on "
                      f"R{b['radius_mm']:.0f} (R/D "
                      f"{b.get('radius_over_diameter', 0):.2f}{tight}), after "
                      f"{b['straight_before_mm']:6.1f} mm straight, roll "
                      f"{b['roll_deg']:.0f} deg")
            if any(b.get("tight") for b in tube["bends"]):
                raise SystemExit(f"{part}: a bend tighter than 1.5 diameters")
            out.append(tube)
    return out


def shop(d, out_dir="scratch/grill"):
    print_bend_audit(bend_audit(d))
    tube_schedule(d)
    made = blanks(d, out_dir)
    print("  blanks (mm, and bend lines in the DXF):")
    for part, w, l, bends, name in made:
        print(f"    {part:15s} {w:7.1f} x {l:7.1f}  {bends:2d} bends  {name}")
    want_x, want_y, ba = firebox_blank_is_right(d)
    print(f"  firebox blank {want_x:.1f} x {want_y:.1f} mm, "
          f"bend allowance {ba:.3f} mm — matches the arithmetic")
    folded, flat, table = drawing(d, out_dir)
    print(f"  drawing: folded view {folded['curves']} curves, "
          f"blank {flat['curves']} curves with {flat['bend_lines']} bend lines")
    print(f"  bend table: {table['text'].splitlines()[1].strip()} ...")
    return made


def bend_audit(d, parts=None):
    """Every bend on the grill against the rule for the metal it is in.

    ONE call per part: `list_bends` answers the DFM question a shop asks —
    each bend's inside radius against the material's own minimum, the angle,
    the K and where it came from, and whether the bend is a crushed hem and
    so exempt. `check_sheet_metal` beside it is the app's own verdict, and
    the two have to agree about how many bends there are.
    """
    rows = []
    for part in (parts or SHEET_PARTS):
        listed = d.request("list_bends", {"part": part})
        chk = d.request("check_sheet_metal", {"part": part})
        if listed["count"] != chk["bends_checked"]:
            raise SystemExit(f"{part}: list_bends says {listed['count']} bends "
                             f"and check_sheet_metal checked "
                             f"{chk['bends_checked']}")
        r = listed["rules"]
        bends = listed["bends"]
        rows.append({
            "part": part,
            "material": r["material"],
            "gauge": r["gauge"],
            "t": r["thickness_mm"],
            "radius": min((b["radius_mm"] for b in bends), default=0.0),
            "min_radius": r["min_bend_radius_mm"],
            "k": r["k_factor"],
            "k_source": r["k_factor_source"],
            "relief": r["relief"],
            "relief_w": r["relief_width_mm"],
            "relief_d": r["relief_depth_mm"],
            "bends": listed["count"],
            "hems": sum(1 for b in bends if b["crushed"]),
            "under": [b["index"] for b in bends if b["below_minimum"]],
            "holes": chk["holes_checked"],
            "violations": chk["violations"],
        })
    return rows


def print_bend_audit(rows):
    print("  bends, and the rule for the metal they are in:")
    print("    part              material         gauge   t     R    Rmin  "
          "K     relief          bends hems  holes  violations")
    for r in rows:
        ok = "none" if not r["violations"] and not r["under"] else \
            f"{r['violations']} under-minimum bends {r['under']}"
        print(f"    {r['part']:17s} {r['material']:15s} {r['gauge']:6s} "
              f"{r['t']:4.2f} {r['radius']:5.2f} {r['min_radius']:5.2f} "
              f"{r['k']:5.2f} {r['relief']:>11s} "
              f"{r['relief_w']:4.2f}x{r['relief_d']:<4.2f} "
              f"{r['bends']:4d} {r['hems']:4d} {r['holes']:6d}  {ok}")
    worst = [r for r in rows if r["under"] or r["violations"]]
    if worst:
        raise SystemExit(f"bends below the minimum radius, or in violation: "
                         f"{[w['part'] for w in worst]}")
    print(f"    {sum(r['bends'] for r in rows)} bends, "
          f"{sum(r['hems'] for r in rows)} of them crushed hems (exempt from "
          f"the minimum radius), {sum(r['holes'] for r in rows)} holes, "
          f"no violations")
    return rows
