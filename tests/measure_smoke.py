#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: C3 measure + mass-properties surface, headless over RPC.

The GUI Measure tool sits on the `measure` RPC and Measure:: core; this
exercises that core headlessly: a face area, an edge length, a circular
edge's radius (the C3 addition), the distance between two elements, and the
mass_properties center-of-mass the CoG marker draws.

Usage: measure_smoke.py <path-to-designer-binary>
"""
import math
import os
import subprocess
import sys
import tempfile

from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box, sphere  # noqa: E402




def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_measure_")
    env["XDG_STATE_HOME"] = tmp
    # ...and its own CONFIG dir, or the run edits the developer's real
    # preferences: set_settings persists immediately (headless runs no GUI
    # loop to debounce it), so a suite reset their font size to 120%.
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        expect(d.call("ping")["ok"], "ping")

        # A cylinder (r=5, h=10) via a circle extrude: circular edges + caps.
        d.call("new_project")
        sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "S",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "circle", "center": [0, 0], "radius": 5}]})
        d.call("add_feature", feature={
            "type": "Extrude", "name": "Pad", "sketch_id": sk["id"],
            "distance": 10, "mode": "new_body"})

        els = d.call("list_elements")["bodies"][0]
        fid = els["created_by"]

        # A circular edge reports its radius (the C3 Describe addition).
        radius_seen = None
        length_seen = None
        for e in els["edges"]:
            r = d.call("measure", a={"feature_id": fid, "element": e["name"]})
            expect(r["kind"] == "edge", f"edge kind: {r}")
            if "radius" in r:
                radius_seen = r["radius"]
                expect(abs(r["length_mm"] - 2 * math.pi * 5) < 1e-3,
                       f"circumference: {r}")
            else:
                length_seen = r["length_mm"]
        expect(radius_seen is not None and abs(radius_seen - 5.0) < 1e-6,
               f"a circular edge reported radius 5: {radius_seen}")

        # A cap face area = pi r^2, and the CYLINDRICAL WALL reports the bore
        # it is. That second half is the regression: this loop measured every
        # face and only ever asserted area, so a wall came back as 314 mm^2 of
        # surface and no diameter — the number every CAD system gives you for
        # a hole — while query_geometry had the radius all along.
        cap = None
        wall = None
        for f in els["faces"]:
            a = d.call("measure", a={"feature_id": fid, "element": f["name"]})
            if a["shape"] == "cylinder":
                wall = a
            elif abs(a["area_mm2"] - math.pi * 25) < 1e-2:
                cap = a
        expect(cap is not None, "a cap face reports pi*r^2 area")
        expect(cap["shape"] == "plane", f"a cap is a plane: {cap}")
        expect("radius" not in cap, f"a flat cap has no radius: {cap}")
        expect(wall is not None, "the cylindrical wall was measured")
        expect(abs(wall["radius"] - 5.0) < 1e-6, f"wall radius 5: {wall}")
        expect(abs(wall["diameter"] - 10.0) < 1e-6, f"wall diameter 10: {wall}")
        expect("axis" in wall, f"a cylinder reports its axis: {wall}")

        # list_elements carries the same radius. It read it off DescribeFaceAt
        # and dropped it on the floor, so the obvious listing did not carry
        # the obvious number.
        listed = [f for f in els["faces"] if f["kind"] == "cylinder"]
        expect(len(listed) == 1, f"one cylindrical face listed: {listed}")
        expect(abs(listed[0]["radius"] - 5.0) < 1e-6,
               f"list_elements keeps the face radius: {listed[0]}")


        # Distance between the two cap faces = the height (10).
        cap_names = []
        for f in els["faces"]:
            a = d.call("measure", a={"feature_id": fid, "element": f["name"]})
            if abs(a["area_mm2"] - math.pi * 25) < 1e-2:
                cap_names.append(f["name"])
        expect(len(cap_names) == 2, f"two caps: {cap_names}")
        dist = d.call("measure",
                      a={"feature_id": fid, "element": cap_names[0]},
                      b={"feature_id": fid, "element": cap_names[1]})
        expect(abs(dist["distance"] - 10.0) < 1e-3,
               f"cap-to-cap distance is the height: {dist}")

        # `describe` must name what the handler DOES, and this row advertised
        # "point/distance/angle/radius" for a year while only distance was
        # implemented. Introspection is what an agent trusts INSTEAD of
        # guessing, so a row promising an unwritten branch is worse than no
        # row at all. Asserted in BOTH directions, against the live handler,
        # so it cannot drift again either way: a claim with no branch fails,
        # and a branch nobody documented fails too.
        row = None
        for mrow in d.call("describe")["methods"]:
            if mrow["name"] == "measure":
                row = mrow["doc"].lower()
        expect(row is not None, "describe has a measure row")

        pair = dict(a={"feature_id": fid, "element": cap_names[0]},
                    b={"feature_id": fid, "element": cap_names[1]})

        def works(**kw):
            try:
                d.call("measure", **kw)
                return True
            except RuntimeError:
                return False

        # An unrecognised kind is REFUSED naming what IS accepted, never
        # quietly measured as a distance — a substituted answer is always
        # plausible (CLAUDE.md, "refuse, don't substitute"). That refusal is
        # also the machine-readable vocabulary the drift check below needs.
        try:
            d.call("measure", kind="nonesuch", **pair)
            expect(False, "an unknown measure kind was accepted")
            accepted = []
        except RuntimeError as e:
            expect("nonesuch" in str(e), f"the refusal names the bad kind: {e}")
            # The contract's structured refusal lists them
            # (docs/scripting/rpc.md#structured-errors).
            accepted = e.diagnostic["accepted"]
        expect(len(accepted) >= 2, f"the refusal listed the alternatives: {accepted}")

        # Every kind the refusal advertises must WORK, and must be named in
        # the describe row. Driven off the refusal rather than off a literal
        # list here, so adding a kind and forgetting to document it fails.
        for k in accepted:
            expect(works(kind=k, **pair), f"advertised kind {k!r} does not work")
            expect(k in row, f"describe does not name the {k!r} kind: {row}")
        expect(not works(kind="point", **pair),
               "an unlisted kind was accepted anyway")

        # The two caps of a cylinder face opposite ways: 180 degrees between
        # their outward normals, and PARALLEL — the planes are parallel, and
        # it is the angle that is degenerate, which is what the flag marks.
        caps = d.call("measure", kind="both", **pair)
        expect(abs(caps["angle"] - 180.0) < 1e-6, f"caps are 180 apart: {caps}")
        expect(abs(caps["acute"]) < 1e-6, f"acute folds to 0: {caps}")
        expect(caps["parallel"], f"caps are parallel: {caps}")
        expect(caps["basis"] == "between the face normals", f"basis: {caps}")
        # kind=both keeps the distance half intact, with a SIGNED delta.
        expect(abs(caps["distance"] - 10.0) < 1e-3, f"both keeps distance: {caps}")
        expect(len(caps["delta"]) == 3, f"a signed delta: {caps}")

        # A cap against the cylindrical WALL: the plane normal vs the cylinder
        # axis, so the sign is arbitrary and the answer folds into 0-90. An
        # end cap is perpendicular to the wall it caps.
        wall_ref = None
        for f in els["faces"]:
            if f["kind"] == "cylinder":
                wall_ref = {"feature_id": fid, "element": f["name"]}
        mixed = d.call("measure", a={"feature_id": fid, "element": cap_names[0]},
                       b=wall_ref, kind="angle")
        expect(abs(mixed["angle"]) < 1e-6 or abs(mixed["angle"] - 90.0) < 1e-6,
               f"cap vs wall axis is 0 or 90: {mixed}")
        expect("unsigned" in mixed["basis"], f"mixed pair says unsigned: {mixed}")


        # Mass properties: CoM on the axis at half height (the CoG marker).
        total = d.call("mass_properties")["total"]
        com = total["center_of_mass_mm"]
        expect(abs(com[0]) < 1e-3 and abs(com[1]) < 1e-3
               and abs(com[2] - 5.0) < 1e-3, f"CoM at axis mid-height: {com}")
        expect(abs(total["volume_mm3"] - math.pi * 25 * 10) < 1e-1,
               f"volume pi*r^2*h: {total['volume_mm3']}")

        # A SPHERE defines no direction, and an angle against one is REFUSED
        # naming which element and what would work — not answered with a
        # plausible zero.
        sphere(d, (0, 0, 100), 3, "Ball")
        ball = None
        for b in d.call("list_elements")["bodies"]:
            if b["name"] == "Ball":
                ball = {"feature_id": b["created_by"],
                        "element": b["faces"][0]["name"]}
        expect(ball is not None, "the sphere listed a face")
        try:
            d.call("measure", a=ball, b=wall_ref, kind="angle")
            expect(False, "an angle against a sphere was answered")
        except RuntimeError as e:
            expect("element a" in str(e), f"the refusal names which one: {e}")

        # mode=mesh: the same minimum distance, off the triangles, with the
        # deflection it was decided to. It exists because the exact one is
        # pathological when two solids' faces are all about equally far
        # apart — two lofted propellers 151 mm apart took 55 s, and the same
        # answer off the tessellations takes 40 ms.
        d.call("new_project")
        box(d, (0, 0, 0), (10, 10, 10), "A")
        d.call("add_part", name="second")
        box(d, (0, 0, 0), (10, 10, 10), "B")
        asm = d.call("add_assembly", name="pair")
        first = d.call("add_occurrence", part="Part 1",
                       transform={"position": [0, 0, 0],
                                  "orientation": [0, 0, 0, 1]})
        second = d.call("add_occurrence", part="second",
                        transform={"position": [40, 0, 0],
                                   "orientation": [0, 0, 0, 1]})
        exact = d.call("measure", a={"occurrence": first["id"]},
                       b={"occurrence": second["id"]},
                       distance_kind="minimum")
        mesh = d.call("measure", a={"occurrence": first["id"]},
                      b={"occurrence": second["id"]},
                      distance_kind="minimum", mode="mesh")
        expect(abs(exact["distance"] - 30.0) < 1e-6,
               f"the exact distance is the gap: {exact}")
        expect(mesh["mode"] == "mesh" and mesh["deflection_mm"] > 0,
               f"mesh mode says how coarsely it decided: {mesh}")
        expect(mesh["tolerance_mm"] >= mesh["deflection_mm"],
               f"the bound is the SUM of both deflections: {mesh}")
        expect(abs(mesh["distance"] - exact["distance"])
               <= mesh["tolerance_mm"],
               f"mesh agrees with exact to its tolerance: {mesh} {exact}")
        # It answers a DISTANCE between whole shapes, and refuses the rest
        # rather than inventing a number off a tessellation.
        for kw, want in ((dict(kind="angle"), "angle is exact"),
                         (dict(distance_kind="maximum"), "MINIMUM")):
            try:
                d.call("measure", a={"occurrence": first["id"]},
                       b={"occurrence": second["id"]}, mode="mesh", **kw)
                raise AssertionError(f"mesh mode answered {kw}")
            except RuntimeError as e:
                expect(want in str(e), f"the refusal explains: {e}")

        print("measure_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
