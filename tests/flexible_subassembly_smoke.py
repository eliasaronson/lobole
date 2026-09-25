#!/usr/bin/env python3
"""Flexible sub-assemblies over RPC, through the SDK
(docs/assemblies/README.md#flexible-sub-assemblies).

A crank on a revolute inside its own assembly, placed FLEXIBLE in a parent
whose drive is chained to the crank's inner mate by path: turning the drive
turns the crank; set_mate by path poses it and drives the parent back; a
study driven by path records the crank's pose by path; the GLB export gives
the moving leaf a node of its own; and what must be refused is, naming why.

Usage: flexible_subassembly_smoke.py <path-to-designer-binary>
"""
import math
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
import designer_sdk as sdk  # noqa: E402
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)
    print(f"  ok: {message}", flush=True)


def refused(fn, needle, what):
    try:
        fn()
    except Exception as e:  # noqa: BLE001 - the refusal text is the test
        expect(needle in str(e), f"{what} refused naming '{needle}'")
        return
    raise AssertionError(f"{what} was accepted")


def crank_tip(app, top, unit, crank):
    top.activate()
    f = app.frame(occurrence=f"{unit.id}/{crank.id}")
    x = f["x"]
    return math.degrees(math.atan2(x[1], x[0]))


def main():
    binary = sys.argv[1]
    tmp = tempfile.mkdtemp(prefix="designer_flexible_")
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    proc = launch_app(binary, env=env)
    try:
        app = sdk.App(Designer.wait_for(proc.port))
        app.new()
        housing = app.part("housing")
        housing.box(position=(-40, -5, -5), w=8, h=10, d=10)
        crank = app.part("crank")
        crank.box(position=(2, -2, -2), w=28, h=4, d=4)
        frame = app.part("frame")
        frame.box(position=(-5, -5, -30), w=10, h=10, d=10)

        sub = app.assembly("crank unit")
        ho = sub.place(housing, fixed=True, name="housing")
        co = sub.place(crank, name="crank")
        crank_mate = sub.revolute(ho, co, name="crank")
        held = app.call("add_configuration", name="held")["id"]
        app.call("set_configuration", id=held, mate_overrides=[
            {"mate": crank_mate["id"], "rotation_deg": 30.0}])

        top = app.assembly("machine")
        fo = top.place(frame, fixed=True, name="frame")
        do = top.place(crank, name="drive")
        drive = top.revolute(fo, do, name="drive")
        unit = top.place(sub, at=(100, 0, 0), fixed=True, flexible=True,
                         name="unit")
        chain = top.chain(drive, unit.mate(crank_mate), 1, 1,
                          name="drive to crank")
        expect(chain.get("mate_b_path") == [unit.id],
               "the relation names the inner mate by path")

        app.set_mate(id=drive["id"], rotation_deg=90.0)
        expect(abs(crank_tip(app, top, unit, co) - 90.0) < 1e-3,
               "turning the parent's drive turns the unit's crank")
        occ = next(o for o in app.list_occurrences()["occurrences"]
                   if o["id"] == unit.id)
        expect(occ.get("flexible") and occ["inner_poses"],
               "the pose lives on the parent's occurrence")
        solved = top.solve()
        rows = solved.get("flexible", [])
        expect(rows and rows[0]["occurrence"] == unit.id and
               any(m["id"] == crank_mate["id"] and m["path"] == [unit.id]
                   for m in rows[0]["mates"]),
               "solve_assembly reports the inner mates by path")

        # Posed by path: the crank goes to 45, and the chain drives back.
        r = app.set_mate(id=crank_mate["id"], path=[unit.id],
                         rotation_deg=45.0)
        expect(abs(r["rotation_deg"] - 45.0) < 1e-3, "set_mate poses by path")
        drive_now = next(m for m in app.list_mates()["mates"]
                         if m["id"] == drive["id"])
        expect(abs(drive_now["rotation_deg"] - 45.0) < 1e-3,
               "an inner mate posed by path drives the parent back")
        sub.activate()
        own = next(m for m in app.list_mates()["mates"]
                   if m["id"] == crank_mate["id"])
        expect(abs(own["rotation_deg"]) < 1e-6,
               "the sub-assembly's own document is not written")

        # A study driven by the inner mate, poses by path.
        study = top.motion(unit.mate(crank_mate), dof="rotation",
                           **{"from": 0.0}, to=180.0, samples=5,
                           check_clash=True, clash_mode="mesh")
        expect(study.get("driver_path") == [unit.id],
               "a study's driver is named by path")
        res = app.run_motion_study(study=study["id"], include_poses=True)
        last = res["samples"][-1]
        expect(any(p.get("path") == [unit.id, co.id] for p in last["poses"]),
               "every sample poses the moving leaf by path")
        expect(any(m.get("path") == [unit.id] and
                   abs(m["rotation_deg"] - 180.0) < 1e-3
                   for m in last["mates"]),
               "and its mate by path")
        ex = app.export_motion(study=study["id"],
                               path=os.path.join(tmp, "unit.glb"))
        expect(ex["nodes"] == 4,
               "the GLB has a node for the moving leaf (frame, drive, unit, "
               "crank)")

        # Its own configuration holds the crank.
        top.activate()
        app.set_occurrence_configuration(occurrence=unit.id,
                                         configuration="held")
        expect(abs(crank_tip(app, top, unit, co) - 30.0) < 1e-3,
               "the placement runs under its own configuration")
        refused(lambda: app.set_mate(id=crank_mate["id"], path=[unit.id],
                                     rotation_deg=10.0),
                "held", "posing a mate the placement's configuration holds")
        mine = app.call("add_configuration", name="parent")["id"]
        refused(lambda: app.call("set_configuration", id=mine, mate_overrides=[
            {"mate": crank_mate["id"], "path": [unit.id], "rotation_deg": 5}]),
            "OWN configuration", "a parent configuration holding an inner mate")
        refused(lambda: top.freewheel(drive, unit.mate(crank_mate), "positive"),
                "freewheel", "a freewheel into a flexible placement")
        refused(lambda: app.set_occurrence_flexible(occurrence=fo.id,
                                                    flexible=True),
                "places a part", "a part made flexible")
        refused(lambda: app.set_mate(id=crank_mate["id"], path=[unit.id],
                                     flipped=False),
                "sub-assembly's to edit", "editing an inner mate's definition")
        # Rigid again: the relation says so rather than driving nothing.
        app.set_occurrence_flexible(occurrence=unit.id, flexible=False)
        rel = next(x for x in app.list_relations()["relations"]
                   if x["id"] == chain["id"])
        expect("not flexible" in rel.get("error", ""),
               "a relation into a rigid placement reports it")
        print("flexible sub-assembly smoke: passed")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except Exception:  # noqa: BLE001
            proc.kill()


if __name__ == "__main__":
    main()
