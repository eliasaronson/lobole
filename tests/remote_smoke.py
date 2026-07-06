#!/usr/bin/env python3
"""Integration test: drive a live Designer through the remote interface.

Usage: remote_smoke.py <path-to-designer-binary>
"""
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
        sketch = d.call("add_feature", feature={
            "type": "Sketch", "name": "Sketch 1",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "rect", "c1": [0, 0], "c2": [4, 2]}]})
        d.call("add_feature", feature={
            "type": "Extrude", "name": "Pad", "sketch_id": sketch["id"],
            "distance": 3.0, "mode": "new_body"})
        state = d.call("get_state")
        expect(len(state["features"]) == 2, "sketch + extrude")
        expect(abs(state["bodies"][0]["volume"] - 24.0) < 1e-6,
               "extruded volume 24")

        # Screenshot: the controller can see what the user sees
        d.call("fit_view")
        shot = os.path.join(tmp, "view.png")
        d.call("screenshot", path=shot)
        expect(os.path.exists(shot) and os.path.getsize(shot) > 1000,
               "screenshot written")

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
