#!/usr/bin/env python3
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

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402


def free_port():
    import socket
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]
    port = free_port()
    env = dict(os.environ)
    env.pop("DISPLAY", None)  # prove no display server is needed
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_mates_")
    env["XDG_STATE_HOME"] = tmp
    # ...and its own CONFIG dir, or the run edits the developer's real
    # preferences: set_settings persists immediately (headless runs no GUI
    # loop to debounce it), so a suite reset their font size to 120%.
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = subprocess.Popen([binary, "--headless", "--remote", str(port)],
                           env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    try:
        d = Designer.wait_for(port)
        expect(d.call("ping")["ok"], "ping")

        # --- Project: two box parts + an assembly --------------------------
        d.call("new_project")
        d.call("add_part", name="base")
        d.call("add_feature", feature={
            "type": "Box", "name": "Blk", "position": [0, 0, 0],
            "width": 4, "height": 4, "depth": 4})
        d.call("add_part", name="arm")
        d.call("add_feature", feature={
            "type": "Box", "name": "Blk", "position": [0, 0, 0],
            "width": 2, "height": 6, "depth": 2})
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
            expect("unknown mate type 'revolut'" in str(e),
                   f"refusal should name the typo: {e}")
            expect("revolute" in str(e),
                   f"refusal should name what IS accepted: {e}")

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
        expect(len(lm) == 1 and lm[0]["mate_type"] == "slider", f"one slider: {lm}")
        expect("error" not in lm[0], f"no error on the good mate: {lm[0]}")

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
        proj = os.path.join(tmp, "robot.dsgn")
        d.call("save_project", path=proj)
        expect(os.path.isdir(os.path.join(proj, "assemblies", "robot", "mates")),
               "mates directory written")

        d.call("new_project")
        d.call("open_project", path=proj)
        lp = {p["name"]: p for p in d.call("list_parts")["parts"]}
        d.call("set_active_part", index=lp["robot"]["index"])

        lm2 = d.call("list_mates")["mates"]
        expect(len(lm2) == 1, f"the slider mate reopened: {lm2}")
        expect(lm2[0]["mate_type"] == "slider", "type survived")
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
        d.call("add_feature", feature={"type": "Sphere", "name": "Ball",
                                       "center": [0, 0, 0], "radius": 5})
        d.call("add_part", name="seat")
        d.call("set_active_part", name="seat")
        d.call("add_feature", feature={"type": "Box", "name": "Block",
                                       "position": [-10, -10, -10],
                                       "width": 20, "height": 20, "depth": 10})
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
