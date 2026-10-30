#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every study kind's result is saved with the project and reused after a
reopen (docs/simulation/README.md#studies-as-jobs).

A static, a modal and a topology study of one bar, the topology result's
check (check_topology, a job), and an exact motion study of the
slider-crank: each run, the project saved and opened again, and then

- every study lists its result, not stale;
- each run again answers `reused: true`, with the input hash it ran with,
  and computes nothing -- check_topology answers at once, no job;
- an input moved after the reopen makes the result stale, and the next run
  computes.

The pan/tilt yoke's topology result read stale on every reopen and ran again
(~80 s): the run meshed the envelope in place, which changed flags the
B-Rep hash read, so the saved part hashed otherwise.

Usage: study_reopen_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "generators"))
sys.path.insert(0, HERE)
from lobole.client import Client  # noqa: E402
from lobole.sdk import connect  # noqa: E402
from slider_crank import build  # noqa: E402
from smoke_app import launch_app  # noqa: E402
import host_load  # noqa: E402

SKIP = 77


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def until_ended(d, timeout=240.0, **which):
    deadline = time.time() + timeout
    while time.time() < deadline:
        st = d.call("study_status", **which)
        if not st["running"]:
            return st
        time.sleep(0.1)
    raise AssertionError(f"{which} did not end in {timeout}s")


def face_at(faces, axis, value, sign):
    for f in faces:
        n = f.get("normal")
        c = f.get("centroid")
        if n and c and abs(c[axis] - value) < 1e-6 and n[axis] * sign > 0.5:
            return f["name"]
    raise AssertionError(f"no face at axis {axis} = {value}")


def studies(d):
    return {s["id"]: s for s in d.call("list_studies")["studies"]}


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_reopen_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        d.timeout = 300
        feats = d.call("ping").get("features", {})
        if not (feats.get("fem") and feats.get("mesher")):
            print("skip: built without FEM or its mesher")
            return SKIP
        a = connect(app.port)

        # ---- The mechanism and its exact motion study ----------------------
        rig, axle, _, _ = build(a, 20.0, 70.0)
        rig.activate()
        motion = d.call("add_motion_study", driver=axle, dof="rotation",
                        to=360, samples=13, clash_mode="exact",
                        name="turn", **{"from": 0})["id"]
        ran_motion = d.call("run_motion_study", study=motion)
        expect(ran_motion["reused"] is False, f"computed: {ran_motion}")

        # ---- A bar, and a study of each kind --------------------------------
        bar = a.part("bar")
        bar.activate()
        r = d.call("add_feature", feature={"type": "Box", "name": "Bar",
                                           "on": "Top", "centered": False,
                                           "width": 120, "depth": 30,
                                           "height": 30})
        d.call("set_material", feature_id=r["id"], material="steel")
        faces = d.call("list_elements")["bodies"][0]["faces"]
        root = face_at(faces, 0, 0.0, -1)
        tip = face_at(faces, 0, 120.0, 1)
        ids = {}
        for kind in ("static", "modal", "topology"):
            s = d.call("add_study", name=f"bar {kind}", type=kind)["id"]
            d.call("set_study", study=s, mesh_size=8.0)
            d.call("add_load", study=s, type="fixed", face=root)
            if kind != "modal":
                d.call("add_load", study=s, type="force", face=tip,
                       magnitude=500.0, direction=[0, 0, -1])
            ids[kind] = s
        d.call("set_study", study=ids["modal"], num_modes=3)
        d.call("set_study", study=ids["topology"], volume_fraction=0.4,
               max_iterations=4)
        hashes = {}
        for kind in ("static", "modal"):
            hashes[kind] = d.call("run_study", study=ids[kind])["input_hash"]
        started = d.call("run_study", study=ids["topology"])
        expect(started.get("started"), f"topology started: {started}")
        st = until_ended(d, study=ids["topology"])
        expect(st["has_results"] and not st.get("error"), f"ran: {st}")
        hashes["topology"] = d.call("study_results",
                                    study=ids["topology"])["input_hash"]
        chk = d.call("check_topology", study=ids["topology"])
        expect(chk.get("started"), f"the check is a job: {chk}")
        st = until_ended(d, job=chk["job"])
        expect(st["outcome"] == "done", f"the check ran: {st}")
        checked = d.call("check_topology", study=ids["topology"])
        expect(checked.get("reused"), f"answered from its results: {checked}")
        ids["shape"] = checked["shape_study"]
        ids["solid"] = checked["solid_study"]
        before = d.call("study_results", study=ids["static"])

        # ---- Saved, and opened again ----------------------------------------
        path = os.path.join(tmp, "reopen.lob")
        d.call("save_project", path=path)
        for kind in ("static", "topology"):
            expect(any(f.endswith(".result.json") and f.startswith(
                str(ids[kind])) for f in os.listdir(
                    os.path.join(path, "parts", "bar", "studies"))),
                f"the {kind} result is in the project")
        d.call("open_project", path=path)

        a = connect(app.port)
        a.part("bar", create=False).activate()
        listed = studies(d)
        for kind, sid in ids.items():
            expect(listed[sid]["has_results"] and not listed[sid]["stale"],
                   f"{kind} reopened current: {listed[sid]}")
        for kind in ("static", "modal", "topology"):
            with host_load.Timed(app.pid) as took:
                again = d.call("run_study", study=ids[kind])
            expect(again.get("reused") is True,
                   f"{kind} reused after the reopen: {again}")
            # Responsiveness: wall time is the point, judged on a quiet machine
            # (docs/ui/testing.md#tests-that-time-things).
            host_load.judge(took, took.seconds < 5.0,
                            f"{kind} reused after the reopen in "
                            f"{took.seconds:.2f}s", expect,
                            "study_reopen_smoke")
            expect(again["input_hash"] == hashes[kind],
                   f"{kind}: the inputs it was saved for")
        after = d.call("study_results", study=ids["static"])
        expect(after["max_displacement_mm"] == before["max_displacement_mm"],
               f"the static answer as saved: {after['max_displacement_mm']}")
        rechecked = d.call("check_topology", study=ids["topology"])
        expect(rechecked.get("reused") and "job" not in rechecked,
               f"the check answers from the saved results: {rechecked}")

        # ---- The motion study too -------------------------------------------
        a.assembly("slider_crank", create=False).activate()
        ms = d.call("motion_status", study=motion)
        expect(ms["has_result"] and not ms["stale"],
               f"the motion result reopened current: {ms.get('stale')}")
        with host_load.Timed(app.pid) as took:
            again = d.call("run_motion_study", study=motion)
        expect(again["reused"] is True,
               f"the motion study reused after the reopen: {again['reused']}")
        host_load.judge(took, took.seconds < 5.0,
                        f"the motion study reused after the reopen in "
                        f"{took.seconds:.2f}s", expect, "study_reopen_smoke")
        expect(again["input_hash"] == ran_motion["input_hash"],
               "the inputs it was saved for")

        # ---- An input moved: stale, and computed again ----------------------
        a.part("bar", create=False).activate()
        d.call("set_material", feature_id=r["id"], material="aluminum_6061_t6")
        expect(studies(d)[ids["static"]]["stale"],
               "a moved input makes the reopened result stale")
        moved = d.call("run_study", study=ids["static"])
        expect(moved["reused"] is False and
               moved["input_hash"] != hashes["static"],
               f"and the next run computes: {moved}")
        print("study_reopen_smoke: ok")
        return 0
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except Exception:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
