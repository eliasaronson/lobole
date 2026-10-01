#!/usr/bin/env python3
"""A study is a job: it has an id, it dies with its client, and a run of
inputs it has already answered is not computed again
(docs/simulation/README.md#studies-as-jobs).

Found by the pan/tilt head (generators/pantilt/README.md, the friction list):
an exact motion study ran minutes with no handle, kept running after the
script that started it died (the only stop was killing the app), was run
whole again by `leave_at`, and was repeated identically by a rebuild.

Over the RPC, on the slider-crank rig:

- an async run answers a job id; study_status {job} reports it and
  cancel_study {job} stops it; an unknown id is refused, naming the jobs;
- a job whose connection closes is cancelled, unless it was started with
  `outlive_client`; a synchronous run whose caller hangs up stops too;
- the same inputs again are `reused` in well under a second, and a moved
  input (the start pose) is run;
- `leave_at` from the stored run costs one solve.

Usage: study_jobs_smoke.py <path-to-designer-binary>
"""
import os
import socket
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, os.path.join(HERE, "..", "generators"))
sys.path.insert(0, HERE)
from designer_remote import Designer  # noqa: E402
from designer_sdk import connect  # noqa: E402
from slider_crank import build  # noqa: E402
from smoke_app import launch  # noqa: E402

# Long enough that no run below finishes before it is cancelled: the solves
# alone are seconds at this count, and a cancel lands between two of them.
LONG = 4000


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def until_ended(d, job, timeout=60.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        st = d.call("study_status", job=job)
        if not st["running"]:
            return st
        time.sleep(0.05)
    raise AssertionError(f"job {job} did not end in {timeout}s")


def main():
    with launch(sys.argv[1]) as app:
        a = connect(app.port)
        rig, axle, _, _ = build(a, 20.0, 70.0)
        d = Designer(app.port, timeout=300)
        long_study = d.call("add_motion_study", driver=axle, dof="rotation",
                            to=360, samples=LONG, clash_mode="exact",
                            name="long", **{"from": 0})["id"]
        short = d.call("add_motion_study", driver=axle, dof="rotation",
                       to=360, samples=25, clash_mode="exact", name="short",
                       **{"from": 0})["id"]

        # ---- A job dies with the connection that started it ---------------
        starter = Designer(app.port)
        job = starter.call("run_motion_study", study=long_study,
                           **{"async": True})
        expect(job["started"] and isinstance(job["job"], int)
               and job["outlives_client"] is False, f"a job: {job}")
        st = d.call("study_status", job=job["job"])
        expect(st["running"] and st["kind"] == "motion"
               and st["study"] == long_study, f"reported by id: {st}")
        listed = d.call("background_jobs")["jobs"]
        expect(any(j["id"] == job["job"] for j in listed),
               f"background_jobs names it by id: {listed}")
        starter.close()
        st = until_ended(d, job["job"])
        expect(st["outcome"] == "cancelled"
               and st["cancel_reason"] == "its client disconnected",
               f"its client went, so did it: {st}")

        # ---- ...unless it was asked to outlive it; then cancel by id -------
        starter = Designer(app.port)
        job = starter.call("run_motion_study", study=long_study,
                           outlive_client=True, **{"async": True})
        expect(job["outlives_client"] is True, f"outlives: {job}")
        starter.close()
        time.sleep(0.3)  # the pump has seen the close by now
        st = d.call("study_status", job=job["job"])
        expect(st["running"] and st["outlives_client"],
               f"still running without its client: {st}")
        c = d.call("cancel_study", job=job["job"])
        expect(c["cancelled"] is True, f"cancelled by id: {c}")
        st = until_ended(d, job["job"])
        expect(st["outcome"] == "cancelled"
               and st["cancel_reason"] == "cancel_study", f"{st}")

        # ---- An id that names no study job is refused, naming them ---------
        try:
            d.call("cancel_study", job=999999)
            raise AssertionError("an unknown job id was accepted")
        except RuntimeError as e:
            expect("no study job 999999" in str(e) and str(job["job"]) in str(e),
                   f"refused, naming the jobs there are: {e}")

        # ---- A synchronous run stops when its caller hangs up --------------
        raw = socket.create_connection(("127.0.0.1", app.port))
        raw.sendall(b'{"id": 1, "method": "run_motion_study", "params": '
                    b'{"study": %d}}\n' % long_study)
        raw.close()
        t0 = time.time()
        st = d.call("motion_status", study=long_study)
        expect(time.time() - t0 < 30, "the hung-up run did not stop")
        expect(st["has_result"] and st["result"].get("cancelled") is True,
               f"a run nobody waits for stops: {st.get('result', {}).keys()}")

        # ---- The same inputs again are reused; a moved one is not ----------
        first = d.call("run_motion_study", study=short)
        expect(first["reused"] is False, "the first run computes")
        t0 = time.time()
        again = d.call("run_motion_study", study=short)
        took = time.time() - t0
        expect(again["reused"] is True and took < 1.0,
               f"reused in well under a second: {again['reused']}, {took:.2f}s")
        expect(again["input_hash"] == first["input_hash"]
               and again["samples"] == first["samples"], "the same run")
        d.call("set_motion_pose", study=short, t=0.5)  # the start pose moves
        moved = d.call("run_motion_study", study=short)
        expect(moved["reused"] is False
               and moved["input_hash"] != first["input_hash"],
               "a moved input is run again")

        # ---- leave_at from the stored run: one solve -----------------------
        left = d.call("run_motion_study", study=short, leave_at="last")
        expect(left["reused"] is True and left["left_at"] == 24
               and left["left_solves"] == 1, f"one solve: {left}")
        mate = next(m for m in d.call("list_mates")["mates"] if m["id"] == axle)
        expect(abs(mate["rotation_deg"] % 360.0) < 1e-6
               or abs(mate["rotation_deg"] % 360.0 - 360.0) < 1e-6,
               f"left at 360: {mate}")
        left = d.call("run_motion_study", study=short, leave_at_deg=100.0)
        expect(left["left_solves"] == 1 and left["left_at"] == 7,
               f"a value between samples, one solve: {left}")
        mate = next(m for m in d.call("list_mates")["mates"] if m["id"] == axle)
        expect(abs(mate["rotation_deg"] - 100.0) < 1e-6, f"at 100: {mate}")
        try:
            d.call("run_motion_study", study=short, leave_at="last",
                   **{"async": True})
            raise AssertionError("leave_at with async was accepted")
        except RuntimeError as e:
            expect("async run has none yet" in str(e), f"says why: {e}")
        d.close()
        print("study_jobs_smoke: ok")


if __name__ == "__main__":
    main()
