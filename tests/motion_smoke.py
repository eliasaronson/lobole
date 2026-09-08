#!/usr/bin/env python3
"""Motion studies over the RPC, headless (docs/agent-scale-plan.md, G2).

A carrier, an arm on a revolute axle, and a stop fixed on the arm's circle:
the study turns the arm through 360 degrees and the collision table must
name the stop at 90 degrees and nowhere else. Then: the same run as a
background job, a scrub that moves the arm and pushes no undo step, the
animated GLB, the definition surviving a save/reopen, and every refusal
naming what exists.

Usage: motion_smoke.py <path-to-designer-binary>
"""
import json
import math
import os
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def refused(d, method, needles, **kw):
    try:
        d.call(method, **kw)
    except RuntimeError as e:
        for n in needles:
            expect(n in str(e), f"{method} refused, but not naming '{n}': {e}")
        return str(e)
    raise AssertionError(f"{method} was accepted: {kw}")


def box(d, name, lo, size):
    d.call("add_part", name=name)
    d.call("add_feature", feature={
        "type": "Box", "name": "Block", "position": list(lo),
        "width": size[0], "height": size[1], "depth": size[2]})


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_motion_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        expect(d.call("ping")["ok"], "ping")
        d.call("new_project")
        # The rig: an arm 28 long reaching out along +X from the axle, and a
        # 4 mm stop on a 20 mm radius at 90 degrees.
        box(d, "arm", (2, -2, -2), (28, 4, 4))
        box(d, "stop", (-2, -2, -2), (4, 4, 4))
        d.call("add_part", name="carrier")
        d.call("add_assembly", name="rig")
        carrier = d.call("add_occurrence", part="carrier", fixed=True)["id"]
        arm = d.call("add_occurrence", part="arm")["id"]
        stop = d.call("add_occurrence", part="stop", fixed=True, transform={
            "position": [0.0, 20.0, 0.0], "orientation": [0, 0, 0, 1]})["id"]
        axle = d.call("add_mate", type="revolute", name="axle",
                      a={"occurrence": carrier, "origin": True},
                      b={"occurrence": arm, "origin": True},
                      flipped=False)["id"]

        # ---- Define ------------------------------------------------------
        study = d.call("add_motion_study", driver=axle, **{"from": 0},
                       to=360, samples=25, name="turn", clash_mode="exact")
        expect(study["driver_dof"] == "rotation", f"a revolute turns: {study}")
        expect(study["unit"] == "deg" and abs(study["to"] - 360) < 1e-9,
               f"the surface speaks degrees: {study}")
        expect("why_not" not in study, f"it can run: {study}")
        sid = study["id"]
        listed = d.call("list_motion_studies")["studies"]
        expect(len(listed) == 1 and not listed[0]["has_result"],
               f"listed, not yet run: {listed}")

        # ---- Run, synchronously: the collision table --------------------
        t0 = time.time()
        r = d.call("run_motion_study", study=sid)
        print(f"  25 samples, sync: {time.time() - t0:.2f}s, "
              f"{r['pairs_checked']} pair checks")
        expect(len(r["samples"]) == 25, f"25 samples: {len(r['samples'])}")
        expect(all(s["solved"] for s in r["samples"]), "every sample solved")
        warm = d.call("run_motion_study", study=sid)
        expect(warm["pairs_cached"] == warm["pairs_checked"], "repeat reuses exact checks")
        expect(warm["pairs_executed"] == 0, "repeat executes no booleans")
        expect(warm["samples"] == r["samples"], "cached results preserve collision locations")
        expect(not d.call("motion_status", study=sid)["stale"], "fresh result")
        # A geometry edit below the assembly must invalidate the verdict.
        d.call("set_active_part", part="stop")
        d.call("set_variable", name="unrelated", expression="1", scope="part")
        d.call("set_active_part", part="rig")
        status = d.call("motion_status", study=sid)
        # Tab switching can discard results altogether; retained results must be stale.
        if status["has_result"]:
            expect(status["stale"] and not status["result"]["clean"], "stale cannot be green")
        d.call("set_motion_study", study=sid, clash_mode="mesh")
        r = d.call("run_motion_study", study=sid)
        hits = [s["index"] for s in r["samples"] if s["clashes"]]
        expect(6 in hits, f"the stop at 90 degrees is hit: {hits}")
        expect(all(4 <= i <= 8 for i in hits),
               f"and nothing on the far side of the circle: {hits}")
        expect(not r["clean"] and r["worst_sample"] == 6,
               f"not clean, worst at 90: {r['clean']} {r['worst_sample']}")
        c = r["samples"][6]["clashes"][0]
        expect({c["a_name"], c["b_name"]} == {"arm", "stop"},
               f"the pair is named: {c}")
        expect(abs(r["samples"][6]["value"] - 90.0) < 1e-6,
               f"values in degrees: {r['samples'][6]['value']}")
        expect("90.00" in r["table"] and "NOT CLEAN" in r["table"],
               f"the table is printable: {r['table'][:200]}")
        expect(r.get("deflection_mm", 0) > 0, f"mesh mode reports how close: {r}")
        # The document was not left at the last sample.
        mate = next(m for m in d.call("list_mates")["mates"] if m["id"] == axle)
        expect(abs(mate["rotation"]) < 1e-9,
               f"the run drove a COPY; the mate is where it was: {mate}")

        # ---- The same run as a job --------------------------------------
        r2 = d.call("run_motion_study", study=sid, **{"async": True})
        expect(r2["started"] and r2["job"] == "motion", f"started: {r2}")
        for _ in range(300):
            st = d.call("motion_status", study=sid)
            # The prior synchronous result remains available while rerunning.
            # A result alone does not mean this background job has finished.
            if st["has_result"] and not st["running"]:
                break
            time.sleep(0.05)
        expect(st["has_result"] and not st["running"], "the job landed")
        expect(st["result"]["worst_sample"] == 6 and not st["result"]["clean"],
               f"the same table from the worker: {st['result']['worst_sample']}")
        jobs = d.call("background_jobs")["jobs"]
        expect(not any(j["kind"] == "motion" for j in jobs),
               f"and it is off the registry: {jobs}")

        # ---- The scrub moves the arm and pushes no undo step ------------
        before = len(d.call("list_mates")["mates"])
        p = d.call("set_motion_pose", study=sid, t=0.25)
        expect(abs(p["value"] - 90.0) < 1e-9 and p["unit"] == "deg",
               f"t=0.25 of a full turn is 90 degrees: {p}")
        mate = next(m for m in d.call("list_mates")["mates"] if m["id"] == axle)
        expect(abs(mate["rotation"] - math.pi / 2) < 1e-6,
               f"the mate was driven: {mate['rotation']}")
        fr = d.call("frame", occurrence=arm)
        expect(abs(fr["x"][1] - 1.0) < 1e-4,
               f"and the arm's X now points along +Y: {fr['x']}")
        # First undo the explicit exact-to-mesh change above.
        d.call("undo")
        # Undo takes back the ADD of the study (the snapshot before it), not
        # the scrub — a scrub is a look, not an edit. So after one undo the
        # study is gone and the pose stays.
        d.call("undo")
        expect(d.call("list_motion_studies")["studies"] == [],
               "one undo removed the study, since the scrub was not a step")
        d.call("redo")
        d.call("redo")  # restore the mesh setting
        expect(len(d.call("list_motion_studies")["studies"]) == 1, "redo")
        expect(len(d.call("list_mates")["mates"]) == before, "mates intact")
        d.call("set_motion_pose", study=sid, t=0.0)

        # ---- Export: the run's poses, as a glTF animation ---------------
        glb = os.path.join(tmp, "turn.glb")
        # A study that has never run has no poses to export — the export is
        # the run, not a guess at it.
        fresh = d.call("add_motion_study", driver=axle, **{"from": 0},
                       to=180, samples=5, name="unrun")["id"]
        refused(d, "export_motion", ["has not been run"], study=fresh,
                path=glb)
        d.call("remove_motion_study", study=fresh)
        d.call("run_motion_study", study=sid)
        ex = d.call("export_motion", study=sid, path=glb)
        expect(ex["frames"] == 25 and ex["nodes"] >= 2,
               f"25 keyframes, the arm and the stop: {ex}")
        with open(glb, "rb") as f:
            magic, version, total = struct.unpack("<III", f.read(12))
            jlen, jtype = struct.unpack("<II", f.read(8))
            doc = json.loads(f.read(jlen))
        expect(magic == 0x46546C67 and version == 2, "a GLB")
        expect(total == os.path.getsize(glb), "whole-file length recorded")
        anim = doc["animations"][0]
        expect(len(anim["channels"]) == 2 * ex["nodes"],
               f"translation + rotation per node: {len(anim['channels'])}")
        names = {n["name"] for n in doc["nodes"]}
        expect("arm" in names and "stop" in names, f"nodes named: {names}")

        # ---- Refusals name what exists ----------------------------------
        refused(d, "add_motion_study", ["no mate with id", "axle"],
                driver=99999, **{"from": 0}, to=90)
        refused(d, "add_motion_study", ["equal", "nothing would move"],
                driver=axle, **{"from": 30}, to=30)
        refused(d, "add_motion_study", ["clash_mode", "mesh, exact"],
                driver=axle, **{"from": 0}, to=90, clash_mode="fast")
        refused(d, "motion_status", ["no motion study", "turn"], study=4242)
        refused(d, "set_motion_pose", ["0..1"], study=sid, t=1.5)
        # A fastened mate has nothing to drive.
        fixed = d.call("add_mate", type="fastened", name="weld",
                       a={"occurrence": carrier, "origin": True},
                       b={"occurrence": stop, "origin": True})["id"]
        refused(d, "add_motion_study", ["no degree of freedom", "revolute"],
                driver=fixed, **{"from": 0}, to=1)
        d.call("remove_mate", id=fixed)
        # Editing the definition drops the stale result.
        d.call("set_motion_study", study=sid, samples=13)
        st = d.call("motion_status", study=sid)
        expect(not st["has_result"],
               "a result described the old definition and is gone")

        # ---- Round trip: the definition is in the file -------------------
        path = os.path.join(tmp, "rig.dsgn")
        d.call("save_project", path=path)
        d.call("new_project")
        d.call("open_project", path=path)
        d.call("set_active_part", part="rig")
        studies = d.call("list_motion_studies")["studies"]
        expect(len(studies) == 1 and studies[0]["samples"] == 13
               and studies[0]["driver_mate"] == axle,
               f"the study reloads: {studies}")
        r = d.call("run_motion_study", study=studies[0]["id"])
        expect(not r["clean"], "and still finds the stop after a reopen")
        d.call("set_motion_study", study=studies[0]["id"], check_clash=False)
        preview = d.call("run_motion_study", study=studies[0]["id"])
        expect(preview["collision_status"] == "not_checked" and not preview["clean"],
               "kinematics preview must not claim collision clearance")
        expect(not d.call("list_motion_studies")["studies"][0]["clean"],
               "study list must not turn unchecked motion green")
        print("motion_smoke: ok")
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
