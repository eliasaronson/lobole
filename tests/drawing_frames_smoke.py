#!/usr/bin/env python3
"""Integration test: the pan/tilt round's drawings (v151).

On a bored plate's sheet: datum features A and B, a perpendicularity frame
naming A with a leader to the bore, a position frame stacked under the
bore's Ø, the refusals that keep frames honest (a datum the sheet does not
have, removing a datum a frame names, a form tolerance with a datum), a
DETAIL of the section at 5:1, and vertical values written along their
lines. On an assembly sheet: balloons laid out by the app with no two
leaders crossing, an ASME B18.3 1/4-20 screw as a bought part, and the
plate's grade (6082-T6) in the parts list and the title block. All of it
survives a save and a reopen. Headless
(docs/drawings/README.md#feature-control-frames).

Usage: drawing_frames_smoke.py <path-to-designer-binary>
"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer, RemoteError  # noqa: E402
from smoke_app import launch_app  # noqa: E402
import smoke_shapes as shapes  # noqa: E402


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


def ref(e, **extra):
    r = {("sil_face" if e["kind"] == "silhouette" else "edge"): e["ref"]}
    r.update(extra)
    return r


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_dwgframes_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        d.call("new_project")
        d.call("add_part", name="plate")
        box = shapes.box(d, (-20, -10, 0), (40, 20, 10), "plate")
        refused(d, "set_material", "aluminum_6082_t6", feature_id=box["id"],
                material="6082")
        d.call("set_material", feature_id=box["id"],
               material="aluminum_6082_t6")
        centre = d.call("add_feature", feature={
            "type": "Sketch", "name": "centre",
            "plane": {"origin": [0, 0, 10], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "point", "p": [0, 0]}]})
        d.call("add_feature", feature={
            "type": "Hole", "name": "bore", "sketch_id": centre["id"],
            "hole_kind": "simple", "diameter": 8.0, "through": True})
        d.call("add_part", name="pin")
        shapes.box(d, (0, 0, 0), (5, 5, 5), "pin")
        d.call("add_assembly", name="asm")
        d.call("add_occurrence", part="plate", fixed=True)
        for i in range(4):
            d.call("add_occurrence", part="pin", transform={
                "position": [-18 + 10 * i, -2.5, 10],
                "orientation": [0, 0, 0, 1]})
        refused(d, "add_fastener", "#4-40", standard="asme_b18_3",
                size="1/4-28", length=19.05)
        screw = d.call("add_fastener", standard="asme_b18_3", size="1/4-20",
                       length=19.05, position=[25, 0, 10])
        expect(screw["part"] == "ASME B18.3 1/4-20 x 3/4",
               f"the screw is named as it is ordered: {screw}")
        d.call("solve_assembly")

        # --- the plate's sheet ----------------------------------------------
        d.call("create_drawing", name="plate-dwg",
               sheet={"size": "a3", "landscape": True})
        top = d.call("add_drawing_view", source="plate", orientation="top",
                     scale=2.0, position=[130, 200])["id"]
        sec = d.call("add_drawing_view", source="plate", orientation="front",
                     scale=2.0, position=[130, 80], base_view=top,
                     section={"point": [0, 0, 0], "label": "A"})["id"]
        d.call("set_drawing_view", id=sec, show_hidden=False)
        d.call("update_drawing")
        g = d.call("list_drawing_geometry", view=sec)["geometry"]
        walls = [e for e in g if e["type"] == "line"
                 and abs(e["p1"][0] - e["p2"][0]) < 1e-6
                 and abs(abs(e["p1"][0] - 130) - 8.0) < 0.05]
        bottom = [e for e in g if e["type"] == "line"
                  and abs(e["p1"][1] - 80) < 1e-3
                  and abs(e["p2"][1] - 80) < 1e-3]
        sides = [e for e in g if e["type"] == "line"
                 and abs(e["p1"][0] - e["p2"][0]) < 1e-6
                 and abs(abs(e["p1"][0] - 130) - 40.0) < 0.05]
        expect(walls and bottom and sides, "the section's walls and faces")

        # The Ø across the walls, placed by the app: beyond the view along
        # them, its line square to them.
        dia = d.call("add_drawing_dim", view=sec, kind="diameter",
                     refs=[{"sil_face": walls[0]["ref"]}])
        expect(abs(dia["offset"][0]) < 1e-6 and dia["offset"][1] > 10,
               f"a Ø across two lines goes along them, past the view: {dia}")
        # The thickness, written up its line.
        tall = d.call("add_drawing_dim", view=sec, kind="distance_y",
                      refs=[ref(bottom[0]), ref(sides[0], end=1)])
        expect(tall["resolved"], f"the thickness: {tall}")

        refused(d, "add_drawing_note", "I, O and Q", view=sec,
                style="datum", text="O", position=[60, 60],
                ref=ref(bottom[0], extreme="left"))
        d.call("add_drawing_note", view=sec, style="datum", text="A",
               position=[60, 60], ref=ref(bottom[0], extreme="left"))
        refused(d, "add_drawing_note", "already on this sheet", view=sec,
                style="datum", text="A", position=[70, 60],
                ref=ref(bottom[0], extreme="right"))
        d.call("add_drawing_note", view=sec, style="datum", text="B",
               position=[178, 106], ref={"sil_face": walls[0]["ref"],
                                         "end": 1})
        refused(d, "add_drawing_note", "datum C is not on this sheet",
                view=sec, style="fcf", position=[220, 110],
                ref={"sil_face": walls[0]["ref"], "end": 2},
                fcf={"symbol": "perpendicularity", "tolerance_mm": 0.02,
                     "diameter": True, "datums": ["C"]})
        refused(d, "add_drawing_note", "takes no datum", view=sec,
                style="fcf", position=[220, 110],
                ref={"sil_face": walls[0]["ref"], "end": 2},
                fcf={"symbol": "flatness", "tolerance_mm": 0.02,
                     "datums": ["A"]})
        perp = d.call("add_drawing_note", view=sec, style="fcf",
                      position=[220, 100],
                      ref={"sil_face": walls[0]["ref"], "end": 2},
                      fcf={"symbol": "perpendicularity", "tolerance_mm": 0.02,
                           "diameter": True, "datums": ["A"]})
        under = d.call("add_drawing_note", style="fcf", dim=dia["id"],
                       fcf={"symbol": "position", "tolerance_mm": 0.1,
                            "diameter": True, "modifier": "mmc",
                            "datums": ["A", "B"]})
        notes = {n["id"]: n for n in d.call("list_drawing_notes")["notes"]}
        expect(notes[under["id"]]["dim"] == dia["id"]
               and notes[under["id"]]["fcf"]["modifier"] == "mmc",
               f"the frame under the Ø: {notes[under['id']]}")
        datum_a = next(n for n in notes.values() if n["style"] == "datum"
                       and n["text"] == "A")
        refused(d, "remove_drawing_note", f"frame {perp['id']}",
                id=datum_a["id"])

        # A detail of the section's corner at 5:1.
        refused(d, "add_drawing_view", "parent view's",
                detail={"view": sec, "center": [4, 0, 10], "radius_mm": 3},
                source="plate", scale=5.0, position=[330, 100])
        det = d.call("add_drawing_view",
                     detail={"view": sec, "center": [4, 0, 10],
                             "radius_mm": 3},
                     scale=5.0, position=[330, 110])
        expect(det["label"] == "B", f"the next free view letter: {det}")
        d.call("update_drawing")
        views = {v["id"]: v for v in d.call("list_drawing_views")["views"]}
        dv = views[det["id"]]
        expect(dv["has_geometry"] and dv["curves"] > 0,
               f"the detail draws its parent's lines: {dv}")
        (lo, hi) = dv["bbox"]
        expect(abs((hi[0] - lo[0]) - 30.0) < 1e-6,
               f"the detail is its circle, 3 mm at 5:1: {dv['bbox']}")
        chk = d.call("check_drawing")
        expect(chk["ok"], f"the plate's sheet checks clean: {chk['issues']}")
        svg = os.path.join(tmp, "plate.svg")
        d.call("export_drawing", path=svg)
        text = open(svg).read()
        expect("DETAIL B (5:1)" in text, "the detail's caption")
        expect("rotate(-90 " in text, "a vertical value turned with its line")
        expect(">6082-T6<" in text, "the title block's grade, derived")
        d.call("export_drawing", path=os.path.join(tmp, "plate.pdf"))

        # --- the assembly's sheet -------------------------------------------
        d.call("create_drawing", name="asm-dwg",
               sheet={"size": "a3", "landscape": True})
        front = d.call("add_drawing_view", source="asm", orientation="top",
                       scale=2.0, position=[150, 170])["id"]
        d.call("update_drawing")
        bom = d.call("add_drawing_note", view=front, table="bom", text="",
                     position=[410, 58])
        rows = [r.split("\t") for r in bom["text"].split("\n")[1:]]
        by_part = {r[2]: r for r in rows}
        expect(by_part["plate"][4] == "6082-T6",
               f"the plate's grade in the parts list: {rows}")
        expect(by_part["ASME B18.3 1/4-20 x 3/4"][3] == "BOUGHT",
               f"the inch screw is bought: {rows}")
        laid = d.call("layout_balloons", views=[front])
        expect(len(laid["placed"]) == len(rows) and laid["crossings"] == 0
               and not laid["unplaced"],
               f"a balloon on every row, no leaders crossing: {laid}")
        again = d.call("layout_balloons", views=[front])
        expect(len(again["placed"]) == len(rows),
               "laid out again, not added twice")
        balloons = [n for n in d.call("list_drawing_notes")["notes"]
                    if n["style"] == "balloon"]
        expect(len(balloons) == len(rows), f"one per row: {balloons}")
        chk = d.call("check_drawing")
        expect(chk["ok"], f"the assembly sheet checks clean: {chk['issues']}")

        # --- saved and opened again -----------------------------------------
        proj = os.path.join(tmp, "frames.dsgn")
        d.call("save_project", path=proj)
        d.call("open_project", path=proj)
        d.call("set_active_part", name="plate-dwg")
        notes = d.call("list_drawing_notes")["notes"]
        frames = [n for n in notes if n["style"] == "fcf"]
        expect(len(frames) == 2 and all(n["resolved"] for n in frames),
               f"the frames reopen and resolve: {frames}")
        views = d.call("list_drawing_views")["views"]
        expect(any(v.get("detail", {}).get("label") == "B" for v in views),
               f"the detail reopens: {views}")
        print("drawing_frames_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
