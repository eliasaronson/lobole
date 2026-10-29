#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A static study solved on CalculiX, through the RPCs alone.

What an agent with only the MCP surface sees (docs/simulation/README.md
#calculix): `ping` says which ccx a study would run; a static study set to
`solver: calculix` runs on it and says so in `study_results` (`solver`, the
residual against our own stiffness, the agreement with an in-house run of
the same study, a receipt whose oracle is calculix/static and whose basis is
still a prediction); `export_study_inp` writes a deck ccx itself runs; the
refusals name what they refuse.

The physics and the same-mesh agreement over every load kind are
tests/test_fem_calculix.cpp's. This is the contract over the wire.

Skips (exit 77) on a build without Netgen, or where `ping` finds no ccx.

Usage: calculix_static_smoke.py <path-to-designer-binary>
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
    work = tempfile.mkdtemp(prefix="lobole_ccx_smoke_")
    try:
        with launch(binary) as app:
            d = app.client
            features = d.call("ping").get("features", {})
            if not features.get("mesher"):
                print("calculix_static_smoke: SKIP (built without Netgen)")
                return SKIP
            ccx = features.get("calculix")
            if not ccx:
                print("calculix_static_smoke: SKIP (ping finds no ccx)")
                return SKIP
            expect(ccx["version"].startswith("2."), f"a version: {ccx}")

            d.call("new_project")
            fid = shapes.box(d, (0, 0, 0), (100, 10, 10), "Bar")["id"]
            d.call("set_material", feature_id=fid, material="steel")
            els = d.call("list_elements")["bodies"][0]
            created_by = els["created_by"]

            def end(sign):
                return [f["name"] for f in els["faces"]
                        if abs(f["normal"][0] - sign) < 1e-6][0]

            def study(name, solver=None):
                sid = d.call("add_study", name=name)["id"]
                d.call("set_study", id=sid, mesh_size=4.0,
                       **({"solver": solver} if solver else {}))
                d.call("add_load", study=sid, type="fixed",
                       face={"feature_id": created_by, "element": end(-1.0)})
                d.call("add_load", study=sid, type="force",
                       face={"feature_id": created_by, "element": end(1.0)},
                       magnitude=500.0, direction=[0, 0, -1])
                return sid

            own = study("In-house")
            theirs = study("CalculiX", "calculix")
            listed = {s["id"]: s for s in d.call("list_studies")["studies"]}
            expect(listed[theirs].get("solver") == "calculix",
                   f"list_studies names the solver: {listed[theirs]}")
            expect("solver" not in listed[own],
                   f"auto is not listed: {listed[own]}")

            # The refusals, before anything starts.
            refused(d, "run_study", "timeout_s bounds a CalculiX run",
                    id=own, timeout_s=10)
            modal = d.call("add_study", name="Modes", type="modal")["id"]
            refused(d, "set_study", "solved in-house", id=modal,
                    solver="calculix")
            refused(d, "set_study", "auto, in_house, calculix", id=own,
                    solver="ccx")
            refused(d, "set_study", "set solver: auto", id=theirs,
                    type="modal")
            d.call("remove_study", id=modal)

            # The same study in-house first, then on CalculiX: the session
            # holds the in-house answer its CalculiX one is compared with.
            d.call("set_study", id=theirs, solver="auto")
            a = d.call("run_study", id=theirs)
            expect("in_house_agreement" not in a and "calculix" not in a,
                   f"an in-house answer carries no CalculiX keys: {a.keys()}")
            d.call("set_study", id=theirs, solver="calculix")
            b = d.call("run_study", id=theirs, timeout_s=300)
            expect(not b["reused"], "a CalculiX run is not the in-house one")
            expect(b["solver"] == f"calculix {ccx['version']}",
                   f"solver named with its version: {b.get('solver')}")
            check = b.get("calculix", {})
            expect(0 < check.get("residual", 0) <=
                   1e-6 + check.get("residual_floor", 0),
                   f"residual against our K: {check}")
            expect(check.get("energy_residual", 1) < 1e-5,
                   f"energy balance against our K: {check}")
            agree = b.get("in_house_agreement")
            expect(agree is not None, f"agreement reported: {b.keys()}")
            expect(agree["max_displacement_rel"] < 1e-5
                   and agree["max_von_mises_rel"] < 1e-5,
                   f"agrees with the in-house run: {agree}")
            rel = (abs(a["max_displacement_mm"]["value"]
                       - b["max_displacement_mm"]["value"])
                   / a["max_displacement_mm"]["value"])
            expect(rel < 1e-5, f"the same displacement: {rel}")
            claim = b["receipt"]["claims"][0]
            expect(claim["oracle"]["id"] == "calculix/static"
                   and claim["oracle"]["version"] == ccx["version"],
                   f"the receipt names CalculiX: {claim}")
            expect(claim["basis"] == "predicted",
                   f"a second answer, not a stronger one: {claim}")
            expect("in_house_agreement" not in claim.get("note", "")
                   and "agree" not in claim.get("note", ""),
                   f"agreement is not folded into the claim: {claim}")

            # The deck, as a person would run it.
            deck = os.path.join(work, "bar.inp")
            out = d.call("export_study_inp", id=theirs, path=deck)
            expect(out["nodes"] > 0 and os.path.getsize(deck) == out["bytes"],
                   f"the deck written: {out}")
            run = subprocess.run([ccx["path"], "-i", "bar"], cwd=work,
                                 capture_output=True, text=True, timeout=300)
            expect(run.returncode == 0 and "*ERROR" not in run.stdout,
                   f"ccx runs the exported deck: {run.stdout[-800:]}")
            with open(os.path.join(work, "bar.dat")) as f:
                expect("displacements (vx,vy,vz)" in f.read(),
                       "ccx printed the displacement")
            refused(d, "export_study_inp", "does not exist", id=theirs,
                    path=os.path.join(work, "no", "such", "dir", "x.inp"))
            print(f"calculix_static_smoke: OK -- calculix {ccx['version']} "
                  f"agrees with the in-house solve to "
                  f"{agree['max_displacement_rel']:.1e} (displacement), "
                  f"{agree['max_von_mises_rel']:.1e} (von Mises); residual "
                  f"{check['residual']:.1e}")
            return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
