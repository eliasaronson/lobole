#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Named datum frames over the RPC, headless (docs/assemblies/solver.md#connector-frames).

The property under test is the one the SO-101 gripper dogfood lacked: a
position an agent never transcribed. A Frame is built FROM the geometry (a
bolt circle, a face), a mate anchors to it BY NAME, and when the geometry
moves — a variable drives the boss height — the mate follows with nothing
re-entered. Also: the `frame` query in part and world space, and the
refusals, each naming what exists.

Usage: frame_smoke.py <path-to-designer-binary>
"""
import math
import os
import sys
import tempfile

from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def refused(d, method, needles, **kw):
    try:
        d.call(method, **kw)
    except RuntimeError as e:
        for needle in needles:
            expect(needle in str(e),
                   f"{method} refused, but not naming '{needle}': {e}")
        return str(e)
    raise AssertionError(f"{method} was accepted: {kw}")


def close(a, b, tol=1e-3):
    return abs(a - b) <= tol


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_frame_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        expect(d.call("ping")["ok"], "ping")
        d.call("new_project")

        # ---- A plate with a four-hole bolt pattern ----------------------
        d.call("add_part", name="plate")
        d.call("set_variable", name="boss_h", expression="12", scope="part")
        sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "Outline",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "rect", "c1": [0, 0], "c2": [40, 40]}]})["id"]
        plate = d.call("add_feature", feature={
            "type": "Extrude", "name": "Plate", "sketch_id": sk,
            "distance": 4.0, "mode": "new_body"})["id"]
        holes_sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "Holes",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [
                {"type": "circle", "center": [30, 20], "radius": 1.5},
                {"type": "circle", "center": [20, 30], "radius": 1.5},
                {"type": "circle", "center": [10, 20], "radius": 1.5},
                {"type": "circle", "center": [20, 10], "radius": 1.5}]})["id"]
        d.call("add_feature", feature={
            "type": "Extrude", "name": "Bores", "sketch_id": holes_sk,
            "distance": 5.0, "mode": "cut"})

        els = d.call("list_elements")["bodies"][0]
        bores = [f["name"] for f in els["faces"] if f["kind"] == "cylinder"]
        expect(len(bores) == 4, f"four bores expected, got {len(bores)}")

        # ---- A frame FROM the pattern: no coordinate typed --------------
        pattern = d.call("add_feature", feature={
            "type": "Frame", "name": "bolt_circle", "kind": "from_element",
            "elements": [{"feature_id": plate, "element": n} for n in bores]})
        expect(pattern["regen"]["ok"],
               f"bolt-circle frame built clean: {pattern}")
        pattern_id = pattern["id"]

        fr = d.call("frame", frame="bolt_circle")
        expect(fr["space"] == "part", f"part space: {fr}")
        expect(close(fr["origin"][0], 20.0) and close(fr["origin"][1], 20.0),
               f"the pattern's centre is (20, 20): {fr['origin']}")
        expect(close(abs(fr["z"][2]), 1.0), f"pattern axis is Z: {fr['z']}")
        expect(len(fr["matrix"]) == 16, "a 4x4 comes back as 16 numbers")
        # X aims at the first hole named, so the frame is oriented by the
        # pattern rather than by face order.
        first = d.call("frame", feature_id=plate, element=bores[0])
        aim = [first["origin"][i] - fr["origin"][i] for i in range(3)]
        norm = math.sqrt(sum(c * c for c in aim))
        dot = sum(fr["x"][i] * aim[i] / norm for i in range(3))
        expect(close(dot, 1.0),
               f"X points at the first hole (dot {dot:.4f})")

        # An OFFSET frame off that one, driven by an expression. The offset
        # is applied in the PARENT's axes — a bore's axis sign is the
        # kernel's, so the expected place is computed from the parent's own
        # frame rather than assumed to be +Z.
        d.call("add_feature", feature={
            "type": "Frame", "name": "seat", "kind": "offset",
            "parent_id": pattern_id, "offset": [0, 0, 0],
            "offset_expr": ["", "", "boss_h"]})
        seat = d.call("frame", frame="seat")
        want_z = fr["origin"][2] + 12.0 * fr["z"][2]
        expect(close(seat["origin"][2], want_z),
               f"the seat rides boss_h along the pattern's Z: "
               f"{seat['origin'][2]} vs {want_z}")
        seat_z_before = seat["origin"][2]

        # ---- The refusals name what exists ------------------------------
        refused(d, "frame", ["no Frame", "bolt_circle"], frame="nope")
        refused(d, "frame", ["feature_id", "element"], feature_id=plate)
        refused(d, "add_feature", ["from_element", "elements"], feature={
            "type": "Frame", "name": "Empty", "kind": "from_element"})
        refused(d, "add_feature", ["explicit", "from_element", "offset"],
                feature={"type": "Frame", "name": "Bad", "kind": "sideways"})
        # A frame whose element is gone reports it and does not resolve.
        broken = d.call("add_feature", feature={
            "type": "Frame", "name": "broken", "kind": "from_element",
            "elements": [{"feature_id": plate, "element": "no-such-face"}]})
        failed = broken["regen"]["failed"]
        expect(failed and "no-such-face" in failed[0]["error"]
               and "list_elements" in failed[0]["error"],
               f"a dangling element is reported by the call that made it, "
               f"naming the discovery call: {broken}")
        d.call("delete_feature", id=broken["id"])

        # ---- A second part, and a mate BY FRAME NAME --------------------
        d.call("add_part", name="arm")
        arm_sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "Arm",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "rect", "c1": [0, 0], "c2": [6, 6]}]})["id"]
        d.call("add_feature", feature={
            "type": "Extrude", "name": "Arm", "sketch_id": arm_sk,
            "distance": 3.0, "mode": "new_body"})

        d.call("add_assembly", name="rig")
        base = d.call("add_occurrence", part="plate", fixed=True)["id"]
        mover = d.call("add_occurrence", part="arm", transform={
            "position": [100, 100, 100], "orientation": [0, 0, 0, 1]})["id"]

        # The name, not an id and not a coordinate.
        mate = d.call("add_mate", type="fastened", name="arm on the pattern",
                      a={"occurrence": base, "frame": "seat"},
                      b={"occurrence": mover, "origin": True},
                      flipped=False)
        expect(mate.get("errored_mates", 0) == 0,
               f"the frame mate solved clean: {mate}")

        placed = d.call("frame", occurrence=mover)
        expect(placed["space"] == "world", f"world space: {placed}")
        expect(close(placed["origin"][0], 20.0)
               and close(placed["origin"][1], 20.0),
               f"the arm sits on the pattern's centre: {placed['origin']}")
        expect(close(placed["origin"][2], seat_z_before),
               f"and at the seat's height: {placed['origin']}")

        # The same query, reaching through the occurrence to the frame.
        through = d.call("frame", occurrence=base, frame="seat")
        expect(close(through["origin"][2], seat_z_before),
               f"a frame placed in the world: {through}")
        expect("seat" in through["source"], f"source names it: {through}")

        # ---- THE POINT: move the geometry, the mate follows -------------
        d.call("set_active_part", part="plate")
        d.call("set_variable", name="boss_h", expression="30", scope="part")
        d.call("set_active_part", part="rig")
        d.call("solve_assembly")
        moved = d.call("frame", occurrence=mover)
        drift = moved["origin"][2] - placed["origin"][2]
        expect(close(abs(drift), 18.0),
               f"the arm followed the variable by 18 mm, not {drift:.3f}")

        # ---- Refusals on the mate side ----------------------------------
        refused(d, "add_mate", ["no Frame", "bolt_circle", "seat"],
                type="fastened",
                a={"occurrence": base, "frame": "not_a_frame"},
                b={"occurrence": mover, "origin": True})
        refused(d, "add_mate", ["frame", "origin", "face"], type="fastened",
                a={"occurrence": base},
                b={"occurrence": mover, "origin": True})

        # ---- Round-trip: the anchor is stored as a frame -----------------
        path = os.path.join(tmp, "frames.dsgn")
        d.call("save_project", path=path)
        d.call("new_project")
        d.call("open_project", path=path)
        d.call("set_active_part", part="rig")
        mates = d.call("list_mates")["mates"]
        expect(len(mates) == 1, f"one mate survives: {mates}")
        anchor = mates[0]["a"]["anchor"]
        expect("frame" in anchor,
               f"the anchor reloads as a frame, not a face: {anchor}")
        after = d.call("frame", occurrence=mover)
        expect(close(after["origin"][2], moved["origin"][2]),
               f"and the pose survives: {after['origin']} vs "
               f"{moved['origin']}")
        print("frame_smoke: ok")
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()


if __name__ == "__main__":
    main()
