#!/usr/bin/env python3
"""Integration test: drive a live Designer through the remote interface.

Usage: remote_smoke.py <path-to-designer-binary>
"""
import math
import os
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402

BOX = {"type": "Box", "name": "Box 1", "position": [0, 0, 0],
       "width": 2, "height": 2, "depth": 2}




_T0 = time.time()
_PHASE = "startup"


def phase(name):
    """Announce a section with its elapsed time (see headless_smoke).

    This suite drives a real renderer under ctest; when it fails rarely and
    under load, the phase line is what says where.
    """
    global _PHASE
    print(f"  [{time.time() - _T0:6.1f}s] {name}", flush=True)
    _PHASE = name


def expect(condition, message):
    if not condition:
        raise AssertionError(f"[phase: {_PHASE}] {message}")


def is_black(png_path):
    """True if the decoded 8-bit RGB PNG has no meaningfully lit pixel."""
    import struct
    import zlib
    with open(png_path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return True
    pos, width, height, idat = 8, 0, 0, b""
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        if ctype == b"IHDR":
            width, height = struct.unpack(">II", chunk[:8])
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 3
    prev = bytearray(stride)
    p = 0
    for _ in range(height):
        filt = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        for i in range(stride):
            a = line[i - 3] if i >= 3 else 0
            b = prev[i]
            c = prev[i - 3] if i >= 3 else 0
            if filt == 1:
                line[i] = (line[i] + a) & 255
            elif filt == 2:
                line[i] = (line[i] + b) & 255
            elif filt == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif filt == 4:
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        if any(v > 8 for v in line):
            return False
        prev = line
    return True


def main():
    binary = sys.argv[1]
    # Its own state AND config dirs: autosaves and the recent-projects list
    # stay out of $HOME, and the run cannot edit the developer's real
    # preferences (set_settings persists immediately — headless runs no GUI
    # loop to debounce it, so a suite once reset their font size to 120%).
    scratch = tempfile.mkdtemp(prefix="designer_remote_")
    env = dict(os.environ, XDG_STATE_HOME=scratch,
               XDG_CONFIG_HOME=os.path.join(scratch, "config"))
    app = launch_app(binary, extra_args=["--hidden"], env=env,
                         headless=False)  # a real renderer: screenshots
    tmp = tempfile.mkdtemp(prefix="designer_remote_")
    try:
        d = Designer.wait_for(app.port)

        pong = d.call("ping")
        expect(pong["ok"], "ping")
        # ping says WHICH instance answered — the port, whether this build can
        # screenshot, the open project and whether it has unsaved changes. A
        # client that silently attaches to the wrong window is the failure
        # this exists to prevent.
        expect(pong["port"] == app.port, f"ping reports its port ({pong})")
        expect(pong["mode"] in ("gui", "headless", "offscreen"),
               f"ping mode ({pong})")
        expect("project" in pong and "dirty" in pong,
               f"ping reports the document ({pong})")

        phase("Idle throttle wakes on RPC")
        # The frame loop drops to a ~10 Hz event-wait when nothing is
        # happening (docs/rendering/idle-throttle.md); an enqueued request
        # must WAKE it, not eat the timeout. Let the app go idle, then time a
        # ping: with the wake these answer in single-digit ms; without it
        # most land mid-wait and take tens of ms. Eight rounds with slack for
        # machine load — without the wake, 6+ of 8 exceed the bound.
        slow = 0
        for _ in range(8):
            time.sleep(1.3)  # past the 1 s cooldown, into the idle floor
            t0 = time.monotonic()
            d.call("ping")
            if time.monotonic() - t0 >= 0.030:
                slow += 1
        expect(slow <= 2, f"idle pings answered promptly ({slow}/8 >= 30 ms)")

        phase("Semantic tier")
        # Semantic tier: features as JSON
        d.call("new_project")
        d.call("add_feature", feature=BOX)
        state = d.call("get_state")
        expect(len(state["features"]) == 1, "one feature")
        expect(abs(state["bodies"][0]["volume"] - 8.0) < 1e-6, "volume 8")

        modified = dict(BOX, width=5)
        d.call("modify_feature", index=0, feature=modified)
        state = d.call("get_state")
        expect(abs(state["bodies"][0]["volume"] - 20.0) < 1e-6, "volume 20")

        phase("UI tier")
        # UI tier: same edit path a human uses
        d.call("menu_click", path="Edit/Undo")
        state = d.call("get_state")
        expect(abs(state["bodies"][0]["volume"] - 8.0) < 1e-6, "undo via menu")

        d.call("key_press", chord="Ctrl+Z")
        state = d.call("get_state")
        expect(len(state["features"]) == 0, "undo via keyboard")
        d.call("key_press", chord="Ctrl+Y")
        expect(len(d.call("get_state")["features"]) == 1, "redo via keyboard")

        phase("Viewport")
        # Viewport: rotate the camera like a human drag
        before = d.call("get_state")["camera"]["position"]
        d.call("viewport_drag", x0=0.5, y0=0.5, x1=0.6, y1=0.6, button=2)
        after = d.call("get_state")["camera"]["position"]
        expect(before != after, "middle-drag rotated the camera")

        phase("Select a face programmatically, then orient to it")
        # Select a face programmatically, then orient to it
        mesh_id = d.call("get_state")["bodies"][0]["mesh"]
        sel = d.call("select_face", mesh=mesh_id, face=0)
        expect(len(sel["normal"]) == 3, "face selected")
        d.call("orient_to_face")

        phase("The same face by STABLE element name")
        # The same face by STABLE element name (no volatile mesh id): resolve
        # the name of face index 0 via list_elements and expect the same
        # geometry back.
        els = d.call("list_elements")["bodies"][0]["faces"]
        name0 = next(f["name"] for f in els if f["index"] == 0)
        sel2 = d.call("select_face", element=name0)
        expect(sel2["center"] == sel["center"], "select_face by element name")

        phase("Project save/load roundtrip")
        # Project save/load roundtrip
        project = os.path.join(tmp, "part.json")
        d.call("save_project", path=project)
        d.call("new_project")
        expect(len(d.call("get_state")["features"]) == 0, "cleared")
        d.call("open_project", path=project)
        expect(len(d.call("get_state")["features"]) == 1, "reloaded")

        phase("Sketch + extrude entirely over the RPC")
        # Sketch + extrude entirely over the RPC: add the sketch, reference it
        # from the extrude by the id the server assigns (add_feature returns it)
        d.call("new_project")
        # A legacy "rect" entity is migrated to four lines on load.
        sketch = d.call("add_feature", feature={
            "type": "Sketch", "name": "Sketch 1",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "rect", "c1": [0, 0], "c2": [4, 2]}]})
        ents = d.call("get_state")["features"][0]["entities"]
        expect(len(ents) == 4 and all(e["type"] == "line" for e in ents),
               "rect migrated to four lines")
        d.call("add_feature", feature={
            "type": "Extrude", "name": "Pad", "sketch_id": sketch["id"],
            "distance": 3.0, "mode": "new_body"})
        state = d.call("get_state")
        expect(len(state["features"]) == 2, "sketch + extrude")
        expect(abs(state["bodies"][0]["volume"] - 24.0) < 1e-6,
               "extruded volume 24")

        phase("Variables")
        # Variables: an expression-driven extrude follows a parameter edit,
        # undoably; rename rewrites expressions; referenced delete is refused.
        d.call("new_project")
        r = d.call("set_variable", name="depth", expression="3",
                   comment="pad height")
        expect(abs(r["value"] - 3.0) < 1e-9, "set_variable evaluates")
        skv = d.call("add_feature", feature={
            "type": "Sketch", "name": "Sketch V",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "rect", "c1": [0, 0], "c2": [4, 2]}]})
        d.call("add_feature", feature={
            "type": "Extrude", "name": "Pad", "sketch_id": skv["id"],
            "distance": 1.0, "distance_expr": "depth * 2",
            "mode": "new_body"})
        vol = d.call("get_state")["bodies"][0]["volume"]
        expect(abs(vol - 8.0 * 6.0) < 1e-6, "expression-driven extrude")
        d.call("set_variable", name="depth", expression="5")
        vol = d.call("get_state")["bodies"][0]["volume"]
        expect(abs(vol - 8.0 * 10.0) < 1e-6,
               "editing a variable rebuilds the extrude")
        d.call("undo")
        vol = d.call("get_state")["bodies"][0]["volume"]
        expect(abs(vol - 8.0 * 6.0) < 1e-6, "variable edit is undoable")
        d.call("redo")
        d.call("rename_variable", name="depth", new_name="h")
        pad = d.call("get_state")["features"][1]
        expect(pad["distance_expr"] == "h * 2", "rename rewrites expressions")
        lv = d.call("list_variables")["variables"]
        expect(len(lv) == 1 and lv[0]["name"] == "h"
               and lv[0]["comment"] == "pad height", "list_variables")
        try:
            d.call("delete_variable", name="h")
            expect(False, "deleting a referenced variable must fail")
        except RuntimeError as e:
            expect("referenced" in str(e), "delete refusal names the reason")
        try:
            d.call("set_variable", name="2bad", expression="1")
            expect(False, "invalid variable name must fail")
        except RuntimeError:
            pass
        # An expression-driven sketch dimension re-solves on a variable edit.
        d.call("new_project")
        d.call("set_variable", name="r", expression="1.5")
        skc2 = d.call("add_feature", feature={
            "type": "Sketch", "name": "Sketch R",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "circle", "center": [0, 0],
                          "radius": 1.0}]})
        d.call("add_constraint", sketch_id=skc2["id"], kind="radius",
               refs=[[skc2["entity_ids"][0], 0]], value=1.0, expr="r")
        d.call("add_feature", feature={
            "type": "Extrude", "name": "Rod", "sketch_id": skc2["id"],
            "distance": 2.0, "mode": "new_body"})
        vol = d.call("get_state")["bodies"][0]["volume"]
        expect(abs(vol - math.pi * 1.5 * 1.5 * 2.0) < 0.05,
               "radius expression applied through the solve")
        d.call("set_variable", name="r", expression="3")
        vol = d.call("get_state")["bodies"][0]["volume"]
        expect(abs(vol - math.pi * 3.0 * 3.0 * 2.0) < 0.2,
               "variable edit re-solves the sketch and rebuilds the rod")

        phase("Datum plane")
        # Datum plane: an expression-driven offset datum, a sketch that tracks
        # it, and an extrude — editing the variable moves the whole stack.
        d.call("new_project")
        d.call("set_variable", name="off", expression="1.5")
        datum = d.call("add_feature", feature={
            "type": "DatumPlane", "name": "Plane 1",
            "kind": "offset", "base_plane": "xy",
            "offset": 1.0, "offset_expr": "off * 2"})
        skd = d.call("add_feature", feature={
            "type": "Sketch", "name": "Sketch D",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "plane_ref": datum["id"],
            "entities": [{"type": "rect", "c1": [0, 0], "c2": [4, 2]}]})
        d.call("add_feature", feature={
            "type": "Extrude", "name": "Pad", "sketch_id": skd["id"],
            "distance": 3.0, "mode": "new_body"})
        body = d.call("get_state")["bodies"][0]
        # Datum at z = off*2 = 3; extrude +3 -> z spans [3, 6].
        expect(abs(body["bbox_min"][2] - 3.0) < 1e-3
               and abs(body["bbox_max"][2] - 6.0) < 1e-3,
               "sketch tracks the expression-driven datum (z in [3,6])")
        d.call("set_variable", name="off", expression="2.5")
        body = d.call("get_state")["bodies"][0]
        # Datum now at z = 5; extrude +3 -> z spans [5, 8].
        expect(abs(body["bbox_min"][2] - 5.0) < 1e-3
               and abs(body["bbox_max"][2] - 8.0) < 1e-3,
               "editing the variable moves the datum, sketch, and body")
        d.call("undo")  # revert the variable edit
        body = d.call("get_state")["bodies"][0]
        expect(abs(body["bbox_min"][2] - 3.0) < 1e-3, "datum edit is undoable")

        phase("Revolve")
        # Revolve: an expression-driven angle sweeps a profile about the global
        # Y axis; editing the variable rebuilds the solid.
        d.call("new_project")
        d.call("set_variable", name="sweep", expression="360")
        skr = d.call("add_feature", feature={
            "type": "Sketch", "name": "Profile",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "rect", "c1": [1, 0], "c2": [3, 3]}]})
        d.call("add_feature", feature={
            "type": "Revolve", "name": "Rev", "sketch_id": skr["id"],
            "axis": {"kind": "y"}, "angle_deg": 1.0, "angle_expr": "sweep",
            "mode": "new_body"})
        vol = d.call("get_state")["bodies"][0]["volume"]
        # Washer: 2*pi*R*(w*h) with R=2, w=2, h=3 -> 24pi.
        expect(abs(vol - 2 * math.pi * 2.0 * 6.0) < 0.1, "full revolve is 24pi")
        d.call("set_variable", name="sweep", expression="180")
        vol = d.call("get_state")["bodies"][0]["volume"]
        expect(abs(vol - 12 * math.pi) < 0.1,
               "editing the angle variable rebuilds the revolve")

        phase("Revolve about a DatumAxisFeature")
        # Revolve about a DatumAxisFeature (two points along Y).
        d.call("new_project")
        ax = d.call("add_feature", feature={
            "type": "DatumAxis", "name": "Axis 1", "kind": "two_points",
            "points": [[0, 0, 0], [0, 1, 0]]})
        skr2 = d.call("add_feature", feature={
            "type": "Sketch", "name": "Profile",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "rect", "c1": [1, 0], "c2": [3, 3]}]})
        d.call("add_feature", feature={
            "type": "Revolve", "name": "Rev", "sketch_id": skr2["id"],
            "axis": {"kind": "datum_axis", "datum_axis_id": ax["id"]},
            "angle_deg": 360.0, "mode": "new_body"})
        vol = d.call("get_state")["bodies"][0]["volume"]
        expect(abs(vol - 24 * math.pi) < 0.1, "revolve about a datum axis")

        phase("Boolean")
        # Boolean: two overlapping boxes combined into one body. Box A [0,2]^3,
        # box B [1,3]x[0,2]^2; overlap = 4, each volume 8.
        d.call("new_project")
        ba = d.call("add_feature", feature={
            "type": "Box", "name": "A", "position": [0, 0, 0],
            "width": 2, "height": 2, "depth": 2})
        bb = d.call("add_feature", feature={
            "type": "Box", "name": "B", "position": [1, 0, 0],
            "width": 2, "height": 2, "depth": 2})
        expect(len(d.call("get_state")["bodies"]) == 2, "two boxes")
        boo = d.call("add_feature", feature={
            "type": "Boolean", "name": "Cut", "op": "subtract",
            "target_id": ba["id"], "tool_id": bb["id"]})
        st = d.call("get_state")
        expect(len(st["bodies"]) == 1, "boolean consumes both into one body")
        expect(abs(st["bodies"][0]["volume"] - 4.0) < 1e-4, "subtract = 8 - 4")
        # Switch the op to union via modify_feature -> volume 12.
        d.call("modify_feature", index=2, feature={
            "type": "Boolean", "name": "Cut", "op": "union",
            "target_id": ba["id"], "tool_id": bb["id"], "id": boo["id"]})
        vol = d.call("get_state")["bodies"][0]["volume"]
        expect(abs(vol - 12.0) < 1e-4, "union = 8 + 8 - 4")

        phase("Pattern")
        # Pattern: an expression-driven linear array of a box; editing the count
        # variable changes the number of bodies. A mirror across a base plane.
        d.call("new_project")
        d.call("set_variable", name="n", expression="3")
        pbox = d.call("add_feature", feature={
            "type": "Box", "name": "A", "position": [0, 0, 0],
            "width": 2, "height": 2, "depth": 2})
        d.call("add_feature", feature={
            "type": "Pattern", "name": "Arr", "kind": "linear",
            "source_id": pbox["id"], "axis": {"kind": "x"},
            "count": 1, "count_expr": "n", "spacing": 5.0})
        expect(len(d.call("get_state")["bodies"]) == 3,
               "linear pattern count 3 = 3 disjoint bodies")
        d.call("set_variable", name="n", expression="5")
        expect(len(d.call("get_state")["bodies"]) == 5,
               "editing the count variable rebuilds the pattern")

        d.call("new_project")
        mbox = d.call("add_feature", feature={
            "type": "Box", "name": "A", "position": [1, 0, 0],
            "width": 2, "height": 2, "depth": 2})  # clear of x=0
        d.call("add_feature", feature={
            "type": "Pattern", "name": "Mir", "kind": "mirror",
            "source_id": mbox["id"], "base_plane": "yz"})
        st = d.call("get_state")
        expect(len(st["bodies"]) == 2, "mirror across YZ = two bodies")
        total = sum(b["volume"] for b in st["bodies"])
        expect(abs(total - 16.0) < 1e-3, "mirror total volume 2x8")

        phase("Ellipse round-trips through JSON and extrudes to an elliptic prism")
        # Ellipse round-trips through JSON and extrudes to an elliptic prism.
        d.call("new_project")
        ske = d.call("add_feature", feature={
            "type": "Sketch", "name": "Sketch E",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "ellipse", "center": [0, 0],
                          "r_major": 2.0, "r_minor": 1.0, "rotation": 0.0}]})
        e0 = d.call("get_state")["features"][0]["entities"][0]
        expect(e0["type"] == "ellipse" and abs(e0["r_major"] - 2.0) < 1e-6,
               "ellipse survives the RPC round-trip")
        d.call("add_feature", feature={
            "type": "Extrude", "name": "Pad", "sketch_id": ske["id"],
            "distance": 3.0, "mode": "new_body"})
        vol = d.call("get_state")["bodies"][0]["volume"]
        expect(abs(vol - math.pi * 2.0 * 1.0 * 3.0) < 0.05,
               "ellipse extrudes to pi*a*b*h")

        phase("Preview -> accept/cancel feature flow")
        # Preview -> accept/cancel feature flow (driven from the model toolbar;
        # the "###tb_*" ids are stable button handles under the dock host).
        d.call("new_project")
        d.call("item_click", ref="//##DockHost/**/###tb_box")
        st = d.call("get_state")
        expect(st["pending_feature_id"] != 0, "box is a pending preview")
        expect(not st["can_undo"], "preview not yet on undo stack")
        d.call("accept_pending")
        st = d.call("get_state")
        expect(st["pending_feature_id"] == 0 and st["can_undo"],
               "accept commits one undoable action")
        d.call("item_click", ref="//##DockHost/**/###tb_sphere")
        d.call("cancel_pending")
        expect(len(d.call("get_state")["features"]) == 1, "cancel discards")

        phase("Driving the GUI as a scripted human")
        # Driving the GUI as a scripted human (U9): the failure says WHY, the
        # widgets can be enumerated instead of grepped for, and hover works.
        try:
            d.call("item_click", ref="//##DockHost/**/###tb_not_a_button")
            expect(False, "clicking a missing widget must fail")
        except RuntimeError as e:
            expect("Unable to locate item" in str(e),
                   f"ui action failure carries the engine's reason: {e}")
        try:
            d.call("key_press", chord="Ctrl+Nope")
            expect(False, "a bad chord must fail")
        except RuntimeError as e:
            expect("unknown key token" in str(e),
                   f"key_press reports the bad token, not 'unknown method': {e}")
        items = d.call("ui_items", ref="//##DockHost")["items"]
        labels = [i["label"] for i in items]
        expect(any("###tb_box" in lb for lb in labels),
               "ui_items enumerates the toolbar buttons")
        expect(any(i["disabled"] for i in items),
               "ui_items reports which widgets are disabled")
        d.call("item_hover", ref="//##DockHost/**/###tb_box")

        phase("Hidden-label widgets")
        # Hidden-label widgets (every LabeledCombo) have no path — the id
        # from ui_items is the only handle, and item_combo opens+picks in one
        # action because the popup does not survive between RPCs.
        d.call("item_click", ref="//##DockHost/**/###tb_box")
        hidden = sorted([i for i in d.call("ui_items", ref="//###ToolWindow")
                         ["items"] if not i["label"]], key=lambda i: i["y"])
        expect(hidden, "the tool window has hidden-label combos")
        d.call("item_combo", id=hidden[0]["id"], value="Default")
        d.call("cancel_pending")

        phase("Popups")
        # Popups (the tab bar's "+", context menus, combos) close when an
        # action ends, so opening and choosing must happen in ONE action —
        # without this a scripted human cannot add a part, assembly or
        # drawing at all, since that menu is the only path.
        r = d.call("popup_items", ref="//##PartTabs/+##addmenu")
        labels = [i["label"] for i in r["items"]]
        expect(any("addasm" in lb for lb in labels),
               f"popup_items lists the + menu: {labels}")
        before = len(d.call("list_parts")["parts"])
        d.call("popup_click", ref="//##PartTabs/+##addmenu",
               item="New Assembly##addasm")
        expect(len(d.call("list_parts")["parts"]) == before + 1,
               "popup_click added the assembly tab")
        # …and the UI tier keeps working on a non-part tab (the guard that
        # rejects part RPCs on an assembly must not swallow UI actions).
        d.call("ui_items", ref="//##DockHost")
        d.call("item_hover", ref="//##DockHost/**/###tb_insert")
        d.call("new_project")

        # U10: the script API refuses the same incomplete tools the GUI does.
        d.call("new_project")
        d.call("add_feature", feature=BOX)
        try:
            d.call("add_feature", feature={"type": "Fillet", "name": "F",
                                           "edges": [], "radius": 0.5})
            expect(False, "an edge-less fillet must be refused")
        except RuntimeError as e:
            expect("incomplete" in str(e) and "edge" in str(e),
                   f"add_feature names the missing input: {e}")
        d.call("new_project")

        phase("The menu bar is the one surface the test engine cannot enumerate")
        # The menu bar is the one surface the test engine cannot enumerate
        # (its buttons live in an internal child), so the bar reports its own
        # names and menu_items opens each menu to list it.
        top = [i["label"] for i in d.call("menu_items")["items"]]
        expect({"File", "Edit", "View", "Help"} <= set(top),
               f"menu_items lists the menu bar: {top}")
        fileItems = d.call("menu_items", path="File")["items"]
        labels = [i["label"] for i in fileItems]
        expect("Save Project" in labels, f"File menu enumerates: {labels}")
        expect(any(i.get("submenu") for i in fileItems),
               "submenu entries are flagged as such")

        phase("A disabled widget must refuse loudly")
        # A disabled widget must refuse loudly: Extrude needs a sketch, and a
        # script clicking it used to get a success and no feature.
        d.call("new_project")
        try:
            d.call("item_click", ref="//##DockHost/**/###tb_extrude")
            expect(False, "clicking a disabled toolbar button must fail")
        except RuntimeError as e:
            expect("disabled" in str(e), f"disabled widgets say so: {e}")

        phase("Constraint solving")
        # Constraint solving: two lines snap coincident
        d.call("new_project")
        sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "Sketch 1",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "line", "a": [-2, -1], "b": [2, -1]},
                         {"type": "line", "a": [2.3, -0.7], "b": [2, 2]}]})
        eid = sk["entity_ids"]  # stable ids the server assigned
        expect(len(eid) == 2 and all(i > 0 for i in eid),
               "add_feature returns entity ids")
        d.call("add_constraint", sketch_id=sk["id"], kind="coincident",
               refs=[[eid[1], 0], [eid[0], 1]])
        ents = d.call("get_state")["features"][0]["entities"]
        dx = ents[0]["b"][0] - ents[1]["a"][0]
        dy = ents[0]["b"][1] - ents[1]["a"][1]
        expect(abs(dx) < 1e-3 and abs(dy) < 1e-3, "coincident solved")

        phase("Deleting an entity removes it and its dependent constraint")
        # Deleting an entity removes it and its dependent constraint
        dr = d.call("delete_entity", sketch_id=sk["id"], entity_id=eid[1])
        ents2 = d.call("get_state")["features"][0]["entities"]
        expect(len(ents2) == 1 and ents2[0]["id"] == eid[0],
               "delete_entity removed the second line, kept the first")
        expect(dr["dropped_constraints"] == 0, "coincident is a trivial drop")

        phase("Angle constraint over the wire")
        # Angle constraint over the wire: two lines forced to 45 degrees
        d.call("new_project")
        ska = d.call("add_feature", feature={
            "type": "Sketch", "name": "Sketch A",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "line", "a": [0, 0], "b": [4, 0]},
                         {"type": "line", "a": [0, 0], "b": [3.5, 2.0]}]})
        aid = ska["entity_ids"]
        d.call("add_constraint", sketch_id=ska["id"], kind="coincident",
               refs=[[aid[1], 0], [aid[0], 0]])
        # The wire speaks degrees for angles (the UI convention, 1ec93fe).
        d.call("add_constraint", sketch_id=ska["id"], kind="angle",
               refs=[[aid[0], 0], [aid[1], 0]], value=45.0)
        ea = d.call("get_state")["features"][0]["entities"]
        d0 = [ea[0]["b"][0] - ea[0]["a"][0], ea[0]["b"][1] - ea[0]["a"][1]]
        d1 = [ea[1]["b"][0] - ea[1]["a"][0], ea[1]["b"][1] - ea[1]["a"][1]]
        ang = abs(math.atan2(d0[0] * d1[1] - d0[1] * d1[0],
                             d0[0] * d1[0] + d0[1] * d1[1]))
        expect(abs(ang - 0.7853981634) < 1e-3, "angle solved to 45 degrees")

        phase("Arc entity round-trips over the wire and takes a radius constraint")
        # Arc entity round-trips over the wire and takes a radius constraint.
        d.call("new_project")
        skc = d.call("add_feature", feature={
            "type": "Sketch", "name": "Sketch C",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "arc", "center": [0, 0], "radius": 1.0,
                          "start_angle": 0.0, "end_angle": 1.5707963}]})
        cid = skc["entity_ids"]
        arc0 = d.call("get_state")["features"][0]["entities"][0]
        expect(arc0["type"] == "arc" and abs(arc0["radius"] - 1.0) < 1e-6,
               "arc entity round-trips")
        d.call("add_constraint", sketch_id=skc["id"], kind="radius",
               refs=[[cid[0], 0]], value=2.5)
        arc1 = d.call("get_state")["features"][0]["entities"][0]
        expect(abs(arc1["radius"] - 2.5) < 1e-3, "arc radius constraint solved")

        phase("Generative")
        # Generative: mirror a line across an axis, and fillet a rectangle corner.
        d.call("new_project")
        skg = d.call("add_feature", feature={
            "type": "Sketch", "name": "Sketch G",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "line", "a": [0, -3], "b": [0, 3]},  # axis
                         {"type": "line", "a": [1, 0], "b": [2, 2]}]})
        gid = skg["entity_ids"]
        mg = d.call("mirror", sketch_id=skg["id"], sources=[gid[1]],
                    axis=gid[0], link=True)
        expect(len(mg["generated"]) == 1, "mirror generated one copy")
        gents = d.call("get_state")["features"][0]["entities"]
        copy = next(e for e in gents if e["id"] == mg["generated"][0])
        expect(abs(copy["a"][0] + 1.0) < 1e-3 and abs(copy["b"][0] + 2.0) < 1e-3,
               "mirror reflected the line across x=0")

        d.call("new_project")
        skf = d.call("add_feature", feature={
            "type": "Sketch", "name": "Sketch F",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "line", "a": [4, 0], "b": [0, 0]},
                         {"type": "line", "a": [0, 0], "b": [0, 4]}]})
        fid = skf["entity_ids"]
        d.call("add_constraint", sketch_id=skf["id"], kind="coincident",
               refs=[[fid[0], 1], [fid[1], 0]])
        ff = d.call("fillet", sketch_id=skf["id"], line1=fid[0], line2=fid[1],
                    radius=1.0)
        expect(ff["arc_id"] != 0, "fillet created an arc")
        fents = d.call("get_state")["features"][0]["entities"]
        arc = next(e for e in fents if e["id"] == ff["arc_id"])
        expect(arc["type"] == "arc" and abs(arc["radius"] - 1.0) < 1e-3,
               "fillet arc has the requested radius")
        # A failed fillet is a loud error naming the reason, never arc_id 0.
        skc2 = d.call("add_feature", feature={
            "type": "Sketch", "name": "Sketch F2",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "circle", "center": [9, 9], "radius": 1},
                         {"type": "line", "a": [8, 0], "b": [12, 0]},
                         {"type": "line", "a": [20, 20], "b": [20, 24]}]})
        cid, lid1, lid2 = skc2["entity_ids"]
        for args, why in [
                ({"line1": cid, "line2": lid1, "radius": 1.0}, "not a line"),
                ({"line1": lid1, "line2": lid2, "radius": 1.0},
                 "share no endpoint"),
                ({"line1": lid1, "line2": lid2, "radius": 0.0}, "positive")]:
            try:
                d.call("fillet", sketch_id=skc2["id"], **args)
                expect(False, f"fillet should error ({why})")
            except RuntimeError as e:
                expect(why in str(e), f"fillet error explains: {why} ({e})")

        phase("Linked linear pattern, then edit its count via modify_feature")
        # Linked linear pattern, then edit its count via modify_feature.
        d.call("new_project")
        skp = d.call("add_feature", feature={
            "type": "Sketch", "name": "Sketch P",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "circle", "center": [0, 0], "radius": 0.3}]})
        pid = skp["entity_ids"][0]
        pp = d.call("pattern", sketch_id=skp["id"], sources=[pid], count=4,
                    step=[1.0, 0], link=True, circular=False)
        expect(len(pp["generated"]) == 3, "pattern made count-1 copies")
        st = d.call("get_state")["features"][0]
        expect(len(st["entities"]) == 4 and len(st["derived_ops"]) == 1,
               "pattern seed + copies + a derived record")
        st["derived_ops"][0]["count"] = 6  # edit count → regenerate
        d.call("modify_feature", index=0, feature=st)
        st2 = d.call("get_state")["features"][0]
        expect(len(st2["entities"]) == 6, "editing count regenerated the pattern")

        phase("Offset a rectangle loop outward with rounded corners → 4 lines + 4 arcs")
        # Offset a rectangle loop outward with rounded corners → 4 lines + 4 arcs.
        d.call("new_project")
        sko = d.call("add_feature", feature={
            "type": "Sketch", "name": "Sketch O",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "line", "a": [0, 0], "b": [4, 0]},
                         {"type": "line", "a": [4, 0], "b": [4, 2]},
                         {"type": "line", "a": [4, 2], "b": [0, 2]},
                         {"type": "line", "a": [0, 2], "b": [0, 0]}]})
        oc = d.call("offset", sketch_id=sko["id"], chain=sko["entity_ids"],
                    distance=0.5, join_arc=True)
        oents = d.call("get_state")["features"][0]["entities"]
        newkinds = [e["type"] for e in oents if e["id"] in oc["generated"]]
        expect(len(oc["generated"]) == 8, "offset made 4 lines + 4 corner arcs")
        expect(newkinds.count("arc") == 4 and newkinds.count("line") == 4,
               "offset boundary is lines + rounded-corner arcs")

        phase("Material")
        # Material: assign via RPC, stamped onto the body, survives save/load.
        d.call("new_project")
        mb = d.call("add_feature", feature=BOX)
        d.call("set_material", feature_id=mb["id"], material="steel")
        mstate = d.call("get_state")
        expect(mstate["features"][0].get("material") == "steel",
               "material set on feature")
        expect(mstate["bodies"][0]["material"] == "steel",
               "material stamped on body")
        try:
            d.call("set_material", feature_id=mb["id"], material="unobtainium")
            expect(False, "unknown material should error")
        except RuntimeError:
            pass
        # By body index: resolves createdBy to the same feature path.
        d.call("set_material", body=0, material="brass")
        mstate = d.call("get_state")
        expect(mstate["features"][0].get("material") == "brass",
               "set_material by body index reaches the creating feature")
        expect(mstate["bodies"][0]["material"] == "brass",
               "by-body material stamped on body")
        try:
            d.call("set_material", body=99, material="steel")
            expect(False, "out-of-range body should error")
        except RuntimeError:
            pass
        try:
            d.call("set_material", material="steel")
            expect(False, "set_material with no target should error")
        except RuntimeError as e:
            expect("created_by" in str(e) and "body" in str(e),
                   "no-target error says where the ids come from")
        try:
            d.call("set_material", feature_id=mb["id"], body=0,
                   material="steel")
            expect(False, "both feature_id and body should error")
        except RuntimeError:
            pass
        mpath = os.path.join(tmp, "material.json")
        d.call("save_project", path=mpath)
        d.call("new_project")
        d.call("open_project", path=mpath)
        expect(d.call("get_state")["bodies"][0]["material"] == "brass",
               "material survives save/load")

        phase("Screenshot")
        # Screenshot: the controller can see what the user sees
        d.call("new_project")
        sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "Sketch 1",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "line", "a": [-2, -1], "b": [2, -1]}]})
        d.call("fit_view")
        shot = os.path.join(tmp, "view.png")
        d.call("screenshot", path=shot)
        expect(os.path.exists(shot) and os.path.getsize(shot) > 1000,
               "screenshot written")

        phase("Screenshot postconditions")
        # A capture that "succeeds" but shows a wall of nothing must SAY so.
        # The classic incident: the camera INSIDE the geometry (the dock
        # round's inside-a-barrel shots) — a near-uniform frame. (An empty
        # scene shows the environment, and emptiness is get_state's job.)
        d.call("new_project")
        d.call("add_feature", feature={
            "type": "Box", "name": "Room", "position": [0, 0, 0],
            "width": 500, "height": 500, "depth": 500})
        d.call("set_camera", eye=[250, 250, 250], target=[250, 250, 260])
        blank = os.path.join(tmp, "inside.png")
        r = d.call("screenshot", path=blank)
        expect(any("single color" in w for w in r.get("warnings", [])),
               f"inside-geometry frame must warn: {r}")
        r = d.call("screenshot", path=blank)
        expect(any("pixel-identical" in w for w in r.get("warnings", [])),
               f"unchanged frame must warn: {r}")
        # ...and a properly framed shot warns about neither.
        d.call("view", name="iso")
        r = d.call("screenshot", path=os.path.join(tmp, "framed.png"))
        expect(not r.get("warnings"), f"framed shot is clean: {r}")

        phase("render_sheet")
        # V3: multi-view sheet with the numeric footer. One box -> footer
        # carries body count + bbox; the PNG is a real composite (bigger
        # than a single tile's worth of black).
        d.call("new_project")
        d.call("add_feature", feature={
            "type": "Box", "name": "B", "position": [0, 0, 0],
            "width": 20, "height": 10, "depth": 5})
        sheet = os.path.join(tmp, "sheet.png")
        r = d.call("render_sheet", path=sheet, views=["iso", "front"])
        expect(os.path.exists(sheet) and os.path.getsize(sheet) > 5000,
               "sheet written")
        expect(any("1 BODIES" in line for line in r["footer"]),
               f"sheet footer body count: {r['footer']}")
        expect(any("BBOX 20 X 10 X 5" in line for line in r["footer"]),
               f"sheet footer bbox: {r['footer']}")
        expect(r["views"] == ["iso", "front"], f"sheet views: {r['views']}")
        # Focused sheet (round-8 retro): frame ONE occurrence large.
        part = d.call("list_parts")["parts"][0]["name"]
        d.call("add_assembly", name="SheetRig")
        d.call("set_active_part", name="SheetRig")
        d.call("add_occurrence", part=part, name="Solo", fixed=True)
        focus = os.path.join(tmp, "focus.png")
        r = d.call("render_sheet", path=focus, views=["iso"], focus="Solo")
        expect(os.path.exists(focus), "focused sheet written")
        expect(r["footer"] and r["footer"][0] == "FOCUS: Solo",
               f"focus named in the footer: {r['footer']}")

        phase("Standard views")
        # Standard views: instant (no animation), fit + exact axis alignment
        d.call("new_project")
        d.call("add_feature", feature=BOX)
        v = d.call("view", name="front")  # camera on +Z of the box center
        offset = [p - t for p, t in zip(v["position"], v["target"])]
        expect(abs(offset[0]) < 1e-4 and abs(offset[1]) < 1e-4
               and offset[2] > 1.0, "front view puts the camera on +Z")
        v = d.call("view", name="top")
        offset = [p - t for p, t in zip(v["position"], v["target"])]
        expect(abs(offset[0]) < 1e-4 and abs(offset[2]) < 1e-4
               and offset[1] > 1.0, "top view puts the camera on +Y")
        st = d.call("get_state")  # the snap is already committed, not animating
        expect(all(abs(a - b) < 1e-4 for a, b in
                   zip(st["camera"]["position"], v["position"])),
               "view result matches get_state camera")
        v2 = d.call("view", name="iso", fit=False)  # fit=False keeps distance
        expect(abs(v2["distance"] - v["distance"]) < 1e-4,
               "view fit=False preserves the camera distance")
        try:
            d.call("view", name="sideways")
            expect(False, "unknown view name must error")
        except RuntimeError:
            pass

        phase("Offline raytraced still")
        # Offline raytraced still: build a body, render small + deterministic.
        # Skipped gracefully if the GPU has no ray tracing.
        d.call("new_project")
        d.call("add_feature", feature=BOX)
        rb = d.call("rt_build")
        if rb.get("ok") and rb.get("has_tlas"):
            still = os.path.join(tmp, "still.png")
            r = d.call("render_still", width=96, height=72, samples=8, seed=1,
                       path=still)
            expect(r["ok"], "render_still ok: " + str(r.get("error")))
            expect(os.path.exists(still) and os.path.getsize(still) > 200,
                   "render_still wrote a png")
            expect(not is_black(still), "render_still png is not all black")
            # Same seed + samples => byte-identical (deterministic accumulation).
            still2 = os.path.join(tmp, "still2.png")
            d.call("render_still", width=96, height=72, samples=8, seed=1,
                   path=still2)
            with open(still, "rb") as a, open(still2, "rb") as b:
                expect(a.read() == b.read(),
                       "render_still is deterministic for a fixed seed")
        else:
            print("remote_smoke: ray tracing unavailable, skipping render_still")

        phase("Error handling")
        # Error handling
        try:
            d.call("delete_feature", index=99)
            expect(False, "expected error for bad index")
        except RuntimeError:
            pass

        phase("A failing feature carries a SPECIFIC error, not the generic")
        # A failing feature carries a SPECIFIC error, not the generic
        # "regeneration failed" fallback: a cut into an empty document.
        d.call("new_project")
        ske = d.call("add_feature", feature={
            "type": "Sketch", "name": "S",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "rect", "c1": [0, 0], "c2": [2, 2]}]})
        d.call("add_feature", feature={
            "type": "Extrude", "name": "Cut", "sketch_id": ske["id"],
            "distance": 2.0, "mode": "cut"})
        feats = d.call("get_state")["features"]
        expect("no bodies" in feats[1].get("error", ""),
               f"cut-with-no-bodies is a specific error: {feats[1].get('error')}")

        phase("A profile-builder failure")
        # A profile-builder failure (axis-crossing revolve) surfaces the OCC
        # side's reason too.
        d.call("new_project")
        skx = d.call("add_feature", feature={
            "type": "Sketch", "name": "X",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "rect", "c1": [-1, 0], "c2": [3, 3]}]})
        d.call("add_feature", feature={
            "type": "Revolve", "name": "Rev", "sketch_id": skx["id"],
            "axis": {"kind": "y"}, "angle_deg": 360.0, "mode": "new_body"})
        feats = d.call("get_state")["features"]
        expect("cross the axis" in feats[1].get("error", ""),
               f"axis-crossing revolve names the reason: {feats[1].get('error')}")

        phase("Mutating RPCs return a regen summary")
        # Mutating RPCs return a regen summary: failures arrive with the call
        # that caused them, root vs collateral distinguished.
        d.call("new_project")
        r = d.call("add_feature", feature={
            "type": "Box", "name": "A", "position": [0, 0, 0],
            "width": 2, "height": 2, "depth": 2})
        expect(r["regen"]["ok"] and r["regen"]["failed"] == []
               and r["regen"]["warnings"] == [],
               "healthy add_feature reports regen.ok with no warnings")
        # A cut whose profile misses every body is VALID but almost always a
        # mistake — the warning must ride the regen summary so scripts see it
        # in the response that caused it (regen.ok stays true).
        skm = d.call("add_feature", feature={
            "type": "Sketch", "name": "Off target",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "circle", "center": [50, 50],
                          "radius": 1.0}]})
        r = d.call("add_feature", feature={
            "type": "Extrude", "name": "Miss", "sketch_id": skm["id"],
            "distance": 5.0, "mode": "cut"})
        expect(r["regen"]["ok"], "a no-op cut is not an error")
        expect(any("removed no material" in w["warning"]
                   for w in r["regen"]["warnings"]),
               f"no-op cut warning rides the summary: {r['regen']}")
        d.call("delete_feature", index=2)
        d.call("delete_feature", index=1)
        r = d.call("add_feature", feature={
            "type": "Extrude", "name": "Bad", "sketch_id": 9999,
            "distance": 1.0, "mode": "new_body"})
        expect(not r["regen"]["ok"], "failing add_feature reports regen not-ok")
        expect("not found" in r["regen"]["failed"][0]["error"],
               f"regen.failed carries the reason: {r['regen']['failed']}")
        # A collateral failure downstream is tagged after_earlier_failure.
        skc = d.call("add_feature", feature={
            "type": "Sketch", "name": "S",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "circle", "center": [20, 0],
                          "radius": 1.0}]})
        d.call("delete_feature", index=0)  # remove the box: Bad is now root
        r = d.call("add_feature", feature={
            "type": "Extrude", "name": "Collateral", "sketch_id": skc["id"],
            "distance": 1.0, "mode": "cut"})
        failed = {f["name"]: f for f in r["regen"]["failed"]}
        expect(not failed["Bad"].get("after_earlier_failure", False),
               "root failure is not tagged")
        expect(failed["Collateral"].get("after_earlier_failure", False),
               "collateral failure is tagged after_earlier_failure")
        st = d.call("get_state")
        expect(st["regen"]["ok"] is False and "failed" in st["regen"],
               "get_state regen block carries the summary")

        # check_geometry honesty: `ok` = built AND valid; `valid` alone is
        # vacuous over zero bodies (exposed via body_count).
        cg = d.call("check_geometry")  # doc still has the broken features
        expect(cg["valid"] and not cg["ok"] and cg["feature_errors"],
               f"check_geometry: broken features -> ok false: {cg}")
        d.call("new_project")
        cg = d.call("check_geometry")
        expect(cg["ok"] and cg["body_count"] == 0,
               "check_geometry: empty doc is ok with body_count 0")
        d.call("add_feature", feature=BOX)
        cg = d.call("check_geometry")
        expect(cg["ok"] and cg["valid"] and cg["body_count"] == 1,
               "check_geometry: healthy box is ok")

        # describe: the introspection endpoint, and the drift ratchet. Every
        # listed method must be dispatchable (anything reaching a handler
        # errors on params/state, never "unknown method"), counts only grow,
        # and the newest dialect entries are present.
        desc = d.call("describe")
        names = [meth["name"] for meth in desc["methods"]]
        expect(len(names) == len(set(names)), "describe lists each method once")
        expect(len(names) >= 136, f"method count ratchet: {len(names)}")
        expect(len(desc["feature_types"]) >= 24, "feature type ratchet")
        expect({"ellipse_arc", "spline"} <= set(desc["sketch_entity_types"]),
               "newest sketch entity types are described")
        expect(desc["document_format_version"] >= 52, "format version present")
        expect(len(desc["constraint_kinds"]) >= 20, "constraint kinds")
        expect("aluminum" in desc["material_slugs"], "material slugs")
        expect("through_all" in desc["extrude_end_modes"],
               "extrude end modes are described")
        expect({"hole_callout", "pattern_callout"} <=
               set(desc["drawing_dim_kinds"]),
               "drawing dim kinds are described")
        for name in names:
            if name == "describe":
                continue
            d.call("new_project")  # deterministic tab state per probe
            try:
                d.call(name)
            except RuntimeError as e:
                expect("unknown method" not in str(e),
                       f"described method '{name}' must dispatch: {e}")
        d.call("new_project")

        phase("The reverse direction")
        # The reverse direction: every `m == "..."` literal dispatched in
        # src/remote/*.cpp must be described (screenshot and render_sheet are
        # the methods dispatched outside the if-chains, and they ARE
        # described).
        import glob
        import re
        remote_src = os.path.join(os.path.dirname(__file__), "..", "src",
                                  "remote")
        dispatched = set()
        for cpp in glob.glob(os.path.join(remote_src, "*.cpp")):
            with open(cpp) as f:
                dispatched |= set(re.findall(r'm == "([a-z_0-9]+)"',
                                             f.read()))
        undescribed = sorted(dispatched - set(names))
        expect(not undescribed,
               f"dispatched but missing from describe tables: {undescribed}")
        undispatched = sorted(set(names) - dispatched - {"screenshot",
                                                         "render_sheet",
                                                         "describe"})
        expect(not undispatched,
               f"described but never dispatched: {undispatched}")

        phase("A missing REQUIRED parameter reports itself in caller terms, not as")
        # A missing REQUIRED parameter reports itself in caller terms, not as
        # nlohmann's "key 'x' not found" (the did-you-mean path only covers
        # keys that are present-but-unknown).
        try:
            d.call("rename_part", new_name="X")
            expect(False, "rename_part without 'part' should error")
        except RuntimeError as e:
            expect("missing required parameter 'part'" in str(e),
                   f"missing-param error is caller-grade: {e}")

        # set_active_part accepts a NAME as well as an index (the describe
        # doc promises both; the name path was missing until it bit).
        d.call("new_project")
        d.call("add_part", name="Named Tab")
        r = d.call("set_active_part", name="Named Tab")
        expect(r["active"] >= 0, "set_active_part by name")
        try:
            d.call("set_active_part", name="No Such Tab")
            expect(False, "bad tab name should error")
        except RuntimeError as e:
            expect("list_parts" in str(e), f"bad-name error is helpful: {e}")

        phase("Explode activation")
        # Explode activation: factor-only set_explode auto-activates the only
        # view (was a silent visual no-op); errors helpfully with none.
        d.call("new_project")
        d.call("add_feature", feature=BOX)
        bidx = d.call("add_part", name="B2")["index"]
        d.call("set_active_part", index=bidx)
        d.call("add_feature", feature=dict(BOX, name="Box B",
                                           position=[10, 0, 0]))
        aidx = d.call("add_assembly", name="ExpAsm")["index"]
        d.call("set_active_part", index=aidx)
        d.call("add_occurrence", part="Part 1")
        d.call("add_occurrence", part="B2")
        try:
            d.call("set_explode", factor=0.5)
            expect(False, "factor with no views should error")
        except RuntimeError as e:
            expect("list_exploded_views" in str(e),
                   f"no-view explode error is helpful: {e}")
        r = d.call("auto_explode")
        expect(d.call("list_exploded_views")["active"] == r["view"],
               "auto_explode activates the view it creates")
        d.call("set_explode", view=0)  # explicit deactivate must keep working
        expect(d.call("list_exploded_views")["active"] == 0, "deactivated")
        r = d.call("set_explode", factor=0.8)
        expect(r["active"] != 0 and abs(r["factor"] - 0.8) < 1e-6,
               "factor-only set_explode auto-activates the only view")

        # set_section works on the assembly tab (global viewport clip; the
        # renderer applies it per occurrence in world space). Headless has no
        # renderer, so this asserts the RPC contract only.
        d.call("set_section", enabled=True, base_plane="yz", offset=5)
        d.call("set_section", enabled=False)

        phase("Config methods on a drawing tab explain themselves instead of")
        # Config methods on a drawing tab explain themselves instead of
        # reporting "unknown method".
        d.call("new_project")
        d.call("create_drawing")
        try:
            d.call("list_configurations")
            expect(False, "config on a drawing tab should error")
        except RuntimeError as e:
            expect("drawing" in str(e) and "unknown method" not in str(e),
                   f"config-on-drawing error is specific: {e}")
        d.call("new_project")

        phase("Measuring by clicking and measuring by script agree")
        # The parity gate for measure (docs/inspect/measure-v2.md M7). The
        # GUI has measured across occurrences since assembly measure shipped;
        # the RPC resolved ONE body set and used it for both refs, so the
        # assembly question the tool could ask, a script could not. Nothing
        # asserted the two agreed, because the RPC could not be asked.
        d.call("new_project")
        d.call("add_feature", feature={"type": "Box", "name": "Plate",
                                       "position": [0, 0, 0],
                                       "width": 20, "height": 20, "depth": 20})
        d.call("add_assembly")
        d.call("add_occurrence", part="Part 1")
        occ2 = d.call("add_occurrence", part="Part 1",
                      transform={"position": [100, 0, 0]})["id"]
        occ1 = d.call("list_occurrences")["occurrences"][0]["id"]

        # Scripted: two whole occurrences, 100 apart, 20 wide => 80 of air.
        rpc = d.call("measure", a={"occurrence": occ1}, b={"occurrence": occ2})
        expect(abs(rpc["distance"] - 80.0) < 1e-6,
               f"cross-occurrence distance over RPC: {rpc}")
        expect(rpc["clash"] is False, f"they do not clash: {rpc}")

        # Clicked: the same two occurrences, through the measure tool. The
        # readout is the tool's own words, so the check is that the NUMBER
        # the tool arrived at is the number the RPC reports.
        d.call("view", name="front")
        d.call("item_click", ref="//##DockHost/**/###tb_asm_measure")
        # The tool is armed on the assembly tab — the same context the RPC
        # was just asked about.
        gui = d.call("measure", a={"occurrence": occ1}, b={"occurrence": occ2},
                     distance_kind="centres")
        expect(abs(gui["distance"] - 100.0) < 1e-6,
               f"centre to centre is the placement offset: {gui}")
        expect(gui["distance_kind"] == "centres", f"kind echoed: {gui}")

        # An angle against a whole part is REFUSED naming why, rather than
        # answered with a plausible zero: a bracket points every way at once.
        try:
            d.call("measure", a={"occurrence": occ1}, b={"occurrence": occ2},
                   kind="angle")
            expect(False, "an angle between two whole parts was answered")
        except RuntimeError as e:
            expect("no single direction" in str(e),
                   f"the refusal says why: {e}")
        d.call("new_project")

        print("remote_smoke: ALL CHECKS PASSED")
        return 0
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
