#!/usr/bin/env python3
"""Integration test: the assemblies A2 project container, driven headless.

Covers: creating a mixed part+assembly project, placing occurrences that
reference parts by stable id, list_parts reporting kind+id, the clear error
when a part-only RPC runs on an active assembly, and a full save -> reopen
round-trip that preserves documents, ids, kinds, and occurrence placement.

Usage: assembly_smoke.py <path-to-designer-binary>
"""
import os
import subprocess
import sys
import tempfile
import json as jsonlib

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
    tmp = tempfile.mkdtemp(prefix="designer_asm_")
    env["XDG_STATE_HOME"] = tmp
    app = subprocess.Popen([binary, "--headless", "--remote", str(port)],
                           env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    try:
        d = Designer.wait_for(port)
        expect(d.call("ping")["ok"], "ping")

        # --- Build a project: two parts + one assembly ---------------------
        d.call("new_project")  # starts with one part ("Part 1")
        # Rename-free: the default part is index 0. Add a named part "base".
        base = d.call("add_part", name="base")
        d.call("add_feature", feature={
            "type": "Box", "name": "Blk", "position": [0, 0, 0],
            "width": 4, "height": 4, "depth": 4})
        arm = d.call("add_part", name="arm")
        d.call("add_feature", feature={
            "type": "Box", "name": "Blk", "position": [0, 0, 0],
            "width": 2, "height": 6, "depth": 2})

        asmResult = d.call("add_assembly", name="robot")
        asmId = asmResult["id"]
        expect(asmId > 0, "assembly got a stable id")

        # list_parts reports kind + stable id for every tab.
        lp = d.call("list_parts")
        kinds = {p["name"]: p["type"] for p in lp["parts"]}
        expect(kinds.get("base") == "part", f"base is a part: {kinds}")
        expect(kinds.get("robot") == "assembly", f"robot is an assembly: {kinds}")
        part_ids = {p["name"]: p["id"] for p in lp["parts"]}
        expect(len(set(part_ids.values())) == len(part_ids), "ids are unique")

        # The assembly tab is active now. A part-only RPC must fail clearly,
        # not crash.
        try:
            d.call("get_state")
            raise AssertionError("get_state must fail on an active assembly")
        except RuntimeError as e:
            expect("assembly" in str(e).lower(), f"clear error: {e}")

        # --- Place occurrences: two of 'base' (the instancing case) + arm ---
        o1 = d.call("add_occurrence", part="base", fixed=True)
        o2 = d.call("add_occurrence", part="base",
                    transform={"position": [10, 0, 0],
                               "orientation": [0, 0, 0.70710677, 0.70710677]})
        o3 = d.call("add_occurrence", part="arm", visible=False)
        expect(o1["part_id"] == o2["part_id"], "both base occurrences share a part id")
        expect(o1["part_id"] != o3["part_id"], "arm is a different part")

        occ = d.call("list_occurrences")["occurrences"]
        expect(len(occ) == 3, f"three occurrences: {len(occ)}")
        expect(occ[0]["fixed"] is True, "first is fixed")
        expect(occ[2]["visible"] is False, "third is hidden")

        # add_occurrence for a missing part fails.
        try:
            d.call("add_occurrence", part="nope")
            raise AssertionError("unknown part must fail")
        except RuntimeError as e:
            expect("nope" in str(e), f"names the missing part: {e}")

        # --- Save, reopen, verify the whole project survives ---------------
        proj = os.path.join(tmp, "robot.dsgn")
        d.call("save_project", path=proj)

        # On disk: project.json v2 with typed documents; the assembly dir.
        with open(os.path.join(proj, "project.json")) as f:
            pj = jsonlib.load(f)
        expect(pj["version"] == 2, f"project format v2: {pj.get('version')}")
        types = {docu["name"]: docu["type"] for docu in pj["documents"]}
        expect(types.get("robot") == "assembly", f"robot typed: {types}")
        expect(os.path.isdir(os.path.join(proj, "assemblies", "robot")),
               "assembly directory written")

        # Reopen in a fresh instance and re-check.
        d.call("new_project")
        d.call("open_project", path=proj)
        lp2 = {p["name"]: p for p in d.call("list_parts")["parts"]}
        expect(set(lp2) == {"Part 1", "base", "arm", "robot"},
               f"all tabs reopened: {set(lp2)}")
        expect(lp2["robot"]["type"] == "assembly", "robot still an assembly")

        d.call("set_active_part", index=lp2["robot"]["index"])
        occ2 = d.call("list_occurrences")["occurrences"]
        expect(len(occ2) == 3, f"occurrences survived: {len(occ2)}")
        # Placement survived the round-trip (quaternion + position).
        expect(abs(occ2[1]["transform"]["position"][0] - 10.0) < 1e-5,
               "occurrence position round-trips")
        expect(abs(occ2[1]["transform"]["orientation"][2] - 0.70710677) < 1e-5,
               "occurrence orientation round-trips")
        # part_id references still resolve to the reopened part.
        base_id = lp2["base"]["id"]
        expect(occ2[0]["part_id"] == base_id,
               f"occurrence part_id matches the part: {occ2[0]['part_id']} vs {base_id}")

        print("assembly_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
