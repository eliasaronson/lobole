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
import time
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
    # ...and its own CONFIG dir, or the run edits the developer's real
    # preferences: set_settings persists immediately (headless runs no GUI
    # loop to debounce it), so a suite reset their font size to 120%.
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
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

        # --- async static run: same physics through the worker-thread path
        # (the big-mesh escape from synchronous-solve client timeouts).
        d.call("set_variable", name="load_n", expression="10000")
        started = d.call("run_study", study=study, **{"async": True})
        expect(started.get("started") and started.get("running"),
               f"async static run starts: {started}")
        deadline = time.time() + 120
        while time.time() < deadline:
            st = d.call("study_status", study=study)
            if not st.get("running", False):
                break
            expect("elapsed_seconds" in st,
                   f"running status reports elapsed: {st}")
            try:
                d.call("cancel_study", study=study)
                raise AssertionError("static solve must refuse to cancel")
            except RuntimeError as e:
                expect("cannot be cancelled" in str(e),
                       f"cancel error explains: {e}")
            time.sleep(0.2)
        res3 = d.call("study_results", study=study)
        expect(res3["stale"] is False, f"async result cached: {res3}")
        sigma3 = res3["max_von_mises_mpa"]["value"]
        expect(abs(sigma3 / sigma - 1.0) < 0.05,
               f"async solve matches the sync physics: {sigma3} vs {sigma}")

        # --- modal analysis: cantilever natural frequencies (v2) ------------
        modal = d.call("add_study", name="Modes")["id"]
        d.call("set_study", study=modal, type="modal", num_modes=4, mesh_size=4.0)
        d.call("add_load", study=modal, type="fixed",
               face={"feature_id": created_by, "element": fixed_face})
        try:
            mres = d.call("run_study", study=modal)
            freqs = mres["bodies"][0]["frequencies_hz"]
            expect(len(freqs) >= 1, "modal returned frequencies")
            # first bending ~815 Hz for a 100x10x10 steel bar; generous bound.
            expect(700 < freqs[0] < 950, f"first frequency ~815 Hz, got {freqs[0]}")
            expect(freqs[1] >= freqs[0], "frequencies ascend")

            # --- gravity / self-weight static study -------------------------
            grav = d.call("add_study", name="Weight")["id"]
            d.call("set_study", study=grav, mesh_size=4.0, gravity=[0, 0, -9.81])
            d.call("add_load", study=grav, type="fixed",
                   face={"feature_id": created_by, "element": fixed_face})
            gres = d.call("run_study", study=grav)
            expect(gres["max_displacement_mm"]["value"] > 0,
                   "self-weight produced a deflection")
            print("fem_smoke: OK")
        except Exception as e:
            if "modal support" in str(e) or "Spectra" in str(e):
                print("fem_smoke: OK (static; modal skipped, no Spectra)")
            else:
                raise
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
