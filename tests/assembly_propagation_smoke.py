#!/usr/bin/env python3
"""Integration test: A4 part->assembly change propagation, headless JSON-RPC.

The contract under test: an assembly whose referenced parts regenerated since
its last solve re-solves on tab activation — WITHOUT an explicit
solve_assembly. A fastened mate anchors an arm to the top cap face of an
expression-driven pad; editing the driving variable in the part tab and then
merely activating the assembly tab must move the arm.

Usage: assembly_propagation_smoke.py <path-to-designer-binary>
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


def main():
    binary = sys.argv[1]
    port = free_port()
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_propagation_")
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
        expect(d.call("ping")["ok"], "ping")

        # --- base: a pad whose height is variable-driven --------------------
        d.call("new_project")
        d.call("add_part", name="base")
        d.call("set_variable", name="h", expression="4")
        sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "S",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "rect", "c1": [0, 0], "c2": [10, 10]}]})
        d.call("add_feature", feature={
            "type": "Extrude", "name": "Pad", "sketch_id": sk["id"],
            "distance": 4.0, "distance_expr": "h", "mode": "new_body"})

        # The pad's top cap face — a persistent ElementRef for the mate.
        body = d.call("list_elements")["bodies"][0]
        caps = [f["name"] for f in body["faces"] if "cap1" in f["name"]]
        expect(len(caps) == 1, f"one top cap face: {body['faces']}")
        top = {"feature_id": body["created_by"], "element": caps[0]}

        # --- arm + assembly: arm fastened onto the pad's top face -----------
        d.call("add_part", name="arm")
        d.call("add_feature", feature={
            "type": "Box", "name": "Blk", "position": [0, 0, 0],
            "width": 2, "height": 2, "depth": 2})
        d.call("add_assembly", name="rig")
        o1 = d.call("add_occurrence", part="base", fixed=True)["id"]
        o2 = d.call("add_occurrence", part="arm",
                    transform={"position": [50, 50, 50],
                               "orientation": [0, 0, 0, 1]})["id"]
        mate = d.call("add_mate", type="fastened", name="seat",
                      a={"occurrence": o1, "face": top},
                      b={"occurrence": o2, "origin": True},
                      flipped=False)
        expect(mate.get("errored_mates", 0) == 0, f"fastened solved: {mate}")

        def arm_z():
            occ = [o for o in d.call("list_occurrences")["occurrences"]
                   if o["id"] == o2][0]
            return occ["transform"]["position"][2]

        expect(abs(arm_z() - 4.0) < 1e-4,
               f"arm seated on the h=4 top face: z={arm_z()}")

        # --- edit the part, come back: NO solve_assembly anywhere -----------
        parts = {p["name"]: p for p in d.call("list_parts")["parts"]}
        d.call("set_active_part", index=parts["base"]["index"])
        d.call("set_variable", name="h", expression="8")  # pad grows
        d.call("set_active_part", index=parts["rig"]["index"])
        expect(abs(arm_z() - 8.0) < 1e-4,
               f"activation re-solved: arm followed the face to z={arm_z()}")

        # --- no spurious re-solve when nothing changed -----------------------
        d.call("set_active_part", index=parts["arm"]["index"])
        d.call("set_active_part", index=parts["rig"]["index"])
        expect(abs(arm_z() - 8.0) < 1e-4, "clean round trip: pose unchanged")

        # --- in-context entry/exit (A4 phase 3) drives the same machinery ---
        occs = d.call("list_occurrences")["occurrences"]
        base_occ = [o for o in occs if o["id"] == o1][0]["id"]
        ctx = d.call("edit_in_context", occurrence=base_occ)
        expect(ctx["active"] == parts["base"]["index"],
               f"in-context activated the base part tab: {ctx}")
        d.call("set_variable", name="h", expression="6")  # edit in context
        d.call("set_active_part", index=parts["rig"]["index"])  # Return
        expect(abs(arm_z() - 6.0) < 1e-4,
               f"return from in-context re-solved: z={arm_z()}")

        print("assembly_propagation_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
