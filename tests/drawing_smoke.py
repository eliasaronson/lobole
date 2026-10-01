#!/usr/bin/env python3
"""Integration test: the drawings D1 sheet spine, driven headless.

Covers: creating a drawing tab (a third document kind) with an initial sheet
size + title block, list_drawings/list_parts reporting it, the clear error
when a part-only RPC runs on an active drawing tab, and a full save -> reopen
round-trip that preserves the drawing document and its sheet through the v2
project container.

Usage: drawing_smoke.py <path-to-designer-binary>
"""
import os
import subprocess
import sys
import tempfile
import json as jsonlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402




def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)  # prove no display server is needed
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_dwg_")
    env["XDG_STATE_HOME"] = tmp
    # ...and its own CONFIG dir, or the run edits the developer's real
    # preferences: set_settings persists immediately (headless runs no GUI
    # loop to debounce it), so a suite reset their font size to 120%.
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        expect(d.call("ping")["ok"], "ping")

        # --- A project with a part + a drawing -----------------------------
        d.call("new_project")  # starts with one part ("Part 1")
        d.call("add_feature", feature={
            "type": "Box", "name": "Blk", "position": [0, 0, 0],
            "width": 40, "height": 20, "depth": 20})
        # A cylinder body so views carry silhouettes (the "shaft" case).
        d.call("add_feature", feature={
            "type": "Cylinder", "name": "Shaft", "center": [60, 10, 0],
            "direction": [0, 0, 1], "radius": 8, "height": 30})

        dwg = d.call("create_drawing", name="bracket-dwg", sheet={
            "size": "a3", "landscape": True,
            "title_block": {"project": "Gearbox", "document": "Bracket",
                            "author": "E. Aronsson", "date": "2026-07-14"}})
        expect(dwg["id"] > 0, "drawing got a stable id")
        expect(dwg["size"] == "a3", f"sheet size applied: {dwg}")

        # list_drawings reports it; list_parts types it as a drawing.
        ld = d.call("list_drawings")["drawings"]
        expect(len(ld) == 1, f"one drawing: {ld}")
        expect(ld[0]["name"] == "bracket-dwg", f"named: {ld}")
        expect(ld[0]["views"] == 0 and ld[0]["dims"] == 0,
               f"empty of views/dims in D1: {ld}")

        lp = {p["name"]: p["type"] for p in d.call("list_parts")["parts"]}
        expect(lp.get("bracket-dwg") == "drawing", f"typed as drawing: {lp}")

        # The drawing tab is active. A part-only RPC must fail clearly.
        try:
            d.call("get_state")
            raise AssertionError("get_state must fail on an active drawing")
        except RuntimeError as e:
            expect("part" in str(e).lower(), f"clear error: {e}")

        # --- D2: projected views ------------------------------------------
        for orient, pos in [("front", [110, 220]), ("top", [110, 140]),
                            ("iso", [300, 190])]:
            r = d.call("add_drawing_view", source="Part 1", orientation=orient,
                       position=pos)
            expect(r["id"] > 0 and r["needs_update"], f"view added: {r}")
        # A view before update has no geometry (projection is explicit).
        pre = d.call("list_drawing_views")["views"]
        expect(all(v["curves"] == 0 for v in pre),
               f"views not projected before update: {pre}")
        # Update projects them; every view yields visible + hidden edges.
        upd = d.call("update_drawing")["views"]
        expect(len(upd) == 3, f"three views projected: {len(upd)}")
        for v in upd:
            expect(v["has_geometry"] and v["visible"] > 0 and v["hidden"] > 0,
                   f"view {v['name']} has visible+hidden edges: {v}")
        # The iso and front views see the cylinder as silhouette edges.
        by_name = {v["name"]: v for v in upd}
        expect(by_name["iso"]["silhouettes"] >= 2,
               f"iso has cylinder silhouettes: {by_name['iso']}")

        # --- D3: driven dimensions -----------------------------------------
        front_id = by_name["front"]["id"]
        geo = d.call("list_drawing_geometry", view=front_id)["geometry"]
        vlines = sorted([g for g in geo if g.get("type") == "line" and
                        abs(g["p1"][1] - g["p2"][1]) > 5], key=lambda g: g["p1"][0])
        expect(len(vlines) >= 2, f"front view has vertical box edges: {geo}")
        # The shaft's side lines are its face's silhouettes, the one on its
        # seam included (v147), so each is picked by what it is.
        def pick(g):
            key = "sil_face" if g["kind"] == "silhouette" else "edge"
            return {key: g["ref"], "occurrence": g["source_id"]}
        # The value stands ABOVE its line (ISO 129-1), so the line goes a
        # value's height lower than a label centred on it would need.
        wd = d.call("add_drawing_dim", view=front_id, kind="distance_x",
                    refs=[pick(vlines[0]), pick(vlines[-1])],
                    offset=[0, -18])
        # The box is 40 mm wide; the shaft sits to its right, so the overall
        # width between outermost vertical edges is the model span.
        expect(wd["resolved"], f"width dim resolves: {wd}")
        expect(wd["value"] > 0, f"width dim measured: {wd}")

        # A driven dim re-measures from the geometry; it is read-only (measured,
        # never set). Confirm it appears in the list with a formatted value.
        dims = d.call("list_drawing_dims")["dims"]
        expect(len(dims) == 1 and dims[0]["resolved"] and dims[0]["text"],
               f"one resolved dim: {dims}")

        # --- R1 (2026-08-26): wrong shapes are refusals, not type_errors ---
        try:
            d.call("create_drawing", name="bad-sheet", sheet="A4")
            raise AssertionError("string sheet must be refused")
        except RuntimeError as e:
            expect("sheet is an object" in str(e), f"names the shape: {e}")
        try:
            d.call("add_drawing_dim", view=front_id, kind="distance_x",
                   refs=[{"edge": vlines[0]["ref"]},
                         {"edge": vlines[-1]["ref"]}], offset=5)
            raise AssertionError("scalar offset must be refused")
        except RuntimeError as e:
            expect("[x, y]" in str(e), f"names the shape: {e}")
        # A distance with one pick, and a pick that names no curve the view
        # drew: both refused at the add, not left on the sheet dangling for
        # check_drawing to blame on "an edge consumed upstream".
        try:
            d.call("add_drawing_dim", view=front_id, kind="distance_y",
                   refs=[{"edge": vlines[0]["ref"],
                          "occurrence": vlines[0]["source_id"]}])
            raise AssertionError("a one-pick distance must be refused")
        except RuntimeError as e:
            expect("TWO picks" in str(e), f"says it needs two: {e}")
        try:
            d.call("add_drawing_dim", view=front_id, kind="distance_x",
                   refs=[{"edge": vlines[0]["ref"],
                          "occurrence": vlines[0]["source_id"]},
                         {"edge": {"feature_id": vlines[0]["ref"]["feature_id"],
                                   "element": "no/such/edge"},
                          "occurrence": vlines[0]["source_id"]}])
            raise AssertionError("an unresolvable dim must be refused")
        except RuntimeError as e:
            expect("does not resolve" in str(e), f"says why: {e}")
        expect(len(d.call("list_drawing_dims")["dims"]) == 1,
               "neither refusal left a dimension behind")
        try:
            d.call("set_drawing_sheet", title_block="W170S")
            raise AssertionError("string title_block must be refused")
        except RuntimeError as e:
            expect("object of strings" in str(e), f"names the shape: {e}")

        # --- R2 (2026-08-26): `distance` measures perpendicular to an edge.
        # An edge names a direction, not a point: anchoring it at its
        # midpoint measured the DIAGONAL (an edge-to-hole read 170 for a
        # 144-centred hole; two parallel edges with offset midpoints read 78
        # for a 45 mm rail). Perpendicular for edge cases; two holes stay
        # centre-to-centre.
        d.call("add_part", name="r2plate")
        d.call("set_active_part", name="r2plate")
        d.call("add_feature", feature={"type": "Box", "name": "plate",
               "position": [0, 0, 0], "width": 100, "height": 80,
               "depth": 10})
        sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "holes",
            "plane": {"origin": [0, 0, 10], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [
                {"type": "circle", "center": [30, 55], "radius": 10},
                {"type": "circle", "center": [70, 20], "radius": 6},
                {"type": "rect", "c1": [0, 60], "c2": [15, 70]}]})
        d.call("add_feature", feature={"type": "Extrude", "name": "cuts",
               "sketch_id": sk["id"], "distance": 1, "mode": "cut",
               "end_mode": "through_all", "reversed": True})
        d.call("create_drawing", name="r2-dwg", sheet={"size": "a4"})
        # Top view: the holes are drilled along Z (up), so the plan view is
        # the one that shows them as circles (Z-up world, docs/conventions.md#world-axes).
        v2 = d.call("add_drawing_view", source="r2plate",
                    orientation="top")
        g2 = d.call("list_drawing_geometry", view=v2["id"])["geometry"]
        vis = [e for e in g2 if e["visible"]]
        circles = sorted([e for e in vis if e["type"] == "circle"],
                         key=lambda e: -e["radius"])
        expect(len(circles) >= 2, f"both holes visible: {len(circles)}")
        vl2 = [e for e in vis if e["type"] == "line"
               and abs(e["p1"][0] - e["p2"][0]) < 1e-6]
        xs = sorted({round(e["p1"][0], 3) for e in vl2})
        expect(len(xs) == 3, f"left, notch, right verticals: {xs}")
        at_x = lambda x: next(e for e in vl2 if abs(e["p1"][0] - x) < 1e-3)

        def dval(a, b):
            r = d.call("add_drawing_dim", view=v2["id"], kind="distance",
                       refs=[{"edge": a["ref"]}, {"edge": b["ref"]}])
            d.call("remove_drawing_dim", id=r["id"])
            return r["value"]
        expect(abs(dval(at_x(xs[0]), circles[0]) - 30) < 0.1,
               "edge-to-hole is perpendicular (30, not the 39.1 diagonal)")
        expect(abs(dval(circles[0], circles[1]) -
                   (40 ** 2 + 35 ** 2) ** 0.5) < 0.1,
               "hole-to-hole stays centre-to-centre")
        expect(abs(dval(at_x(xs[2]), at_x(xs[1])) - 85) < 0.1,
               "offset parallel edges measure perpendicular (85, not 88.6)")
        # --- R12 (2026-08-26): auto-placement clears views, title block and
        # other labels. Seven no-offset dims on one view — the pile-up that
        # cost five real sheets 2-4 manual passes each — must land with ZERO
        # placement issues from check_drawing's overlap validator (which
        # shares LabelBox with the placement, so they cannot drift apart).
        def auto_dim(kind, refs):
            r = d.call("add_drawing_dim", view=v2["id"], kind=kind,
                       refs=[{"edge": e["ref"]} for e in refs])
            expect(r["resolved"], f"auto dim resolves: {r}")
        auto_dim("distance", [at_x(xs[0]), at_x(xs[2])])
        auto_dim("distance_x", [at_x(xs[0]), circles[0]])
        auto_dim("distance_x", [at_x(xs[0]), circles[1]])
        hl2 = [e for e in vis if e["type"] == "line"
               and abs(e["p1"][1] - e["p2"][1]) < 1e-6]
        ys2 = sorted({round(e["p1"][1], 3) for e in hl2})
        at_y = lambda y: next(e for e in hl2 if abs(e["p1"][1] - y) < 1e-3)
        auto_dim("distance", [at_y(ys2[0]), at_y(ys2[-1])])
        auto_dim("distance_y", [at_y(ys2[0]), circles[0]])
        auto_dim("diameter", [circles[0]])
        auto_dim("diameter", [circles[1]])
        chk = d.call("check_drawing")
        placement = [i for i in chk["issues"]
                     if i["kind"] in ("dim_labels_overlap",
                                      "dim_over_title_block",
                                      "dim_label_over_view",
                                      "dim_outside_border")]
        expect(not placement,
               f"seven auto-placed dims, zero collisions: {placement}")
        # R13: centre marks on every visible circle in the export.
        svg_path = os.path.join(tmp, "r2.svg")
        d.call("export_drawing", path=svg_path)
        expect(open(svg_path).read().count('stroke-width="0.15"') >= 4,
               "centre marks on both holes in the SVG")
        # ...and the validator flags a label parked mid-view on purpose.
        r = d.call("add_drawing_dim", view=v2["id"], kind="distance",
                   refs=[{"edge": at_x(xs[0])["ref"]},
                         {"edge": at_x(xs[2])["ref"]}], offset=[0, 0])
        chk = d.call("check_drawing")
        expect(any(i["kind"] == "dim_label_over_view"
                   for i in chk["issues"]),
               f"mid-view label flagged: {chk['issues']}")
        d.call("remove_drawing_dim", id=r["id"])

        # --- D4 (2026-09-03): four auto-placed views of a part the size of
        # the SO-101 gripper body (87 x 43 x 60 mm) on A3 came out as one
        # pile: the wrapped row was clamped back up onto the views already
        # there. Auto-placement must lay the sheet out (one scale, no view on
        # another, none on the title block) or say so.
        d.call("add_part", name="tallbody")
        d.call("add_feature", feature={"type": "Box", "name": "body",
               "position": [-5, -22, -28], "width": 87, "height": 43,
               "depth": 60})
        d.call("create_drawing", name="tallbody-dwg", sheet={"size": "a3"})
        ids = [d.call("add_drawing_view", source="tallbody", orientation=o)["id"]
               for o in ("front", "top", "right", "iso")]
        views = d.call("list_drawing_views")["views"]
        scales = {round(v["scale"], 6) for v in views}
        expect(len(scales) == 1, f"one scale per sheet: {scales}")

        # What the view COST, by the name a reader can act on. An imported
        # driver was 16 s of an 18 s assembly view and nothing said which
        # occupant to leave out of `hidden`; the clocks the hide loop already
        # keeps are attributed per source now
        # (docs/drawings/README.md#what-a-view-costs). Asserted on an ordinary
        # view: the field exists without needing pathological geometry, and
        # only the WARNING waits for a slow one.
        cost = views[0]["cost"]
        expect("by_occurrence" in cost, f"cost names its sources: {cost}")
        for entry in cost["by_occurrence"]:
            expect(entry["ms"] >= 0.0, f"a cost is not negative: {entry}")
            expect("id" in entry, f"a cost names its source id: {entry}")
        chk = d.call("check_drawing")
        piles = [i for i in chk["issues"]
                 if i["kind"] in ("view_overlap", "view_overlaps_title_block",
                                  "view_outside_border")]
        expect(not piles, f"four auto-placed views, no pile: {piles}")
        d.call("delete_part", part="tallbody-dwg")
        d.call("delete_part", part="tallbody")

        # Leave the project as the later save/reopen assertions expect it.
        tabs = d.call("list_parts")["parts"]
        for t in sorted(tabs, key=lambda t: -t["index"]):
            if t["name"] in ("r2-dwg", "r2plate"):
                d.call("delete_part", index=t["index"])
        d.call("set_active_part", name="bracket-dwg")  # back to the D3 sheet

        # --- Save, reopen, verify the drawing survives ---------------------
        # --- R13: notes (free + leadered) persist and export ---------------
        d.call("add_drawing_note", text="MDF 19 - break all edges",
               position=[40, 180])
        d.call("add_drawing_note", text="datum edge", position=[240, 180],
               view=front_id, ref=vlines[0]["ref"])
        notes = d.call("list_drawing_notes")["notes"]
        expect(len(notes) == 2 and notes[1]["leader_resolved"],
               f"two notes, leader resolved: {notes}")
        nsvg = os.path.join(tmp, "notes.svg")
        d.call("export_drawing", path=nsvg)
        expect("break all edges" in open(nsvg).read(), "note text exported")
        try:
            d.call("add_drawing_note", text="", position=[0, 0])
            raise AssertionError("empty note text must refuse")
        except RuntimeError as e:
            expect("non-empty" in str(e), f"note refusal: {e}")

        proj = os.path.join(tmp, "gearbox.dsgn")
        d.call("save_project", path=proj)

        # On disk: project.json v2 with a typed "drawing" document + its dir.
        with open(os.path.join(proj, "project.json")) as f:
            pj = jsonlib.load(f)
        types = {docu["name"]: docu["type"] for docu in pj["documents"]}
        expect(types.get("bracket-dwg") == "drawing", f"drawing typed: {types}")
        dwg_dir = os.path.join(proj, "drawings", "bracket-dwg")
        expect(os.path.isdir(dwg_dir), "drawing directory written")
        with open(os.path.join(dwg_dir, "index.json")) as f:
            idx = jsonlib.load(f)
        expect(idx["kind"] == "drawing", f"kind discriminator: {idx.get('kind')}")
        expect(idx["version"] >= 30, f"format >= v30 (drawings): {idx.get('version')}")
        expect(idx["sheet"]["size"] == "a3", f"sheet persisted: {idx['sheet']}")
        expect(idx["sheet"]["title_block"]["project"] == "Gearbox",
               "title block persisted")
        expect(len(idx["views"]) == 3, f"three views persisted: {idx['views']}")
        expect(os.path.isdir(os.path.join(dwg_dir, "views")),
               "views directory written")
        expect(len(idx["dims"]) == 1, f"the dimension persisted: {idx['dims']}")
        expect(os.path.isdir(os.path.join(dwg_dir, "dims")),
               "dims directory written")

        # Reopen in a fresh instance and re-check.
        d.call("new_project")
        d.call("open_project", path=proj)
        lp2 = {p["name"]: p for p in d.call("list_parts")["parts"]}
        expect(set(lp2) == {"Part 1", "bracket-dwg"},
               f"all tabs reopened: {set(lp2)}")
        expect(len(d.call("list_drawing_notes")["notes"]) == 2,
               "notes survive the save/reopen round-trip")
        expect(lp2["bracket-dwg"]["type"] == "drawing",
               "bracket-dwg still a drawing after reopen")
        ld2 = d.call("list_drawings")["drawings"]
        expect(len(ld2) == 1 and ld2[0]["name"] == "bracket-dwg",
               f"drawing survived reopen: {ld2}")
        expect(ld2[0]["views"] == 3, f"views survived reopen: {ld2}")

        # --- D2: the drawing re-projects on open; export SVG closes the loop.
        lv = d.call("list_drawing_views")["views"]
        expect(all(not v["stale"] and v["has_geometry"] for v in lv),
               f"reopened views re-projected fresh: {lv}")
        svg_path = os.path.join(tmp, "bracket.svg")
        exp = d.call("export_drawing", path=svg_path)
        expect(exp["views"] == 3, f"exported three views: {exp}")
        svg = open(svg_path).read()
        # Parses as XML, is an <svg> with the A3 landscape viewBox, and carries
        # polylines (solid + dashed) and the title-block text. Never byte-golden
        # (HLR discretization is not stable across OCC versions).
        import xml.dom.minidom as minidom
        minidom.parseString(svg)  # raises on malformed XML
        expect('viewBox="0 0 420 297"' in svg, "A3 landscape viewBox")
        expect(svg.count("<polyline") >= 20, f"has view polylines: {svg.count('<polyline')}")
        expect("stroke-dasharray" in svg, "hidden edges are dashed")
        expect("Gearbox" in svg, "title block project in the SVG")

        # The dimension survived the round-trip and re-measures on open.
        dims2 = d.call("list_drawing_dims")["dims"]
        expect(len(dims2) == 1 and dims2[0]["resolved"],
               f"dimension survived reopen and re-measured: {dims2}")
        dim_id = dims2[0]["id"]

        # PDF export produces a valid single-page vector document.
        pdf_path = os.path.join(tmp, "bracket.pdf")
        d.call("export_drawing", path=pdf_path)
        pdf = open(pdf_path, "rb").read()
        expect(pdf.startswith(b"%PDF-1."), "PDF header")
        expect(pdf.rstrip().endswith(b"%%EOF"), "PDF trailer")
        expect(b"/BaseFont /Helvetica" in pdf, "PDF font")
        expect(b"startxref" in pdf, "PDF xref")

        # --- Round 2: layout validation + view/sheet editing -----------------
        chk = d.call("check_drawing")
        expect(chk["ok"] and chk["issues"] == [],
               f"well-laid-out sheet validates clean: {chk}")
        # Sheet-size strictness: case-insensitive accept, loud reject.
        up = d.call("set_drawing_sheet", size="A3")
        expect(up["size"] == "a3", f"'A3' accepted case-insensitively: {up}")
        try:
            d.call("set_drawing_sheet", size="b5")
            raise AssertionError("unknown sheet size must error")
        except RuntimeError as e:
            expect("a4, a3, a2, a1, a0, letter" in str(e),
                   f"size error lists accepted values: {e}")
        try:
            d.call("create_drawing", name="bad", sheet={"size": "ansi_b"})
            raise AssertionError("create_drawing bad size must error")
        except RuntimeError:
            pass
        expect(len(d.call("list_drawings")["drawings"]) == 1,
               "failed create_drawing left no half-configured tab")

        # set_drawing_view: park the iso off the sheet -> check flags it,
        # and the export reply echoes the same issues.
        iso_id = by_name["iso"]["id"]
        d.call("set_drawing_view", id=iso_id, position=[500, 190])
        chk = d.call("check_drawing")
        expect(not chk["ok"] and any(i["kind"] == "view_outside_border"
                                     and i.get("view") == iso_id
                                     for i in chk["issues"]),
               f"off-sheet view reported: {chk}")
        exp2 = d.call("export_drawing", path=os.path.join(tmp, "issues.svg"))
        expect(any(i["kind"] == "view_outside_border" for i in exp2["issues"]),
               f"export echoes issues: {exp2}")
        # Drop it onto the title block -> overlap issue; then restore.
        d.call("set_drawing_view", id=iso_id, position=[350, 30])
        chk = d.call("check_drawing")
        expect(any(i["kind"] == "view_overlaps_title_block"
                   for i in chk["issues"]),
               f"title-block collision reported: {chk}")
        d.call("set_drawing_view", id=iso_id, position=[300, 190])
        expect(d.call("check_drawing")["ok"], "restored layout is clean")

        # Discovery hints: bbox on views; visible/closed/source_feature on
        # geometry rows (the Ø-dim-measured-a-corner-fillet fix).
        lv = {v["id"]: v for v in d.call("list_drawing_views")["views"]}
        expect(lv[front_id]["bbox"] is not None and
               len(lv[front_id]["bbox"]) == 2,
               f"projected view exposes its bbox: {lv[front_id]}")
        geo2 = d.call("list_drawing_geometry", view=front_id)["geometry"]
        expect(all("visible" in g for g in geo2),
               f"geometry rows carry visibility: {geo2[0]}")
        expect(any(g.get("source_feature", {}).get("type") == "Box"
                   for g in geo2),
               f"geometry rows name their source feature: {geo2[0]}")
        circles = [g for g in geo2 if g.get("type") == "circle"]
        expect(all("closed" in g for g in circles),
               f"circles carry the closed hint: {circles}")

        # Auto-placement: a dim with NO offset lands clear of the view, not
        # at the old useless {0,0}.
        auto = d.call("add_drawing_dim", view=front_id, kind="distance_x",
                      refs=[pick(vlines[0]), pick(vlines[-1])])
        expect(auto["resolved"] and auto["offset"] != [0, 0],
               f"omitted offset is auto-placed: {auto}")
        chk = d.call("check_drawing", update=False)
        expect(not any(i["kind"] == "dim_outside_border" and
                       i.get("dim") == auto["id"] for i in chk["issues"]),
               f"auto-placed dim stays on the paper: {chk}")
        d.call("remove_drawing_dim", id=auto["id"])

        # Folding: the top view folds from front, sharing sheet-X. Moving its
        # x is overridden; the reply says so.
        top_id = by_name["top"]["id"]
        d.call("set_drawing_view", id=top_id, base_view=front_id)
        moved = d.call("set_drawing_view", id=top_id, position=[250, 140])
        expect(moved["effective_position"][0] == 110.0,
               f"folded x follows the base: {moved}")
        expect("x" in moved.get("folded_axes", []) and "note" in moved,
               f"fold override is called out: {moved}")
        d.call("set_drawing_view", id=top_id, base_view=0)  # unfold again

        # --- D3: dangling refs are first-class, never rebound ---------------
        # Delete the box feature its width dim references; the projected edges
        # vanish, so the dim reads dangling after an explicit update.
        part_idx = [p for p in d.call("list_parts")["parts"]
                    if p["name"] == "Part 1"][0]["index"]
        d.call("set_active_part", index=part_idx)
        d.call("delete_feature", index=0)  # the Box
        dwg_idx = [p for p in d.call("list_parts")["parts"]
                   if p["name"] == "bracket-dwg"][0]["index"]
        d.call("set_active_part", index=dwg_idx)
        d.call("update_drawing")
        after = {x["id"]: x for x in d.call("list_drawing_dims")["dims"]}
        expect(not after[dim_id]["resolved"],
               f"dim dangles after its edges are removed: {after[dim_id]}")

        # A hole callout with NO offset is auto-placed CLEAR of the view.
        # Auto-placement re-resolved the dim itself, without the source
        # document a callout needs, so it fell through to the fallback
        # offset and landed 12 mm from its own hole — on the part. Every
        # sheet with a hole callout on it was wrong.
        d.call("add_part", name="callout-plate")
        d.call("add_feature", feature={
            "type": "Box", "name": "plate", "position": [-50, -40, 0],
            "width": 100, "height": 80, "depth": 3})
        centres = d.call("add_feature", feature={
            "type": "Sketch", "name": "centres",
            "plane": {"origin": [0, 0, 3], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "point", "p": [10, 8]}]})
        d.call("add_feature", feature={
            "type": "Hole", "name": "bore", "sketch_id": centres["id"],
            "hole_kind": "simple", "diameter": 3.2, "through": True,
            "fit": {"thread": "M3", "kind": "close"}})
        d.call("create_drawing", name="callout-dwg", sheet={"size": "a3"})
        cv = d.call("add_drawing_view", source="callout-plate",
                    orientation="top", scale=1.0)
        d.call("update_drawing")
        bore = [e for e in d.call("list_drawing_geometry",
                                  view=cv["id"])["geometry"]
                if e["type"] == "circle" and e.get("visible")
                and e.get("closed") and abs(e["radius"] - 1.6) < 0.05]
        expect(len(bore) == 1, f"one bore on the sheet: {len(bore)}")
        callout = d.call("add_drawing_dim", view=cv["id"],
                         kind="hole_callout",
                         refs=[{"edge": bore[0]["ref"]}])
        expect(callout["resolved"], f"callout resolves: {callout}")
        # This asserted endswith("THRU") and called it "reads the fit", which
        # it never did — the callout said Ø3.2 and the recorded M3 close
        # clearance went unmentioned all the way to the sheet.
        expect(callout["text"] == "\u00d83.2 THRU, M3 CLOSE CLEARANCE",
               f"the callout names the fit the hole recorded: {callout}")
        # A DIAMETER dim on the same bore picks up the tolerance the model
        # records (docs/tolerances/README.md#drawings) — provenance, not a new
        # authored field: the feature ids in the projected circle's mapped
        # name narrow the candidates and the radius cross-checks them.
        d.call("set_active_part", name="callout-plate")
        bore_face = [(b["created_by"], f["name"])
                     for b in d.call("list_elements")["bodies"]
                     for f in b.get("faces", [])
                     if f.get("kind") == "cylinder"]
        expect(len(bore_face) == 1, f"one bore face: {bore_face}")
        d.call("set_tolerance", feature_id=bore_face[0][0],
               element=bore_face[0][1], **{"class": "H8"})
        d.call("set_active_part", name="callout-dwg")
        d.call("update_drawing")
        dia = d.call("add_drawing_dim", view=cv["id"], kind="diameter",
                     refs=[{"edge": bore[0]["ref"]}])
        # ...and its limits, which are what the shop holds (v147).
        expect(dia["text"] == "\u00d83.2 H8 (+0.018/0)",
               f"the dim reads the band the model records: {dia}")

        # A RADIUS dim must NOT: an ISO class states how a DIAMETER varies,
        # so "R1.6 H8" would be a band twice the one written.
        rad = d.call("add_drawing_dim", view=cv["id"], kind="radius",
                     refs=[{"edge": bore[0]["ref"]}])
        expect(rad["text"] == "R1.6", f"a radius takes no class: {rad}")

        report = d.call("check_drawing", update=True)
        over = [i for i in report["issues"]
                if i["kind"] == "dim_label_over_view"]
        expect(not over, f"auto-placed callout is clear of the view: {over}")

        print("drawing_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
