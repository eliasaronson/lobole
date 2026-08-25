#!/usr/bin/env python3
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
    tmp = tempfile.mkdtemp(prefix="designer_measure_")
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
                expect(abs(r["length"] - 2 * math.pi * 5) < 1e-3,
                       f"circumference: {r}")
            else:
                length_seen = r["length"]
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
            elif abs(a["area"] - math.pi * 25) < 1e-2:
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
            if abs(a["area"] - math.pi * 25) < 1e-2:
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

        expect(("angle" in row) == works(kind="angle", **pair),
               f"describe and the handler disagree about angle: {row}")
        expect(("point" in row) == works(kind="point", **pair),
               f"describe and the handler disagree about point: {row}")

        # Mass properties: CoM on the axis at half height (the CoG marker).
        total = d.call("mass_properties")["total"]
        com = total["center_of_mass"]
        expect(abs(com[0]) < 1e-3 and abs(com[1]) < 1e-3
               and abs(com[2] - 5.0) < 1e-3, f"CoM at axis mid-height: {com}")
        expect(abs(total["volume"] - math.pi * 25 * 10) < 1e-1,
               f"volume pi*r^2*h: {total['volume']}")

        print("measure_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
