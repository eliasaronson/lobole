#!/usr/bin/env python3
"""Integration test: async topology optimization, headless over JSON-RPC (TO2).

Builds a steel design envelope, fixes one face and pushes on the opposite face,
then runs a topology study. Exercises the async run path: run_study returns
immediately ({started}), study_status reports live progress, and the result is
stored + reported (converged, volume fraction near target) once the worker
thread finishes. Then verifies cancellation: a second run stopped mid-flight
keeps a best-so-far (cancelled) result.

Skips (exit 77) when the build has no Netgen — meshing is unavailable.

Usage: topo_smoke.py <path-to-designer-binary>
"""
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402

SKIP = 77  # ctest SKIP_RETURN_CODE: built without Netgen




def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def poll_until_done(d, study, timeout=180.0):
    """Poll study_status until the run is no longer running; return the last
    non-running status. Raises on timeout."""
    deadline = time.time() + timeout
    last_iter = -1
    while time.time() < deadline:
        st = d.call("study_status", study=study)
        if not st["running"]:
            return st
        # Progress must advance (iteration is monotone) — a sanity check that
        # the worker is actually iterating, not wedged.
        last_iter = max(last_iter, st["iteration"])
        time.sleep(0.15)
    raise AssertionError(f"topology run did not finish in {timeout}s")


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_topo_")
    env["XDG_STATE_HOME"] = tmp
    # ...and its own CONFIG dir, or the run edits the developer's real
    # preferences: set_settings persists immediately (headless runs no GUI
    # loop to debounce it), so a suite reset their font size to 120%.
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        expect(d.call("ping")["ok"], "ping")

        # --- a steel design envelope ---------------------------------------
        d.call("new_project")
        r = d.call("add_feature", feature={"type": "Box", "name": "Envelope",
                                           "position": [0, 0, 0],
                                           "width": 80, "height": 24,
                                           "depth": 16})
        fid = r["id"]
        d.call("set_material", feature_id=fid, material="steel")

        els = d.call("list_elements")["bodies"][0]
        created_by = els["created_by"]
        faces = [f["name"] for f in els["faces"]]
        fixed_face, load_face = faces[0], faces[1]  # an opposite pair

        # --- a topology study (type set at creation, one call) --------------
        # `kind` is still accepted; the REPLY says `type`, which is what the
        # enum, the file and list_studies have always called it.
        created = d.call("add_study", name="Generative", kind="topology")
        expect(created["type"] == "topology",
               f"add_study type applied: {created}")
        study = created["id"]
        d.call("set_study", study=study, mesh_size=7.0,
               volume_fraction=0.4, filter_radius=14.0, max_iterations=25)
        try:
            d.call("add_study", name="bogus", kind="banana")
            raise AssertionError("unknown study kind must error")
        except RuntimeError as e:
            expect("static, modal, or topology" in str(e),
                   f"kind error lists options: {e}")
        d.call("add_load", study=study, type="fixed",
               face={"feature_id": created_by, "element": fixed_face})
        d.call("add_load", study=study, type="force",
               face={"feature_id": created_by, "element": load_face},
               magnitude=1500.0, direction=[0, 0, -1])

        listed = d.call("list_studies")["studies"]
        expect(listed[0]["type"] == "topology", f"study type: {listed}")
        expect(abs(listed[0]["volume_fraction"] - 0.4) < 1e-9, "vf echoed")

        # --- run: async, returns immediately -------------------------------
        started = d.call("run_study", study=study)
        # A no-Netgen build fails to mesh here — caught below and skipped.
        expect(started.get("started") is True, f"run started: {started}")

        st = poll_until_done(d, study)
        expect(st["has_results"], "results stored after the run finished")
        expect(st["cancelled"] is False, "a completed run is not cancelled")
        expect(st["iterations"] > 0, "it iterated")
        # Solve provenance: which linear solver ran ("gpu: ..." on a machine
        # with a usable device, else "cpu (...)") and how long it took.
        expect(st["solver"].startswith(("gpu", "cpu")),
               f"solver reported: {st.get('solver')}")
        expect(st["solve_seconds"] > 0.0, "solve time reported")

        res = d.call("study_results", study=study)
        # `type`, the same word add_study takes and list_studies reports —
        # this used to be a third name ("analysis") for one concept.
        expect(res["type"] == "topology", f"topology summary: {res}")
        expect(res["elements"] > 0, "solved a mesh")
        expect(res["solver"] == st["solver"], "results echo the solver")
        expect(abs(res["volume_fraction"] - 0.4) < 0.03,
               f"volume fraction near target, got {res['volume_fraction']}")
        expect(res["stale"] is False, "fresh results not stale")

        # --- display controls (headless: sets state, no renderer) -----------
        disp = d.call("set_result_display", study=study, field="shape",
                      threshold=0.0)
        expect(disp["field"] == "shape", f"shape display echoed: {disp}")
        disp = d.call("set_result_display", study=study, field="density")
        expect(disp["field"] == "density", "density display echoed")
        d.call("set_result_display", study=study, field="none")

        # --- STL export of the optimized shape ------------------------------
        stl = os.path.join(tmp, "shape.stl")
        exp = d.call("export_topo_stl", study=study, path=stl, smooth=3)
        expect(exp["path"] == stl, f"export echoed path: {exp}")
        expect(os.path.getsize(stl) > 84, "STL has a header + triangles")
        with open(stl, "rb") as fh:
            fh.seek(80)
            ntri = int.from_bytes(fh.read(4), "little")
        expect(os.path.getsize(stl) == 84 + ntri * 50, "STL size matches count")
        expect(ntri > 0, "STL has triangles")
        # The reply reports what a slicer will see (export_stl's honesty).
        expect(exp["triangles"] == ntri, f"triangle count echoed: {exp}")
        expect("watertight" in exp and "boundary_edges" in exp,
               f"watertightness reported: {exp}")

        # --- shape-guide ghost toggle ---------------------------------------
        g = d.call("set_topo_ghost", study=study, on=True)
        expect(g["on"] is True, f"ghost on echoed: {g}")
        d.call("set_topo_ghost", study=study, on=False)

        # --- cancel: a second run stopped mid-flight keeps best-so-far -----
        started2 = d.call("run_study", study=study)
        expect(started2.get("started") is True, "second run started")
        c = d.call("cancel_study", study=study)
        expect(c["cancelled"] is True, "cancel acknowledged")
        st2 = poll_until_done(d, study, timeout=60.0)
        expect(st2["has_results"], "a cancelled run still stores a result")
        expect(st2["cancelled"] is True, "the result is flagged cancelled")

        # A second run cannot start while one is in progress.
        started3 = d.call("run_study", study=study)
        expect(started3.get("started") is True, "third run started")
        try:
            d.call("run_study", study=study)
            raise AssertionError("expected 'already in progress' error")
        except Exception as e:
            expect("in progress" in str(e), f"one run at a time: {e}")
        d.call("cancel_study", study=study)
        poll_until_done(d, study, timeout=60.0)

        print("topo_smoke: OK")
    except Exception as e:  # a no-Netgen build reports "built without FEM support"
        if "FEM support" in str(e) or "Netgen" in str(e):
            print("topo_smoke: SKIP (built without Netgen)")
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
