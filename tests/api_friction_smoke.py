#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""What the PES pump relies on, over the RPC and through both clients
(generators/pes_pump/README.md#how-it-moves): a revolve about a custom axis, a helix that
starts where it is told, a study that names its configuration and reports
its mates, one clash vocabulary, a section that says what it kept, and the
Python verbs (describe(method=...), spline, arc chaining, rack).

Usage: api_friction_smoke.py <path-to-designer-binary>
"""
import math
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lobole.client import Client  # noqa: E402
from lobole.sdk import Plane, connect  # noqa: E402
from smoke_app import launch_app  # noqa: E402
import smoke_shapes as shapes  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def refused(d, method, needles, **kw):
    try:
        d.call(method, **kw)
    except RuntimeError as e:
        for n in needles:
            expect(n in str(e), f"{method} refused, but not naming '{n}': {e}")
        return
    raise AssertionError(f"{method} was accepted: {kw}")


def rect_sketch(d, name, c1, c2):
    return d.call("add_feature", feature={
        "type": "Sketch", "name": name,
        "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1], "uAxis": [1, 0, 0]},
        "entities": [{"type": "rect", "c1": list(c1), "c2": list(c2)}]})["id"]


def part_checks(d):
    d.call("new_project")
    # A 2 x 3 rectangle 11..13 out, turned about the Y-parallel axis x = 10.
    sk = rect_sketch(d, "Profile", (11, 0), (13, 3))
    r = d.call("add_feature", feature={
        "type": "Revolve", "name": "Ring", "sketch_id": sk,
        "axis": {"kind": "custom", "origin": [10, 0, 0], "direction": [0, 1, 0]},
        "angle_deg": 360, "mode": "new_body"})
    expect(r["regen"]["ok"], f"custom-axis revolve regenerates: {r}")
    vol = d.call("mass_properties")["bodies"][0]["volume_mm3"]
    expect(abs(vol - 24 * math.pi) < 0.1, f"Pappus volume 24 pi: {vol}")
    refused(d, "add_feature", ["direction is zero"], feature={
        "type": "Revolve", "name": "Bad", "sketch_id": sk,
        "axis": {"kind": "custom", "origin": [10, 0, 0], "direction": [0, 0, 0]},
        "angle_deg": 90, "mode": "new_body"})
    refused(d, "add_feature", ["custom {origin"], feature={
        "type": "Revolve", "name": "Bad", "sketch_id": sk,
        "axis": {"kind": "diagonal"}, "angle_deg": 90, "mode": "new_body"})
    kinds = d.call("describe")["axis_kinds"]
    expect("custom" in kinds and "datum_axis" in kinds, f"axis kinds: {kinds}")

    # A helix told to start a quarter turn round: its first point is on +Y.
    h = d.call("add_feature", feature={
        "type": "Curve3D", "name": "Coil", "kind": "helix", "origin": [0, 0, 0],
        "axis": [0, 0, 1], "radius": 2, "pitch": 1, "turns": 1,
        "start_deg": 90})
    expect(h["regen"]["ok"], f"helix with start_deg: {h}")
    feats = d.call("get_state")["features"]
    coil = next(f for f in feats if f.get("name") == "Coil")
    expect(abs(coil.get("start_deg", 0) - 90) < 1e-9,
           f"start_deg is kept: {coil}")

    # A primitive's size is a dimension in its sketch: changing that value
    # through modify_feature re-solves the sketch, so the solid follows
    # (docs/modeling/README.md#primitives). A stored primitive type is not
    # something modify_feature makes.
    d.call("new_project")
    box = d.call("add_feature", feature={
        "type": "Box", "name": "Blk", "on": "Top", "width": 10, "depth": 4,
        "height": 2})
    solid, sketch = box["ids"]
    sk = next(f for f in d.call("get_state")["features"] if f["id"] == sketch)
    widened = False
    for c in sk["constraints"]:
        if c["kind"] == "distance_x" and abs(c["value"] - 10) < 1e-9:
            c["value"] = 25.0
            widened = True
    expect(widened, f"the box sketch dimensions its width: {sk}")
    for key in ("index", "error", "warning", "after_earlier_failure"):
        sk.pop(key, None)
    r = d.call("modify_feature", feature_id=sketch, feature=sk)
    vol = [b for b in r["digest"]["bodies"] if b["created_by"] == solid]
    expect(vol and abs(vol[0]["volume_mm3"] - 25 * 4 * 2) < 1e-6,
           f"a changed width re-solves the sketch: {r['digest']}")
    refused(d, "modify_feature", ["not a stored feature", "modify_feature on "
                                  "the sketch"], feature_id=solid,
            feature={"type": "Box", "on": "Top", "width": 1, "depth": 1,
                     "height": 1})

    # `name` is optional in describe's contract: a spec without one builds,
    # named for its type. It used to be refused as "missing the required
    # field name" (the imported-edits dogfood's first Refine).
    r = d.call("add_feature", feature={"type": "Refine", "body_id": solid})
    expect(r["regen"]["ok"], f"an unnamed Refine builds: {r}")
    named = [f for f in d.call("get_state")["features"] if f["id"] == r["id"]]
    expect(named and named[0]["name"] == "Refine",
           f"an unnamed feature is named for its type: {named}")


def assembly_checks(d):
    def box(name, lo, size):
        d.call("add_part", name=name)
        shapes.box(d, list(lo), (size[0], size[1], size[2]), "Block")
    d.call("new_project")
    box("arm", (2, -2, -2), (28, 4, 4))
    box("stop", (-2, -2, -2), (4, 4, 4))
    d.call("add_part", name="carrier")
    d.call("add_assembly", name="rig")
    carrier = d.call("add_occurrence", part="carrier", fixed=True)["id"]
    arm = d.call("add_occurrence", part="arm")["id"]
    stop = d.call("add_occurrence", part="stop", fixed=True, transform={
        "position": [0.0, 20.0, 0.0], "orientation": [0, 0, 0, 1]})["id"]
    axle = d.call("add_mate", type="revolute", name="axle",
                  a={"occurrence": carrier, "origin": True},
                  b={"occurrence": arm, "origin": True}, flipped=False)["id"]
    no_stop = d.call("add_configuration", name="no stop")["id"]
    d.call("set_configuration", id=no_stop, suppressed_occurrences=[stop])

    # One clash vocabulary: interference names AND ids its pairs.
    d.call("set_mate", id=axle, rotation_deg=90)
    d.call("solve_assembly")
    hit = d.call("interference", mode="exact")
    pair = hit["pairs"][0]
    expect({pair["a_name"], pair["b_name"]} == {"arm", "stop"},
           f"interference names its sides: {pair}")
    expect({pair["a_id"], pair["b_id"]} == {arm, stop},
           f"...and gives their ids: {pair}")
    one = d.call("interference", a=arm, b=stop)
    expect(one["a_id"] == arm and one["b_name"] == "stop",
           f"a named pair too: {one}")
    d.call("set_mate", id=axle, rotation_deg=0)

    # A study that names its configuration runs under it, the tab untouched.
    s = d.call("add_motion_study", driver=axle, **{"from": 0}, to=360,
               samples=13, clash_mode="exact", configuration=no_stop)
    expect(s["configuration"] == no_stop, f"kept: {s}")
    r = d.call("run_motion_study", study=s["id"], include_poses=True)
    expect(r["clean"], "the stop is out under the study's configuration")
    quarter = r["samples"][3]
    m = next(x for x in quarter["mates"] if x["mate"] == axle)
    expect(m["name"] == "axle" and abs(m["rotation_deg"] - 90) < 1e-4,
           f"sample mates carry the value in degrees: {m}")
    # 0 is the BASE: the stop is back in.
    d.call("set_motion_study", study=s["id"], configuration=0)
    expect(not d.call("run_motion_study", study=s["id"])["clean"],
           "configuration 0 runs the base, stop and all")
    refused(d, "set_motion_study", ["no configuration 999", "no stop"],
            study=s["id"], configuration=999)

    # The section says which half it kept.
    kept = d.call("set_section", base_plane="xz", offset=0.0)
    expect(kept["keeps"] == "y <= 0", f"default keeps below: {kept}")
    kept = d.call("set_section", base_plane="yz", offset=5.0, reverse=True)
    expect(kept["keeps"] == "x >= 5", f"reverse keeps above: {kept}")
    d.call("set_section", enabled=False)

    # solve_assembly on a tree: no loop keys, which the doc now says.
    sol = d.call("solve_assembly")
    expect("errored_mates" in sol and "loop_mates" not in sol,
           f"loop keys only with loops: {sorted(sol)}")


def client_checks(port):
    # `method` is positional-only in both clients, so a parameter of that
    # name reaches the RPC.
    d = Client.wait_for(port)
    row = d.call("describe", method="add_mate")
    expect("add_mate" in str(row), f"describe(method=) via lobole.client")
    app = connect(port)
    expect("add_mate" in str(app.describe(method="add_mate")),
           "describe(method=) via the SDK")
    # Narrowed means narrowed: that one row, not the catalogue around it.
    expect([r["name"] for r in row["methods"]] == ["add_mate"]
           and "feature_types" not in row and "material_slugs" not in row,
           f"describe(method=) returns the one row: {sorted(row)}")
    full = d.call("describe")
    expect(len(full["methods"]) > 100 and "feature_types" in full,
           "describe() with no method is still the whole catalogue")
    try:
        d.call("describe", method="add_mat")
        raise AssertionError("describe accepted an unknown method")
    except RuntimeError as e:
        expect("unknown method 'add_mat'" in str(e) and "add_mate" in str(e),
               f"an unknown method is refused naming close matches: {e}")
    # help() over the python client, the same lookup the MCP server's is.
    h = d.help("add_mate")
    expect(h["method"] == "add_mate" and h["doc"] and "type" in h["params"],
           f"help(method) via lobole.client: {h}")
    expect(d.call("help", method="add_mate") == h,
           "call('help', method=) is the same answer")
    expect("add_mate" in d.help()["methods"], "help() indexes the methods")
    try:
        d.help("list_mate")
        raise AssertionError("help accepted an unknown method")
    except RuntimeError as e:
        expect("list_mates" in str(e), f"help names close matches: {e}")
    # A CALL to an unknown method says what describe says: it used to be a
    # bare "unknown method: add_mat". A name that asks for a format
    # (import_stl, import_mesh) is pointed at the one importer, import_step,
    # which edit distance alone cannot reach from import_mesh.
    for typo, wanted in (("add_mat", "add_mate"),
                         ("import_stl", "import_step"),
                         ("import_mesh", "import_step"),
                         ("load_obj", "import_step")):
        try:
            d.call(typo, path="/nonexistent.stl")
            raise AssertionError(f"{typo} was accepted")
        except RuntimeError as e:
            expect(f"unknown method '{typo}'" in str(e) and wanted in str(e),
                   f"calling {typo} names {wanted}: {e}")
            if wanted == "import_step":
                expect("STL" in str(e) and "import_sketch" in str(e),
                       f"{typo} is told which method reads which file: {e}")

    app.new()
    p = app.part("sdk verbs")
    # arc() chains now, and spline() draws a closed smooth profile.
    sk = p.sketch(Plane.xy(0), name="Arc")
    got = sk.arc((0, 0), 5, 0, 90)
    expect(got is sk, "arc() returns the builder")
    pts = [(10 * math.cos(math.radians(a)), 6 * math.sin(math.radians(a)))
           for a in range(0, 360, 10)]
    sk = p.sketch(Plane.xy(0), name="Ellipse-ish")
    sk.spline(pts, closed=True)
    p.extrude(sk.add(), 2.0, name="Lobe")
    vol = p.volume()
    expect(abs(vol - math.pi * 10 * 6 * 2) < 0.03 * math.pi * 10 * 6 * 2,
           f"a closed spline profile extrudes: {vol}")


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_friction_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        part_checks(d)
        assembly_checks(d)
        client_checks(app.port)
        print("api_friction_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()


if __name__ == "__main__":
    main()
