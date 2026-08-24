#!/usr/bin/env python3
"""Integration test: V2 strict feature inputs + regen digests + BOM warnings.

Covers: unknown feature-JSON fields error on the RPC path (the Box
mode:"add" class the dock round hit), valid dialects keep working, mutating
responses carry a body digest with deltas (added/removed/volume), and bom
explains WHY a mass is null.

Usage: strict_inputs_smoke.py <path-to-designer-binary>
"""
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


def expect_error(d, needle, **call):
    try:
        d.call(**call)
    except RuntimeError as e:
        expect(needle in str(e), f"expected '{needle}' in: {e}")
        return
    raise AssertionError(f"call {call} should have errored with '{needle}'")


def main():
    binary = sys.argv[1]
    port = free_port()
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_strict_")
    env["XDG_STATE_HOME"] = tmp
    # ...and its own CONFIG dir, or the run edits the developer's real
    # preferences: set_settings persists immediately (headless runs no GUI
    # loop to debounce it), so a suite reset their font size to 120%.
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = subprocess.Popen([binary, "--headless", "--remote", str(port)],
                           env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    try:
        d = Designer.wait_for(port)
        d.call("new_project")

        # --- Unknown fields error instead of vanishing --------------------
        expect_error(d, "unknown field 'mode'", method="add_feature",
                     feature={"type": "Box", "name": "B", "position": [0, 0, 0],
                              "width": 10, "height": 10, "depth": 10,
                              "mode": "add"})
        expect_error(d, "unknown field 'distace'", method="add_feature",
                     feature={"type": "Box", "name": "B", "position": [0, 0, 0],
                              "width": 10, "height": 10, "depth": 10,
                              "distace": 5})

        # --- Valid dialects still work, and carry a digest ----------------
        r = d.call("add_feature", feature={
            "type": "Box", "name": "Base", "position": [0, 0, 0],
            "width": 10, "height": 10, "depth": 10})
        expect(r["regen"]["ok"], f"box regen: {r}")
        dg = r["digest"]
        expect(len(dg["bodies"]) == 1, f"digest bodies: {dg}")
        expect(abs(dg["delta"]["volume"] - 1000.0) < 1e-6,
               f"digest volume delta: {dg['delta']}")
        expect(dg["delta"]["added"] == [dg["bodies"][0]["name"]],
               f"digest added: {dg['delta']}")
        expect(dg["bodies"][0]["solids"] == 1, f"solids: {dg['bodies']}")

        # Sketch + expression-driven cut extrude: the expr fields must not be
        # rejected, and the cut's digest delta is negative.
        d.call("set_variable", name="hole_r", expression="2", scope="project")
        sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "S",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "circle", "center": [5, 5], "radius": 2,
                          "r_expr": "hole_r"}]})
        r = d.call("add_feature", feature={
            "type": "Extrude", "name": "Cut", "sketch_id": sk["id"],
            "distance": 1.0, "distance_expr": "hole_r * 6", "mode": "cut"})
        expect(r["regen"]["ok"], f"cut regen: {r}")
        dv = r["digest"]["delta"]["volume"]
        import math
        expect(abs(dv + math.pi * 4 * 10) < 1.0, f"cut delta: {dv}")
        expect(r["digest"]["delta"]["added"] == [], f"cut added: {r['digest']}")

        # Delete the cut: volume returns, no bodies added/removed.
        idx = d.call("get_state")
        n = len(idx["features"])
        r = d.call("delete_feature", index=n - 1)
        expect(abs(r["digest"]["delta"]["volume"] - math.pi * 4 * 10) < 1.0,
               f"delete delta: {r['digest']['delta']}")

        # modify_feature validates too.
        expect_error(d, "unknown field 'wdith'", method="modify_feature",
                     index=0,
                     feature={"type": "Box", "name": "Base",
                              "position": [0, 0, 0], "wdith": 20,
                              "height": 10, "depth": 10, "width": 10})

        # --- BOM warnings name the material-less body ---------------------
        d.call("add_part", name="naked")
        d.call("set_active_part", name="naked")
        d.call("add_feature", feature={
            "type": "Box", "name": "Bare", "position": [0, 0, 0],
            "width": 5, "height": 5, "depth": 5})  # no set_material
        part = d.call("list_parts")["parts"][0]["name"]
        d.call("add_assembly", name="Rig")
        d.call("set_active_part", name="Rig")
        d.call("add_occurrence", part="naked", name="N1", fixed=True)
        bom = d.call("bom")
        expect(bom["total"]["mass_g"] is None, f"total must be null: {bom}")
        warns = bom.get("warnings", [])
        expect(any("no material" in w and "Bare" in w for w in warns),
               f"bom warnings must name the body: {warns}")

        print("strict_inputs_smoke: all checks passed")
        return 0
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
