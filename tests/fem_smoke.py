#!/usr/bin/env python3
"""Integration test: the FEM study CONTRACT, headless over JSON-RPC.

WHAT THIS DOES AND DOES NOT CHECK
---------------------------------
It checks the parts only a running server can show: the study CRUD RPCs,
that a solve returns the fields a caller reads, the staleness contract (an
edit invalidates results), and that an expression-driven load re-scales the
answer on re-run. That last one is the headless design loop in miniature —
set a variable, run, read stress, iterate.

It does NOT check the physics. `tests/test_fem.cpp` already does, against
analytic solutions and to tight tolerances: `Solver: uniaxial bar matches
sigma=F/A and u=FL/EA`, `cantilever tip deflection near Euler-Bernoulli`,
`Modal: cantilever first bending frequency`, `Solver: cantilever
self-weight tip deflection`. This file used to re-assert all four with
deliberately loose bounds ("generous headroom", "generous bound"), which is
duplication that costs four extra solves and catches nothing the unit tests
miss — a loose check next to an exact one only ever fires when the exact one
already has. What survives here is the RATIO, because scaling a variable and
getting twice the stress is about the RPC and the expression pass, not about
elasticity.

The async worker-thread path lives in `fem_async_smoke.py`: it is the slow
half, and keeping it separate means a failure there names itself instead of
taking this contract down with it.

Skips (exit 77) when the build has no Netgen — meshing is unavailable.

Usage: fem_smoke.py <path-to-designer-binary>
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smoke_app import launch  # noqa: E402

SKIP = 77  # ctest SKIP_RETURN_CODE: built without Netgen


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def build_bar(d):
    """A 100 x 10 x 10 mm steel bar, fixed at one end, pulled at the other.

    Returns (study_id, created_by, fixed_face, load_face)."""
    d.call("new_project")
    d.call("set_variable", name="load_n", expression="10000")
    fid = d.call("add_feature", feature={"type": "Box", "name": "Bar",
                                         "position": [0, 0, 0],
                                         "width": 100, "height": 10,
                                         "depth": 10})["id"]
    d.call("set_material", feature_id=fid, material="steel")

    els = d.call("list_elements")["bodies"][0]
    created_by = els["created_by"]
    faces = [f["name"] for f in els["faces"]]
    # x=0 and x=100 are face0/face1 in the primitive naming. Which pair does
    # not matter here: the physics is test_fem.cpp's job, and any opposite
    # pair exercises the same RPCs.
    fixed_face, load_face = faces[0], faces[1]

    study = d.call("add_study", name="Pull")["id"]
    d.call("set_study", study=study, mesh_size=4.0)
    d.call("add_load", study=study, type="fixed",
           face={"feature_id": created_by, "element": fixed_face})
    d.call("add_load", study=study, type="force",
           face={"feature_id": created_by, "element": load_face},
           magnitude="load_n", direction=[1, 0, 0])
    return study, created_by, fixed_face, load_face


def main():
    binary = sys.argv[1]
    try:
        with launch(binary) as app:
            d = app.designer
            expect(d.call("ping")["ok"], "ping")

            study, created_by, fixed_face, _ = build_bar(d)

            studies = d.call("list_studies")["studies"]
            expect(len(studies) == 1, f"one study: {studies}")

            # Meshing is its own RPC, and a no-Netgen build surfaces its error
            # here — which is what the skip below reads.
            mesh = d.call("mesh_study", study=study)
            expect(mesh["bodies"][0]["elements"] > 0, "meshed to elements")

            res = d.call("run_study", study=study)
            expect(res["elements"] > 0, "solved a mesh")
            expect(res["stale"] is False, "fresh results not stale")
            sigma = res["max_von_mises_mpa"]["value"]
            # A number came back and it is a stress, not a NaN or a zero. How
            # CLOSE it is to F/A is test_fem.cpp's assertion, not this one.
            expect(sigma > 0, f"a positive von Mises came back: {sigma}")
            expect("factor_of_safety" in res, "steel yield gives a FoS")

            # study_results reads the cache without re-solving.
            again = d.call("study_results", study=study)
            expect(again["stale"] is False
                   and again["elements"] == res["elements"],
                   "study_results matches run_study")

            # The receipt (core/Receipt.h, peer-survey-plan W2). An FEM
            # factor of safety is a PREDICTION, and the number is quoted
            # downstream as though it were measured — so the basis travels
            # with it and a passing study can never roll up better than
            # provisional.
            rec = again["receipt"]
            expect(rec["standing"] == "provisional",
                   f"a solved study is provisional, not verified: {rec}")
            claim = rec["claims"][0]
            expect(claim["basis"] == "predicted",
                   f"FEM is a prediction: {claim}")
            expect(claim["verdict"] in ("pass", "fail"),
                   f"the solve ran, so it is not unverifiable: {claim}")
            expect(claim["oracle"]["id"] == "designer/fem-static",
                   f"the oracle names itself: {claim}")

            # The design loop: edit a driving variable, results go stale, and
            # re-running scales the answer with it. Doubling the load doubles
            # the stress in a linear static solve, so the RATIO is exact —
            # this is about the expression pass reaching the solver, which no
            # unit test covers.
            d.call("set_variable", name="load_n", expression="20000")
            stale = d.call("study_results", study=study)
            expect(stale["stale"] is True, "results stale after a variable edit")
            # And the receipt says so where a caller cannot miss it: numbers
            # about a part that no longer exists are UNVERIFIED, not a pass.
            expect(stale["receipt"]["standing"] == "unverified",
                   f"stale results are unverified: {stale['receipt']}")
            expect(stale["receipt"]["claims"][0]["verdict"] == "unverifiable",
                   "the claim itself, not just a flag beside it")
            res2 = d.call("run_study", study=study)
            ratio = res2["max_von_mises_mpa"]["value"] / sigma
            expect(abs(ratio - 2.0) < 0.05,
                   f"stress scales 2x with the load, got {ratio}")

            print("fem_smoke: OK")
            return 0
    except Exception as e:
        if "FEM support" in str(e) or "Netgen" in str(e):
            print("fem_smoke: SKIP (built without Netgen)")
            return SKIP
        raise


if __name__ == "__main__":
    sys.exit(main())
