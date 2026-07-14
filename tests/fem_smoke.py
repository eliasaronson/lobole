#!/usr/bin/env python3
"""Integration test: FEM stress study, headless over JSON-RPC.

Builds a steel bar, fixes one end, pulls the other with an axial force, and
solves. Checks: the study CRUD RPCs, that run_study returns a von Mises stress
near the analytic F/A with a factor of safety vs yield, that results are not
stale right after solving, that an expression-driven load re-scales the stress
on re-run, and that results go stale after an edit. Exercises exactly the
headless design loop (set a variable, run, read stress vs yield, iterate).

Skips (exit 77) when the build has no Netgen — meshing is unavailable.

Usage: fem_smoke.py <path-to-designer-binary>
"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402

SKIP = 77  # ctest SKIP_RETURN_CODE: built without Netgen


def free_port():
    import socket
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]
    port = free_port()
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_fem_")
    env["XDG_STATE_HOME"] = tmp
    app = subprocess.Popen([binary, "--headless", "--remote", str(port)],
                           env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    try:
        d = Designer.wait_for(port)
        expect(d.call("ping")["ok"], "ping")

        # --- a 100 x 10 x 10 mm steel bar ----------------------------------
        d.call("new_project")
        d.call("set_variable", name="load_n", expression="10000")
        r = d.call("add_feature", feature={"type": "Box", "name": "Bar",
                                           "position": [0, 0, 0],
                                           "width": 100, "height": 10,
                                           "depth": 10})
        fid = r["id"]
        d.call("set_material", feature_id=fid, material="steel")

        els = d.call("list_elements")["bodies"][0]
        created_by = els["created_by"]
        faces = [f["name"] for f in els["faces"]]
        # x=0 and x=100 faces are face0/face1 in the primitive naming; the
        # solver benchmarks validate the physics, so any opposite pair works.
        fixed_face = faces[0]
        load_face = faces[1]

        # --- a study: fix one end, pull the other axially ------------------
        study = d.call("add_study", name="Pull")["id"]
        d.call("set_study", study=study, mesh_size=4.0)
        d.call("add_load", study=study, type="fixed",
               face={"feature_id": created_by, "element": fixed_face})
        d.call("add_load", study=study, type="force",
               face={"feature_id": created_by, "element": load_face},
               magnitude="load_n", direction=[1, 0, 0])

        studies = d.call("list_studies")["studies"]
        expect(len(studies) == 1, f"one study: {studies}")

        # --- mesh preview then solve ---------------------------------------
        mesh = d.call("mesh_study", study=study)
        # A mesh-less (no-Netgen) build surfaces its error here; skip cleanly.
        expect(mesh["bodies"][0]["elements"] > 0, "meshed to elements")

        res = d.call("run_study", study=study)
        expect(res["elements"] > 0, "solved a mesh")
        expect(res["stale"] is False, "fresh results not stale")
        sigma = res["max_von_mises_mpa"]["value"]
        # Uniaxial bar: peak von Mises near F/A = 10000/100 = 100 MPa. The
        # fully-clamped end concentrates stress, so allow generous headroom.
        expect(80.0 < sigma < 200.0, f"von Mises ~100 MPa, got {sigma}")
        expect("factor_of_safety" in res, "steel yield gives a FoS")

        # study_results reads the cache without re-solving.
        again = d.call("study_results", study=study)
        expect(again["stale"] is False and again["elements"] == res["elements"],
               "study_results matches run_study")

        # --- the design loop: change the load, results go stale, re-run ----
        d.call("set_variable", name="load_n", expression="20000")
        stale = d.call("study_results", study=study)
        expect(stale["stale"] is True, "results stale after variable change")
        res2 = d.call("run_study", study=study)
        ratio = res2["max_von_mises_mpa"]["value"] / sigma
        expect(abs(ratio - 2.0) < 0.05, f"stress scales 2x with load, got {ratio}")

        print("fem_smoke: OK")
    except Exception as e:  # a no-Netgen build reports "built without FEM support"
        if "FEM support" in str(e) or "Netgen" in str(e):
            print("fem_smoke: SKIP (built without Netgen)")
            return SKIP
        raise
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
