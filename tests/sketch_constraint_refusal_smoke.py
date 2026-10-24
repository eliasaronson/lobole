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

from lobole.client import Client  # noqa: E402
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
        d = Client.wait_for(app.port)
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
        # A configuration overrides VARIABLES, and a driven dimension reads
        # none: no path gives it an expression, so no override can be meant
        # for it. A dimension a configuration drives (through `w`) cannot be
        # made driven with its expression kept -- refused, unchanged.
        d.call("set_variable", name="w", expression="40")
        wide = d.call("add_feature", feature={
            "type": "Sketch", "name": "Wide",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "line", "a": [0, 0], "b": [40, 0]}]})
        wl = wide["entity_ids"][0]
        d.call("add_constraint", sketch_id=wide["id"], kind="distance",
               refs=[[wl, 0], [wl, 1]], expr="w")
        cfg = d.call("add_configuration", name="Narrow")["id"]
        d.call("set_configuration", id=cfg,
               parameter_overrides=[{"name": "w", "expression": "25"}])
        wj = sketch_json(d, wide["id"])
        made = dict(wj)
        made["constraints"] = [dict(c) for c in wj["constraints"]]
        made["constraints"][0]["driven"] = True
        expect_error(d, "modify_feature", ["cannot carry an expression"],
                     feature_id=wide["id"], feature=made)
        expect_error(d, "add_constraint", ["a driven dimension takes no expr"],
                     sketch_id=wide["id"], kind="distance_x",
                     refs=[[wl, 0], [wl, 1]], driven=True, expr="w")
        expect(sketch_json(d, wide["id"]) == wj,
               "a dimension a configuration drives stayed driving")

        # G2 (curvature) and arc_length: what each cannot hold is refused,
        # naming why, and the sketch is left as it was
        # (docs/sketching/README.md#curvature-continuity).
        g2 = d.call("add_feature", feature={
            "type": "Sketch", "name": "Curves",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [
                {"type": "line", "a": [0, 0], "b": [5, 0]},
                {"type": "line", "a": [5, 0], "b": [9, 1]},
                {"type": "arc", "center": [9, 4], "radius": 3.0,
                 "start_angle": -math.pi / 2, "end_angle": 0.0},
                {"type": "arc", "center": [15, 4], "radius": 3.0,
                 "start_angle": math.pi, "end_angle": 1.5 * math.pi},
                {"type": "spline", "degree": 1,
                 "poles": [[0, 0], [-2, 3], [-4, 0]]},
                {"type": "spline", "degree": 3,
                 "poles": [[20, 0], [21, 2], [23, 2], [24, 0]]},
                {"type": "circle", "center": [30, 0], "radius": 2.0}]})
        gid = g2["id"]
        l1, l2, a1, a2, poly, loose, circ = g2["entity_ids"]
        d.call("add_constraints", sketch_id=gid, constraints=[
            {"kind": "coincident", "refs": [[l1, 1], [l2, 0]]},
            {"kind": "coincident", "refs": [[l2, 1], [a1, 1]]},
            {"kind": "coincident", "refs": [[a1, 2], [a2, 1]]},
            {"kind": "coincident", "refs": [[l1, 0], [poly, 0]]}])
        g2_before = sketch_json(d, gid)
        for a, b, why in ((l1, l2, "collinear"), (l2, a1, "infinite"),
                          (a1, a2, "one circle"), (poly, l1, "degree-1"),
                          (loose, l1, "do not share an end")):
            expect_error(d, "add_constraint",
                         ["curvature takes a spline", why],
                         sketch_id=gid, kind="curvature",
                         refs=[[a, 0], [b, 0]])
        for ref, why in ((l1, "`distance`"), (circ, "`diameter`"),
                         (loose, "no arc-length dimension")):
            expect_error(d, "add_constraint", ["arc_length takes", why],
                         sketch_id=gid, kind="arc_length", refs=[[ref, 0]],
                         value=5)
        expect(sketch_json(d, gid) == g2_before,
               "every refused G2 and arc length left the sketch as it was")
        # On an arc, arc_length is millimetres along it.
        d.call("add_constraint", sketch_id=gid, kind="arc_length",
               refs=[[a2, 0]], value=6.0)
        arc = next(e for e in sketch_json(d, gid)["entities"]
                   if e["id"] == a2)
        sweep = arc["end_angle"] - arc["start_angle"]
        expect(abs(arc["radius"] * sweep - 6.0) < 1e-9,
               f"the arc is 6 mm long: {arc}")

        # add_constraint takes the geometry literally: the first dimension
        # of a sketch drawn by eye does NOT rescale it over the RPC (the GUI
        # does; docs/sketching/README.md#the-first-dimension).
        rough = d.call("add_feature", feature={
            "type": "Sketch", "name": "Rough",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "line", "a": [0, 0], "b": [3, 0]},
                         {"type": "line", "a": [0, 1], "b": [2, 1]}]})
        ra, rb = rough["entity_ids"]
        d.call("add_constraint", sketch_id=rough["id"], kind="distance",
               refs=[[ra, 0], [ra, 1]], value=30)
        other = next(e for e in sketch_json(d, rough["id"])["entities"]
                     if e["id"] == rb)
        expect(other["a"] == [0, 1] and other["b"] == [2, 1],
               f"the other line is as it was drawn: {other}")

        # `name = value` over the RPC: a batch of set_variable and
        # add_constraint naming it. With scope "part" both land on the
        # part's history, one undo step (a project variable has a history
        # of its own: two).
        before_rough = sketch_json(d, rough["id"])
        r = d.call("batch", calls=[
            {"method": "set_variable",
             "params": {"name": "gap", "expression": "12",
                        "scope": "part"}},
            {"method": "add_constraint",
             "params": {"sketch_id": rough["id"], "kind": "distance",
                        "refs": [[rb, 0], [rb, 1]], "expr": "gap"}}])
        expect("failed" not in r, f"the batch ran: {r}")
        line = next(e for e in sketch_json(d, rough["id"])["entities"]
                    if e["id"] == rb)
        expect(abs(math.dist(line["a"], line["b"]) - 12.0) < 1e-9,
               f"gap drives the line: {line}")
        d.call("undo")
        expect(sketch_json(d, rough["id"]) == before_rough,
               "one undo takes the batch back")
        expect(all(v["name"] != "gap"
                   for v in d.call("list_variables")["variables"]),
               "and the variable with it")
        print("sketch_constraint_refusal_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    main()
