#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The design sweep on a generator part, over the RPC
(docs/requirements/README.md#design-sweep).

The pan/tilt head's camera (generators/pantilt/build_tilt.py `camera`,
built alone over spec.py's variables) swept over `cam_d` from a quarter of
its value to its value. The grip stands `cam_d / 3` off the front and its
edges are filleted at `cam_grip_r`, which does not follow `cam_d`, so the
grip cannot take its fillet below cam_d = 6 * cam_grip_r: first_failure
brackets that value (read from the part, not written here) and names
"Grip edges"; refine narrows the bracket. The sweep changes nothing in the
document.

The async form is a job study_status follows; a second sweep while it runs
is refused naming it; cancel_study stops one between points and keeps the
points done; and a sweep whose client leaves is cancelled with it. Each
point is held LOBOLE_TEST_SWEEP_MS so the job is still running when asked.

Usage: design_sweep_smoke.py <path-to-designer-binary>
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "generators", "pantilt"))
sys.path.insert(0, HERE)
from lobole.client import Client  # noqa: E402
from lobole.sdk import connect  # noqa: E402
from smoke_app import launch  # noqa: E402

HOLD_MS = 150


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def build_camera(port):
    """spec.py's variables on a first part, then the camera, as
    build_tilt.py builds it."""
    import build_tilt
    import spec
    app = connect(port)
    app.new()
    first = app.part("spec")
    first.activate()
    S = spec.declare(app, first)
    part, _ = build_tilt.camera(app, S)
    return app, part.id


def wait_done(d, job, timeout=240.0):
    deadline = time.monotonic() + timeout
    while True:
        st = d.call("study_status", job=job)
        if not st.get("running"):
            return st
        expect(time.monotonic() < deadline, f"job {job} still running: {st}")
        time.sleep(0.1)


def main():
    binary = sys.argv[1]
    env = dict(os.environ, LOBOLE_TEST_SWEEP_MS=str(HOLD_MS))
    with launch(binary, env=env) as app:
        d = Client(app.port, timeout=600)
        sdk, camera = build_camera(app.port)
        values = {v["name"]: v for v in d.call("list_variables")["variables"]}
        cam_d = values["cam_d"]["value"]
        grip_r = values["cam_grip_r"]["value"]
        breaks = 6.0 * grip_r
        expect(cam_d / 4 < breaks < cam_d,
               f"the grip's fillet must break inside the range: cam_d "
               f"{cam_d}, 6 * cam_grip_r {breaks}")
        before = d.call("get_state")
        dirty = d.call("ping").get("dirty")
        doc_before = d.call("mass_properties", part=camera)["total"]

        # ---- Synchronous: parked on its job, answered with the table -----
        r = d.call("design_sweep",
                   variables=[{"name": "cam_d", "min": "cam_d / 4",
                               "max": "cam_d"}],
                   refine=3)
        expect(r["planned"] == 5 + 2 * 3 and r["cap"] == 64, r)
        expect([p["document"] for p in r["base"]["parts"]] == [camera],
               f"only the camera reads cam_d: {r['base']['parts']}")
        expect(r["base"]["ok"], f"the camera builds as it is: {r['base']}")
        below = r["first_failure"]["cam_d"]["below"]
        expect(below["failed"] < breaks <= below["last_ok"] + 1e-6,
               f"the bracket holds 6 * cam_grip_r = {breaks:.3f}: {below}")
        # Five samples over 56 mm, bisected three times: 14 mm / 8.
        expect(below["last_ok"] - below["failed"] <= (cam_d * 0.75) / 4 / 8
               + 1e-6, f"refine narrowed the bracket: {below}")
        expect("Grip edges" in below["why"], below)
        failing = [p for p in r["points"] if not p["ok"]]
        expect(failing and all(
            any(b["name"] == "Grip edges"
                for part in p["parts"] for b in part.get("broken", []))
            for p in failing), f"each failing point names the fillet: "
               f"{[p.get('why') for p in failing]}")
        expect("above" not in r["first_failure"]["cam_d"], r["first_failure"])
        res = d.call("design_sweep_result")
        expect(res["first_failure"] == r["first_failure"],
               "design_sweep_result answers the last sweep")
        # The document is as it was.
        after = d.call("get_state")
        expect(after.get("version") == before.get("version")
               and after["features"] == before["features"],
               "the sweep touched the document")
        now = {v["name"]: (v["expression"], v["value"])
               for v in d.call("list_variables")["variables"]}
        expect(now == {k: (v["expression"], v["value"])
                       for k, v in values.items()},
               "the sweep moved a variable")
        expect(d.call("mass_properties", part=camera)["total"]["volume_mm3"]
               == doc_before["volume_mm3"], "the camera's volume moved")
        expect(d.call("ping").get("dirty") == dirty,
               "the sweep made the project dirty")

        # ---- The SDK's form: a job, waited on, the same table ------------
        t = sdk.sweep({"cam_d": ("cam_d / 4", "cam_d")}, samples=3)
        expect(t["first_failure"]["cam_d"]["below"]["failed"] < breaks, t)

        # ---- Async: a job; a second is refused; cancel keeps the points --
        a = d.call("design_sweep", **{
            "variables": [{"name": "cam_d", "min": "cam_d / 4",
                           "max": "cam_d"}],
            "samples": 33, "max_points": 64, "async": True})
        job = a["job"]
        expect(a.get("started") and a["planned"] == 33, a)
        st = d.call("study_status", job=job)
        expect(st["running"] and st["kind"] == "design_sweep", st)
        try:
            d.call("design_sweep", variables=[{"name": "cam_d", "min": 50,
                                               "max": 60}])
            raise AssertionError("a second sweep was accepted")
        except RuntimeError as e:
            expect(f"already running (job {job})" in str(e), str(e))
        deadline = time.monotonic() + 120
        while d.call("design_sweep_result", job=job).get("fraction", 0) <= 0:
            expect(time.monotonic() < deadline, "no point was measured")
            time.sleep(0.05)
        d.call("cancel_study", job=job)
        st = wait_done(d, job)
        expect(st["outcome"] == "cancelled", st)
        kept = d.call("design_sweep_result", job=job)
        expect(kept["cancelled"] and 1 <= len(kept["points"]) < 33,
               f"cancel keeps the points done: {len(kept['points'])}")

        # ---- A sweep whose client leaves goes with it --------------------
        other = Client(app.port, timeout=60)
        gone = other.call("design_sweep", **{
            "variables": [{"name": "cam_d", "min": 50, "max": 75}],
            "samples": 20, "async": True})["job"]
        other.close()
        st = wait_done(d, gone)
        expect(st["outcome"] == "cancelled", f"the client left: {st}")
        # ...unless it asked to outlive it.
        other = Client(app.port, timeout=60)
        stays = other.call("design_sweep", **{
            "variables": [{"name": "cam_d", "min": 60, "max": 75}],
            "samples": 2, "async": True, "outlive_client": True})["job"]
        other.close()
        st = wait_done(d, stays)
        expect(st["outcome"] == "done", f"outlive_client: {st}")
    print("design_sweep_smoke: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
