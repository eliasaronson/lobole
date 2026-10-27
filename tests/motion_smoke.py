#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Motion studies over the RPC, headless (docs/assemblies/README.md#motion-studies).

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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
import host_load  # noqa: E402
import smoke_shapes as shapes  # noqa: E402


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
    shapes.box(d, lo, size, "Block")


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
        d = Client.wait_for(app.port)
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
        # Unchanged inputs: the stored run answers, nothing is computed.
        with host_load.Timed(app.pid) as took:
            same = d.call("run_motion_study", study=sid)
        expect(same["reused"] is True,
               f"an unchanged study is reused: {same.get('reused')}")
        # Responsiveness: wall time is the point, judged on a quiet machine
        # (docs/ui/testing.md#tests-that-time-things).
        host_load.judge(took, took.seconds < 1.0,
                        f"a reuse is well under a second: "
                        f"{took.seconds:.2f}s", expect, "motion_smoke")
        expect(same["samples"] == r["samples"] and
               same["input_hash"] == r["input_hash"], "the same run")
        # recompute runs it again, and the exact checks come from the cache.
        warm = d.call("run_motion_study", study=sid, recompute=True)
        expect(warm["reused"] is False, "recompute runs")
        expect(warm["pairs_cached"] + warm["pairs_repeated"] ==
               warm["pairs_checked"], "repeat reuses exact checks")
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

        # ---- Branch watching (docs/assemblies/README.md#branch-jumps) ------
        # A plain revolute turns at the driver's own rate: nothing refined,
        # nothing jumped, and max_refine stated, refused or listed.
        expect(r["refined_samples"] == 0 and r["branch_jumps"] == 0 and
               all("branch_jump" not in s for s in r["samples"]),
               f"an arm on an axle stays on its one branch: "
               f"{r['refined_samples']} {r['branch_jumps']}")
        # (Refused, so no undo step: the undo checks below count them.)
        refused(d, "set_motion_study", ["from 0 to 12"], study=sid,
                max_refine=13)
        refused(d, "set_motion_study", ["from 0 to 12"], study=sid,
                max_refine=1.5)
        listed = d.call("list_motion_studies")["studies"]
        expect(listed[0]["max_refine"] == 6,
               f"the default, down to a 64th of a step: {listed[0]}")

        # ---- Trace a point (docs/assemblies/README.md#tracing-a-point) ----
        # The arm's far end, (30, 0, 0) in its own part, sweeps a circle of
        # 30 about the axle; the carrier's point stands still.
        r = d.call("run_motion_study", study=sid, trace=[
            {"name": "tip", "occurrence": arm, "point_mm": [30, 0, 0]},
            {"name": "still", "occurrence": carrier, "point_mm": [1, 2, 3]}])
        tip, still = r["trace"]
        expect(tip["name"] == "tip" and tip["occurrence"] == arm,
               f"keyed by name and occurrence: {tip.keys()}")
        expect(len(tip["points_mm"]) == 25 and tip["unsolved_samples"] == 0,
               f"a point per sample: {len(tip['points_mm'])}")
        for smp, pt in zip(r["samples"], tip["points_mm"]):
            a = math.radians(smp["value"])
            expect(abs(pt[0] - 30 * math.cos(a)) < 1e-6
                   and abs(pt[1] - 30 * math.sin(a)) < 1e-6
                   and abs(pt[2]) < 1e-9,
                   f"the tip on its circle at {smp['value']}: {pt}")
        expect(abs(tip["extent_mm"][0] - 60) < 1e-6
               and abs(tip["min_mm"][1] + 30) < 1e-6,
               f"the extent of a full circle: {tip}")
        expect(still["extent_mm"] == [0, 0, 0]
               and still["points_mm"][7] == [1, 2, 3],
               f"a fixed occurrence's point stands still: {still}")
        # Asked of the stored result, the same paths.
        st = d.call("motion_status", study=sid, trace=[
            {"name": "tip", "occurrence": arm, "point_mm": [30, 0, 0]}])
        expect(st["result"]["trace"][0]["points_mm"] == tip["points_mm"],
               "motion_status traces the stored run")
        # Refusals name what is accepted and change nothing: the mate stays
        # where it was although each asks to leave the run at its end.
        for bad, needle in (
                ([{"name": "x", "occurrence": 9999, "point_mm": [0, 0, 0]}],
                 "no occurrence 9999"),
                ([{"name": "x", "occurrence": arm, "point_mm": [0, 0, 0]},
                  {"name": "x", "occurrence": arm, "point_mm": [1, 0, 0]}],
                 "already traced"),
                ([{"name": "x", "occurrence": arm, "point": [0, 0, 0]}],
                 "accepted: name, occurrence, path, point_mm"),
                ([{"name": "x", "occurrence": arm}], "point_mm"),
                ([{"name": "x", "path": [arm, 3], "point_mm": [0, 0, 0]}],
                 "not a FLEXIBLE")):
            refused(d, "run_motion_study", [needle], study=sid, trace=bad,
                    leave_at="last")
            mate = next(m for m in d.call("list_mates")["mates"]
                        if m["id"] == axle)
            expect(abs(mate["rotation"]) < 1e-9,
                   f"a refused trace left the arm where it was: {mate}")
        refused(d, "run_motion_study", ["async"], study=sid, trace=[
            {"name": "tip", "occurrence": arm, "point_mm": [30, 0, 0]}],
            **{"async": True})

        # ---- The same run as a job --------------------------------------
        # Unchanged inputs answer from the stored run even asked async;
        # recompute runs it as a job, with an id.
        same = d.call("run_motion_study", study=sid, **{"async": True})
        expect(same["reused"] and not same["started"], f"reused: {same}")
        r2 = d.call("run_motion_study", study=sid, recompute=True,
                    **{"async": True})
        expect(r2["started"] and r2["kind"] == "motion"
               and isinstance(r2["job"], int), f"started: {r2}")
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
        # The mesh check's deflection: what it is asked for is what it
        # meshes at and reports, and it means nothing to an exact check.
        fine = d.call("add_motion_study", driver=axle, **{"from": 0}, to=360,
                      samples=13, name="fine", clash_deflection_mm=0.05)
        expect(abs(fine["clash_deflection_mm"] - 0.05) < 1e-12,
               f"the study keeps its deflection: {fine}")
        ran = d.call("run_motion_study", study=fine["id"])
        expect(abs(ran["deflection_mm"] - 0.05) < 1e-12 and
               not ran["clean"],
               f"meshed at 0.05 mm, and the stop still found: "
               f"{ran.get('deflection_mm')}")
        refused(d, "add_motion_study", ["clash_deflection_mm", "mesh"],
                driver=axle, **{"from": 0}, to=90, clash_mode="exact",
                clash_deflection_mm=0.05)
        refused(d, "add_motion_study", ["positive"], driver=axle,
                **{"from": 0}, to=90, clash_deflection_mm=-1)
        # A refused edit changes nothing; null derives it per body again.
        refused(d, "set_motion_study", ["clash_deflection_mm", "null"],
                study=fine["id"], clash_mode="exact", samples=7)
        kept = next(s for s in d.call("list_motion_studies")["studies"]
                    if s["id"] == fine["id"])
        expect(kept["samples"] == 13 and kept["clash_mode"] == "mesh",
               f"the refused edit changed nothing: {kept}")
        cleared = d.call("set_motion_study", study=fine["id"],
                         clash_mode="exact", clash_deflection_mm=None)
        expect("clash_deflection_mm" not in cleared and
               cleared["clash_mode"] == "exact", f"cleared: {cleared}")
        d.call("remove_motion_study", study=fine["id"])
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
        path = os.path.join(tmp, "rig.lob")
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
