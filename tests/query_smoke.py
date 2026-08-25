#!/usr/bin/env python3
"""Integration test: query_geometry — declarative element predicates with
the expected_count assertion (survey round 2 adoption).

Model: a 30x20x10 block with two Ø6 and one Ø10 through-holes.

Usage: query_smoke.py <path-to-designer-binary>
"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402




def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_query_")
    env["XDG_STATE_HOME"] = tmp
    # ...and its own CONFIG dir, or the run edits the developer's real
    # preferences: set_settings persists immediately (headless runs no GUI
    # loop to debounce it), so a suite reset their font size to 120%.
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        d.call("new_project")
        d.call("add_feature", feature={
            "type": "Box", "name": "Block", "position": [0, 0, 0],
            "width": 30, "height": 20, "depth": 10})
        sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "Holes",
            "plane": {"origin": [0, 0, 10], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "circle", "center": [6, 10], "radius": 3},
                         {"type": "circle", "center": [24, 10], "radius": 3},
                         {"type": "circle", "center": [15, 10],
                          "radius": 5}]})
        r = d.call("add_feature", feature={
            "type": "Extrude", "name": "Drill", "sketch_id": sk["id"],
            "distance": 12, "mode": "cut", "reversed": True})
        expect(r["regen"]["ok"], f"drill: {r['regen']}")

        # "There are exactly 2 Ø6 bores with axis Z" — one call, one bool.
        r = d.call("query_geometry", geometry_type="cylinder", radius=3,
                   axis=[0, 0, 1], expected_count=2)
        expect(r["ok"] and r["count"] == 2, f"two Ø6 bores: {r}")
        expect(all("radius" in m and abs(m["radius"] - 3) < 1e-6
                   for m in r["matches"]), f"matches carry radius: {r}")

        # Axis mismatch: no X-axis bores exist.
        r = d.call("query_geometry", geometry_type="cylinder", radius=3,
                   axis=[1, 0, 0], expected_count=0)
        expect(r["ok"] and r["count"] == 0, f"no X bores: {r}")

        # The Ø10 bore, by radius alone.
        r = d.call("query_geometry", geometry_type="cylinder", radius=5,
                   expected_count=1)
        expect(r["ok"], f"one Ø10 bore: {r}")

        # A failed assertion is ok:false, not an exception — with the count.
        r = d.call("query_geometry", geometry_type="cylinder", radius=3,
                   expected_count=4)
        expect(not r["ok"] and r["count"] == 2, f"mismatch reported: {r}")

        # One top plane with normal +Z (the drilled face stays one face).
        r = d.call("query_geometry", geometry_type="plane", normal=[0, 0, 1],
                   expected_count=1)
        expect(r["ok"], f"one top plane: {r}")
        expect("element" in r["matches"][0],
               f"named faces carry their stable name: {r['matches'][0]}")

        # Edges: each through-hole has a top and bottom rim -> 4 Ø6 circles;
        # near_point narrows to the one rim by hole 1's top centre.
        r = d.call("query_geometry", element_type="edge",
                   geometry_type="circle", radius=3, expected_count=4)
        expect(r["ok"], f"four Ø6 rims: {r}")
        r = d.call("query_geometry", element_type="edge",
                   geometry_type="circle", radius=3, near_point=[6, 10, 10],
                   max_distance=4, expected_count=1)
        expect(r["ok"], f"one rim near the top of hole 1: {r}")

        # Torus faces are assertable (round-8 retro: bent tubes were
        # "other"). A half-donut: circle 20 off the z axis, swept 180.
        sk2 = d.call("add_feature", feature={
            "type": "Sketch", "name": "Bend",
            "plane": {"origin": [0, 0, 0], "normal": [0, -1, 0],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "circle", "center": [20, 40],
                          "radius": 4}]})
        r = d.call("add_feature", feature={
            "type": "Revolve", "name": "Half donut", "sketch_id": sk2["id"],
            "axis": {"kind": "z"}, "angle_deg": 180, "mode": "new_body"})
        expect(r["regen"]["ok"], f"half donut: {r['regen']}")
        r = d.call("query_geometry", geometry_type="torus", radius=4,
                   axis=[0, 0, 1], expected_count=1)
        expect(r["ok"], f"one torus face, minor radius 4: {r}")

        # max_results caps the listing, never the count.
        r = d.call("query_geometry", geometry_type="cylinder", radius=3,
                   max_results=1)
        expect(r["count"] == 2 and len(r["matches"]) == 1 and r["truncated"],
               f"truncation: {r}")

        print("query_smoke: all checks passed")
        return 0
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
