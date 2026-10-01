#!/usr/bin/env python3
"""Integration test: the sheet furniture a shop drawing needs (v147).

A section view through a bored plate (hatching, its caption, the lettered
cutting plane on the view it folds from, a Ø across the bore's walls that
reads the toleranced class AND its limits), a datum symbol, the title
block's material and its derived scale, and on an assembly sheet a parts
list with balloons numbered from it -- an occurrence the view hides is
neither listed nor ballooned. Everything survives a save and a reopen.
Headless -- no display (docs/drawings/README.md#section-views).

Usage: drawing_sheet_smoke.py <path-to-designer-binary>
"""
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer, RemoteError  # noqa: E402
from smoke_app import launch_app  # noqa: E402


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def refused(d, method, needle, **params):
    try:
        d.call(method, **params)
    except RemoteError as e:
        expect(needle in str(e), f"{method} refused, naming {needle}: {e}")
        return
    raise AssertionError(f"{method} accepted {params}")


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_dwgsheet_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        d.call("new_project")
        # A 40 x 20 x 10 plate with a Ø8 bore through it, held to H7.
        d.call("add_part", name="plate")
        d.call("add_feature", feature={
            "type": "Box", "name": "plate", "position": [-20, -10, 0],
            "width": 40, "height": 20, "depth": 10})
        centre = d.call("add_feature", feature={
            "type": "Sketch", "name": "centre",
            "plane": {"origin": [0, 0, 10], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "point", "p": [0, 0]}]})
        d.call("add_feature", feature={
            "type": "Hole", "name": "bore", "sketch_id": centre["id"],
            "hole_kind": "simple", "diameter": 8.0, "through": True})
        bore = [(b["created_by"], f["name"])
                for b in d.call("list_elements")["bodies"]
                for f in b.get("faces", []) if f.get("kind") == "cylinder"]
        expect(len(bore) == 1, f"one bore face: {bore}")
        d.call("set_tolerance", feature_id=bore[0][0], element=bore[0][1],
               **{"class": "H7"})
        d.call("add_part", name="pin")
        d.call("add_feature", feature={
            "type": "Box", "name": "pin", "position": [0, 0, 0],
            "width": 5, "height": 5, "depth": 5})
        d.call("add_assembly", name="asm")
        d.call("add_occurrence", part="plate", fixed=True)
        pin = d.call("add_occurrence", part="pin", transform={
            "position": [30, 0, 0], "orientation": [0, 0, 0, 1]})
        spare = d.call("add_occurrence", part="plate", transform={
            "position": [0, 60, 0], "orientation": [0, 0, 0, 1]})
        d.call("solve_assembly")

        # --- the part's sheet: a top view and section A-A under it ----------
        refused(d, "create_drawing", "materal", name="bad",
                sheet={"size": "a3", "title_block": {"materal": "6082"}})
        d.call("create_drawing", name="plate-dwg", sheet={
            "size": "a3", "landscape": True,
            "title_block": {"material": "6082-T6"}})
        top = d.call("add_drawing_view", source="plate", orientation="top",
                     scale=2.0, position=[150, 200])["id"]
        refused(d, "add_drawing_view", "section is {point",
                source="plate", orientation="front", scale=2.0,
                position=[150, 80], section={"label": "A"})
        sec = d.call("add_drawing_view", source="plate", orientation="front",
                     scale=2.0, position=[150, 80], base_view=top,
                     section={"point": [0, 0, 0], "label": "A"})["id"]
        d.call("set_drawing_view", id=sec, show_hidden=False)
        d.call("update_drawing")
        g = d.call("list_drawing_geometry", view=sec)["geometry"]
        # The bore's walls in the section are its face, named like a
        # silhouette: one ref, reported once, from either side.
        walls = [e for e in g if e["type"] == "line"
                 and abs(e["p1"][0] - e["p2"][0]) < 1e-6
                 and abs(abs(e["p1"][0] - 150) - 8.0) < 0.05]
        expect(walls and walls[0]["kind"] == "silhouette",
               f"the bore's wall is its face in section: {walls}")
        dia = d.call("add_drawing_dim", view=sec, kind="diameter",
                     refs=[{"sil_face": walls[0]["ref"]}], offset=[0, 20])
        expect(dia["resolved"] and dia["text"] == "Ø8 H7 (+0.015/0)",
               f"a Ø across the walls reads the class and limits: {dia}")
        bottom = [e for e in g if e["type"] == "line"
                  and abs(e["p1"][1] - 80) < 1e-3
                  and abs(e["p2"][1] - 80) < 1e-3]
        expect(bottom, "the plate's underside in the section")
        refused(d, "add_drawing_note", "unknown note style", view=sec,
                style="datums", text="A", position=[100, 60],
                ref={("sil_face" if bottom[0]["kind"] == "silhouette"
                      else "edge"): bottom[0]["ref"], "extreme": "left"})
        datum = d.call("add_drawing_note", view=sec, style="datum", text="A",
                       position=[100, 60],
                       ref={("sil_face" if bottom[0]["kind"] == "silhouette"
                             else "edge"): bottom[0]["ref"],
                            "extreme": "left"})
        expect(datum["text"] == "A", f"the datum: {datum}")
        chk = d.call("check_drawing")
        expect(chk["ok"], f"the plate's sheet checks clean: {chk['issues']}")
        svg = os.path.join(tmp, "plate.svg")
        d.call("export_drawing", path=svg)
        text = open(svg).read()
        expect("SECTION A-A" in text, "the section's caption")
        expect(text.count('stroke-width="0.18"') > 10, "the hatching")
        expect('stroke-dasharray="6 1.2 0.6 1.2"' in text,
               "the cutting plane, a chain line, on the view it folds from")
        expect(">6082-T6<" in text and ">2:1<" in text,
               "the title block's material as typed and its scale as drawn")
        expect(d.call("export_drawing",
                      path=os.path.join(tmp, "plate.pdf")) is not None,
               "the PDF")

        # --- the assembly's sheet: the parts list and its balloons ----------
        d.call("create_drawing", name="asm-dwg",
               sheet={"size": "a3", "landscape": True})
        front = d.call("add_drawing_view", source="asm", orientation="top",
                       scale=1.0, position=[100, 180],
                       hidden=[spare["id"]])["id"]
        d.call("update_drawing")
        bom = d.call("add_drawing_note", view=front, table="bom", text="",
                     position=[410, 58])
        rows = bom["text"].split("\n")
        expect(rows[0].startswith("ITEM\tQTY\tPART"), f"the header: {rows}")
        expect(len(rows) == 3, f"two parts the view draws: {rows}")
        expect(rows[1].split("\t")[:3] == ["1", "1", "plate"],
               f"the hidden plate is not counted: {rows}")
        expect(rows[2].split("\t")[:3] == ["2", "1", "pin"], f"pin: {rows}")
        pin_curve = next(e for e in d.call("list_drawing_geometry",
                                           view=front)["geometry"]
                         if e["source_id"] == pin["id"] and e["visible"])
        kind = "sil_face" if pin_curve["kind"] == "silhouette" else "edge"
        balloon = d.call("add_drawing_note", view=front, style="balloon",
                         text="", position=[160, 230],
                         ref={kind: pin_curve["ref"],
                              "occurrence": pin["id"]})
        expect(balloon["text"] == "2", f"the pin's item number: {balloon}")
        chk = d.call("check_drawing")
        expect(chk["ok"], f"the assembly sheet checks clean: {chk['issues']}")
        svg = os.path.join(tmp, "asm.svg")
        d.call("export_drawing", path=svg)
        text = open(svg).read()
        expect(">pin<" in text and ">ITEM<" in text, "the table on paper")

        # --- saved and opened again ----------------------------------------
        proj = os.path.join(tmp, "sheet.dsgn")
        d.call("save_project", path=proj)
        d.call("open_project", path=proj)
        d.call("set_active_part", name="asm-dwg")
        notes = d.call("list_drawing_notes")["notes"]
        expect([n["table"] for n in notes] == ["bom", "none"]
               and notes[1]["style"] == "balloon"
               and notes[1]["resolved_text"] == "2",
               f"the list and its balloon reopen: {notes}")
        d.call("set_active_part", name="plate-dwg")
        views = {v["name"]: v for v in d.call("list_drawing_views")["views"]}
        expect(views["front"].get("section", {}).get("label") == "A",
               f"the section reopens: {views['front']}")
        print("drawing_sheet_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
