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


def assembly_study(d):
    """A study on an ASSEMBLY tab (docs/simulation/README.md#assembly-studies),
    over the same verbs. Two 100 x 100 x 16 steel blocks as parts, the
    upper placed on the lower; the upper meshed, the lower a rigid support
    clamped at their contact, a pressure on the upper's top."""
    d.call("new_project")
    parts = {}
    for name in ("Lower", "Upper"):
        slot = d.call("add_part", name=name)
        d.call("set_active_part", part=slot["id"])
        fid = d.call("add_feature", feature={"type": "Box", "name": name,
                                             "position": [0, 0, 0],
                                             "width": 100, "height": 100,
                                             "depth": 16})["id"]
        d.call("set_material", feature_id=fid, material="steel")
        els = d.call("list_elements")["bodies"][0]
        top = [f["name"] for f in els["faces"]
               if abs(f["normal"][2] - 1.0) < 1e-6][0]
        bottom = [f["name"] for f in els["faces"]
                  if abs(f["normal"][2] + 1.0) < 1e-6][0]
        parts[name] = (slot["id"], top, bottom)
    asm = d.call("add_assembly", name="Stack")
    d.call("set_active_part", part=asm["id"])
    lower = d.call("add_occurrence", part=parts["Lower"][0], name="Lower",
                   fixed=True)["id"]
    upper = d.call("add_occurrence", part=parts["Upper"][0], name="Upper",
                   transform={"position": [0, 0, 16]})["id"]

    # A study verb on an assembly tab is NOT refused any more (the twenty
    # rows used to carry NeedsPart), and its participants are occurrences.
    study = d.call("add_study", name="Upper on lower")["id"]
    d.call("set_study", id=study, mesh_size=12.0,
           participants=[{"occurrence": "Upper", "role": "meshed"},
                         {"occurrence": lower, "role": "rigid"}])
    listed = d.call("list_studies")["studies"][0]
    roles = {p["name"]: p["role"] for p in listed["participants"]}
    expect(roles == {"Upper": "meshed", "Lower": "rigid"},
           f"participants by name with roles: {listed}")

    # A fixed support anchored on the CONTACT: no face to name.
    clamp = d.call("add_load", study=study, type="fixed", contact_with="Lower")
    expect("id" in clamp, "contact anchor accepted")
    # A pressure on a face, saying whose face.
    d.call("add_load", study=study, type="pressure", occurrence=upper,
           face=parts["Upper"][1], magnitude=0.5)
    loads = d.call("list_studies")["studies"][0]["loads"]
    expect(loads[0]["contact_with"] == lower
           and loads[0]["contact_with_name"] == "Lower", f"contact: {loads}")
    expect(loads[1]["occurrence_name"] == "Upper", f"whose face: {loads}")

    # Refusals name what they refuse.
    try:
        d.call("add_load", study=study, type="force", contact_with="Lower",
               magnitude=10)
        raise AssertionError("a force took a contact anchor")
    except RuntimeError as e:
        expect("fixed support" in str(e), f"names the rule: {e}")
    try:
        d.call("add_load", study=study, type="pressure",
               face=parts["Upper"][1], magnitude=0.5)
        raise AssertionError("an assembly load without an occurrence")
    except RuntimeError as e:
        expect("occurrence" in str(e), f"asks whose face: {e}")
    try:
        d.call("set_study", id=study, type="topology")
        raise AssertionError("topology on an assembly")
    except RuntimeError as e:
        expect("part study" in str(e), f"topology is a part study: {e}")

    mesh = d.call("mesh_study", study=study)
    expect(mesh["bodies"][0]["elements"] > 0
           and mesh["bodies"][0]["body"] == "Upper", f"meshed: {mesh}")
    res = d.call("run_study", study=study)
    expect(res["elements"] > 0 and res["max_displacement_mm"]["value"] > 0,
           f"solved: {res}")
    expect(res["stale"] is False, "fresh")
    got = {p["name"]: p["role"] for p in res["participants"]}
    expect(got == {"Upper": "meshed", "Lower": "rigid"},
           f"results carry the role table: {res['participants']}")
    expect("factor_of_safety" in res, "steel gives a FoS on the assembly too")
    # A static solve factors K directly; it used to be labelled CG-IC, the
    # topology solver's name (fem::CpuSolverName).
    solver = d.call("study_status", study=study).get("solver")
    expect(solver == "cpu (Eigen SimplicialLDLT)",
           f"the static solver is named for what ran ({solver})")

    # Stale against EVERY document the study read: the assembly (a moved
    # occurrence), and a part (regenerated behind the assembly).
    d.call("set_occurrence_transform", id=upper,
           transform={"position": [0, 0, 16.0]})
    expect(d.call("study_results", study=study)["stale"] is True,
           "an assembly edit staled the result")
    d.call("run_study", study=study)
    expect(d.call("study_results", study=study)["stale"] is False, "fresh")
    d.call("set_active_part", part=parts["Upper"][0])
    box = d.call("get_state")["features"][0]
    spec = dict(box)
    spec["depth"] = 18
    d.call("modify_feature", feature_id=box["id"], feature=spec)
    d.call("set_active_part", part=asm["id"])
    expect(d.call("study_results", study=study)["stale"] is True,
           "a PART edit staled the assembly's result")

    # The pre-flight: lift the upper block clear, and the contact anchor
    # refuses by name instead of clamping nothing.
    d.call("set_occurrence_transform", id=upper,
           transform={"position": [0, 0, 40]})
    try:
        d.call("run_study", study=study)
        raise AssertionError("a support in mid-air clamped something")
    except RuntimeError as e:
        expect("does not touch" in str(e) and "Lower" in str(e),
               f"names the support: {e}")
    # And sink it into the part: interpenetration is refused, not fused.
    d.call("set_occurrence_transform", id=upper,
           transform={"position": [0, 0, 8]})
    try:
        d.call("run_study", study=study)
        raise AssertionError("an interpenetrating support was fused")
    except RuntimeError as e:
        expect("INTERPENETRATE" in str(e), f"names the overlap: {e}")

    # S2: the lower block BONDED instead of rigid — one mesh over both, the
    # lower's own bottom fixed, and a softer answer than the rigid support.
    d.call("set_occurrence_transform", id=upper,
           transform={"position": [0, 0, 16]})
    rigid_disp = d.call("run_study", study=study)["max_displacement_mm"]["value"]
    bonded = d.call("add_study", name="Bonded")["id"]
    d.call("set_study", id=bonded, mesh_size=12.0,
           participants=[{"occurrence": upper, "role": "meshed"},
                         {"occurrence": lower, "role": "bonded"}])
    d.call("add_load", study=bonded, type="fixed", occurrence=lower,
           face=parts["Lower"][2])
    d.call("add_load", study=bonded, type="pressure", occurrence=upper,
           face=parts["Upper"][1], magnitude=0.5)
    res = d.call("run_study", study=bonded)
    expect(res["bodies"][0]["body"] == "Upper + Lower",
           f"one body for the group: {res['bodies']}")
    expect(res["max_displacement_mm"]["value"] > rigid_disp,
           f"bonded is softer than rigid: {res['max_displacement_mm']} vs "
           f"{rigid_disp}")
    roles = {p["name"]: p["role"] for p in res["participants"]}
    expect(roles == {"Upper": "meshed", "Lower": "bonded"}, f"roles: {roles}")

    # The window: enter_simulation on the assembly tab opens its study
    # window rather than refusing.
    st = d.call("enter_simulation", study=study)
    expect(st["active"] and st["study"] == study, f"entered: {st}")
    d.call("exit_simulation")
    print("fem_smoke: assembly study OK")


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

            # --- one word per thing, and `id` means what it does elsewhere
            # This family used to ask for `kind` and report `type`, and name
            # its own subject `study` where a mate, a section, a relation, a
            # configuration and a CAM setup all answer to `id`.
            made = d.call("add_study", type="modal", name="Modes")
            expect(made["type"] == "modal",
                   f"add_study reports the word it was given: {made}")
            listed = [x for x in d.call("list_studies")["studies"]
                      if x["id"] == made["id"]][0]
            expect(listed["type"] == "modal",
                   f"list_studies says the same word: {listed}")
            expect(d.call("add_study", kind="modal", name="Legacy")["type"]
                   == "modal", "`kind` still works")

            # A verb that ACTS ON a study takes `id`...
            d.call("set_study", id=made["id"], num_modes=4)
            expect(d.call("study_status", id=made["id"]) is not None,
                   "study_status by id")
            # ...and `study` still means the same thing...
            d.call("set_study", study=made["id"], num_modes=4)
            # ...but two different studies in one call is a refusal, not a
            # silent pick.
            try:
                d.call("set_study", id=made["id"], study=study, num_modes=4)
                raise AssertionError("set_study took two different studies")
            except RuntimeError as e:
                expect("different studies" in str(e),
                       f"refusal names the problem: {e}")

            # A verb that only names a study for CONTEXT keeps `study`, and
            # its own `id` is the thing it acts on: the load.
            d.call("add_load", study=made["id"], type="fixed",
                   face={"feature_id": created_by, "element": fixed_face})
            loads = [x for x in d.call("list_studies")["studies"]
                     if x["id"] == made["id"]][0]["loads"]
            expect(len(loads) == 1, f"one load: {loads}")
            d.call("remove_load", study=made["id"], id=loads[0]["id"])
            loads = [x for x in d.call("list_studies")["studies"]
                     if x["id"] == made["id"]][0]["loads"]
            expect(not loads, f"remove_load by id took it away: {loads}")
            d.call("remove_study", id=made["id"])

            assembly_study(d)

            print("fem_smoke: OK")
            return 0
    except Exception as e:
        if "FEM support" in str(e) or "Netgen" in str(e):
            print("fem_smoke: SKIP (built without Netgen)")
            return SKIP
        raise


if __name__ == "__main__":
    sys.exit(main())
