#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A bearing load, a free-free modal study and a full result through the
RPCs alone (docs/simulation/README.md#bearing-loads, #free-free-modes,
#a-full-result).

A steel lug, clamped at one end and pulled by a bearing load on its bore:
the clamp carries the force. The same lug hanging free: six rigid modes,
labelled, holding the whole mass. Its static study keeps its whole field:
sampled inside, saved, the app closed and a new one opened on the project,
the same point answers the same field, and the reopened result exports.
Every refused call names what is accepted and changes nothing
(docs/scripting/rpc.md#a-refused-call-changes-nothing).

Skips (exit 77) on a build without Netgen.

Usage: sim_loads_results_smoke.py <path-to-designer-binary>
"""
import json
import os
import shutil
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
        return str(e)
    raise AssertionError(f"{method} {params} was accepted")


INSIDE = [30.0, 10.0, 2.0]  # the lug's middle, inside the steel


def build_lug(d):
    # 60 x 20 x 4, a bore of 8 at x = 12.
    shapes.box(d, (0, 0, 0), (60, 20, 4), "Lug", material="steel")
    d.call("add_feature", feature={
        "type": "Sketch", "name": "Centre",
        "plane": {"origin": [0, 0, 4], "normal": [0, 0, 1],
                  "uAxis": [1, 0, 0]},
        "entities": [{"type": "point", "p": [12.0, 10.0]}]})
    feats = {f["name"]: f["id"] for f in d.call("get_state")["features"]}
    d.call("add_feature", feature={
        "type": "Hole", "name": "Pin hole", "hole_kind": "simple",
        "sketch_id": feats["Centre"], "diameter": 8.0, "through": True})
    els = d.call("list_elements")["bodies"][0]

    def face(axis, at):
        name = [f["name"] for f in els["faces"]
                if abs(abs(f["normal"][axis]) - 1) < 1e-9
                and abs(f["centroid"][axis] - at) < 1e-6][0]
        return {"feature_id": els["created_by"], "element": name}

    bore = [f["name"] for f in els["faces"] if "bore" in f["name"]][0]
    return face(0, 60.0), face(0, 0.0), face(2, 4.0), bore


def main():
    binary = os.path.abspath(sys.argv[1])
    tmp = tempfile.mkdtemp(prefix="lobole-sim-loads-")
    try:
        return run(binary, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def run(binary, tmp):
    path = os.path.join(tmp, "lug.lob")
    with launch(binary) as app:
        d = app.client
        if not d.call("ping").get("features", {}).get("mesher"):
            print("sim_loads_results_smoke: SKIP (built without Netgen)")
            return SKIP
        d.call("new_project")
        end, start, top, bore = build_lug(d)
        sid = d.call("add_study", name="Pulled")["id"]
        d.call("set_simulation", mesh_size_mm=2.0)
        d.call("set_study", id=sid, converge="never", keep="full")
        d.call("add_load", study=sid, type="fixed", face=end)

        # ---- Refused, each changing nothing --------------------------------
        before = d.call("list_studies")
        for needle, params in [
                ("takes `force_n`", {"type": "bearing", "face": bore,
                                     "magnitude": 10.0,
                                     "direction": [-1, 0, 0]}),
                ("needs force_n", {"type": "bearing", "face": bore,
                                   "direction": [-1, 0, 0]}),
                ("needs `direction`", {"type": "bearing", "face": bore,
                                       "force_n": 400.0}),
                ("not cylindrical", {"type": "bearing", "face": top,
                                     "force_n": 400.0,
                                     "direction": [-1, 0, 0]}),
                ("across its bore's axis", {"type": "bearing", "face": bore,
                                            "force_n": 400.0,
                                            "direction": [-1, 0, 1]}),
                ("`region`",
                 {"type": "bearing", "face": bore, "force_n": 400.0,
                  "direction": [-1, 0, 0],
                  "region": {"center_mm": [12, 6, 2], "radius_mm": 2}})]:
            refused(d, "add_load", needle, study=sid, **params)
        refused(d, "set_study", "surface, full", id=sid, keep="whole")
        refused(d, "set_study", "modal study's", id=sid, free=True)
        expect(d.call("list_studies") == before,
               "the refused calls changed nothing")

        # ---- A bearing load: the clamp carries its force -------------------
        lid = d.call("add_load", study=sid, type="bearing", face=bore,
                     force_n=400.0, direction=[-1, 0, 0])["id"]
        listed = [s for s in d.call("list_studies")["studies"]
                  if s["id"] == sid][0]
        expect(listed["keep"] == "full", listed)
        expect([l for l in listed["loads"] if l["id"] == lid][0]["type"]
               == "bearing", listed["loads"])
        r = d.call("run_study", id=sid)
        total = r["reaction_total"]["force_n"]
        expect(abs(total[0] - 400.0) < 1e-3 * 400 and abs(total[1]) < 0.4
               and abs(total[2]) < 0.4, f"the clamp answers the pull: {r}")
        inside = d.call("sample_study", id=sid, at_mm=INSIDE)
        expect(inside["inside"] is True and inside["distance_mm"] == 0.0,
               f"a full result answers inside the part: {inside}")
        expect(inside["von_mises_mpa"] > 0.0, inside)

        # ---- Free-free: six rigid modes and the whole mass -----------------
        mid = d.call("add_study", type="modal", name="Hanging")["id"]
        d.call("add_load", study=mid, type="fixed", face=start)
        before = d.call("list_studies")
        refused(d, "set_study", "no support", id=mid, free=True)
        expect(d.call("list_studies") == before,
               "the refused free changed nothing")
        d.call("remove_load", study=mid,
               id=[l for l in before["studies"] if l["id"] == mid][0]
               ["loads"][0]["id"])
        d.call("set_study", id=mid, free=True, num_modes=3)
        refused(d, "add_load", "free-free", study=mid, type="fixed",
                face=start)
        modal = d.call("run_study", id=mid)
        body = modal["bodies"][0]
        expect(modal["free"] is True and body["rigid_modes"] == 6, modal)
        expect([m["rigid"] for m in body["modes"]] == [True] * 6 +
               [False] * 3, body["modes"])
        f1 = body["modes"][6]["frequency_hz"]
        expect(all(m["frequency_hz"] < 1e-3 * f1 for m in body["modes"][:6]),
               body["modes"])
        expect(all(abs(x - 1.0) < 1e-5 for x in body["effective_mass_fraction"]),
               f"the rigid modes hold the whole mass: {body}")

        d.call("save_project", path=path)
        sims = os.listdir(os.path.join(path, "simulations"))
        res = os.path.join(path, "simulations", sims[0], "results",
                           f"{sid}.result.json")
        with open(res) as f:
            saved = json.load(f)
        expect(saved["keep"] == "full" and "mesh" in saved["bodies"][0]
               and "nodes" not in saved["bodies"][0],
               "the full result is written against the stored mesh")

    # ---- Closed, and opened again in a new instance ------------------------
    with launch(binary) as app:
        d = app.client
        d.call("open_project", path=path)
        studies = {s["id"]: s for s in d.call("list_studies")["studies"]}
        expect(studies[sid]["has_results"] and not studies[sid]["stale"],
               f"reopened current: {studies[sid]}")
        again = d.call("sample_study", id=sid, at_mm=INSIDE)
        for key in ("displacement_mm", "stress_mpa", "von_mises_mpa",
                    "element", "inside"):
            expect(again[key] == inside[key],
                   f"{key} as before the save: {again[key]} vs {inside[key]}")
        out = d.call("export_study_result", id=sid,
                     path=os.path.join(tmp, "lug.vtu"))
        expect(out["cells"] > 0 and os.path.getsize(
            os.path.join(tmp, "lug.vtu")) > 0,
            f"the reopened full result exports: {out}")
        modal = d.call("study_results", id=mid)
        expect([m["rigid"] for m in modal["bodies"][0]["modes"]][:6] ==
               [True] * 6, "the rigid labels came back")
    print("sim_loads_results_smoke: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
