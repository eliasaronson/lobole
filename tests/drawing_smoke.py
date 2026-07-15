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


def free_port():
    import socket
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]
    port = free_port()
    env = dict(os.environ)
    env.pop("DISPLAY", None)  # prove no display server is needed
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_dwg_")
    env["XDG_STATE_HOME"] = tmp
    app = subprocess.Popen([binary, "--headless", "--remote", str(port)],
                           env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    try:
        d = Designer.wait_for(port)
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
        wd = d.call("add_drawing_dim", view=front_id, kind="distance_x",
                    refs=[{"edge": vlines[0]["ref"], "occurrence": vlines[0]["source_id"]},
                          {"edge": vlines[-1]["ref"], "occurrence": vlines[-1]["source_id"]}],
                    offset=[0, -14])
        # The box is 40 mm wide; the shaft sits to its right, so the overall
        # width between outermost vertical edges is the model span.
        expect(wd["resolved"], f"width dim resolves: {wd}")
        expect(wd["value"] > 0, f"width dim measured: {wd}")

        # A driven dim re-measures from the geometry; it is read-only (measured,
        # never set). Confirm it appears in the list with a formatted value.
        dims = d.call("list_drawing_dims")["dims"]
        expect(len(dims) == 1 and dims[0]["resolved"] and dims[0]["text"],
               f"one resolved dim: {dims}")

        # --- Save, reopen, verify the drawing survives ---------------------
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
        expect(idx["version"] == 31, f"format v31: {idx.get('version')}")
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

        print("drawing_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
