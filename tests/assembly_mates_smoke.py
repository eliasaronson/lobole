#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: assemblies A3 mates, driven headless over JSON-RPC.

Covers the mate RPC surface: add_mate (with a direct frame-alignment solve),
set_mate moving a mechanism numerically (a slider's offset), list_mates,
per-mate error reporting for a dangling connector reference, remove_mate, undo,
and a save -> reopen round-trip that preserves mates and the last solved pose.

The face-anchor payoff (edit a part -> the mated occurrence follows) is covered
exhaustively in the C++ test_assembly_solve.cpp; this exercises the wire layer,
so it uses origin anchors (no toponaming knowledge needed) for clean solves.

Usage: assembly_mates_smoke.py <path-to-designer-binary>
"""
import os
import subprocess
import sys
import tempfile

from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box, sphere  # noqa: E402




def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)  # prove no display server is needed
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_mates_")
    env["XDG_STATE_HOME"] = tmp
    # ...and its own CONFIG dir, or the run edits the developer's real
    # preferences: set_settings persists immediately (headless runs no GUI
    # loop to debounce it), so a suite reset their font size to 120%.
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        expect(d.call("ping")["ok"], "ping")

        # --- Project: two box parts + an assembly --------------------------
        d.call("new_project")
        d.call("add_part", name="base")
        box(d, (0, 0, 0), (4, 4, 4), "Blk")
        d.call("add_part", name="arm")
        box(d, (0, 0, 0), (2, 6, 2), "Blk")
        d.call("add_assembly", name="robot")

        # base grounded at the origin; arm parked far away so the solve visibly
        # moves it.
        o1 = d.call("add_occurrence", part="base", fixed=True)["id"]
        o2 = d.call("add_occurrence", part="arm",
                    transform={"position": [50, 50, 50],
                               "orientation": [0, 0, 0, 1]})["id"]

        # --- A mis-spelled mate type is REFUSED, not demoted ---------------
        # It used to fall through to Fastened: add_mate(type="revolut") was
        # accepted and produced a RIGID joint, and the assembly then solved
        # perfectly well with the wrong degrees of freedom. RpcParams rejects
        # unknown request KEYS; nothing checked enum VALUES.
        try:
            d.call("add_mate", type="revolut",
                   a={"occurrence": o1, "origin": True},
                   b={"occurrence": o2, "origin": True})
            raise AssertionError("add_mate accepted the mis-spelled type")
        except RuntimeError as e:
            # Refused by the contract, before the handler
            # (docs/scripting/rpc.md#typed-contracts), with the same remedy.
            expect("got \"revolut\"" in str(e),
                   f"refusal should name the typo: {e}")
            expect("revolute" in e.diagnostic["accepted"],
                   f"refusal should name what IS accepted: {e}")
            expect("add_relation" in str(e),
                   f"refusal should point a coupling at relations: {e}")

        # --- A slider mate: origin-to-origin, free to slide along Z --------
        mate = d.call("add_mate", type="slider", name="rail",
                      a={"occurrence": o1, "origin": True},
                      b={"occurrence": o2, "origin": True},
                      flipped=False)
        expect(mate.get("errored_mates", 0) == 0, f"slider solved clean: {mate}")
        mate_id = mate["id"]
        expect(mate_id != o1 and mate_id != o2,
               "mate id is distinct from the occurrence ids (shared counter)")

        # offset 0 -> the arm origin coincides with the base origin.
        arm = d.call("list_occurrences")["occurrences"]
        arm = [o for o in arm if o["id"] == o2][0]
        pos = arm["transform"]["position"]
        expect(all(abs(c) < 1e-4 for c in pos),
               f"arm pulled onto the base origin: {pos}")

        # --- set_mate slides the mechanism numerically ---------------------
        d.call("set_mate", id=mate_id, offset=25.0)
        arm = [o for o in d.call("list_occurrences")["occurrences"]
               if o["id"] == o2][0]
        pos = arm["transform"]["position"]
        expect(abs(pos[2] - 25.0) < 1e-4 and abs(pos[0]) < 1e-4,
               f"slider offset moved the arm 25 along Z: {pos}")

        lm = d.call("list_mates")["mates"]
        # `type` is the KIND, the word add_mate took — not a constant "Mate"
        # beside a `mate_type` that held it.
        expect(len(lm) == 1 and lm[0]["type"] == "slider", f"one slider: {lm}")
        expect("mate_type" not in lm[0], f"one name for the kind: {lm[0]}")
        expect("error" not in lm[0], f"no error on the good mate: {lm[0]}")

        # --- derive: the app reads the joint off where the parts lie ------
        # Without this, a fastened mate between two frames has eight outcomes
        # (which side, which quarter turn) and none is predictable from the
        # call, so a caller tries them and tests the result. Park the arm
        # turned a quarter about Z and half over, and ask for a joint that
        # keeps that: the mate must come back holding it, not one of the
        # seven others (docs/assemblies/README.md#deriving-a-mate-from-the-placement).
        d.call("remove_mate", id=mate_id)
        turned = [0.0, 0.0, 0.70710678, 0.70710678]  # +90 deg about Z
        d.call("set_occurrence_transform", id=o2,
               transform={"position": [0, 0, 0], "orientation": turned})
        got = d.call("add_mate", type="fastened", name="derived",
                     a={"occurrence": o1, "origin": True},
                     b={"occurrence": o2, "origin": True}, derive="pose")
        expect(got.get("errored_mates", 0) == 0, f"derived mate solved: {got}")
        arm = [o for o in d.call("list_occurrences")["occurrences"]
               if o["id"] == o2][0]
        q = arm["transform"]["orientation"]
        expect(max(abs(q[i] - turned[i]) for i in range(4)) < 1e-3,
               f"the derived mate held the pose it was read off: {q}")
        expect(abs(got["rotation_deg"]) > 1e-6 or got["flipped"],
               f"derive actually set a DOF rather than defaulting: {got}")

        # 'twist' keeps the turn and drops the offsets — the anchors coincide,
        # which is what makes it a joint and not a copy of a placement.
        d.call("set_occurrence_transform", id=o2,
               transform={"position": [40, 0, 0], "orientation": turned})
        tw = d.call("set_mate", id=got["id"], offset=0.0)
        d.call("remove_mate", id=got["id"])
        tw = d.call("add_mate", type="fastened", name="twisted",
                    a={"occurrence": o1, "origin": True},
                    b={"occurrence": o2, "origin": True}, derive="twist")
        expect(abs(tw.get("dx", 0.0)) < 1e-9 and abs(tw.get("dy", 0.0)) < 1e-9
               and abs(tw["offset"]) < 1e-9,
               f"twist leaves the anchors coincident: {tw}")
        arm = [o for o in d.call("list_occurrences")["occurrences"]
               if o["id"] == o2][0]
        expect(all(abs(c) < 1e-3 for c in arm["transform"]["position"]),
               f"twist pulled the arm onto the anchor: {arm}")

        # A pose a fastened joint CANNOT hold is refused, not rounded to the
        # nearest one it can. The adjust turns about Z (and may be flipped
        # half a turn about X); a quarter turn about Y is not of that form,
        # and the nearest joint that is would look exactly like a fit.
        d.call("remove_mate", id=tw["id"])
        d.call("set_occurrence_transform", id=o2,
               transform={"position": [0, 0, 0],
                          "orientation": [0.0, 0.70710678, 0.0, 0.70710678]})
        try:
            d.call("add_mate", type="fastened",
                   a={"occurrence": o1, "origin": True},
                   b={"occurrence": o2, "origin": True}, derive="pose")
            raise AssertionError("derive accepted a skew placement")
        except RuntimeError as e:
            expect("not square to one axis" in str(e),
                   f"refusal should say why: {e}")
        expect(len(d.call("list_mates")["mates"]) == 0,
               "a refused derive leaves NO mate behind")

        # Refusals: an unknown word, and a mate type with a free DOF.
        for kw, want in (({"derive": "sideways"}, "one of: twist, pose"),
                         ({"derive": "twist", "type": "revolute"},
                          "FASTENED")):
            try:
                d.call("add_mate", type=kw.get("type", "fastened"),
                       a={"occurrence": o1, "origin": True},
                       b={"occurrence": o2, "origin": True},
                       derive=kw["derive"])
                raise AssertionError(f"add_mate accepted derive={kw}")
            except RuntimeError as e:
                expect(want in str(e), f"refusal should name {want}: {e}")
        mate = d.call("add_mate", type="slider", name="rail",
                      a={"occurrence": o1, "origin": True},
                      b={"occurrence": o2, "origin": True}, flipped=False)
        mate_id = mate["id"]
        d.call("set_mate", id=mate_id, offset=25.0)

        # --- A dangling connector reference is reported, not fatal ---------
        bad = d.call("add_mate", type="fastened",
                     a={"occurrence": o1,
                        "face": {"feature_id": 999999, "element": "nope"}},
                     b={"occurrence": o2, "origin": True})
        expect("error" in bad, f"dangling ref surfaced an error: {bad}")
        # The reply also NAMES the offender (errored_mates alone is the
        # global count — ambiguous when an earlier mate is the broken one).
        expect(any(e["id"] == bad["id"] and e["error"]
                   for e in bad.get("errored", [])),
               f"errored list names the broken mate: {bad}")
        solve = d.call("solve_assembly")
        expect(solve["errored_mates"] == 1, f"exactly one errored mate: {solve}")
        # The good slider still holds the arm at 25.
        arm = [o for o in d.call("list_occurrences")["occurrences"]
               if o["id"] == o2][0]
        expect(abs(arm["transform"]["position"][2] - 25.0) < 1e-4,
               "the healthy mate still solved despite the broken one")

        # Remove the broken mate; the assembly is clean again.
        d.call("remove_mate", id=bad["id"])
        expect(len(d.call("list_mates")["mates"]) == 1, "broken mate removed")

        # --- undo the removal, then the removal again + re-add check -------
        d.call("undo")  # brings the dangling mate back
        expect(len(d.call("list_mates")["mates"]) == 2, "undo restored the mate")
        d.call("redo")  # removes it again
        expect(len(d.call("list_mates")["mates"]) == 1, "redo removed it again")

        # --- Save, reopen, verify mates + solved pose survive --------------
        proj = os.path.join(tmp, "robot.lob")
        d.call("save_project", path=proj)
        expect(os.path.isdir(os.path.join(proj, "assemblies", "robot", "mates")),
               "mates directory written")

        d.call("new_project")
        d.call("open_project", path=proj)
        lp = {p["name"]: p for p in d.call("list_parts")["parts"]}
        d.call("set_active_part", index=lp["robot"]["index"])

        lm2 = d.call("list_mates")["mates"]
        expect(len(lm2) == 1, f"the slider mate reopened: {lm2}")
        expect(lm2[0]["type"] == "slider", "type survived")
        expect(abs(lm2[0]["offset"] - 25.0) < 1e-4, "offset survived")
        arm = [o for o in d.call("list_occurrences")["occurrences"]
               if o["id"] == o2][0]
        expect(abs(arm["transform"]["position"][2] - 25.0) < 1e-4,
               "the last solved pose reopened")

        # --- Sphere + cone connector frames (ball-and-seat, no origin
        # gymnastics): the sphere anchors at its CENTER, the cone at its
        # APEX; a ball mate coincides the two.
        d.call("add_part", name="ballpart")
        d.call("set_active_part", name="ballpart")
        sphere(d, (0, 0, 0), 5, "Ball")
        d.call("add_part", name="seat")
        d.call("set_active_part", name="seat")
        box(d, (-10, -10, -10), (20, 20, 10), "Block")
        sks = d.call("add_feature", feature={
            "type": "Sketch", "name": "C",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "point", "p": [0, 0]}]})
        # 45-deg countersink: cone tapers 5.5 -> 2.75 radius over 2.75 deep,
        # so the apex extrapolates to z = -5.5 on the hole axis.
        d.call("add_feature", feature={
            "type": "Hole", "name": "Seat", "sketch_id": sks["id"],
            "hole_kind": "countersink", "diameter": 5.5, "depth": 8,
            "cs_diameter": 11.0, "cs_depth": 2.75})

        def face_of(part, kind):
            els = d.call("list_elements", part=part)["bodies"][0]
            for f in els["faces"]:
                if f["kind"] == kind:
                    return els["created_by"], f["name"]
            raise AssertionError(f"no {kind} face in {part}")
        sph = face_of("ballpart", "sphere")
        cone = face_of("seat", "cone")

        d.call("set_active_part", name="robot")
        ob_ball = d.call("add_occurrence", part="ballpart")["id"]
        ob_seat = d.call("add_occurrence", part="seat", fixed=True)["id"]
        bm = d.call("add_mate", type="ball", name="ball-in-seat",
                    a={"occurrence": ob_ball,
                       "face": {"feature_id": sph[0], "element": sph[1]}},
                    b={"occurrence": ob_seat,
                       "face": {"feature_id": cone[0], "element": cone[1]}})
        expect("error" not in bm,
               f"sphere/cone faces carry connector frames now: {bm}")
        ball_occ = [o for o in d.call("list_occurrences")["occurrences"]
                    if o["id"] == ob_ball][0]
        bp = ball_occ["transform"]["position"]
        expect(abs(bp[0]) < 1e-3 and abs(bp[1]) < 1e-3
               and abs(bp[2] - (-5.5)) < 1e-2,
               f"ball centre at the cone apex (0,0,-5.5): {bp}")


        print("assembly_mates_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
