#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A buckling study, through the RPCs alone.

What an agent with only the MCP surface does (docs/simulation/README.md
#buckling): build a column, add a buckling study, state a buckling_factor
requirement, read the factor from study_results, and check the requirement.
The requirement is `unchecked` -- not met -- until it is checked; checked, it
reads the governing factor, which is below the elastic one. The refusals
name what they refuse: load cases on a buckling study, num_modes on a static
one.

The physics (Euler, the plate, the cylinder's knockdown, Rankine-Gordon) is
tests/test_fem_buckling.cpp's. This is the contract over the wire.

Skips (exit 77) on a build without Netgen.

Usage: buckling_smoke.py <path-to-designer-binary>
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smoke_app import launch  # noqa: E402
import smoke_shapes as shapes  # noqa: E402

SKIP = 77


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def refused(d, method, needle, **params):
    try:
        d.call(method, **params)
    except RuntimeError as e:
        expect(needle in str(e), f"{method} refused without '{needle}': {e}")
        return
    raise AssertionError(f"{method} {params} was accepted")


def main():
    binary = os.path.abspath(sys.argv[1])
    with launch(binary) as app:
        d = app.client
        if not d.call("ping").get("features", {}).get("mesher"):
            print("buckling_smoke: SKIP (built without Netgen)")
            return SKIP
        d.call("new_project")
        fid = shapes.box(d, (0, 0, 0), (10, 10, 200), "Column")["id"]
        d.call("set_material", feature_id=fid, material="steel")
        els = d.call("list_elements")["bodies"][0]
        created_by = els["created_by"]

        def end(sign):
            return [f["name"] for f in els["faces"]
                    if abs(f["normal"][2] - sign) < 1e-6][0]

        sid = d.call("add_study", name="Buckle", type="buckling")["id"]
        listed = {s["id"]: s for s in d.call("list_studies")["studies"]}
        expect(listed[sid]["type"] == "buckling"
               and listed[sid].get("num_modes") == 3,
               f"a buckling study, three factors by default: {listed[sid]}")
        d.call("set_study", id=sid, mesh_size=4.0, converge="never")
        d.call("add_load", study=sid, type="fixed",
               face={"feature_id": created_by, "element": end(-1.0)})
        force = 1000.0
        d.call("add_load", study=sid, type="force",
               face={"feature_id": created_by, "element": end(1.0)},
               magnitude=force, direction=[0, 0, -1])

        # The refusals, before anything starts.
        refused(d, "set_study", "takes no load cases", id=sid,
                load_cases=[{"name": "a"}])
        static = d.call("add_study", name="Static")["id"]
        refused(d, "set_study", "num_modes applies to a modal study", id=static,
                num_modes=3)
        refused(d, "add_study", "static, modal, topology, buckling",
                type="bucking")
        d.call("remove_study", id=static)

        doc = d.call("list_parts")["parts"][0]["id"]
        d.call("add_requirement", name="does not buckle",
               measure={"kind": "study_result", "document": doc,
                        "study": sid, "quantity": "buckling_factor"},
               bound={"kind": "at_least", "value": 2.0},
               source="a factor of two on the elastic load, knocked down")
        rows = d.call("list_requirements")
        expect(rows["summary"]["unchecked"] == 1 and not rows["all_pass"],
               f"stated, never checked -- not met: {rows['summary']}")

        r = d.call("run_study", id=sid)
        expect(r["type"] == "buckling", f"a buckling answer: {r.get('type')}")
        body = r["bodies"][0]
        factors = body["buckling_factors"]
        expect(len(factors) >= 2, f"factors listed: {factors}")
        expect(abs(factors[0] - factors[1]) < 1e-3 * factors[0],
               f"a square section's two first modes: {factors}")
        euler = math.pi ** 2 * 200000.0 * (10 ** 4 / 12.0) / (
            4 * 200.0 ** 2) / force
        expect(abs(body["buckling_factor"] / euler - 1) < 0.01,
               f"Euler's {euler}: {body['buckling_factor']}")
        expect(body["knockdown"]["value"] == 1.0
               and body["knockdown"]["class"] in ("flat", "member"),
               f"a flat-faced member is not knocked down: {body['knockdown']}")
        gov = r["governing_factor"]
        expect(gov < body["buckling_factor"] and gov < body["yield_factor"],
               f"the governing factor below both: {r}")
        expect(r["factor_of_safety"] == gov,
               "factor_of_safety reads the governing factor")
        claim = r["receipt"]["claims"][0]
        expect(claim["oracle"]["id"] == "in_house/buckle"
               and claim["basis"] == "predicted",
               f"a prediction by the in-house solver: {claim}")
        expect("Snap-through" in claim["note"],
               f"the note names what linear buckling misses: {claim['note']}")
        expect(r["convergence"]["status"] == "not checked",
               f"converge: never is reported: {r['convergence']}")

        d.call("set_result_display", study=sid, field="displacement", mode=1)

        checked = d.call("check_requirements")
        row = checked["requirements"][0]
        expect(row["state"] == "pass" and abs(row["value"] - gov) < 1e-9,
               f"checked against the governing factor: {row}")
        print(f"buckling_smoke: OK -- factor {body['buckling_factor']:.4g} "
              f"(Euler {euler:.4g}), governing {gov:.4g}")
        return 0


if __name__ == "__main__":
    sys.exit(main())
