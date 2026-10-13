#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: add_constraint refuses a constraint that constrains
nothing, and adds a driven (reference) dimension when asked.

On a fully defined triangle, `add_constraint` used to answer `added: 1`,
`regen.ok`, no warnings, for a `distance_x` repeating the base length -- and
for a `distance` of 10 between two corners that are 72 apart, whose solve
then failed and left the sketch holding a dimension it does not meet. Both
are refused now, naming what they repeat or contradict, and the sketch is
left exactly as it was (docs/sketching/README.md#refusals). `driven: true`
adds the same dimension as a reference that measures the sketch.

Usage: sketch_constraint_refusal_smoke.py <path-to-designer-binary>
"""
import math
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


def expect_error(d, method, fragments, **params):
    try:
        d.call(method, **params)
    except RuntimeError as e:
        for fragment in fragments:
            expect(fragment in str(e), f"{method} error names '{fragment}': {e}")
        return str(e)
    raise AssertionError(f"{method} {params} must be refused")


def sketch_json(d, sketch_id):
    for f in d.call("get_state")["features"]:
        if f.get("id") == sketch_id:
            f.pop("index", None)
            return f
    raise AssertionError(f"no feature {sketch_id} in get_state")


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_driven_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        d.call("new_project")
        # Base fixed at its start and horizontal; sides 100, 60, 80 (a right
        # angle at the apex, (64, 48)).
        sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "Triangle",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [
                {"type": "line", "a": [0, 0], "b": [100, 0]},
                {"type": "line", "a": [100, 0], "b": [64, 48]},
                {"type": "line", "a": [64, 48], "b": [0, 0]}]})
        sid = sk["id"]
        base, l1, l2 = sk["entity_ids"]
        r = d.call("add_constraints", sketch_id=sid, constraints=[
            {"kind": "coincident", "refs": [[base, 1], [l1, 0]]},
            {"kind": "coincident", "refs": [[l1, 1], [l2, 0]]},
            {"kind": "coincident", "refs": [[l2, 1], [base, 0]]},
            {"kind": "fixed", "refs": [[base, 0]]},
            {"kind": "horizontal", "refs": [[base, 0]]},
            {"kind": "distance", "refs": [[base, 0], [base, 1]], "value": 100},
            {"kind": "distance", "refs": [[l1, 0], [l1, 1]], "value": 60},
            {"kind": "distance", "refs": [[l2, 0], [l2, 1]], "value": 80}])
        expect(r["added"] == 8, f"the triangle's constraints are added: {r}")
        expect(r["dof"] == 0, f"the triangle is fully defined: {r}")
        before = sketch_json(d, sid)

        # A distance_x repeating the base length: refused, naming the
        # base's length (constraint 5), and offering the driven form.
        expect_error(d, "add_constraint",
                     ["the sketch already fixes that", "constraint 5",
                      "driven: true", "unchanged"],
                     sketch_id=sid, kind="distance_x",
                     refs=[[base, 0], [base, 1]], value=100)
        expect(sketch_json(d, sid) == before,
               "the refused distance_x left the sketch as it was")

        # 10 between the base's end and the apex, which are 60 apart: a
        # contradiction, refused, the geometry untouched.
        expect_error(d, "add_constraint",
                     ["contradicts what the sketch already fixes",
                      "unchanged"],
                     sketch_id=sid, kind="distance",
                     refs=[[base, 1], [l2, 0]], value=10)
        expect(sketch_json(d, sid) == before,
               "the refused distance left the sketch as it was")

        # A relation that repeats one: refused, no driven offer.
        msg = expect_error(d, "add_constraint",
                           ["the sketch already fixes that", "constraint 4"],
                           sketch_id=sid, kind="horizontal",
                           refs=[[base, 0]])
        expect("driven" not in msg, f"no driven offer for a relation: {msg}")

        # The same distance_x, driven: added, measured, DOF still 0.
        r = d.call("add_constraint", sketch_id=sid, kind="distance_x",
                   refs=[[base, 0], [base, 1]], driven=True)
        expect(r["added"] == 1, f"a driven distance_x is added: {r}")
        expect(abs(r["measured"] - 100.0) < 1e-9, f"it measures 100: {r}")
        expect(r["dof"] == 0, f"a driven dimension constrains nothing: {r}")
        expect(r["regen"]["ok"], f"and the sketch is healthy: {r}")
        now = sketch_json(d, sid)
        driven = now["constraints"][-1]
        expect(driven.get("driven") is True and
               abs(driven["value"] - 100.0) < 1e-9,
               f"get_state writes it driven, valued as its measure: {driven}")
        # An angle answers radians, its kind's unit.
        r = d.call("add_constraint", sketch_id=sid, kind="angle",
                   refs=[[base, 0], [l2, 0]], driven=True)
        expect(abs(abs(r["measured"]) - math.atan2(48, 64)) < 1e-9 or
               abs(abs(r["measured"]) - (math.pi - math.atan2(48, 64))) < 1e-9,
               f"a driven angle measures in radians: {r}")

        # driven with an input, or on a relation, is refused.
        for extra in ({"value": 100}, {"expr": "100"}, {"value_deg": 3}):
            expect_error(d, "add_constraint", ["a driven dimension takes no"],
                         sketch_id=sid, kind="distance_x",
                         refs=[[base, 0], [base, 1]], driven=True, **extra)
        expect_error(d, "add_constraint",
                     ["cannot be driven", "distance_pl"],
                     sketch_id=sid, kind="horizontal", refs=[[l1, 0]],
                     driven=True)
        expect_error(d, "add_constraint", ["true or false"],
                     sketch_id=sid, kind="distance",
                     refs=[[base, 0], [base, 1]], driven="yes")

        # add_constraints: one bad spec in three adds nothing, and names
        # index 1. A fresh, free line takes the good two.
        before = sketch_json(d, sid)
        free = d.call("add_feature", feature={
            "type": "Sketch", "name": "Free",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "line", "a": [0, 0], "b": [40, 3]}]})
        fl = free["entity_ids"][0]
        free_before = sketch_json(d, free["id"])
        expect_error(d, "add_constraints", ["constraints[1]:",
                                            "the sketch already fixes that"],
                     sketch_id=free["id"], constraints=[
                         {"kind": "horizontal", "refs": [[fl, 0]]},
                         {"kind": "horizontal", "refs": [[fl, 0]]},
                         {"kind": "distance", "refs": [[fl, 0], [fl, 1]],
                          "value": 40}])
        expect(sketch_json(d, free["id"]) == free_before,
               "a refused add_constraints added nothing")
        # A spec that fails to parse is refused the same way, by index.
        expect_error(d, "add_constraints", ["constraints[2]:", "Known kinds"],
                     sketch_id=free["id"], constraints=[
                         {"kind": "horizontal", "refs": [[fl, 0]]},
                         {"kind": "distance", "refs": [[fl, 0], [fl, 1]],
                          "value": 40},
                         {"kind": "horizontl", "refs": [[fl, 0]]}])
        expect(sketch_json(d, free["id"]) == free_before,
               "a spec that does not parse adds nothing either")
        r = d.call("add_constraints", sketch_id=free["id"], constraints=[
            {"kind": "horizontal", "refs": [[fl, 0]]},
            {"kind": "distance", "refs": [[fl, 0], [fl, 1]], "value": 40}])
        expect(r["added"] == 2 and r["dof"] == 2,
               f"the good two are added, the line's position left free: {r}")

        # modify_feature making the driven distance_x driving would
        # over-constrain the triangle: refused, the document unchanged.
        tri = sketch_json(d, sid)
        flipped = dict(tri)
        flipped["constraints"] = [dict(c) for c in tri["constraints"]]
        k = next(i for i, c in enumerate(flipped["constraints"])
                 if c["kind"] == "distance_x")
        flipped["constraints"][k].pop("driven")
        expect_error(d, "modify_feature", ["over-constrain", "unchanged"],
                     feature_id=sid, feature=flipped)
        expect(sketch_json(d, sid) == tri,
               "a refused driving switch changed nothing")
        print("sketch_constraint_refusal_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    main()
