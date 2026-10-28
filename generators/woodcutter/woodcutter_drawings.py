# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The woodcutter's drawings and bill of materials (phase 5 of
docs/plans/woodcutter-automaton.md, now generators/woodcutter/README.md).

- **The assembly sheet** (A2): the automaton from its front (the open side,
  the app's `back`) and from the crank's side, an exploded view (the box
  pulled apart along the ways its parts go in), the parts list and a balloon
  on every row laid out by the app (`layout_balloons`).
- **A sheet per cam** (A3): the cam seen down the shaft at 2:1 and from the
  side, its width, its bore and the bore's flat, and its motion as two ruled
  tables: the segments the CamProfile is built from (read from the feature)
  and the lift every 15 degrees. The app draws no lift diagram of a
  CamProfile (docs/drawings/README.md#not-done), so the lift is a table;
  the run checks it against the outline the sheet draws.
- **`export_bom`**: every printed part, its material, printed mass and the
  plate its pieces print on.

Every sheet is checked (`check_drawing` must be clean) and exported as PDF
and SVG beside the project.
"""
import csv
import math
import re
from pathlib import Path

import woodcutter_spec as S

ASM = "woodcutter automaton"
AUTHOR = "generators/woodcutter"
LIFT_STEP_DEG = 15.0
# How far the drawn outline may stand from the lift the table says, sheet
# mm: half the visible line's 0.35 mm width, a miss nobody can see. The
# exported outline is a polyline whose chords sag up to about 0.1 mm on
# the sheet (0.05 on the cam at 2:1).
OUTLINE_TOL_MM = 0.175


def fail(msg):
    raise SystemExit(f"woodcutter drawings: {msg}")


# ---- the exploded view ---------------------------------------------------------

PANEL_OF = {"floor": "floor", "back": "back", "left wall": "left wall",
            "right wall": "right wall", "top": "top"}


def explode_offsets(s, names):
    """Each occurrence's step in the exploded view, from the box's sizes:
    the walls off the shaft's ends, the bushings and the crank further out
    along it, the back down and out from under the shaft, the top lifted
    off the rods with the figures on it and the rods half way, each peg half way
    between the two panels it joins. The camshaft and its cams stay, the
    spine everything else is pulled off."""
    W, H, depth = s["width"], s["H"], s["depth"]
    out_x = W / 8.0                  # each wall off the shaft's end
    up = H / 3.0                     # the top off the rods
    # The back drops and comes forward of the floor: the iso looks at its
    # outside, so where it stood it would hide the shaft and the cams.
    panel = {"floor": (0.0, 0.0, 0.0),
             "back": (0.0, -depth / 2.0, -H / 2.0),
             "left wall": (-out_x, 0.0, 0.0),
             "right wall": (out_x, 0.0, 0.0),
             "top": (0.0, 0.0, up)}
    step = W / 12.0                  # between the parts strung on the shaft
    fixed = {
        "bushing L": (-out_x - step, 0.0, 0.0),
        "bushing R": (out_x + step, 0.0, 0.0),
        "crank": (-out_x - 2 * step, 0.0, 0.0),
        "grip": (-out_x - 3 * step, 0.0, 0.0),
        "handle pin": (-out_x - 4 * step, 0.0, 0.0),
        "axe rod": (0.0, 0.0, up / 2.0),
        "bird rod": (0.0, 0.0, up / 2.0),
    }
    # The figures stand in the top's slots: they rise with it, and a figure
    # height more, so their tabs show.
    figures = ("woodcutter", "perch", "log", "arms", "bird", "axe link",
               "bird link", "axe rod pin", "axe lever pin", "axe pivot pin",
               "bird rod pin", "bird lever pin", "bird pivot pin")
    out = {}
    for n in names:
        if n in panel:
            off = panel[n]
        elif n in fixed:
            off = fixed[n]
        elif n in figures:
            off = (0.0, 0.0, up + H / 4.0)
        elif n.startswith("peg ") and "(" in n:
            a, b = n[n.index("(") + 1:n.index(")")].split("/")
            pa, pb = panel[PANEL_OF[a]], panel[PANEL_OF[b]]
            off = tuple(0.5 * (pa[i] + pb[i]) for i in range(3))
        elif n in ("camshaft", "axe cam", "bird cam"):
            off = (0.0, 0.0, 0.0)
        else:
            fail(f"no exploded step for '{n}'")
        if any(abs(v) > 0.0 for v in off):
            out[n] = off
    return out


def exploded_view(d, s):
    """The assembly's exploded view (the model's, so the GUI's slider shows
    it too), left inactive on the model tab."""
    d.call("set_active_part", part=ASM)
    occ = d.call("list_occurrences")["occurrences"]
    ids = {o["name"]: o["id"] for o in occ}
    for v in d.call("list_exploded_views")["views"]:
        if v["name"] == "drawing":
            d.call("remove_exploded_view", id=v["id"])
    vid = d.call("add_exploded_view", name="drawing")["id"]
    for name, off in explode_offsets(s, list(ids)).items():
        d.call("set_explode_step", view=vid, occurrence=ids[name],
               offset=list(off))
    d.call("set_explode", view=0)    # the model tab stays assembled
    return vid


# ---- sheets ---------------------------------------------------------------------

def export(d, out, name):
    paths = {}
    for ext in ("pdf", "svg"):
        p = out / f"{name}.{ext}"
        d.call("export_drawing", path=str(p))
        paths[ext] = str(p)
    chk = d.call("check_drawing")
    if not chk["ok"]:
        fail(f"{name}: check_drawing is not clean: {chk['issues'][:6]}")
    return {"paths": paths, "check": {"ok": chk["ok"],
                                      "issues": chk["issues"]}}


def drop_drawing(d, name):
    """A drawing of this name from an earlier run on the same project is
    replaced, not added beside."""
    for dw in d.call("list_drawings")["drawings"]:
        if dw["name"] == name:
            d.call("delete_part", part=name)


def view_boxes(d):
    """Each view's sheet box, relative to its position."""
    out = {}
    for v in d.call("list_drawing_views")["views"]:
        (x0, y0), (x1, y1) = v["bbox"]
        px, py = v["position"]
        out[v["id"]] = (x0 - px, y0 - py, x1 - px, y1 - py)
    return out


def assembly_sheet(d, s, out, title, vid, materials):
    """A1: the front and the crank's side at 1:1 down the left, the
    exploded view at 2:1 beside them, the parts list on the title block.
    Each view is placed from its own projected box, with a ring round it
    for the balloons."""
    name = "woodcutter assembly"
    drop_drawing(d, name)
    d.call("create_drawing", name=name, sheet={
        "size": "a1", "landscape": True,
        "title_block": {"project": "Woodcutter automaton",
                        "document": "Assembly", "author": AUTHOR,
                        "material": "/".join(m.upper() for m in materials)
                        + ", SEE THE PARTS LIST",
                        "notes": title}})
    # Front: the open side, the app's `back` (the box's back panel is on
    # -Y, the app's front). The side from the crank, the app's `left`. The
    # exploded view in iso, from behind and above the crank's far end.
    front = d.call("add_drawing_view", source=ASM, orientation="back",
                   scale=1.0, position=[150.0, 450.0], name="front")["id"]
    side = d.call("add_drawing_view", source=ASM, orientation="left",
                  scale=1.0, position=[150.0, 200.0],
                  name="crank side")["id"]
    exploded = d.call("add_drawing_view", source=ASM, orientation="iso",
                      scale=2.0, position=[450.0, 300.0],
                      name="exploded (2:1)", exploded_view=vid)["id"]
    for v in (front, side, exploded):
        d.call("set_drawing_view", id=v, show_hidden=False)
    r = d.call("update_drawing", budget_s=600)
    bad = [(v["id"], v.get("error")) for v in r["views"] if v.get("error")]
    if bad:
        fail(f"assembly views did not project: {bad}")
    # The sheet: A1 is 841 x 594, a 10 mm border.
    box = view_boxes(d)
    ring = 30.0
    left, top = 10.0 + ring, 584.0 - ring
    fb, sb, eb = box[front], box[side], box[exploded]
    fpos = (left - fb[0], top - fb[3])
    spos = (left - sb[0], fpos[1] + fb[1] - 2 * ring - sb[3])
    col = left + max(fb[2] - fb[0], sb[2] - sb[0]) + 2 * ring
    epos = (col - eb[0], 0.5 * (584.0 + 10.0) - 0.5 * (eb[1] + eb[3]))
    for v, p in ((front, fpos), (side, spos), (exploded, epos)):
        d.call("set_drawing_view", id=v, position=list(p))
    d.call("add_drawing_note", view=front, table="bom", text="",
           position=[831.0, 58.0])
    # Captions: the sheet captions sections and details, not a plain view.
    for v, p, b, text in ((front, fpos, fb, "FRONT"),
                          (side, spos, sb, "FROM THE CRANK"),
                          (exploded, epos, eb, "EXPLODED  2:1")):
        d.call("add_drawing_note", text=text,
               position=[p[0] + 0.5 * (b[0] + b[2]), p[1] + b[1] - 8.0])
    laid = d.call("layout_balloons", views=[front, side, exploded])
    if laid["crossings"] or laid["unplaced"]:
        fail(f"the balloons did not lay out cleanly: crossings "
             f"{laid['crossings']}, unplaced {laid['unplaced']}")
    rec = export(d, out, "woodcutter_assembly")
    rec["balloons"] = len(laid["placed"])
    return rec


# ---- the cams -------------------------------------------------------------------

def law_lift(law, t):
    """The fraction of a segment's rise at fraction t of its angle."""
    if law == "cycloidal":
        return t - math.sin(2.0 * math.pi * t) / (2.0 * math.pi)
    if law == "harmonic":
        return 0.5 * (1.0 - math.cos(math.pi * t))
    if law == "poly345":
        return t ** 3 * (10.0 - 15.0 * t + 6.0 * t * t)
    if law == "dwell":
        return 0.0
    fail(f"no lift for the law '{law}'")


def lift_at(segments, deg, after_drop=False):
    """The lift at `deg` of the motion `segments` (the feature's own): each
    segment's law over its span, from the end of the one before; a drop a
    step at its angle, taken there when `after_drop`."""
    lift, a0 = 0.0, 0.0
    for sg in segments:
        a1, law = sg["to_deg"], sg["law"]
        h = sg.get("rise_mm") or 0.0
        if law == "drop":
            if deg > a1 or (after_drop and deg == a1):
                lift += h
            continue
        t = min(1.0, max(0.0, (deg - a0) / (a1 - a0)))
        lift += h * law_lift(law, t)
        a0 = a1
    return lift


def segment_rows(segments):
    rows, a0 = [], 0.0
    for sg in segments:
        h = sg.get("rise_mm")
        rows.append((a0, sg["to_deg"], sg["law"].upper(),
                     "" if h in (None, 0) or sg["law"] == "dwell"
                     else f"{h:+.2f}"))
        a0 = sg["to_deg"]
    return rows


def cam_feature(d, part):
    d.call("set_active_part", part=part)
    st = d.call("get_state")
    cam = next(f for f in st["features"] if f.get("type") == "CamProfile")
    return cam


def drawn_segments(svg_path, view):
    """The visible line work a view draws in the exported SVG, as sheet
    segments (y up, as the sheet's own coordinates): what the reader sees,
    not what the projection meant to draw."""
    text = Path(svg_path).read_text()
    height = float(re.search(r'viewBox="0 0 [\d.]+ ([\d.]+)"', text)[1])
    body = re.search(rf'<g id="view-{view}">(.*?)</g>', text, re.S)
    if body is None:
        fail(f"the SVG has no view {view}")
    segs = []
    for attrs in re.findall(r"<polyline ([^>]*)/>", body[1]):
        if "dasharray" in attrs or 'stroke-width="0.35"' not in attrs:
            continue
        pts = [tuple(float(v) for v in p.split(","))
               for p in re.search(r'points="([^"]+)"', attrs)[1].split()]
        pts = [(x, height - y) for x, y in pts]
        segs += list(zip(pts, pts[1:]))
    return segs


def seg_dist(c, a, b):
    ax, ay = b[0] - a[0], b[1] - a[1]
    L = ax * ax + ay * ay
    t = 0.0 if L == 0 else max(0.0, min(1.0, ((c[0] - a[0]) * ax +
                                              (c[1] - a[1]) * ay) / L))
    return math.hypot(a[0] + t * ax - c[0], a[1] + t * ay - c[1])


def outline_check(segs, segments, base, tip, scale, at):
    """The lift the table says against the outline the sheet draws: at each
    angle the knife's tip centre stands base + tip + lift from the axis on
    the follower's line (theta clockwise from the top, seen down the shaft
    from +X, the cam turning counter-clockwise: the follower stands at
    -theta on the cam), and the drawn outline must pass a tip radius from
    it, at the sheet's scale. `at`: the axis on the sheet. Left out: a
    drop's angle and the turn after it in which the tip, a tip radius
    wide, still meets the step's face (asin(tip / (base + tip))): there it
    is falling, not on the cam, as check_followers leaves the drops'
    corners out. Returns the worst miss, sheet mm, and the angles
    checked."""
    worst, checked = 0.0, []
    clear = math.degrees(math.asin(tip / (base + tip)))
    drops = [sg["to_deg"] for sg in segments if sg["law"] == "drop"]
    for k in range(int(360 / LIFT_STEP_DEG)):
        th = k * LIFT_STEP_DEG
        if any(0.0 <= (th - a) % 360.0 <= clear for a in drops):
            continue
        checked.append(th)
        for after in (False,):
            r = base + tip + lift_at(segments, th, after)
            c = (at[0] + scale * r * math.sin(math.radians(th)),
                 at[1] + scale * r * math.cos(math.radians(th)))
            dmin = min(seg_dist(c, a, b) for a, b in segs)
            worst = max(worst, abs(dmin - scale * tip))
    return worst, checked


def cam_sheet(d, s, out, which, receipt_cam):
    part = f"{which} cam"
    cam = cam_feature(d, part)
    segs = cam["params"]["segments"] if "params" in cam else cam["segments"]
    rep = cam.get("report") or receipt_cam
    name = f"{part} drawing"
    drop_drawing(d, name)
    d.call("create_drawing", name=name, sheet={
        "size": "a3", "landscape": True,
        "title_block": {"project": "Woodcutter automaton",
                        "document": part.capitalize(), "author": AUTHOR,
                        "notes": "Printed flat. Lift: the table, at the "
                                 "follower's tip centre."}})
    sc = 2.0
    x_mid = s[f"x_{which}"]
    # Down the shaft from +X (the app's `right`): world +Y to the sheet's
    # right, +Z up. The axis lands at `axis`.
    axis = (205.0, 170.0)
    pos_r = [axis[0] - sc * 0.0, axis[1] - sc * s["zc"]]
    prof = d.call("add_drawing_view", source=part, orientation="right",
                  scale=sc, position=pos_r, name="down the shaft")["id"]
    side = d.call("add_drawing_view", source=part, orientation="front",
                  scale=sc, position=[95.0 - sc * x_mid, pos_r[1]],
                  base_view=prof, name="side")["id"]
    for v in (prof, side):
        d.call("set_drawing_view", id=v, show_hidden=False)
    d.call("update_drawing", budget_s=300)
    gp = d.call("list_drawing_geometry", view=prof)["geometry"]
    gs = d.call("list_drawing_geometry", view=side)["geometry"]

    # The width, across the side view's two faces.
    # (a B-spline face's edge, straight here, is listed as a curve)
    faces = sorted((e for e in gs if ebox(e) is not None
                    and ebox(e)[1][0] - ebox(e)[0][0] < 1e-6
                    and ebox(e)[1][1] - ebox(e)[0][1] > 1.0),
                   key=lambda e: ebox(e)[0][0])
    if len(faces) < 2:
        fail(f"{part}: the side view shows no two faces")
    d.call("add_drawing_dim", view=side, kind="distance_x",
           refs=[ref(faces[0]), ref(faces[-1])])
    # The bore: its arc's diameter, and the flat.
    bore = [e for e in gp if e["type"] in ("arc", "circle")
            and abs(2.0 * e["radius"] / sc - s["d_shaft"]) < 0.05]
    if not bore:
        fail(f"{part}: no bore of Ø{s['d_shaft']:g} down the shaft")
    # Where the dimensions stand is the app's: clear of the views and the
    # labels already there.
    d.call("add_drawing_dim", view=prof, kind="diameter",
           refs=[ref(bore[0])])
    flat = [e for e in gp if e["type"] == "line"
            and math.hypot(e["p1"][0] - e["p2"][0],
                           e["p1"][1] - e["p2"][1]) < sc * s["d_shaft"]]
    if not flat:
        fail(f"{part}: the bore's flat is not drawn down the shaft")
    f = flat[0]
    level = abs(f["p1"][1] - f["p2"][1]) < abs(f["p1"][0] - f["p2"][0])
    d.call("add_drawing_dim", view=prof,
           kind="distance_y" if level else "distance_x",
           refs=[ref(f), ref(bore[0], extreme=far_side(f, axis))])

    # The motion, as the feature has it; the lift every 15 degrees.
    rows = ["MOTION (CamProfile)", "FROM °\tTO °\tLAW\tRISE mm"]
    for a0, a1, law, h in segment_rows(segs):
        rows.append(f"{a0:g}\t{a1:g}\t{law}\t{h}")
    d.call("add_drawing_note", text="\n".join(rows), position=[405.0, 230.0])
    half = int(180 / LIFT_STEP_DEG)
    lines = ["LIFT AT THE TIP CENTRE, mm",
             "ANGLE °\tLIFT\tANGLE °\tLIFT"]
    for k in range(half):
        a, b = k * LIFT_STEP_DEG, (k + half) * LIFT_STEP_DEG
        lines.append(f"{a:g}\t{fmt_lift(segs, a)}\t{b:g}\t"
                     f"{fmt_lift(segs, b)}")
    d.call("add_drawing_note", text="\n".join(lines),
           position=[405.0, 58.0 + 6.0])
    fol = rep.get("follower") or {}
    note = "\n".join([
        f"BASE CIRCLE R{rep['base_radius_mm']:.2f}, MAX R"
        f"{rep['max_radius_mm']:.2f}",
        f"FOLLOWER: {str(fol.get('kind', 'knife')).upper()} TIP R"
        f"{fol.get('radius_mm', s['tip']):g}, SLIDING ON THE AXIS LINE",
        "TURNS COUNTER-CLOCKWISE AS DRAWN; THE TABLE'S ANGLE IS",
        "MEASURED CLOCKWISE FROM THE TOP, WHERE THE FOLLOWER STANDS",
        f"PRESSURE ANGLE AT MOST {rep['max_pressure_angle_deg']:.1f}° "
        f"(AT {rep['max_pressure_at_deg']:g}°)"])
    # A note is centred on its position: its widest line's half in from the
    # border.
    half = max(len(t) for t in note.split("\n")) * 0.5 * 2.6
    d.call("add_drawing_note", text=note, position=[20.0 + half, 40.0])
    rec = export(d, out, f"woodcutter_{which}_cam")
    worst, checked = outline_check(drawn_segments(rec["paths"]["svg"], prof), segs,
                          rep["base_radius_mm"], s["tip"], sc, axis)
    if worst > OUTLINE_TOL_MM:
        fail(f"{part}: the drawn outline stands {worst:.3f} mm (sheet) "
             f"from the lift the table says")
    rec["outline_vs_table_mm"] = worst
    rec["outline_checked_deg"] = checked
    rec["lift"] = {f"{k * LIFT_STEP_DEG:g}": lift_at(segs, k * LIFT_STEP_DEG)
                   for k in range(int(360 / LIFT_STEP_DEG))}
    return rec


def fmt_lift(segs, a):
    before, after = lift_at(segs, a), lift_at(segs, a, True)
    if abs(before - after) > 1e-9:
        return f"{before:.2f} TO {after:.2f}"
    return f"{before:.2f}"


def ebox(e):
    """A listed curve's sheet box: its own, or its ends'."""
    if "bbox" in e:
        return e["bbox"]
    if "p1" in e and "p2" in e:
        return [[min(e["p1"][0], e["p2"][0]), min(e["p1"][1], e["p2"][1])],
                [max(e["p1"][0], e["p2"][0]), max(e["p1"][1], e["p2"][1])]]
    return None


def far_side(line, axis):
    """The extreme of the bore across from its flat."""
    mx = 0.5 * (line["p1"][0] + line["p2"][0])
    my = 0.5 * (line["p1"][1] + line["p2"][1])
    if abs(mx - axis[0]) > abs(my - axis[1]):
        return "right" if mx < axis[0] else "left"
    return "top" if my < axis[1] else "bottom"


def ref(e, **extra):
    r = {("sil_face" if e["kind"] == "silhouette" else "edge"): e["ref"]}
    r.update(extra)
    return r


# ---- the bill of materials ------------------------------------------------------

def bom(d, out, plated):
    d.call("set_active_part", part=ASM)
    p = out / "woodcutter_bom.csv"
    d.call("export_bom", path=str(p))
    with open(p, newline="") as f:
        rows = list(csv.DictReader(f))
    parts = [r for r in rows if r["part"] != "total"]
    total = next(r for r in rows if r["part"] == "total")
    bought = [r["part"] for r in parts if r["standard"]]
    if bought:
        fail(f"the bill lists bought parts: {bought}")
    unprinted = [r["part"] for r in parts if not r["printed_unit_mass_g"]]
    if unprinted:
        fail(f"no printed mass for {unprinted}")
    unplated = [r["part"] for r in parts if not r["plates"]]
    if plated and unplated:
        fail(f"on no plate: {unplated}")
    return {"path": str(p), "rows": len(parts),
            "pieces": sum(int(r["quantity"]) for r in parts),
            "printed_g": float(total["printed_total_mass_g"]),
            "solid_g": float(total["total_mass_g"]),
            "materials": sorted({r["material"] for r in parts})}


def drawings(d, s, out, receipt_cams, title, plated):
    """Every sheet and the bill. Returns the receipt's `drawings`.
    `plated`: the plates are laid out, so every part must be on one."""
    b = bom(d, out, plated)
    vid = exploded_view(d, s)
    rec = {"bom": b, "assembly": assembly_sheet(d, s, out, title, vid,
                                          b["materials"])}
    for k in ("axe", "bird"):
        rec[f"{k} cam"] = cam_sheet(d, s, out, k, receipt_cams[k])
    d.call("set_active_part", part=ASM)
    return rec
