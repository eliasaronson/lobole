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

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402

BOX = {"type": "Box", "name": "Box 1", "position": [0, 0, 0],
       "width": 2, "height": 2, "depth": 2}


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


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
    port = free_port()
    app = subprocess.Popen([binary, "--remote", str(port)],
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    tmp = tempfile.mkdtemp(prefix="designer_remote_")
    try:
        d = Designer.wait_for(port)

        expect(d.call("ping")["ok"], "ping")

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

        # UI tier: same edit path a human uses
        d.call("menu_click", path="Edit/Undo")
        state = d.call("get_state")
        expect(abs(state["bodies"][0]["volume"] - 8.0) < 1e-6, "undo via menu")

        d.call("key_press", chord="Ctrl+Z")
        state = d.call("get_state")
        expect(len(state["features"]) == 0, "undo via keyboard")
        d.call("key_press", chord="Ctrl+Y")
        expect(len(d.call("get_state")["features"]) == 1, "redo via keyboard")

        # Viewport: rotate the camera like a human drag
        before = d.call("get_state")["camera"]["position"]
        d.call("viewport_drag", x0=0.5, y0=0.5, x1=0.6, y1=0.6, button=2)
        after = d.call("get_state")["camera"]["position"]
        expect(before != after, "middle-drag rotated the camera")

        # Select a face programmatically, then orient to it
        mesh_id = d.call("get_state")["bodies"][0]["mesh"]
        sel = d.call("select_face", mesh=mesh_id, face=0)
        expect(len(sel["normal"]) == 3, "face selected")
        d.call("orient_to_face")

        # Project save/load roundtrip
        project = os.path.join(tmp, "part.json")
        d.call("save_project", path=project)
        d.call("new_project")
        expect(len(d.call("get_state")["features"]) == 0, "cleared")
        d.call("open_project", path=project)
        expect(len(d.call("get_state")["features"]) == 1, "reloaded")

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

        # Preview -> accept/cancel feature flow
        d.call("new_project")
        d.call("menu_click", path="Create/Box")
        st = d.call("get_state")
        expect(st["pending_feature_id"] != 0, "box is a pending preview")
        expect(not st["can_undo"], "preview not yet on undo stack")
        d.call("accept_pending")
        st = d.call("get_state")
        expect(st["pending_feature_id"] == 0 and st["can_undo"],
               "accept commits one undoable action")
        d.call("menu_click", path="Create/Sphere")
        d.call("cancel_pending")
        expect(len(d.call("get_state")["features"]) == 1, "cancel discards")

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

        # Deleting an entity removes it and its dependent constraint
        dr = d.call("delete_entity", sketch_id=sk["id"], entity_id=eid[1])
        ents2 = d.call("get_state")["features"][0]["entities"]
        expect(len(ents2) == 1 and ents2[0]["id"] == eid[0],
               "delete_entity removed the second line, kept the first")
        expect(dr["dropped_constraints"] == 0, "coincident is a trivial drop")

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
        d.call("add_constraint", sketch_id=ska["id"], kind="angle",
               refs=[[aid[0], 0], [aid[1], 0]], value=0.7853981634)
        ea = d.call("get_state")["features"][0]["entities"]
        d0 = [ea[0]["b"][0] - ea[0]["a"][0], ea[0]["b"][1] - ea[0]["a"][1]]
        d1 = [ea[1]["b"][0] - ea[1]["a"][0], ea[1]["b"][1] - ea[1]["a"][1]]
        ang = abs(math.atan2(d0[0] * d1[1] - d0[1] * d1[0],
                             d0[0] * d1[0] + d0[1] * d1[1]))
        expect(abs(ang - 0.7853981634) < 1e-3, "angle solved to 45 degrees")

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
        mpath = os.path.join(tmp, "material.json")
        d.call("save_project", path=mpath)
        d.call("new_project")
        d.call("open_project", path=mpath)
        expect(d.call("get_state")["bodies"][0]["material"] == "steel",
               "material survives save/load")

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

        # Error handling
        try:
            d.call("delete_feature", index=99)
            expect(False, "expected error for bad index")
        except RuntimeError:
            pass

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
