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
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402




def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_papercut_")
    env["XDG_STATE_HOME"] = tmp
    # ...and its own CONFIG dir, or the run edits the developer's real
    # preferences: set_settings persists immediately (headless runs no GUI
    # loop to debounce it), so a suite reset their font size to 120%.
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
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
        box(d, (0, 0, 0), (10, 10, 10), "B")
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

        # --- decision-complete errors: measured state + vocabulary --------
        d.call("set_active_part", index=0)
        try:
            d.call("delete_feature", index=99)
            raise AssertionError("index 99 should error")
        except RuntimeError as e:
            expect("out of range" in str(e) and "0.." in str(e)
                   and "get_state" in str(e), f"ranged index error: {e}")
        sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "Probe",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "circle", "center": [2, 2], "radius": 1}]})
        try:
            d.call("add_constraint", sketch_id=9999, kind="radius",
                   refs=[0], value=2)
            raise AssertionError("bad sketch id should error")
        except RuntimeError as e:
            expect("9999" in str(e) and str(sk["id"]) in str(e),
                   f"names the given AND the candidates: {e}")
        try:
            d.call("view", name="isometric")
            raise AssertionError("bad view should error")
        except RuntimeError as e:
            expect("iso" in str(e) and "front" in str(e),
                   f"lists the accepted views: {e}")
        try:
            d.call("bom")
            raise AssertionError("bom on a part tab should error")
        except RuntimeError as e:
            expect("set_active_part" in str(e), f"carries the remedy: {e}")
        d.call("set_active_part", name="Rig")

        # --- render_still on an assembly tab ------------------------------
        # It used to refuse as "part-only", which was never true of the RT
        # scene sync — that walks every visible occurrence at its solved
        # placement. What must still fail is a tab with nothing in it, which
        # otherwise "succeeds" with a picture of the environment alone.
        # (Headless has no renderer at all, so that is the error here; the
        # rendering itself is exercised where there is a GPU.)
        r = d.call("render_still", path=os.path.join(tmp, "x.png"))
        expect(not r["ok"] and "part-only" not in r.get("error", ""),
               f"an assembly is no longer refused as part-only "
               f"(headless has no renderer: {r.get('error', '')})")
        d.call("add_assembly", name="Empty rig")
        try:
            d.call("render_still", path=os.path.join(tmp, "y.png"))
            raise AssertionError("render_still on an empty assembly should "
                                 "error")
        except RuntimeError as e:
            expect("no occurrences" in str(e), f"names the real problem: {e}")

        print("papercuts_smoke: all checks passed")
        return 0
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
