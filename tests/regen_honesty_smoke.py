#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: a mutating RPC that BREAKS the model says so.

RemoteHelpers.h states the contract: the regeneration summary is "appended as
'regen' to every mutating RPC's response ... so clients learn about failures
from the call that caused them instead of having to poll".

Several handlers did not honour it, and the failure mode was worse than a
missing field. Activating a configuration whose override broke a fillet
returned:

    {"active": 3, "bodies": 1, "volumes": [7999.999999999998]}

A success, one body, a plausible volume — and 7999.999... is exactly 20^3,
i.e. the UNFILLETED box. Nothing in the response said the fillet had failed, so
a script would build its next edit on a broken model. `undo` had the same hole.

This pins the contract for the handlers that can leave a FEATURE broken.
Studies are deliberately excluded: a study edit cannot break a feature, so
regen there would always report ok and tell a caller nothing.

Usage: regen_honesty_smoke.py <path-to-designer-binary>
"""
import math
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box, cylinder  # noqa: E402




def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def broken(response, label):
    """The response must carry regen AND name the failure."""
    expect("regen" in response, f"{label}: response carries no 'regen': {response}")
    reg = response["regen"]
    expect(reg["ok"] is False, f"{label}: regen.ok should be False: {reg}")
    expect(len(reg["failed"]) >= 1, f"{label}: regen.failed is empty: {reg}")
    names = [f["name"] for f in reg["failed"]]
    expect("F" in names, f"{label}: the broken fillet is not named: {reg}")
    # Decision-complete: the error says what went wrong, not just that it did.
    err = next(f["error"] for f in reg["failed"] if f["name"] == "F")
    expect(len(err) > 10, f"{label}: error is not explanatory: {err}")


def healthy(response, label):
    expect("regen" in response, f"{label}: response carries no 'regen': {response}")
    expect(response["regen"]["ok"] is True,
           f"{label}: regen.ok should be True: {response['regen']}")


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_regen_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        d.call("new_project")

        # A 20 mm box with a variable-driven fillet: small enough to build,
        # and an override can make the radius impossible for the geometry.
        d.call("set_variable", name="r", expression="2")
        blk = box(d, (0, 0, 0), (20, 20, 20), "B")["id"]
        edges = d.call("list_elements", body=0)["bodies"][0]["edges"]
        r = d.call("add_feature", feature={
            "type": "Fillet", "name": "F", "radius": 2, "radius_expr": "r",
            "edges": [{"feature_id": blk, "element": edges[0]["name"]}]})
        healthy(r, "the fillet builds to start with")

        cfg = d.call("add_configuration", name="huge")["id"]
        d.call("set_configuration", id=cfg,
               parameter_overrides=[{"name": "r", "expression": "50"}])

        # --- the reproduction ---------------------------------------------
        act = d.call("set_active_configuration", id=cfg)
        broken(act, "set_active_configuration")
        # The legacy hand-rolled keys stay (configuration_smoke reads them),
        # but they are exactly what made this look healthy: one body, and a
        # volume equal to the unfilleted box.
        expect(act["bodies"] == 1, f"legacy 'bodies' key kept: {act}")
        expect(abs(sum(act["volumes"]) - 8000.0) < 1e-3,
               f"the misleading volume is still 20^3: {act}")
        expect("digest" in act, f"and a digest is now carried too: {act}")

        # --- undo/redo had the same hole ----------------------------------
        healthy(d.call("undo"), "undo back to the working configuration")
        broken(d.call("redo"), "redo into the broken one")
        healthy(d.call("set_active_configuration", id=0), "back to default")

        # --- a variable edit that breaks a feature -------------------------
        broken(d.call("set_variable", name="r", expression="50"),
               "set_variable driving the radius impossible")
        healthy(d.call("set_variable", name="r", expression="2"),
                "and back")

        # --- loading is where a broken model most often ARRIVES ------------
        proj = os.path.join(tmp, "broken.dsgn")
        d.call("set_variable", name="r", expression="50")
        d.call("save_project", path=proj)
        d.call("new_project")
        opened = d.call("open_project", path=proj)
        broken(opened, "open_project on a project whose feature does not build")

        # --- a boolean whose result is not a well-formed solid -------------
        # `CADShape::IsValid` is a null check, so a boolean could return a
        # shape with a plausible volume that OpenCASCADE calls invalid, and
        # say nothing. Everything measured on it afterwards was then
        # confidently wrong: this configuration — a bicycle's rear dropouts
        # with two bowed stays landing on each of them, bored for the axle in
        # ONE pass through both plates and the 130 mm of air between — gives
        # "bad wire nesting", and the assembly sweep went on to report
        # 17,297 mm3 of a rear cog buried in a frame whose bounding box does
        # not reach it.
        #
        # The geometry is faithful on purpose. The same test written with
        # straight cylinders builds cleanly and asserts nothing, which is the
        # kind of check that passes for years while the thing it was written
        # for goes unguarded.
        d.call("new_project")

        def bowed(name, a, b, bulge, sag, od):
            """A solid pipe bowing `sag` toward `bulge`, as a frame's stays
            are bent. Mirrors generators/bike/bike_shop.bowed_tube."""
            u = [b[i] - a[i] for i in range(3)]
            length = math.dist(a, b)
            u = [c / length for c in u]
            dot = sum(u[i] * bulge[i] for i in range(3))
            v = [bulge[i] - u[i] * dot for i in range(3)]
            vlen = math.dist(v, [0, 0, 0])
            v = [c / vlen for c in v]
            normal = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                      u[0] * v[1] - u[1] * v[0]]
            # The arc through (0,0), (L/2, sag), (L,0), as the app's entity:
            # a centre, a radius and a counter-clockwise angular span.
            yc = (sag * sag - length * length / 4.0) / (2.0 * sag)
            radius = math.hypot(length / 2.0, yc)
            first = math.atan2(-yc, -length / 2.0)
            last = math.atan2(-yc, length / 2.0)
            mid = math.atan2(sag - yc, 0.0)
            tau = 2 * math.pi
            if (mid - first) % tau > (last - first) % tau:
                first, last = last, first
            sk = d.call("add_feature", feature={
                "type": "Sketch", "name": f"{name} path",
                "plane": {"origin": list(a), "normal": normal, "uAxis": u},
                "entities": [{"type": "arc", "center": [length / 2.0, yc],
                              "radius": radius, "start_angle": first,
                              "end_angle": last}]})["id"]
            return d.call("add_feature", feature={
                "type": "Pipe", "name": name, "path_id": sk,
                "outer_diameter": od, "wall_thickness": 0.0})["id"]

        axle_x, axle_z, dy = -460.43, 65.0, 65.5
        ids = []
        for side in (1, -1):
            drop = [axle_x, side * dy, axle_z]
            ids.append(cylinder(d, [axle_x, side * dy - 2.5, axle_z], "y",
                                26.0, 5.0, f"Dropout {side}")["id"])
            ids.append(bowed(f"Chainstay {side}", [0.0, side * 22.0, 12.0],
                             drop, [0, side, 0], 5.0, 16.0))
            ids.append(bowed(f"Seatstay {side}", [-168.7, side * 11.0, 519.3],
                             drop, [0, side, 0], 4.0, 16.0))
        weld = ids[0]
        for k, tool in enumerate(ids[1:], 1):
            weld = d.call("add_feature", feature={
                "type": "Boolean", "name": f"Weld {k}", "target_id": weld,
                "tool_id": tool, "op": "union"})["id"]
        bore = cylinder(d, [axle_x, -70.0, axle_z], "y", 4.55, 140.0,
                        "Axle bore")["id"]
        reg = d.call("add_feature", feature={
            "type": "Boolean", "name": "One-pass bore", "target_id": weld,
            "tool_id": bore, "op": "subtract"})["regen"]
        expect(not reg["ok"],
               "this is the configuration that produced a badly nested "
               f"solid; if the kernel has changed, find another: {reg}")
        failed = [f for f in reg["failed"] if f["name"] == "One-pass bore"]
        expect(failed, f"the bore is the feature that failed: {reg}")
        expect("INVALID" in failed[0]["error"],
               f"and it says the solid is invalid: {failed[0]['error']}")

        # --- a cut the kernel got wrong while BRepCheck calls it valid ----
        # The PES-on-OM615 intake flange, reduced (tests/test_boolean_volume
        # .cpp builds the same document): a slot whose round ends are two
        # swept bores' own cylinders. The kernel kept the bores' floors
        # inside the slot; in the full manifold the bores came out inside
        # out. The refusal belongs in the response of the call that made
        # the cut, naming the feature and both volumes.
        d.call("new_project")
        plane_x = {"normal": [-1, 0, 0], "uAxis": [0, 1, 0]}
        blk = d.call("add_feature", feature={
            "type": "Sketch", "name": "Block",
            "plane": dict(plane_x, origin=[-100, -97, 283.3999938964844]),
            "entities": [
                {"type": "line", "a": [-40, -25], "b": [40, -25]},
                {"type": "line", "a": [40, -25], "b": [40, 25]},
                {"type": "line", "a": [40, 25], "b": [-40, 25]},
                {"type": "line", "a": [-40, 25], "b": [-40, -25]}]})["id"]
        d.call("add_feature", feature={
            "type": "Extrude", "name": "Block", "sketch_id": blk,
            "distance": 20.0, "mode": "new_body"})
        for y in (-112.0, -82.0):
            arc = d.call("add_feature", feature={
                "type": "Sketch", "name": "Bore arc",
                "plane": {"normal": [0, -1, 0], "origin": [0, y, 0],
                          "uAxis": [1, 0, 0]},
                "entities": [{"type": "arc",
                              "center": [-100, 341.9468688964844],
                              "radius": 58.546875,
                              "start_angle": 2.4474694465108136,
                              "end_angle": -1.5707963267948986}]})["id"]
            d.call("add_feature", feature={
                "type": "Pipe", "name": "Bore", "path_id": arc,
                "outer_diameter": 26.0, "mode": "cut"})
        half = math.pi / 2
        slot = d.call("add_feature", feature={
            "type": "Sketch", "name": "Slot",
            "plane": dict(plane_x, origin=[-99, -97, 283.3999938964844]),
            "entities": [
                {"type": "line", "a": [-15, 13], "b": [15, 13]},
                {"type": "line", "a": [-15, -13], "b": [15, -13]},
                {"type": "arc", "center": [15, 0], "radius": 13.0,
                 "start_angle": -half, "end_angle": half},
                {"type": "arc", "center": [-15, 0], "radius": 13.0,
                 "start_angle": half, "end_angle": 3 * half}]})["id"]
        reg = d.call("add_feature", feature={
            "type": "Extrude", "name": "Slot", "sketch_id": slot,
            "distance": 14.0, "mode": "cut"})["regen"]
        failed = [f for f in reg["failed"] if f["name"] == "Slot"]
        expect(not reg["ok"] and failed,
               f"the slot the kernel got wrong is refused in its own call: {reg}")
        why = failed[0]["error"]
        expect("cut 'Slot'" in why and "mm3 before" in why
               and "mm3 after" in why and "INSIDE the tool" in why,
               f"naming the feature, the operation, both volumes and why: {why}")

        print("regen_honesty_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    main()
