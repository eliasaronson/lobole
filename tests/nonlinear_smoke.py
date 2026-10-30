#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A nonlinear static study, through the RPCs alone.

What an agent with only the MCP surface sees
(docs/simulation/README.md#nonlinear-static): `set_study nonlinear` makes a
static study run on CalculiX (auto picks it) and `list_studies` says so; a
snap-through run of a column reports a peak load factor below its linear
buckling factor, with the path, the equilibrium check and a receipt whose
oracle is calculix/nonlinear; an async run's `study_status` reports the
load reached; `export_study_inp` writes a large-deformation deck and
refuses a snap-through one; the refusals name what they refuse. A column
standing on the ground (a contact support,
docs/simulation/README.md#contact) solves at small deformation with its
base closed, the ground's row in `contacts` and `reactions` carrying the
load. Pulled past its yield (docs/simulation/README.md#plasticity), it
yields on the library steel's hardening line and is left longer once
unloaded; a requirement reads its max_plastic_strain; in PLA, which has no
hardening curve, the run is refused.

The physics (the elastica, a shallow arch's snap, the column's approach to
its linear factor) is tests/test_fem_calculix.cpp's. This is the contract
over the wire.

Skips (exit 77) on a build without Netgen, or where `ping` finds no ccx.

Usage: nonlinear_smoke.py <path-to-designer-binary>
"""
import os
import shutil
import sys
import tempfile
import time

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
    work = tempfile.mkdtemp(prefix="lobole_nl_smoke_")
    try:
        with launch(binary) as app:
            d = app.client
            features = d.call("ping").get("features", {})
            if not features.get("mesher"):
                print("nonlinear_smoke: SKIP (built without Netgen)")
                return SKIP
            if not features.get("calculix"):
                print("nonlinear_smoke: SKIP (ping finds no ccx)")
                return SKIP
            d.call("new_project")
            fid = shapes.box(d, (0, 0, 0), (10, 10, 150), "Column")["id"]
            d.call("set_material", feature_id=fid, material="steel")
            els = d.call("list_elements")["bodies"][0]

            def end(sign):
                return {"feature_id": els["created_by"],
                        "element": [f["name"] for f in els["faces"]
                                    if abs(f["normal"][2] - sign) < 1e-6][0]}

            sid = d.call("add_study", name="Snap")["id"]
            d.call("set_study", id=sid, mesh_size=5.0, converge="never")
            d.call("add_load", study=sid, type="fixed", face=end(-1.0))
            d.call("add_load", study=sid, type="force", face=end(1.0),
                   magnitude=1000.0, direction=[0, 0, -1])

            refused(d, "set_study", "in-house solver is linear", id=sid,
                    solver="in_house",
                    nonlinear={"large_deformation": True})
            refused(d, "set_study", "has no key", id=sid,
                    nonlinear={"large_deformation": True, "arc": True})
            refused(d, "set_study", "large_deformation: true", id=sid,
                    nonlinear={"snap_through": True})

            # Large deformation first: its deck, then a run's progress.
            d.call("set_study", id=sid,
                   nonlinear={"large_deformation": True})
            listed = [s for s in d.call("list_studies")["studies"]
                      if s["id"] == sid][0]
            expect(listed.get("nonlinear") == {"large_deformation": True},
                   f"list_studies says nonlinear: {listed}")
            deck = os.path.join(work, "large.inp")
            d.call("export_study_inp", id=sid, path=deck)
            with open(deck) as f:
                text = f.read()
            expect("*STEP, NLGEOM" in text and "NSET=NSUP" in text,
                   "the exported deck steps the load under NLGEOM")
            def run_async(study=None, **params):
                """Runs the study as a job and waits for it: a synchronous
                run on a loaded machine outlasts the client's socket. The
                largest load_fraction study_status reported on the way."""
                study = study or sid
                job = d.call("run_study", id=study, **{"async": True},
                             **params)["job"]
                seen = -1.0
                deadline = time.monotonic() + 250
                while time.monotonic() < deadline:
                    st = d.call("study_status", id=study)
                    if not st.get("running"):
                        break
                    seen = max(seen, st.get("load_fraction", -1.0))
                    time.sleep(0.2)
                ended = d.call("study_status", job=job)
                expect(not ended.get("running") and
                       ended.get("outcome") == "done",
                       f"job {job} ended done within its deadline: {ended}")
                return seen

            seen = run_async()
            large = d.call("study_results", id=sid)
            expect(large["nonlinear"]["kind"] == "large_deformation",
                   f"a large-deformation result: {large.get('nonlinear')}")
            body = large["bodies"][0]["nonlinear"]
            expect(body["factor"] == 1.0 and body["increments"] >= 1,
                   f"it reached the load: {body}")
            expect(body["equilibrium"]["imbalance"]
                   < body["equilibrium"]["tolerance"],
                   f"it balances: {body['equilibrium']}")
            expect(large["solver"].startswith("calculix"),
                   f"auto ran CalculiX: {large['solver']}")
            print(f"nonlinear_smoke: large deformation, {body['increments']} "
                  f"increments, load_fraction seen {seen:.3g}")

            # Snap-through: a peak below the linear factor.
            d.call("set_study", id=sid,
                   nonlinear={"large_deformation": True,
                              "snap_through": True, "imperfection_mm": 0.5})
            refused(d, "export_study_inp", "snap_through false", id=sid,
                    path=os.path.join(work, "snap.inp"))
            refused(d, "run_study", "peak load factor", id=sid,
                    converge="always")
            run_async(timeout_s=300)
            snap = d.call("study_results", id=sid)
            nl = snap["nonlinear"]
            expect(nl["kind"] == "snap_through" and nl["peak_factor"],
                   f"a peak: {nl}")
            b = snap["bodies"][0]["nonlinear"]
            expect(b["peak_at"] == "deflection_limit",
                   f"a column peaks at its deflection limit: {b}")
            expect(b["peak_factor"] < b["linear_factor"],
                   f"below its linear factor: {b}")
            expect(snap["factor_of_safety"] == nl["governing_factor"],
                   "the factor of safety is the governing factor")
            claim = snap["receipt"]["claims"][0]
            expect(claim["oracle"]["id"] == "calculix/nonlinear"
                   and claim["basis"] == "predicted",
                   f"the receipt names the nonlinear run: {claim}")
            print(f"nonlinear_smoke: snap-through peak {b['peak_factor']:.4g} "
                  f"against linear {b['linear_factor']:.4g}, "
                  f"{b['ccx_runs']} ccx runs")

            # Contact (docs/simulation/README.md#contact): the column
            # standing on the ground, pushed down inside its kern -- a
            # nonlinear study by its contact alone, solved on CalculiX at
            # small deformation, its whole base closed.
            cid = d.call("add_study", name="Standing")["id"]
            refused(d, "set_study", "large_deformation: true", id=cid,
                    nonlinear={"increments": 2})
            d.call("add_load", study=cid, type="contact", face=end(-1.0),
                   friction=0.0)
            d.call("set_study", id=cid, mesh_size=5.0, converge="never",
                   nonlinear={"increments": 2})
            d.call("add_load", study=cid, type="remote", face=end(1.0),
                   force_n=2000.0, direction=[0, 0, -1],
                   point_mm=[5.0 + 10.0 / 12.0, 5.0, 150.0])
            listed = [s for s in d.call("list_studies")["studies"]
                      if s["id"] == cid][0]
            rest = [ld for ld in listed["loads"] if ld["type"] == "contact"]
            expect(rest and rest[0]["friction"] == 0.0,
                   f"list_studies lists the contact: {listed['loads']}")
            deck = os.path.join(work, "contact.inp")
            d.call("export_study_inp", id=cid, path=deck)
            with open(deck) as f:
                text = f.read()
            expect("TYPE=SURFACE TO SURFACE" in text and
                   "NLGEOM" not in text,
                   "the exported deck pairs the contact, without NLGEOM")
            run_async(cid)
            res = d.call("study_results", id=cid)
            expect(res["nonlinear"]["kind"] == "contact",
                   f"a contact result: {res.get('nonlinear')}")
            body = res["bodies"][0]
            contact = body["contacts"][0]
            expect(contact["kind"] == "support" and
                   contact["name"] == "ground",
                   f"the ground: {contact}")
            expect(abs(contact["normal_force_n"] - 2000.0) < 2.0,
                   f"the ground carries the load: {contact}")
            expect(contact["closed_area_mm2"] > 0.99 * contact["area_mm2"],
                   f"inside its kern the base stays closed: {contact}")
            expect(body["nonlinear"]["large_deformation"] is False,
                   f"solved at small deformation: {body['nonlinear']}")
            row = [r for r in body["reactions"] if r["type"] == "contact"][0]
            expect(abs(row["force_n"][2] - 2000.0) < 2.0,
                   f"the support's reaction row says so: {row}")
            print(f"nonlinear_smoke: contact closed "
                  f"{contact['closed_area_mm2']:.4g} of "
                  f"{contact['area_mm2']:.4g} mm^2, peak pressure "
                  f"{contact['max_pressure_mpa']:.4g} MPa")

            # Plasticity (docs/simulation/README.md#plasticity): the column
            # pulled to 300 MPa, past its 250 MPa yield, on the library
            # steel's hardening line; its deck carries *PLASTIC and an
            # unloading step, its result the plastic strain, what unloading
            # left, and a requirement reads max_plastic_strain.
            pid = d.call("add_study", name="Past yield")["id"]
            d.call("set_study", id=pid, mesh_size=5.0, converge="never",
                   nonlinear={"plasticity": True, "increments": 4})
            d.call("add_load", study=pid, type="fixed", face=end(-1.0))
            d.call("add_load", study=pid, type="force", face=end(1.0),
                   magnitude=30000.0, direction=[0, 0, 1])
            listed = [s for s in d.call("list_studies")["studies"]
                      if s["id"] == pid][0]
            expect(listed.get("nonlinear", {}).get("plasticity") is True,
                   f"list_studies says plasticity: {listed}")
            deck = os.path.join(work, "plastic.inp")
            d.call("export_study_inp", id=pid, path=deck)
            with open(deck) as f:
                text = f.read()
            expect("*PLASTIC" in text and "*CLOAD, OP=NEW" in text and
                   "NLGEOM" not in text,
                   "the exported deck is plastic, unloads, small deformation")
            doc = d.call("list_parts")["parts"][0]["id"]
            d.call("add_requirement", name="yields little",
                   measure={"kind": "study_result", "document": doc,
                            "study": pid, "quantity": "max_plastic_strain"},
                   bound={"kind": "at_most", "value": 0.05},
                   source="EN 1993-1-5 C.8's 5 % strain limit")
            res = d.call("run_study", id=pid)
            expect(res["nonlinear"]["kind"] == "plasticity",
                   f"a plastic result: {res.get('nonlinear')}")
            body = res["bodies"][0]
            pe = res["max_plastic_strain"]["value"]
            # (300 - 250) / (E / 99) away from the clamp, E 210 GPa.
            expect(0.02 < pe < 0.05, f"the plastic strain: {pe}")
            left = res["max_residual_displacement_mm"]["value"]
            expect(left > 0.9 * 150 * 50.0 * 99.0 / 210000.0,
                   f"what unloading left: {left} mm")
            expect(body["nonlinear"]["plasticity"]["unloaded"] is True and
                   body["nonlinear"]["plasticity"]["strain_limit"] == 0.05,
                   f"the run: {body['nonlinear']}")
            row = d.call("check_requirements")["requirements"][0]
            expect(row["state"] == "pass" and abs(row["value"] - pe) < 1e-12,
                   f"checked against max_plastic_strain: {row}")
            # The same column in PLA: no hardening curve, refused.
            d.call("set_material", feature_id=fid, material="pla")
            refused(d, "run_study", "has no hardening curve", id=pid)
            d.call("set_material", feature_id=fid, material="steel")
            print(f"nonlinear_smoke: plastic strain {pe:.5f}, "
                  f"{left:.4f} mm left once unloaded")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("nonlinear_smoke: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
