#!/usr/bin/env python3
"""Integration test: V5 ergonomics papercuts — set_camera eye, occurrence
rotate {axis, angle_deg}, per-feature param docs in describe, and the honest
render_still assembly error.

Usage: papercuts_smoke.py <path-to-designer-binary>
"""
import math
import os
import subprocess
import sys
import tempfile

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
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_papercut_")
    env["XDG_STATE_HOME"] = tmp
    app = subprocess.Popen([binary, "--headless", "--remote", str(port)],
                           env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    try:
        d = Designer.wait_for(port)
        d.call("new_project")

        # --- describe: every feature type documents its dialect -----------
        desc = d.call("describe")
        undocumented = [f["type"] for f in desc["feature_types"]
                        if not f.get("params")]
        expect(not undocumented, f"feature types without params: "
                                 f"{undocumented}")

        # --- set_camera eye: intuitive camera placement -------------------
        d.call("set_camera", eye=[100, 50, 200], target=[0, 0, 0])
        cam = d.call("get_state")["camera"]
        expect(all(abs(a - b) < 0.1 for a, b in
                   zip(cam["position"], [100, 50, 200])),
               f"camera eye: {cam}")

        # --- rotate {axis, angle_deg} on occurrences ----------------------
        d.call("add_feature", feature={
            "type": "Box", "name": "B", "position": [0, 0, 0],
            "width": 10, "height": 10, "depth": 10})
        part = d.call("list_parts")["parts"][0]["name"]
        d.call("add_assembly", name="Rig")
        d.call("set_active_part", name="Rig")
        d.call("add_occurrence", part=part, name="Spun", fixed=True,
               transform={"position": [0, 0, 0],
                          "rotate": {"axis": [0, 1, 0], "angle_deg": 90}})
        occs = d.call("list_occurrences")["occurrences"]
        spun = next(o for o in occs if o["name"] == "Spun")
        q = spun["transform"]["orientation"]
        s2 = math.sin(math.radians(45))
        expect(all(abs(a - b) < 1e-4 for a, b in
                   zip(q, [0, s2, 0, math.cos(math.radians(45))])),
               f"rotate quaternion: {q}")
        # ...and both at once is rejected instead of guessing.
        try:
            d.call("add_occurrence", part=part, name="Bad", fixed=True,
                   transform={"position": [0, 0, 0],
                              "orientation": [0, 0, 0, 1],
                              "rotate": {"axis": [0, 1, 0], "angle_deg": 5}})
            raise AssertionError("orientation+rotate should be rejected")
        except RuntimeError as e:
            expect("not both" in str(e), f"wrong error: {e}")

        # --- render_still errors honestly on an assembly tab --------------
        try:
            d.call("render_still", path=os.path.join(tmp, "x.png"))
            raise AssertionError("render_still on assembly should error")
        except RuntimeError as e:
            expect("part-only" in str(e), f"wrong error: {e}")

        print("papercuts_smoke: all checks passed")
        return 0
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
