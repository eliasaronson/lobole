#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Mate limits over the RPC, headless (docs/assemblies/README.md#mate-limits):
a door on a hinge that swings -5..95 degrees; set_mate refusing past it and
refusing a range that leaves the door outside; a motion study refused across
the limit and one stopped by a follower's; a sequence pose refused outside
it; and motion_clean's whole_range passing over the stated swing and failing
short of it.
"""
import math
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def refused(d, method, needles, **kw):
    try:
        d.call(method, **kw)
    except RuntimeError as e:
        for needle in needles:
            expect(needle in str(e), f"{method} refused, but not naming "
                   f"'{needle}': {e}")
        return str(e)
    raise AssertionError(f"{method} was accepted: {kw}")


def origin_mate(d, kind, a, b, **kw):
    return d.call("add_mate", type=kind,
                  a={"occurrence": a, "origin": True},
                  b={"occurrence": b, "origin": True},
                  flipped=False, **kw)


def mate(d, mid):
    return next(m for m in d.call("list_mates")["mates"] if m["id"] == mid)


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="lobole_limits_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        d.call("new_project")
        d.call("add_part", name="frame")
        box(d, (-10, -10, -40), (20, 20, 10), "Post")  # under the swing
        d.call("add_part", name="door")
        box(d, (20, -2, 0), (30, 4, 10), "Leaf")
        d.call("add_assembly", name="hinged")
        frame = d.call("add_occurrence", part="frame", fixed=True)["id"]
        door = d.call("add_occurrence", part="door")["id"]

        # A range on what the type does not free is refused at add_mate.
        refused(d, "add_mate", ["revolute", "no offset range"],
                type="revolute", a={"occurrence": frame, "origin": True},
                b={"occurrence": door, "origin": True}, flipped=False,
                offset_range_mm=[0, 5])
        refused(d, "add_mate", ["above its max"], type="revolute",
                a={"occurrence": frame, "origin": True},
                b={"occurrence": door, "origin": True}, flipped=False,
                rotation_range_deg=[95, -5])
        hinge = origin_mate(d, "revolute", frame, door, name="hinge",
                            rotation_range_deg=[-5, 95])
        expect(hinge["limits"]["rotation_range_deg"] == [-5, 95]
               or all(abs(a - b) < 1e-9 for a, b in
                      zip(hinge["limits"]["rotation_range_deg"], [-5, 95])),
               f"the range answers in degrees: {hinge}")
        h = hinge["id"]
        expect("limits" in mate(d, h), "list_mates shows the range")

        # Inside: accepted. Past: refused naming the range, nothing moved.
        d.call("set_mate", id=h, rotation_deg=60)
        refused(d, "set_mate", ["outside its range", "95"], id=h,
                rotation_deg=100)
        expect(abs(mate(d, h)["rotation_deg"] - 60) < 1e-9,
               "a refused set_mate changed nothing")
        # A range that leaves the door outside, unless the call moves it.
        refused(d, "set_mate", ["outside its range", "60"], id=h,
                rotation_range_deg=[0, 30])
        d.call("set_mate", id=h, rotation_range_deg=[0, 30], rotation_deg=15)
        d.call("set_mate", id=h, rotation_range_deg=[-5, 95], rotation_deg=0)
        # A lock: min equal to max.
        d.call("set_mate", id=h, rotation_range_deg=[0, 0])
        d.call("set_mate", id=h, rotation_range_deg=[-5, 95])

        # A study across the limit is refused before it runs.
        bad = d.call("add_motion_study", driver=h, dof="rotation", to=120,
                     samples=13, check_clash=False, name="too far",
                     **{"from": 0})
        expect("limit" in bad.get("why_not", ""), f"why_not names it: {bad}")
        refused(d, "run_motion_study", ["past the driver's limit"],
                study=bad["id"])

        # A follower's limit stops a run: a second leaf geared to the hinge,
        # limited to 20 degrees either way, turns a third as far, backwards.
        flap = d.call("add_occurrence", part="door",
                      transform={"position": [0, 0, 30]})["id"]
        f = origin_mate(d, "revolute", frame, flap, name="flap",
                        offset=30.0, rotation_range_deg=[-20, 20])["id"]
        gear = d.call("add_relation", type="gear", a=h, b=f, teeth_a=12,
                      teeth_b=36)["id"]
        swing = d.call("add_motion_study", driver=h, dof="rotation", to=90,
                       samples=10, check_clash=False, name="swing",
                       **{"from": 0})
        expect("why_not" not in swing, f"inside the limit it can run: "
               f"{swing}")
        run = d.call("run_motion_study", study=swing["id"])
        stop = run.get("stopped_by")
        expect(stop is not None and stop["reason"] == "limit"
               and stop["mate"] == f and stop["name"] == "flap",
               f"the flap stopped the run: {stop}")
        expect(abs(stop["limit_deg"] + 20) < 1e-6, f"at -20: {stop}")
        expect(stop["at_deg"] > 60 and not run["clean"],
               f"stopped past 60 degrees of the hinge: {stop}")
        expect(len(run["samples"]) < 10, "the run ended at the stop")

        # set_mate whose relation would drive the flap past its limit is
        # refused naming it.
        refused(d, "set_mate", ["flap", "outside its range"], id=h,
                rotation_deg=90)
        d.call("remove_relation", id=gear)
        d.call("remove_mate", id=f)
        d.call("remove_occurrence", id=flap)

        # A sequence pose outside the limit is refused.
        refused(d, "set_assembly_sequence", ["outside its range"],
                steps=[{"pose": [{"mate": h, "rotation_deg": 100}]}])
        d.call("set_assembly_sequence",
               steps=[{"pose": [{"mate": h, "rotation_deg": 45}]}])

        # The swing the design states is reachable without a clash: a study
        # over exactly the range, and motion_clean with whole_range.
        asm = next(p["id"] for p in d.call("list_parts")["parts"]
                   if p["name"] == "hinged")
        full = d.call("add_motion_study", driver=h, dof="rotation", to=95,
                      samples=21, name="full swing", **{"from": -5})["id"]
        short = d.call("add_motion_study", driver=h, dof="rotation", to=80,
                       samples=17, name="short swing", **{"from": 0})["id"]
        d.call("add_requirement", name="swings its range",
               measure={"kind": "motion_clean", "document": asm,
                        "study": full, "whole_range": True},
               bound={"kind": "pass"})
        d.call("add_requirement", name="short of its range",
               measure={"kind": "motion_clean", "document": asm,
                        "study": short, "whole_range": True},
               bound={"kind": "pass"})
        refused(d, "add_requirement", ["whole_range"], name="typo",
                measure={"kind": "motion_clean", "document": asm,
                         "study": full, "whole_range": "yes"},
                bound={"kind": "pass"})
        d.call("check_requirements")
        got = {x["name"]: x
               for x in d.call("list_requirements")["requirements"]}
        expect(got["swings its range"]["state"] == "pass",
               f"the full swing passes: {got['swings its range']}")
        expect(got["short of its range"]["state"] == "fail",
               f"a study short of the range fails: "
               f"{got['short of its range']}")
        findings = got["short of its range"].get("detail", {}).get(
            "findings", [])
        expect(any("whole range" in x for x in findings),
               f"and says why: {findings}")
        d.close()
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()
    print("mate_limits_smoke: limits refuse, stop and are proved")
    return 0


if __name__ == "__main__":
    sys.exit(main())
