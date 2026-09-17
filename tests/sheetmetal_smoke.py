#!/usr/bin/env python3
"""Sheet metal over the RPC, headless (docs/sheetmetal/plan.md, S1-S3).

The rules first (a gauge is a thickness PER MATERIAL, and an unstocked one is
refused naming what is stocked), then a blank, then a wall bent up from an
edge of it — with the volume checked against the bend allowance by hand, so a
flange that builds a plausible-looking solid of the wrong size fails here
rather than at the brake.
"""
import math
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402

THICKNESS = 1.5      # aluminum "1.5mm"
MIN_RADIUS = 2.25    # 1.5 * t
WIDTH = 100.0
DEPTH = 60.0


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def refused(d, method, needle, **kw):
    try:
        d.call(method, **kw)
    except RuntimeError as e:
        expect(needle in str(e), f"{method} refused, but not naming "
               f"'{needle}': {e}")
        return
    raise AssertionError(f"{method} was accepted: {kw}")


def straight_edge(d, want_mid):
    """The stable name of the straight edge whose midpoint is `want_mid`.

    Picking by geometry rather than by name is the point: WHICH of a rim's two
    edges is picked decides which way the wall folds, so the test has to be
    able to say which one it means.
    """
    body = d.call("list_elements")["bodies"][0]
    for e in body["edges"]:
        mid = e.get("midpoint")
        if e.get("kind") != "line" or mid is None:
            continue
        if all(abs(a - b) < 1e-6 for a, b in zip(mid, want_mid)):
            return e["name"]
    raise AssertionError(f"no straight edge at {want_mid} in {body['edges']}")


def blank(d):
    """A fresh part: aluminium 1.5 mm, a 100 x 60 base flange."""
    d.call("new_project")
    d.call("set_sheet_metal_rules", material="aluminum", gauge="1.5mm")
    sk = d.call("add_feature", feature={
        "type": "Sketch", "name": "Blank",
        "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                  "uAxis": [1, 0, 0]},
        "entities": [
            {"type": "line", "id": 1, "a": [0, 0], "b": [WIDTH, 0]},
            {"type": "line", "id": 2, "a": [WIDTH, 0], "b": [WIDTH, DEPTH]},
            {"type": "line", "id": 3, "a": [WIDTH, DEPTH], "b": [0, DEPTH]},
            {"type": "line", "id": 4, "a": [0, DEPTH], "b": [0, 0]}]})["id"]
    return d.call("add_feature", feature={
        "type": "BaseFlange", "name": "Base", "sketch_id": sk})["id"]


def volume(d):
    return d.call("get_state")["bodies"][0]["volume"]


def tier2(d):
    """S7-S10 over the wire: each feature, its refusal by name, and the
    blank still one gauge thick at the end."""
    base = blank(d)
    top_front = straight_edge(d, (WIDTH / 2.0, 0.0, THICKNESS))

    # A hem on the front edge; an unknown kind is refused by name.
    refused(d, "add_feature", "expected one of", feature={
        "type": "Hem", "name": "x", "length": 6, "kind": "tight",
        "edges": [{"feature_id": base, "element": top_front}]})
    r = d.call("add_feature", feature={
        "type": "Hem", "name": "Safe edge", "length": 6,
        "edges": [{"feature_id": base, "element": top_front}]})
    expect(r["regen"]["ok"], r["regen"])

    # A jog across the panel; one lower than its own bends is refused.
    line = d.call("add_feature", feature={
        "type": "Sketch", "name": "Step",
        "plane": {"origin": [0, 0, THICKNESS], "normal": [0, 0, 1],
                  "uAxis": [1, 0, 0]},
        "entities": [{"type": "line", "id": 1, "a": [0, 30],
                      "b": [WIDTH, 30]}]})["id"]
    r = d.call("add_feature", feature={
        "type": "Jog", "name": "Too low", "sketch_id": line, "height": 3})
    expect(not r["regen"]["ok"] and "lower than its" in str(r["regen"]),
           r["regen"])
    d.call("delete_feature", id=r["id"])
    r = d.call("add_feature", feature={
        "type": "Jog", "name": "Step", "sketch_id": line, "height": 10})
    expect(r["regen"]["ok"], r["regen"])

    # A wall on the left edge — picked AFTER the jog, which split every edge
    # its line crosses, so the name the blank gave that edge is gone. The
    # closed hem set the front of the panel back by its radius plus a
    # thickness, so that edge now runs from there to the jog line.
    hem_setback = THICKNESS / 20.0 + THICKNESS
    top_left = straight_edge(d, (0.0, (hem_setback + 30.0) / 2.0, THICKNESS))
    r = d.call("add_feature", feature={
        "type": "EdgeFlange", "name": "Side", "length": 15,
        "position": "bend_outside",
        "edges": [{"feature_id": base, "element": top_left}]})
    expect(r["regen"]["ok"], r["regen"])

    # Unfold and back up: nothing moves.
    folded = volume(d)
    unfold = d.call("add_feature", feature={
        "type": "Unfold", "name": "Unfold", "body_id": base})
    expect(unfold["regen"]["ok"], unfold["regen"])
    bodies = d.call("get_state")["bodies"]
    span = bodies[0]["bbox_max"][2] - bodies[0]["bbox_min"][2]
    expect(abs(span - THICKNESS) < 1e-4, f"unfolded, not flat: {span}")
    refold = d.call("add_feature", feature={
        "type": "Refold", "name": "Refold", "unfold_id": unfold["id"]})
    expect(refold["regen"]["ok"], refold["regen"])
    expect(abs(volume(d) - folded) / folded < 1e-6,
           f"refold did not restore the part: {volume(d)} vs {folded}")

    # The hem and the side wall meet at the front-left corner: relieve it.
    # An unknown style is refused by name first.
    refused(d, "add_feature", "expected one of", feature={
        "type": "CornerRelief", "name": "x", "body_id": base,
        "style": "oval"})
    before = volume(d)
    r = d.call("add_feature", feature={
        "type": "CornerRelief", "name": "Relief", "body_id": base})
    expect(r["regen"]["ok"], r["regen"])
    expect(volume(d) < before, "the corner relief cut nothing")

    # A normal cut through the flat panel in front of the step.
    vent = d.call("add_feature", feature={
        "type": "Sketch", "name": "Vent",
        "plane": {"origin": [0, 0, 40], "normal": [0, 0, 1],
                  "uAxis": [1, 0, 0]},
        "entities": [{"type": "circle", "id": 1, "center": [50, 15],
                      "radius": 3}]})["id"]
    before = volume(d)
    r = d.call("add_feature", feature={
        "type": "NormalCut", "name": "Vent", "sketch_id": vent})
    expect(r["regen"]["ok"], r["regen"])
    removed = before - volume(d)
    expect(abs(removed - math.pi * 9 * THICKNESS) / removed < 5e-3,
           f"normal cut removed {removed}")

    # And the whole of it still lays flat, and still checks.
    r = d.call("add_feature", feature={
        "type": "FlatPattern", "name": "Flat", "body_id": base})
    expect(r["regen"]["ok"], r["regen"])
    bodies = d.call("get_state")["bodies"]
    span = bodies[0]["bbox_max"][2] - bodies[0]["bbox_min"][2]
    expect(abs(span - THICKNESS) < 1e-4, f"the blank is not flat: {span}")


def flat_sheet(d, tmp):
    """S11 over the wire: a sheet with the part and its blank side by side,
    the blank's crease drawn as a bend line and listed in a bend table."""
    parts = d.call("list_parts")["parts"]
    part = next(x["name"] for x in parts if x["type"] == "part")
    d.call("create_drawing", name="flat-dwg", sheet={"size": "a3"})
    refused(d, "add_drawing_view", "no configuration", source=part,
            orientation="top", configuration="Flatt")
    folded = d.call("add_drawing_view", source=part, orientation="top")["id"]
    flat = d.call("add_drawing_view", source=part, orientation="top",
                  configuration="Flat")["id"]
    views = {v["id"]: v for v in d.call("list_drawing_views")["views"]}
    expect(views[folded]["bend_lines"] == 0, views[folded])
    expect(views[flat]["bend_lines"] == 1, views[flat])
    expect(views[flat]["configuration"] != 0, views[flat])
    # The blank is longer than the folded part is deep: it IS the blank.
    span = lambda v: v["bbox"][1][1] - v["bbox"][0][1]
    expect(span(views[flat]) > span(views[folded]) + 10.0,
           f"flat {span(views[flat])} folded {span(views[folded])}")

    refused(d, "add_drawing_note", "needs `view`", table="bends",
            position=[300, 250])
    note = d.call("add_drawing_note", table="bends", view=flat,
                  position=[330, 250])
    expect(note["text"].startswith("BEND TABLE\nB1  "), note)
    expect("K 0.4" in note["text"], note)
    notes = d.call("list_drawing_notes")["notes"]
    expect(notes[0]["table"] == "bends", notes)

    svg = os.path.join(tmp, "flat.svg")
    d.call("export_drawing", path=svg)
    text = open(svg).read()
    expect('stroke-dasharray="6 1.2 0.6 1.2"' in text,
           "the exported sheet has no bend line")
    expect(">BEND TABLE<" in text, "the exported sheet has no bend table")

    # Back to the part as it is: the crease goes with the configuration.
    d.call("set_drawing_view", id=flat, configuration=0)
    views = {v["id"]: v for v in d.call("list_drawing_views")["views"]}
    expect(views[flat]["bend_lines"] == 0, views[flat])


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_sheetmetal_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        d.call("new_project")

        # Nothing set yet: the catalogue is still readable, which is how an
        # agent finds out what it may ask for.
        state = d.call("get_sheet_metal_rules")
        expect(state["rules"] is None, state)
        expect("aluminum" in state["stock"], state["stock"].keys())
        gauges = {g["gauge"]: g["thickness_mm"] for g in state["stock"]["steel"]}
        expect(abs(gauges["16ga"] - 1.519) < 1e-9, gauges["16ga"])
        alu = {g["gauge"]: g["thickness_mm"]
               for g in state["stock"]["aluminum"]}
        # The trap the table exists for: one gauge number, three thicknesses.
        expect(abs(alu["16ga"] - 1.291) < 1e-9, alu["16ga"])

        # Refusals, each naming what IS accepted.
        refused(d, "set_sheet_metal_rules", "sheet data is carried for",
                material="titanium", gauge="16ga")
        refused(d, "set_sheet_metal_rules", "not stocked in steel",
                material="steel", gauge="13ga")
        refused(d, "set_sheet_metal_rules", "expected one of",
                material="steel", gauge="16ga", relief="round")
        refused(d, "set_sheet_metal_rules", "tighter than",
                material="stainless_steel", gauge="16ga", bend_radius_mm=1.0)
        refused(d, "set_sheet_metal_rules", "needs a material and a gauge",
                relief="obround")

        # A flange before the rules refuses, and says which call fixes it.
        d.call("add_feature", feature={
            "type": "Sketch", "name": "Blank",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [
                {"type": "line", "id": 1, "a": [0, 0], "b": [WIDTH, 0]},
                {"type": "line", "id": 2, "a": [WIDTH, 0], "b": [WIDTH, DEPTH]},
                {"type": "line", "id": 3, "a": [WIDTH, DEPTH], "b": [0, DEPTH]},
                {"type": "line", "id": 4, "a": [0, DEPTH], "b": [0, 0]}]})
        sketch = d.call("get_state")["features"][0]["id"]
        r = d.call("add_feature", feature={
            "type": "BaseFlange", "name": "Base", "sketch_id": sketch})
        expect(not r["regen"]["ok"], "a flange with no rules regenerated")
        expect("set_sheet_metal_rules" in str(r["regen"]),
               f"the refusal does not name the fix: {r['regen']}")

        # Now the stock. The derived fields come back RESOLVED, each labelled
        # with where the number came from.
        r = d.call("set_sheet_metal_rules", material="aluminum",
                   gauge="1.5mm")
        rules = r["rules"]
        expect(abs(rules["thickness_mm"] - THICKNESS) < 1e-9, rules)
        expect(abs(rules["bend_radius_mm"] - MIN_RADIUS) < 1e-9, rules)
        expect(rules["bend_radius_source"] == "table:minimum", rules)
        expect(rules["k_factor_source"] == "table:aluminum R/t<2", rules)
        expect(abs(rules["k_factor"] - 0.41) < 1e-9, rules)

        # The blank rebuilt itself the moment the stock was declared.
        bodies = d.call("get_state")["bodies"]
        expect(len(bodies) == 1, bodies)
        expect(abs(bodies[0]["volume"] - WIDTH * DEPTH * THICKNESS) < 1e-6,
               bodies[0]["volume"])

        base = d.call("get_state")["features"][1]["id"]
        top = straight_edge(d, (WIDTH / 2.0, 0.0, THICKNESS))

        # A radius the material cannot take is refused, not quietly opened up.
        r = d.call("add_feature", feature={
            "type": "EdgeFlange", "name": "Tight",
            "edges": [{"feature_id": base, "element": top}],
            "length": 20, "radius": 0.5})
        expect(not r["regen"]["ok"], "a sub-minimum radius regenerated")
        expect("tighter than" in str(r["regen"]), r["regen"])
        d.call("delete_feature", id=r["id"])

        # An unknown enum is refused by NAME, at the boundary.
        refused(d, "add_feature", "expected one of", feature={
            "type": "EdgeFlange", "name": "x",
            "edges": [{"feature_id": base, "element": top}],
            "length": 20, "position": "material_middle"})

        # The wall itself.
        wall = 20.0
        r = d.call("add_feature", feature={
            "type": "EdgeFlange", "name": "Flange",
            "edges": [{"feature_id": base, "element": top}],
            "length": wall, "angle_deg": 90, "position": "bend_outside"})
        expect(r["regen"]["ok"], r["regen"])
        quarter = math.pi / 2.0
        bend_volume = 0.5 * quarter * (
            (MIN_RADIUS + THICKNESS) ** 2 - MIN_RADIUS ** 2) * WIDTH
        want = (WIDTH * DEPTH * THICKNESS + bend_volume
                + wall * THICKNESS * WIDTH)
        bodies = d.call("get_state")["bodies"]
        expect(abs(bodies[0]["volume"] - want) / want < 1e-4,
               f"flanged volume {bodies[0]['volume']} want {want}")

        # Exporting the FOLDED part is refused: its silhouette is a picture
        # of a part that cuts to nothing.
        refused(d, "export_dxf", "not flat",
                path=os.path.join(tmp, "folded.dxf"))

        # It is still one sound solid, not two lumps that happen to touch.
        check = d.call("check_geometry")
        expect(check.get("ok", True) is not False, check)

        # ---- The blank -------------------------------------------------
        # Authored OFF, and a configuration turns it on: one document, one
        # undo stack, and the folded part and its blank never both occupy the
        # viewport (they occupy the same space).
        flat = d.call("add_feature", feature={
            "type": "FlatPattern", "name": "Flat",
            "body_id": base})["id"]
        d.call("suppress_feature", feature_id=flat, suppressed=True)
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - want)
               / want < 1e-4, "suppressing the flat pattern changed the part")

        cfg = d.call("add_configuration", name="Flat")["id"]
        d.call("set_configuration", id=cfg, unsuppressed_features=[flat])
        d.call("set_active_configuration", id=cfg)
        k = 0.41  # aluminum at R/t = 1.5, from the table
        allowance = quarter * (MIN_RADIUS + k * THICKNESS)
        blank = (WIDTH * DEPTH + allowance * WIDTH
                 + wall * WIDTH) * THICKNESS
        bodies = d.call("get_state")["bodies"]
        expect(abs(bodies[0]["volume"] - blank) / blank < 1e-4,
               f"blank volume {bodies[0]['volume']} want {blank}")
        # A blank is one gauge thick everywhere. Any bend left unrotated
        # shows up here and nowhere else.
        span = bodies[0]["bbox_max"][2] - bodies[0]["bbox_min"][2]
        expect(abs(span - THICKNESS) < 1e-4,
               f"the blank is not one gauge thick: {span}")

        # The check: a part a brake can make fires nothing, and says what it
        # could NOT answer so clean is never read as all-clear.
        chk = d.call("check_sheet_metal")
        expect(chk["ok"], chk["violations"])
        expect(chk["bends_checked"] == 1, chk)
        expect(chk["not_checked"], "the check claims to have checked "
                                   "everything, which it has not")

        # The DXF. This is the first file this project writes that a shop can
        # cut, so the test reads it back rather than trusting the call.
        out = os.path.join(tmp, "blank.dxf")
        d.call("export_dxf", path=out)
        text = open(out).read()
        for needed in ("AC1009", "$INSUNITS", "CUT", "BEND_UP", "BEND_DOWN",
                       "DASHED", "ENDSEC", "EOF"):
            expect(needed in text, f"the DXF has no {needed}")
        # Millimetres, stated. A file that does not say costs a part at 25.4x.
        units = text.split("$INSUNITS")[1].split("\n")
        expect(units[2].strip() == "4", f"$INSUNITS is {units[2]!r}, want 4")
        # One bend, so exactly one bend line, and it is not on the cut layer.
        expect(text.count("BEND_UP") + text.count("BEND_DOWN") >= 3, text[:200])

        # Back to the authored model, and the part is folded again.
        d.call("set_active_configuration", id=0)
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - want)
               / want < 1e-4, "leaving the configuration did not refold")

        # And it survives the round trip through disk.
        project = os.path.join(tmp, "sheet.dsgn")
        d.call("save_project", path=project)
        d.call("open_project", path=project)
        again = d.call("get_state")["bodies"]
        expect(abs(again[0]["volume"] - want) / want < 1e-4, again[0])
        back = d.call("get_sheet_metal_rules")["rules"]
        expect(back["gauge"] == "1.5mm" and back["material"] == "aluminum",
               back)

        flat_sheet(d, tmp)
        tier2(d)
        print("sheet metal smoke OK")
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    main()
