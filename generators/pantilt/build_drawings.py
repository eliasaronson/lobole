#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Stage 8: the drawings. Opens stages/cam.dsgn and saves the head's one
project, scratch/pantilt/pantilt.dsgn: parts, assembly, requirements, the
CAM job and the drawings. The sheets go to scratch/pantilt/drawings/ as PDF
and SVG.

    python3 generators/pantilt/build_drawings.py --port N

Three sheets:

- **base_plate**: what a machinist makes the 6082-T6 plate from -- its
  top, the section A-A through the bearing seat and the tripod thread,
  DETAIL C of the seat's shoulder at 5:1, and its underside; the pan
  bearing's K7 seat with its limits, the 3/8-16 UNC-2B thread and the
  four M4 tapped
  holes as callouts the model writes, the chamfers, datum features A and B
  and the feature control frames that name them (the seat perpendicular to
  A, its shoulder's runout to B, the M4s' position to A and B), the
  general tolerance, and the grade in the title block.
- **assembly**: the head (the camera it carries left off), three views,
  the parts list, and a balloon on every row laid out by the app
  (`layout_balloons`), none of whose leaders cross.
- **print**: the PETG parts, how each goes on the bed, and the printed-fit
  allowances the model was built with (asked of the app, never typed).

Every dimension is measured off the projection and every callout is the
model's own text, so a change upstream re-draws rather than going stale.
Geometry is picked by what it IS (a circle of the seat's diameter, the
plate's bottom face in the section), from the project variables, not by
toponame strings that a remodel would rename.

The stage only draws: what the sheets order by -- the plate's grade
(build_base.py) and the camera screw as the ASME B18.3 part it is bought as
(build_assembly.py) -- is the model's.
"""
import datetime
import json

import common
from common import OUT
import spec

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
    seat_d, seat_b = V["pan_brg_D"], V["pan_brg_B"]
    # MATERIAL is left blank: the sheet writes the part's grade.
    app.call("create_drawing", name="base_plate drawing", sheet={
        "size": "a3", "landscape": True,
        "title_block": {
            "project": "Pan/tilt head", "document": "Base plate",
            "author": AUTHOR, "date": DATE,
            "notes": "General tolerances ISO 2768-mK"}})
    tx, ty = 150.0, 200.0     # the top view's centre (the pan axis)
    # The section's centre (the axis at the underside): under the top view
    # by the plate's height and the room for the seat's Ø and its frame.
    px, py = tx, ty - s * (R + t) - 27.0
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
    relief = one(circles(gt, V["pan_relief_d"], s),
                 "relief in the top view")
    dim(app, top, "diameter", [ref(relief)], (70, -48))
    m4 = circles(gt, V["m4_tap_drill"], s)
    m4 = sorted(m4, key=lambda e: (-e["center"][1], e["center"][0]))
    if len(m4) != 4:
        raise SystemExit(f"drawings: expected 4 M4 holes, found {len(m4)}")
    m4_callout = dim(app, top, "pattern_callout", [ref(e) for e in m4],
                     (-40, 30))["id"]
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
    # The seat: Ø across its walls; its class and limits are the model's,
    # and where it goes is the app's (beyond the section along the walls).
    seat_dim = app.call("add_drawing_dim", view=sec, kind="diameter",
                        refs=[ref(wall)])["id"]

    # Underside: the tripod thread, and the chamfers the 6090 cuts.
    tri = one(circles(gb, V["tripod_drill"], 1.0), "tripod drill rim")
    dim(app, bot, "hole_callout", [ref(tri)], (0, 50))
    mouth = one(circles(gb, V["tripod_drill"] + 2 * V["edge_break"], 1.0),
                "tripod mouth chamfer")
    app.call("add_drawing_note", view=bot, ref=ref(mouth, extreme="bottom"),
             position=[bx, by - R - 24],
             text="{{edge_break}} × 45° CHAMFER: TRIPOD MOUTH,\n"
                  "SEAT MOUTH, BOTH OUTER RIMS")

    # Datum features: A the face it stands on, B the bearing seat. The
    # face's leftmost point, over both halves the tripod hole splits it
    # into (`extreme` reads every piece; an `end` only the reported one).
    app.call("add_drawing_note", view=sec, style="datum", text="A",
             ref=ref(bottom, extreme="left"),
             position=[left - 4, py - 12])
    b_end = end_toward(wall, lambda p: p[1])
    b_at = wall["p1"] if b_end == 1 else wall["p2"]
    side = -1 if b_at[0] < px else 1
    app.call("add_drawing_note", view=sec, style="datum", text="B",
             ref=ref(wall, end=b_end),
             position=[b_at[0] + side * 12, b_at[1] + 7])

    # The frames that name them. The seat's axis square to A, in a
    # cylindrical zone, written with the seat's size.
    fcf(app, dim=seat_dim, symbol="perpendicularity", tolerance_mm=0.02,
        diameter=True, datums=["A"])
    # The four M4s located from A and B, with their callout.
    fcf(app, dim=m4_callout, symbol="position", tolerance_mm=0.2,
        diameter=True, datums=["A", "B"])

    # DETAIL C: the seat's shoulder and the relief under it, at 5:1 -- the
    # step between the seat's floor and the relief's, which the section's
    # two depths only say by subtraction.
    detail_sheet(app, V, sec)

    # What the frames and dimensions cannot say, ruled, on the title block.
    # The stock is the CAM job's blank (cam.py, its report).
    plate = json.loads((OUT / "cam_report.json").read_text())["blank"]["plate"]
    notes = "\n".join([
        "NOTES",
        f"1\tSTOCK: {plate:g} THICK PLATE, EN 485-2",
        "2\tM4 HOLES: TAP BY HAND",
        f"3\tØ{seat_d:g} K7 SEAT: GAUGE BEFORE THE SETUP IS BROKEN",
        "4\tBREAK ALL OTHER EDGES 0.2 MAX; NO BURRS IN THE SEAT",
    ])
    app.call("add_drawing_note", text=notes, position=[410, 58])
    app.call("add_drawing_note", text="UNDERSIDE  1:1",
             position=[bx - 30, by + R + 6])
    return export(app, "base_plate")


def fcf(app, view=None, ref_=None, position=None, dim=None, **frame):
    """A feature control frame: with a leader to `ref_` on `view`, or
    stacked with dimension `dim`'s value."""
    args = {"style": "fcf", "fcf": frame}
    if dim is not None:
        args["dim"] = dim
    else:
        args.update(view=view, ref=ref_, position=list(position))
    return app.call("add_drawing_note", **args)


def detail_sheet(app, V, sec):
    """DETAIL C of the section: the seat's shoulder at 5:1, its width and
    the step down to the relief, and the shoulder's runout to B (what the
    bearing's outer ring is squared by)."""
    t, seat_b = V["base_t"], V["pan_brg_B"]
    seat_r, relief_r = V["pan_brg_D"] / 2, V["pan_relief_d"] / 2
    floor_z, relief_z = t - seat_b, t - V["relief_depth"]
    s, R = 5.0, 4.5
    cx, cz = 0.5 * (seat_r + relief_r), 0.5 * (floor_z + relief_z)
    dx, dy = 285.0, 122.0     # where the circle's centre lands
    det = app.call("add_drawing_view", detail={
        "view": sec, "center": [cx, 0.0, cz], "radius_mm": R, "label": "C"},
        scale=s, position=[dx, dy], name="detail C")["id"]
    app.call("update_drawing")
    g = geometry(app, det)

    def at_x(x):  # a wall: vertical, at model x from the circle's centre
        return lines(g, lambda a, b: abs(a[0] - b[0]) < 1e-6
                     and abs(a[0] - (dx + s * (x - cx))) < 0.02)

    def at_z(z):  # a floor
        return lines(g, lambda a, b: abs(a[1] - b[1]) < 1e-6
                     and abs(a[1] - (dy + s * (z - cz))) < 0.02)

    wall = one(at_x(seat_r), "seat wall in detail C")
    relief_wall = one(at_x(relief_r), "relief wall in detail C")
    floor = one(at_z(floor_z), "seat floor in detail C")
    relief_floor = one(at_z(relief_z), "relief floor in detail C")
    dim(app, det, "distance_x", [ref(relief_wall), ref(wall)],
        (0, dy + s * R + 6 - dy))
    dim(app, det, "distance_y", [ref(relief_floor), ref(floor)],
        (dx - s * R - 6 - min(relief_floor["p1"][0], floor["p1"][0]), 0))
    # The shoulder the outer ring bears on, square to its seat's axis: the
    # leader on the middle of the floor the detail shows.
    fcf(app, view=det, ref_=ref(floor), position=(dx + s * R + 38, dy - 14),
        symbol="circular_runout", tolerance_mm=0.03, datums=["B"])
    return det


# ---- the assembly ----------------------------------------------------------

def assembly_sheet(app, clock):
    """A3 at 1:2.5: the head from the front, its plan with the yoke and the
    tilt axis left off (what the pan drive is, under them), and a section
    on both axes from the side; a balloon on every part, the parts list."""
    occs = occurrences(app)  # before the drawing tab is made
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
    # The parts list first: the balloons number themselves from it.
    app.call("add_drawing_note", view=front, table="bom", text="",
             position=[410, 58])
    bom = app.call("list_drawing_notes")["notes"][-1]["resolved_text"]
    rows = [r.split("\t") for r in bom.split("\n")[1:]]
    print(f"parts list: {len(rows)} rows")
    clock("  parts list")
    # A balloon on every row, laid out by the app: each on the view that
    # draws the most of it, round that view, no two leaders crossing.
    laid = app.call("layout_balloons", views=[front, plan, side])
    print(f"balloons: {len(laid['placed'])} placed, crossings "
          f"{laid['crossings']}, unplaced {laid['unplaced']}")
    if laid["crossings"] or laid["unplaced"]:
        raise SystemExit("drawings: the balloons did not lay out cleanly")
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
    pan, tilt, worm = (spec.designation(app, V[k]) for k in (
        "pan_brg_d", "main_brg_d", "worm_brg_d"))
    if pan == tilt:  # PANTILT_PAN_BEARING=tilt: one pair size
        fits = [
            (f"{pan} outer ring seat", "hole", V["main_brg_D"]),
            (f"{pan} relief", "hole", V["relief_d"]),
            (f"{pan} journal (yoke, tilt shaft)", "shaft", V["main_brg_d"])]
    else:
        fits = [
            (f"{pan} outer ring seat (pan wheel)", "hole", V["pan_brg_D"]),
            (f"{pan} relief (pan wheel)", "hole", V["pan_relief_d"]),
            (f"{pan} journal (yoke)", "shaft", V["pan_brg_d"]),
            (f"{tilt} outer ring seat (arms)", "hole", V["main_brg_D"]),
            (f"{tilt} relief (arms)", "hole", V["relief_d"]),
            (f"{tilt} journal (tilt shaft)", "shaft", V["main_brg_d"])]
    fits += [
        (f"{worm} outer ring seat (blocks)", "hole", V["worm_brg_D"]),
        (f"{worm} journal (worm)", "shaft", V["worm_brg_d"]),
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
    app.call("open_project", path=str(common.stage("cam")))
    plate = app.part("base_plate", create=False)
    plate.activate()
    V = variables(app)
    # What the drawing measures to, derived the way build_base.py made it.
    V["relief_depth"] = V["pan_relief_depth"]
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
    common.save_final(app)
    (OUT / "drawings_report.json").write_text(json.dumps(
        {k: {"ok": v["ok"], "issues": v["issues"]} for k, v in report.items()},
        indent=1))
    dirty = [k for k, v in report.items() if not v["ok"]]
    if dirty:
        raise SystemExit(f"drawings: check_drawing found issues on "
                         f"{', '.join(dirty)} (drawings_report.json)")
    print("drawings: every sheet clean")


if __name__ == "__main__":
    main()
