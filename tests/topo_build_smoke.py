#!/usr/bin/env python3
"""Integration test: topology you can build from (docs/topopt/README.md
#topology-you-can-build-from), headless over JSON-RPC.

A steel bar fixed at one end and loaded at the other in two cases (down and
sideways) with a keep-out box through its middle: the run reports each
case's compliance, what the keep-out took and what the connectivity filter
removed; check_topology solves the shape and the solid design domain per
case; probe_study and set_result_display take a case; a load naming no case
and a keep-out on a missing body are refused, naming what there is.
Then split_for_printing cuts the bar into pieces that fit a small bed, and
refuses a bed no piece can fit.

Skips (exit 77) when the build has no Netgen.

Usage: topo_build_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402

SKIP = 77


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def refused(d, method, needle, **params):
    try:
        d.call(method, **params)
    except RuntimeError as e:
        expect(needle in str(e), f"{method} refusal names {needle!r}: {e}")
        return str(e)
    raise AssertionError(f"{method} {params} was accepted")


def wait(d, study, timeout=240.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        st = d.call("study_status", study=study)
        if not st["running"]:
            return st
        time.sleep(0.2)
    raise AssertionError(f"study {study} did not finish in {timeout}s")


def face_at(faces, axis, value, sign):
    for f in faces:
        n = f.get("normal")
        c = f.get("centroid")
        if n and c and abs(c[axis] - value) < 1e-6 and n[axis] * sign > 0.5:
            return f["name"]
    raise AssertionError(f"no face at axis {axis} = {value}")


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_topobuild_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        d.call("new_project")
        r = d.call("add_feature", feature={"type": "Box", "name": "Bar",
                                           "position": [0, 0, 0],
                                           "width": 120, "height": 30,
                                           "depth": 30})
        d.call("set_material", feature_id=r["id"], material="steel")
        body = d.call("list_elements")["bodies"][0]
        faces = body["faces"]
        root = face_at(faces, 0, 0.0, -1)
        tip = face_at(faces, 0, 120.0, 1)

        s = d.call("add_study", name="Bar", type="topology")["id"]
        d.call("set_study", study=s, mesh_size=6.0, volume_fraction=0.35,
               filter_radius=12.0, max_iterations=20,
               load_cases=[{"name": "down", "weight": 1.0},
                           {"name": "side", "weight": 2.0}])
        d.call("add_load", study=s, type="fixed", face=root)
        refused(d, "add_load", "'down', 'side'", study=s, type="force",
                face=tip, magnitude=500.0, direction=[0, 0, -1],
                case="up")
        refused(d, "add_load", "names none", study=s, type="fixed",
                face=root, case="down")
        d.call("add_load", study=s, type="force", face=tip, magnitude=500.0,
               direction=[0, 0, -1], case="down")
        d.call("add_load", study=s, type="force", face=tip, magnitude=300.0,
               direction=[0, 1, 0], case="side")
        refused(d, "add_keep_out", "'Bar'", study=s, body="worm block")
        d.call("add_keep_out", study=s, box_min=[50, -1, 18],
               box_max=[70, 31, 31])
        listed = d.call("list_studies")["studies"][0]
        expect([c["name"] for c in listed["load_cases"]] == ["down", "side"],
               f"cases listed: {listed}")
        expect(len(listed["keep_outs"]) == 1, f"keep-out listed: {listed}")

        started = d.call("run_study", study=s, outlive_client=True)
        expect(started.get("started"), f"run started: {started}")
        st = wait(d, s)
        expect(st["has_results"] and not st.get("error"), f"ran: {st}")
        res = d.call("study_results", study=s)
        names = [c["name"] for c in res["cases"]]
        expect(names == ["down", "side"], f"cases reported: {res}")
        weighted = sum(c["weight"] * c["compliance"] for c in res["cases"])
        expect(abs(weighted - res["compliance"]) < 1e-6 * res["compliance"],
               f"the objective is the weighted sum: {res}")
        ko = res["keep_outs"][0]
        expect(ko["tets"] > 0 and 0 < ko["volume_mm3"] < 20 * 32 * 13,
               f"the keep-out took its tets: {ko}")
        expect("islands" in res and not res["islands"]["load_path_broken"],
               f"islands reported: {res['islands']}")

        chk = d.call("check_topology", study=s)
        expect([c["name"] for c in chk["cases"]] == ["down", "side"],
               f"check per case: {chk}")
        for c in chk["cases"]:
            sh, so = c["shape"], c["solid"]
            expect(sh["max_displacement_mm"] > so["max_displacement_mm"] > 0,
                   f"the shape is softer than the solid: {c}")
            expect(sh["volume_mm3"] < so["volume_mm3"],
                   f"the shape is lighter: {c}")
            expect("mass_g" in sh and "factor_of_safety" in sh, f"{c}")
        # The keep-out is out of the solid too: the solid design domain is
        # the bar less the box.
        expect(chk["cases"][0]["solid"]["volume_mm3"] < 120 * 30 * 30 - 100,
               f"the solid excludes the keep-out: {chk}")
        shape = chk["shape_study"]
        down = d.call("probe_study", study=shape, face=tip, case="down")
        side = d.call("probe_study", study=shape, face=tip, case=1)
        expect(down["translation_mm"][2] < 0 and
               abs(down["translation_mm"][2]) > abs(down["translation_mm"][1]),
               f"case down moves the tip down: {down}")
        expect(side["translation_mm"][1] > 0 and
               abs(side["translation_mm"][1]) > abs(side["translation_mm"][2]),
               f"case side moves it sideways: {side}")
        refused(d, "probe_study", "'down', 'side'", study=shape, face=tip,
                case="up")
        shown = d.call("set_result_display", study=shape,
                       field="displacement", case="side")
        expect(shown["case"] == 1, f"display case: {shown}")
        again = d.call("check_topology", study=s)
        expect(again["shape_study"] == shape, "a second check refreshes")
        expect(len(d.call("list_studies")["studies"]) == 3,
               "no third study from a second check")

        # --- the result as a body ----------------------------------------
        stl = os.path.join(tmp, "bar_shape.stl")
        body = d.call("topo_to_body", study=s, path=stl)
        expect(body["valid"] and body["volume_mm3"] > 0,
               f"the shape imports as a valid solid: {body}")
        expect(body["volume_mm3"] < 120 * 30 * 30, f"lighter: {body}")
        names = [p["name"] for p in d.call("list_parts")["parts"]]
        expect("Bar shape" in names, f"its own part: {names}")
        refused(d, "topo_to_body", "Bar shape", study=s, path=stl)

        # --- split for printing ------------------------------------------
        d.call("set_result_display", field="none")
        plan = d.call("split_for_printing", bed_mm=[50, 50, 50],
                      dry_run=True)
        expect(len(plan["pieces"]) == 3, f"three slabs along X: {plan}")
        refused(d, "split_for_printing", "does not fit", bed_mm=[20, 20, 20],
                dry_run=True)
        made = d.call("split_for_printing", bed_mm=[50, 50, 50])
        expect(len(made["pieces"]) == 3, f"made: {made}")
        parts = d.call("list_parts")["parts"]
        piece_names = [p["name"] for p in parts if "piece" in p["name"]]
        expect(len(piece_names) == 3, f"three parts: {parts}")
        total = sum(pc["volume_mm3"] for pc in made["pieces"])
        expect(abs(total - 120 * 30 * 30) < 1.0,
               f"the pieces hold the bar's material: {made}")
        expect(all(pc["material"] == "steel" for pc in made["pieces"]),
               f"pieces keep the material: {made}")
        print("topo_build_smoke: ok")
    except RuntimeError as e:
        if "without FEM support" in str(e) or "built without" in str(e):
            print(f"skip: {e}")
            return SKIP
        raise
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except Exception:
            app.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
