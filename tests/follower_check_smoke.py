#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A cam cut by CamProfile, a pin that falls onto it, and check_followers
over a motion study, headless
(docs/assemblies/relations.md#the-follower-check): the pin stands on the
cam at every sample, the realised motion is the one the cam was cut for,
gravity's liftoff speed and the drop's fall are reported, the refusals say
what is missing, and a `follower` requirement holds the liftoff speed.
"""
import math
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import cylinder  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def refused(d, method, needle, **kw):
    try:
        d.call(method, **kw)
    except RuntimeError as e:
        expect(needle in str(e), f"{method} refused, but not naming "
               f"'{needle}': {e}")
        return
    raise AssertionError(f"{method} was accepted: {kw}")


def origin_mate(d, kind, a, b, **kw):
    return d.call("add_mate", type=kind,
                  a={"occurrence": a, "origin": True},
                  b={"occurrence": b, "origin": True},
                  flipped=False, **kw)["id"]


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="lobole_follower_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        d.call("new_project")
        d.call("add_part", name="frame")
        cylinder(d, (0, 0, -80), "z", 2, 2, "Peg")
        # A snail cam on a hub: a 10 mm cycloidal rise over 270 degrees,
        # the drop, a dwell. Cut for a pin of 1.5 mm.
        d.call("add_part", name="cam")
        hub = cylinder(d, (0, 0, 0), "z", 10, 6, "Hub")["id"]
        faces = d.call("list_elements")["bodies"][0]["faces"]
        side = next(f["name"] for f in faces if f["kind"] == "cylinder")
        refused(d, "add_feature", "accepted: cycloidal", feature={
            "type": "CamProfile", "name": "Bad",
            "face": {"feature_id": hub, "element": side},
            "segments": [{"to_deg": 360, "rise_mm": 0, "law": "linear"}]})
        spec = {
            "type": "CamProfile", "name": "Snail",
            "face": {"feature_id": hub, "element": side},
            "base_radius_mm": 25.0, "body": "add", "turn": "ccw",
            "segments": [
                {"to_deg": 270, "rise_mm": 10.0, "law": "cycloidal"},
                {"to_deg": 270, "rise_mm": -10.0, "law": "drop"},
                {"to_deg": 360, "law": "dwell"}],
            "follower": {"kind": "knife", "motion": "slide",
                         "radius_mm": 1.5}}
        cam = d.call("add_feature", feature=spec)
        rep = cam["report"]
        expect(rep["drops"][0]["height_mm"] == 10.0 and
               rep["fit_deviation_mm"] < 1e-3, f"the snail's report: {rep}")
        d.call("set_material", feature_id=hub, material="pla")
        # The pin: its axis along the part's Y, which is the cam's axis
        # once the axle is turned horizontal.
        d.call("add_part", name="pin")
        pin = cylinder(d, (0, -8, 1.5), "y", 1.5, 16, "Pin")["id"]
        d.call("set_material", feature_id=pin, material="pla")

        d.call("add_assembly", name="rig")
        frame = d.call("add_occurrence", part="frame", fixed=True,
                       transform={"rotate": {"axis": [1, 0, 0],
                                             "angle_deg": 90}})["id"]
        guide = d.call("add_occurrence", part="frame", fixed=True)["id"]
        camo = d.call("add_occurrence", part="cam")["id"]
        pino = d.call("add_occurrence", part="pin")["id"]
        axle = origin_mate(d, "revolute", frame, camo, name="axle")
        slide = origin_mate(d, "slider", guide, pino, name="slide")
        rel = d.call("add_relation", type="contact", a=axle, b=slide)
        expect("error" not in rel and rel["toward"] == "negative",
               f"the pin falls onto the cam: {rel}")
        study = d.call("add_motion_study", driver=axle, samples=48,
                       check_clash=False,
                       **{"from": 0.0, "to": 360.0})["id"]

        refused(d, "check_followers", "run_motion_study first", study=study)
        d.call("run_motion_study", study=study)
        # Cut to turn counter-clockwise about its own axis, the cam turns
        # the other way here (the axle's Z points along world -Y): the pin
        # meets the snail's step from below. A jam, not a drop.
        wrong = d.call("check_followers", study=study, speed_rpm=60.0)
        drop = wrong["relations"][0]["gravity"]["drops"][0]
        expect(drop["jams"] and
               wrong["relations"][0]["holds_at_speed"] is False,
               f"turned the wrong way, the step jams: {drop}")
        d.call("set_active_part", part="cam")
        spec["turn"] = "cw"
        d.call("modify_feature", feature_id=cam["id"], feature=spec)
        d.call("set_active_part", part="rig")
        refused(d, "check_followers", "stale", study=study)
        d.call("run_motion_study", study=study)
        out = d.call("check_followers", study=study, speed_rpm=60.0,
                     max_pressure_angle_deg=30.0)
        r = out["relations"][0]
        expect(r["touching"], f"the pin stands on the cam: {r['failures']}")
        expect(r["deviation_mm"] <= r["fit_deviation_mm"] + 2e-3,
               f"the motion it was cut for: {r['deviation_mm']}")
        # The pin's centre stands base + tip radius from the axis at the
        # dwell, and its bottom 1.5 below that.
        expect(abs(r["mate_zero_mm"] - 25.0) < 1e-2,
               f"mate zero {r['mate_zero_mm']}")
        g = r["gravity"]
        expect(g["held"] and g["liftoff_rpm"] is not None,
               f"gravity holds it, up to a speed: {g}")
        # A 10 mm drop: sqrt(2 h / g) is 45 ms.
        drop = g["drops"][0]
        expect(abs(drop["fall_s"] - math.sqrt(2 * drop["height_mm"] * 1e-3 /
                                              9.81)) < 1e-9,
               f"the fall: {drop}")
        expect(drop["lands"] == (drop["turn_deg"] <= drop["dwell_deg"]),
               f"lands: {drop}")
        expect(r["pressure_angle"]["limit_deg"] == 30.0 and
               isinstance(r["pressure_angle"]["ok"], bool),
               f"the limit is checked: {r['pressure_angle']}")
        unchecked = d.call("check_followers", study=study)
        expect(unchecked["relations"][0]["pressure_angle"]["ok"] ==
               "unchecked", "no limit, no verdict")

        # The requirement holds the liftoff speed over the study.
        asm_id = next(p["id"] for p in d.call("list_parts")["parts"]
                      if p["name"] == "rig")
        req = d.call("add_requirement", name="stays on at a hand's turn",
                     measure={"kind": "follower", "document": asm_id,
                              "relation": rel["id"], "study": study,
                              "quantity": "liftoff_rpm"},
                     bound={"kind": "at_least", "value": 60.0})["id"]
        refused(d, "add_requirement", "accepted: liftoff_rpm",
                name="bad", measure={"kind": "follower", "document": asm_id,
                                     "relation": rel["id"], "study": study,
                                     "quantity": "liftoff"},
                bound={"kind": "at_least", "value": 1.0})
        ver = d.call("check_requirements")
        mine = next(x for x in ver["requirements"] if x["id"] == req)
        expect(mine["state"] in ("pass", "fail") and
               abs(mine["value"] - g["liftoff_rpm"]) < 1e-6,
               f"the requirement reads check_followers: {mine}")
        expect(mine["state"] == ("pass" if g["liftoff_rpm"] >= 60 else
                                 "fail"), f"judged: {mine}")
        d.close()
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()
    print("follower_check_smoke: a pin on a snail cam, checked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
