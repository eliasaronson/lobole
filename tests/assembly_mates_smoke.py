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

        print("assembly_mates_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
