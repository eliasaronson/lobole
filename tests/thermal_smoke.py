#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A thermal study, through the RPCs alone.

What an agent with only the MCP surface does (docs/simulation/README.md
#thermal): a PLA block beside a heat source -- a heat_generation through it
and a face cooled by air -- solved for its peak temperature and its margin
to PLA's heat deflection temperature; a temperature_margin requirement
unchecked, then checked, with its receipt; a static study expanded by the
thermal study's temperature (PETG, which has an expansion coefficient); the
temperature field displayed; and, where `ping` finds a ccx, the thermal
study solved on CalculiX and its exported heat deck run through ccx.

The physics (slabs, the fin, the pipe, the clamped bar) is
tests/test_fem_thermal.cpp's. This is the contract over the wire.

Skips (exit 77) on a build without Netgen; its CalculiX half is skipped
where ping finds no ccx.

Usage: thermal_smoke.py <path-to-designer-binary>
"""
import os
import shutil
import subprocess
import sys
import tempfile

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
    work = tempfile.mkdtemp(prefix="lobole_thermal_smoke_")
    try:
        with launch(binary) as app:
            d = app.client
            features = d.call("ping").get("features", {})
            if not features.get("mesher"):
                print("thermal_smoke: SKIP (built without Netgen)")
                return SKIP
            ccx = features.get("calculix")
            d.call("new_project")
            fid = shapes.box(d, (0, 0, 0), (40, 20, 10), "Block")["id"]
            d.call("set_material", feature_id=fid, material="pla")
            els = d.call("list_elements")["bodies"][0]
            created_by = els["created_by"]
            body = "Block"

            def face(axis, sign):
                name = [f["name"] for f in els["faces"]
                        if abs(f["normal"][axis] - sign) < 1e-6][0]
                return {"feature_id": created_by, "element": name}

            sid = d.call("add_study", name="Heat", type="thermal")["id"]
            d.call("set_study", id=sid, mesh_size=4.0, converge="never")
            # No default: a convection without its film coefficient is
            # refused, naming the ranges to choose from.
            refused(d, "add_load", "Incropera", study=sid, type="convection",
                    face=face(2, 1.0), ambient_c=25.0)
            refused(d, "add_load", "film_w_m2k and ambient_c", study=sid,
                    type="convection", face=face(2, 1.0), magnitude=5)
            refused(d, "add_load", "a structural load", study=sid,
                    type="force", face=face(2, 1.0), magnitude=5)
            d.call("add_load", study=sid, type="convection", face=face(2, 1.0),
                   film_w_m2k=10.0, ambient_c=25.0)
            d.call("add_load", study=sid, type="heat_generation", body=body,
                   power_w=0.1)

            doc = d.call("list_parts")["parts"][0]["id"]
            d.call("add_requirement", name="stays stiff",
                   measure={"kind": "study_result", "document": doc,
                            "study": sid, "quantity": "temperature_margin"},
                   bound={"kind": "at_least", "value": 10.0},
                   source="ten kelvin below where PLA softens")
            rows = d.call("list_requirements")
            expect(rows["summary"]["unchecked"] == 1 and not rows["all_pass"],
                   f"stated, never checked -- not met: {rows['summary']}")

            r = d.call("run_study", id=sid)
            expect(r["type"] == "thermal", f"a thermal answer: {r.get('type')}")
            peak = r["max_temperature_c"]["value"]
            expect(25.0 < peak < 100.0, f"a plausible peak: {peak}")
            margin = r["temperature_margin_k"]
            expect(abs(margin["value"] - (55.0 - peak)) < 1e-6
                   and "0.45 MPa" in margin["basis"],
                   f"the margin against PLA's HDT: {margin}")
            b = r["bodies"][0]
            expect(abs(b["balance"]["in_w"] - 0.1) < 1e-9
                   and abs(b["balance"]["out_w"] - 0.1) < 1e-6,
                   f"the heat balances: {b['balance']}")
            expect(any("bulk resin" in w for w in r.get("warnings", [])),
                   f"warned that PLA's conductivity is its resin's: {r}")
            claim = r["receipt"]["claims"][0]
            expect(claim["oracle"]["id"] == "in_house/heat"
                   and claim["basis"] == "predicted",
                   f"a prediction by the in-house solver: {claim}")

            d.call("set_result_display", study=sid, field="temperature")
            refused(d, "set_result_display", "temperature, heat_flux",
                    study=sid, field="temprature")

            checked = d.call("check_requirements")
            row = checked["requirements"][0]
            expect(abs(row["value"] - margin["value"]) < 1e-9
                   and row["state"] == ("pass" if margin["value"] >= 10
                                        else "fail"),
                   f"checked against the margin: {row}")

            # A static study expanded by that temperature: PLA has no
            # expansion coefficient (no resin maker publishes a measured
            # one), so it is refused at the run; PETG has one.
            st = d.call("add_study", name="Expanded")["id"]
            d.call("set_study", id=st, mesh_size=4.0, converge="never",
                   thermal_load={"from_study": sid})
            d.call("add_load", study=st, type="fixed", face=face(2, -1.0))
            refused(d, "run_study", "expansion coefficient", id=st)
            d.call("set_material", feature_id=fid, material="petg")
            s = d.call("run_study", id=st)
            t = s["temperature"]
            expect(t["from_study"] == sid and t["stress_free_c"] == 25.0,
                   f"stress-free at the ambient: {t}")
            expect(t["max_c"] > 25.0, f"heated: {t}")
            d.call("set_result_display", study=st, field="temperature")
            refused(d, "set_study", "needs stress_free_c", id=st,
                    thermal_load={"uniform_c": 40.0})

            if ccx:
                # The same study (now PETG) both ways, on one mesh.
                peak = d.call("run_study", id=sid)["max_temperature_c"]["value"]
                d.call("set_study", id=sid, solver="calculix")
                c = d.call("run_study", id=sid)
                expect(c["solver"].startswith("calculix"), f"ccx: {c}")
                expect(c["receipt"]["claims"][0]["oracle"]["id"]
                       == "calculix/heat", f"its oracle: {c['receipt']}")
                rel = abs(c["max_temperature_c"]["value"] - peak) / peak
                expect(rel < 1e-5, f"the same peak on ccx: {rel}")
                inp = os.path.join(work, "heat.inp")
                d.call("export_study_inp", id=sid, path=inp)
                run = subprocess.run([ccx["path"], "-i", "heat"], cwd=work,
                                     capture_output=True, text=True,
                                     timeout=240)
                expect(run.returncode == 0,
                       f"ccx runs the heat deck: {run.stdout[-800:]}")
                with open(os.path.join(work, "heat.dat")) as f:
                    expect("temperatures" in f.read(),
                           "ccx printed the temperatures")
            print(f"thermal_smoke: OK -- PLA peaks at "
                  f"{r['max_temperature_c']['value']:.4g} C, margin "
                  f"{margin['value']:.3g} K"
                  + (", ccx agrees" if ccx else ", no ccx here"))
            return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
