#!/usr/bin/env python3
"""Stage 8: the drawings. Opens cam.dsgn, saves drawings.dsgn; the sheets go
to scratch/pantilt/drawings/ as PDF and SVG.

    python3 generators/pantilt/build_drawings.py --port N

Three sheets:

- **base_plate**: what a machinist makes the aluminium plate from -- its
  top, the section A-A through the bearing seat and the tripod thread, and
  its underside; the Ø32 K7 seat with its limits, the 3/8-16 UNC-2B thread
  and the four M4 tapped holes as callouts the model writes, the chamfers,
  datums A and B, the general tolerance and the material.
- **assembly**: the head (the camera it carries left off), two views, a
  balloon on every part and the parts list the balloons number.
- **print**: the PETG parts, how each goes on the bed, and the printed-fit
  allowances the model was built with (asked of the app, never typed).

Every dimension is measured off the projection and every callout is the
model's own text, so a change upstream re-draws rather than going stale.
Geometry is picked by what it IS (a circle of the seat's diameter, the
plate's bottom face in the section), from the project variables, not by
toponame strings that a remodel would rename.
"""
import datetime
import json
import math

import common
from common import OUT

DRAW = OUT / "drawings"
DRAW.mkdir(parents=True, exist_ok=True)
AUTHOR = "generators/pantilt"
DATE = datetime.date.today().isoformat()


def variables(app):
    return {v["name"]: v["value"] for v in app.call("list_variables")["variables"]}


def geometry(app, view):
    return app.call("list_drawing_geometry", view=view)["geometry"]


def ref(e, occurrence=False, **extra):
    """A geometry entry as a dimension or leader ref."""
    r = {("sil_face" if e["kind"] == "silhouette" else "edge"): e["ref"]}
    if occurrence:
        r["occurrence"] = e["source_id"]
    r.update(extra)
    return r


def circles(g, d, scale, visible=True):
    """Circles of model diameter `d` in a view drawn at `scale`."""
    return [e for e in g if e["type"] == "circle"
            and abs(2.0 * e["radius"] / scale - d) < 0.01
            and (visible is None or e["visible"] == visible)]


def lines(g, test, kind=None):
    out = []
    for e in g:
        if e["type"] != "line":
            continue
        if kind and e["kind"] != kind:
            continue
        if test(e["p1"], e["p2"]):
            out.append(e)
    return out


def one(found, what):
    if not found:
        raise SystemExit(f"drawings: no {what} on the sheet")
    return found[0]


def export(app, name):
    for ext in ("pdf", "svg"):
        app.call("export_drawing", path=str(DRAW / f"{name}.{ext}"))
    chk = app.call("check_drawing")
    print(f"{name}: check ok={chk['ok']}",
          json.dumps(chk["issues"])[:2000])
    return chk


def dim(app, view, kind, refs, offset):
    r = app.call("add_drawing_dim", view=view, kind=kind, refs=refs,
                 offset=list(offset))
    return r


# ---- the base plate --------------------------------------------------------

def base_plate_sheet(app, V):
    """A3 at 2:1: the top, the section A-A folded under it, the underside
    beside it, the notes and the title block in the corner."""
    s = 2.0
    t, R = V["base_t"], V["base_r"]
    seat_d, seat_b = V["main_brg_D"], V["main_brg_B"]
    app.call("create_drawing", name="base_plate drawing", sheet={
        "size": "a3", "landscape": True,
        "title_block": {
            "project": "Pan/tilt head", "document": "Base plate",
            "author": AUTHOR, "date": DATE,
            "material": "6082-T6 or 6061-T6 plate, 30 thick (EN 485)",
            "notes": "General tolerances ISO 2768-mK"}})
    tx, ty = 150.0, 200.0     # the top view's centre (the pan axis)
    px, py = tx, 66.0         # the section's: the axis at the underside
    bx, by = 335.0, 200.0     # the underside's, at 1:1: it shows one hole
    top = app.call("add_drawing_view", source="base_plate",
                   orientation="top", scale=s, position=[tx, ty],
                   name="top")["id"]
    sec = app.call("add_drawing_view", source="base_plate",
                   orientation="front", scale=s, position=[px, py],
                   base_view=top, name="A-A",
                   section={"point": [0.0, 0.0, 0.0], "label": "A"})["id"]
    bot = app.call("add_drawing_view", source="base_plate",
                   orientation="bottom", scale=1.0, position=[bx, by],
                   name="underside")["id"]
    for v in (sec, bot):
        app.call("set_drawing_view", id=v, show_hidden=False)
    app.call("update_drawing")
    gt, gs, gb = geometry(app, top), geometry(app, sec), geometry(app, bot)

    # Top: the disc, the relief seen through the seat, the M4 pattern.
    rim = one(circles(gt, 2 * R, s), "plate rim in the top view")
    dim(app, top, "diameter", [ref(rim)], (-14, -5))
    relief = one(circles(gt, V["relief_d"], s), "relief in the top view")
    dim(app, top, "diameter", [ref(relief)], (70, -48))
    m4 = circles(gt, V["m4_tap_drill"], s)
    m4 = sorted(m4, key=lambda e: (-e["center"][1], e["center"][0]))
    if len(m4) != 4:
        raise SystemExit(f"drawings: expected 4 M4 holes, found {len(m4)}")
    dim(app, top, "pattern_callout", [ref(e) for e in m4], (-40, 30))
    tl, tr = m4[0], m4[1]  # the top pair, left to right
    br = max(m4[2:], key=lambda e: e["center"][0])
    dim(app, top, "distance_x", [ref(tl), ref(tr)],
        (0, ty + R * s + 7 - tl["center"][1]))
    dim(app, top, "distance_y", [ref(tr), ref(br)],
        (tx + R * s + 16 - tr["center"][0], 0))

    # Section A-A: thickness, the seat and its depth, the relief, the drill.
    def horiz(z):
        return lines(gs, lambda a, b: abs(a[1] - b[1]) < 1e-6
                     and abs(a[1] - (py + s * z)) < 0.02)

    def vert(x, z_lo, z_hi):
        """A wall at +-x: a cylinder's two sides share one ref (its face),
        and the geometry list reports it once, from either side."""
        return lines(gs, lambda a, b: abs(a[0] - b[0]) < 1e-6
                     and abs(abs(a[0] - px) - s * x) < 0.02
                     and min(a[1], b[1]) >= py + s * z_lo - 0.02
                     and max(a[1], b[1]) <= py + s * z_hi + 0.02)

    def end_toward(e, key):
        """`end` 1 or 2 of a reported curve: whichever `key` prefers."""
        return 1 if key(e["p1"]) >= key(e["p2"]) else 2

    bottom = one(horiz(0.0), "bottom face in the section")
    topface = one(horiz(t), "top face in the section")
    floor = one(horiz(t - seat_b), "seat floor in the section")
    relief_floor = one(horiz(t - V["relief_depth"]), "relief floor")
    wall = one(vert(seat_d / 2, t - seat_b, t), "seat wall in the section")
    drill = one(vert(V["tripod_drill"] / 2, 0.0, V["tripod_depth"]),
                "tripod drill wall in the section")
    left, right = px - s * R, px + s * R
    dim(app, sec, "distance_y", [ref(bottom), ref(topface)],
        (left - 9 - px, 0))
    dim(app, sec, "distance_y", [ref(topface), ref(floor)],
        (right + 8 - px, 0))
    dim(app, sec, "distance_y", [ref(topface), ref(relief_floor)],
        (right + 20 - px, 0))
    drill_top = ref(drill, end=end_toward(drill, lambda p: p[1]))
    dim(app, sec, "distance_y", [ref(bottom), drill_top],
        (px - s * V["tripod_drill"] / 2 - 7 - px, 0))
    # The seat: Ø across its walls; its class and limits are the model's.
    mid = py + s * (t - seat_b / 2)
    dim(app, sec, "diameter", [ref(wall)], (0, py + s * t + 6 - mid))

    # Underside: the tripod thread, and the chamfers the 6090 cuts.
    tri = one(circles(gb, V["tripod_drill"], 1.0), "tripod drill rim")
    dim(app, bot, "hole_callout", [ref(tri)], (0, 50))
    mouth = one(circles(gb, V["tripod_drill"] + 2 * V["edge_break"], 1.0),
                "tripod mouth chamfer")
    app.call("add_drawing_note", view=bot, ref=ref(mouth, extreme="bottom"),
             position=[bx, by - R - 24],
             text="{{edge_break}} × 45° CHAMFER: TRIPOD MOUTH,\n"
                  "SEAT MOUTH, BOTH OUTER RIMS")

    # Datums: A the face it stands on, B the bearing seat.
    # The face's leftmost point, over both halves the tripod hole splits
    # it into (`extreme` reads every piece; an `end` only the reported one).
    app.call("add_drawing_note", view=sec, style="datum", text="A",
             ref=ref(bottom, extreme="left"),
             position=[left - 4, py - 12])
    b_end = end_toward(wall, lambda p: p[1])
    b_at = wall["p1"] if b_end == 1 else wall["p2"]
    side = -1 if b_at[0] < px else 1
    app.call("add_drawing_note", view=sec, style="datum", text="B",
             ref=ref(wall, end=b_end),
             position=[b_at[0] + side * 12, b_at[1] + 7])

    # What the dimensions cannot say, ruled, on the title block.
    notes = "\n".join([
        "NOTES",
        "1\tSEAT B PERPENDICULAR TO A WITHIN 0.02, RUNOUT OF B TO A 0.03",
        "2\tM4 HOLES: POSITION Ø0.2 TO A AND B; TAP BY HAND",
        "3\tØ32 K7 SEAT: GAUGE BEFORE THE SETUP IS BROKEN",
        "4\tBREAK ALL OTHER EDGES 0.2 MAX; NO BURRS IN THE SEAT",
    ])
    app.call("add_drawing_note", text=notes, position=[410, 58])
    app.call("add_drawing_note", text="UNDERSIDE  1:1",
             position=[bx - 30, by + R + 6])
    return export(app, "base_plate")


# ---- the assembly ----------------------------------------------------------

def extent(e):
    """A geometry entry's sheet bounds (a circle reports its centre and
    radius instead of a box)."""
    if e["type"] == "circle":
        (x, y), r = e["center"], e["radius"]
        return (x - r, y - r), (x + r, y + r)
    return e["bbox"][0], e["bbox"][1]


def shown_length(g, occ):
    """How much of an occurrence a view draws in solid line."""
    total = 0.0
    for e in g:
        if e["source_id"] == occ and e["visible"]:
            lo, hi = extent(e)
            total += math.hypot(hi[0] - lo[0], hi[1] - lo[1])
    return total


def balloon_column(app, view, g, bbox, side, occs, floor_y=16.0):
    """A balloon on each occurrence in `occs`, stacked in a column on `side`
    of the view: each leader from the point of the part nearest the column
    (a ref's `extreme`), the balloons in the order of those points, so
    leaders do not cross."""
    sign = -1.0 if side == "left" else 1.0
    anchors = []
    for occ in occs:
        cands = []
        for e in g:
            if e["source_id"] != occ or not e["visible"]:
                continue
            # By the curve's REPORTED piece (p1..p2, which `end` names): a
            # ref split into several pieces has a box spanning all of them.
            if e["type"] == "circle":
                (cx, _), r = e["center"], e["radius"]
                size, reach = 2 * r, sign * cx + r
            else:
                size = math.dist(e["p1"], e["p2"])
                reach = max(sign * e["p1"][0], sign * e["p2"][0])
            cands.append((size > 1.5, reach, e))
        if not cands:
            print(f"  balloon: occurrence {occ} draws nothing in view {view}")
            continue
        _, _, best = max(cands, key=lambda c: (c[0], c[1]))
        # The point the leader lands on, known here so the column can be
        # ordered by it: a line's or curve's END nearer the column (as
        # reported), a circle's extreme on that side.
        if best["type"] == "circle":
            (cx, cy), r = best["center"], best["radius"]
            at, how = (cx + sign * r, cy), {"extreme": side}
        else:
            end = 1 if sign * best["p1"][0] >= sign * best["p2"][0] else 2
            at, how = best["p1"] if end == 1 else best["p2"], {"end": end}
        anchors.append((at[1], occ, best, how))
    if not anchors:
        return
    anchors.sort(key=lambda a: -a[0])
    x = bbox[0][0] - 12 if side == "left" else bbox[1][0] + 12
    lo_y, hi_y = bbox[0][1], bbox[1][1]
    step = 10.0 if len(anchors) < 2 else \
        min(14.0, max(10.0, (hi_y - lo_y) / (len(anchors) - 1)))
    top = 0.5 * (lo_y + hi_y) + 0.5 * step * (len(anchors) - 1)
    # Clear of the title block and the parts list on it, both of which
    # stand in the sheet's bottom right.
    top = max(top, floor_y + step * (len(anchors) - 1))
    for i, (_, occ, e, how) in enumerate(anchors):
        app.call("add_drawing_note", view=view, style="balloon", text="",
                 ref=ref(e, occurrence=True, **how),
                 position=[x, top - i * step])


def place_balloons(app, views, rows, by_part, sides):
    """Each parts-list row ballooned once, on the view that draws the most
    of it, in the column on whichever of that view's free sides it is
    nearer."""
    g = {v: geometry(app, v) for v in sides}
    columns = {(v, s): [] for v in sides for s in sides[v]}
    for row in rows:
        part = row[2]
        best = max(((shown_length(g[v], o), v, o) for v in g
                    for o in by_part.get(part, [])), default=(0, None, None))
        if best[0] <= 0:
            print(f"  {part}: nothing drawn in solid line to balloon")
            continue
        _, v, occ = best
        (vlo, vhi) = views[v]["bbox"]
        xs = [0.5 * (extent(e)[0][0] + extent(e)[1][0]) for e in g[v]
              if e["source_id"] == occ and e["visible"]]
        mid = sum(xs) / len(xs)
        side = "left" if mid < 0.5 * (vlo[0] + vhi[0]) else "right"
        if side not in sides[v]:
            side = sides[v][0]
        columns[(v, side)].append(occ)
    for (v, side), occs in columns.items():
        x = views[v]["bbox"][0 if side == "left" else 1][0]
        balloon_column(app, v, g[v], views[v]["bbox"], side, occs,
                       floor_y=64.0 if x > 235.0 else 16.0)


def assembly_sheet(app, clock):
    """A3 at 1:2.5: the head from the front, its plan with the yoke and the
    tilt axis left off (what the pan drive is, under them), and a section
    on both axes from the side; a balloon on every part, the parts list."""
    occs = occurrences(app)  # before the drawing tab is made
    by_part = {}
    for o in occs:
        by_part.setdefault(o["part"], []).append(o["id"])
    camera = [o["id"] for o in occs if o["part"] == "camera"]
    # The plan is a section on the pan worm's axis, the lower of the two:
    # through the worm, its 609s in their blocks, the wheel's teeth and the
    # screws that hold the wheel down -- all of it hidden from outside.
    pan_worm = min((o for o in occs if o["part"] == "worm"),
                   key=lambda o: o["bbox_min"][2] + o["bbox_max"][2])
    z_worm = 0.5 * (pan_worm["bbox_min"][2] + pan_worm["bbox_max"][2])
    app.call("create_drawing", name="pantilt drawing", sheet={
        "size": "a3", "landscape": True,
        "title_block": {
            "project": "Pan/tilt head", "document": "Assembly",
            "author": AUTHOR, "date": DATE, "material": "See parts list",
            "notes": "Camera (the 1.5 kg load) not shown. Printed parts: "
                     "sheet 3."}})
    s = 0.4
    fx, fy = 82.0, 32.0
    front = app.call("add_drawing_view", source="pantilt",
                     orientation="front", scale=s, position=[fx, fy],
                     hidden=camera, name="front")["id"]
    plan = app.call("add_drawing_view", source="pantilt", orientation="top",
                    scale=s, position=[fx, 205], base_view=front,
                    hidden=camera, name="C-C",
                    section={"point": [0.0, 0.0, z_worm], "label": "C"})["id"]
    side = app.call("add_drawing_view", source="pantilt",
                    orientation="right", scale=s, position=[192, fy],
                    base_view=front, hidden=camera, name="B-B",
                    section={"point": [0.0, 0.0, 0.0], "label": "B"})["id"]
    for v in (front, plan, side):
        app.call("set_drawing_view", id=v, show_hidden=False)
    r = app.call("update_drawing", budget_s=600)
    clock("  assembly views projected (two of them sections)")
    print("assembly views:", [(v["id"], v["curves"], v.get("error"))
                              for v in r["views"]])
    views = {v["id"]: v for v in app.call("list_drawing_views")["views"]}
    # The parts list first: the balloons number themselves from it.
    app.call("add_drawing_note", view=front, table="bom", text="",
             position=[410, 58])
    bom = app.call("list_drawing_notes")["notes"][-1]["resolved_text"]
    rows = [r.split("\t") for r in bom.split("\n")[1:]]
    print(f"parts list: {len(rows)} rows")
    clock("  parts list")
    # The section has the front view on its left: its balloons go right.
    place_balloons(app, views, rows, by_part,
                   {front: ("left", "right"), plan: ("left", "right"),
                    side: ("right",)})
    clock("  balloons")
    return export(app, "assembly")


def occurrences(app):
    """The head's occurrences: id, part name, world bounds."""
    parts = app.call("list_parts")["parts"]
    names = {p["id"]: p["name"] for p in parts}
    head = next(p["id"] for p in parts if p["name"] == "pantilt")
    app.call("set_active_part", part=head)
    out = []
    for o in app.call("list_occurrences", bounds=True)["occurrences"]:
        out.append({"id": o["id"], "name": o["name"],
                    "part": names.get(o["part_id"]),
                    "bbox_min": o["bbox_min"], "bbox_max": o["bbox_max"]})
    return out


# ---- the print sheet -------------------------------------------------------

PRINTED = [
    # part, quantity, how it goes on the bed, why
    ("yoke: base", "1", "top face down", "the yoke prints as 3 pieces"),
    ("yoke: +Y arm", "1", "upright; web and ledge on support",
     "the whole U has no unsupported pose"),
    ("yoke: -Y arm", "1", "on its inner face", ""),
    ("pan_wheel", "1", "on the face that seats on the plate",
     "teeth as walls; the counterbores open up"),
    ("tilt_wheel", "1", "on the wheel face, shaft up",
     "journals print round with the axis vertical"),
    ("worm", "2", "standing on its plain end, axis vertical",
     "journals round; module-2 flanks bridge"),
    ("worm_block", "4", "on its mounting face",
     "the Ø24 seat prints with its axis level: check it with the bearing"),
    ("knob", "2", "on its flat face", ""),
    ("cradle", "1", "camera face down", "the 1/4-20 slot stays flat"),
]


def print_sheet(app, V):
    app.call("create_drawing", name="print sheet", sheet={
        "size": "a3", "landscape": True,
        "title_block": {
            "project": "Pan/tilt head", "document": "Printed parts",
            "author": AUTHOR, "date": DATE,
            "material": "PETG, 0.4 mm nozzle", "scale": "NOT TO SCALE",
            "notes": f"Walls {V['print_wall']:g} mm (8 perimeters). "
                     "Sizes are modelled with the allowances above."}})
    # Each printed part, iso, for recognising it on the bed -- a look, so
    # the polygonal projection: nothing here is dimensioned.
    cells = [("yoke", 0.3, (70, 215)), ("tilt_wheel", 0.5, (165, 230)),
             ("cradle", 0.4, (70, 120)), ("pan_wheel", 0.5, (165, 150)),
             ("worm", 0.6, (40, 62)), ("worm_block", 0.8, (100, 62)),
             ("knob", 0.8, (160, 62))]
    for part, scale, (cx, cy) in cells:
        v = app.call("add_drawing_view", source=part, orientation="iso",
                     scale=scale, position=[cx, cy], quality="fast",
                     name=part)["id"]
        app.call("set_drawing_view", id=v, show_hidden=False)
        app.call("update_drawing")
        info = next(x for x in app.call("list_drawing_views")["views"]
                    if x["id"] == v)
        (lo, hi) = info["bbox"]
        app.call("set_drawing_view", id=v, position=[
            2 * cx - 0.5 * (lo[0] + hi[0]), 2 * cy - 0.5 * (lo[1] + hi[1])])
        app.call("add_drawing_note", text=part.upper().replace("_", " "),
                 position=[cx, cy - 0.5 * (hi[1] - lo[1]) - 6])
    # A table note stands on its bottom-right corner: under the border's top.
    row_h, top = 6.0, 277.0
    rows = ["PRINTED PARTS (PETG)", "PART\tQTY\tON THE BED\tWHY"]
    rows += ["\t".join(r) for r in PRINTED]
    app.call("add_drawing_note", text="\n".join(rows),
             position=[400, top - row_h * len(rows)])
    top -= row_h * len(rows) + 12
    # The allowances, asked of the app's table for each printed fit.
    fits = [
        ("6002 outer ring seat", "hole", V["main_brg_D"]),
        ("6002 relief", "hole", V["relief_d"]),
        ("6002 journal (yoke, tilt shaft)", "shaft", V["main_brg_d"]),
        ("609 outer ring seat (blocks)", "hole", V["worm_brg_D"]),
        ("609 journal (worm)", "shaft", V["worm_brg_d"]),
        ("M4 counterbore", "hole", V["m4_cb_d"]),
    ]
    rows = ["PRINTED-FIT ALLOWANCES (FDM, PETG)",
            "FIT\tKIND\tPRINTS AT\tMODELLED\tALLOWANCE"]
    for name, kind, size in fits:
        a = app.call("print_allowance", kind=kind, size_mm=size,
                     process="fdm", material="petg")
        rows.append(f"{name}\t{kind.upper()}\tØ{size:.2f}\t"
                    f"Ø{a['model_mm']:.2f}\t{a['allowance_mm']:+.2f}")
    app.call("add_drawing_note", text="\n".join(rows),
             position=[400, top - row_h * len(rows)])
    return export(app, "print")


def main():
    a = common.args()
    app = common.connect(a.port)
    clock = common.Clock()
    app.call("open_project", path=str(OUT / "cam.dsgn"))
    plate = app.part("base_plate", create=False)
    plate.activate()
    V = variables(app)
    # What the drawing measures to, derived the way build_base.py made it.
    V["relief_depth"] = V["main_brg_B"] + V["run_gap"]
    V["tripod_drill"] = 0.3125 * 25.4  # 5/16 tap drill, the Hole's diameter
    V["tripod_depth"] = V["tripod_len"] + 2 * V["tripod_pitch"]
    V["m4_tap_drill"] = 3.3
    clock("open")
    report = {"base_plate": base_plate_sheet(app, V)}
    clock("base plate sheet")
    report["assembly"] = assembly_sheet(app, clock)
    clock("assembly sheet")
    report["print"] = print_sheet(app, V)
    clock("print sheet")
    common.save(app, "drawings.dsgn")
    (OUT / "drawings_report.json").write_text(json.dumps(
        {k: {"ok": v["ok"], "issues": v["issues"]} for k, v in report.items()},
        indent=1))
    print("done")


if __name__ == "__main__":
    main()
