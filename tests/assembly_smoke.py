#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

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

from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402




def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)  # prove no display server is needed
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_asm_")
    env["XDG_STATE_HOME"] = tmp
    # ...and its own CONFIG dir, or the run edits the developer's real
    # preferences: set_settings persists immediately (headless runs no GUI
    # loop to debounce it), so a suite reset their font size to 120%.
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        expect(d.call("ping")["ok"], "ping")

        # --- Build a project: two parts + one assembly ---------------------
        d.call("new_project")  # starts with one part ("Part 1")
        # Rename-free: the default part is index 0. Add a named part "base".
        base = d.call("add_part", name="base")
        box(d, (0, 0, 0), (4, 4, 4), "Blk")
        arm = d.call("add_part", name="arm")
        box(d, (0, 0, 0), (2, 6, 2), "Blk")

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

        # --- Occurrence edits + undo/redo (phase 3) ------------------------
        oid = occ[0]["id"]
        d.call("set_occurrence_transform", id=oid,
               transform={"position": [1, 2, 3], "orientation": [0, 0, 0, 1]})
        moved = d.call("list_occurrences")["occurrences"][0]
        expect(abs(moved["transform"]["position"][1] - 2.0) < 1e-5, "moved")

        d.call("rename_occurrence", id=oid, name="anchor")
        d.call("set_occurrence_fixed", id=oid, fixed=False)
        cur = d.call("list_occurrences")["occurrences"][0]
        expect(cur["name"] == "anchor" and cur["fixed"] is False, "renamed+freed")

        # Undo unwinds edits newest-first: unfix, rename, then the move.
        d.call("undo"); d.call("undo")
        back = d.call("list_occurrences")["occurrences"][0]
        expect(back["name"] == "base" and back["fixed"] is True,
               f"undo restored name+fixed: {back['name']}, {back['fixed']}")
        d.call("undo")
        origin = d.call("list_occurrences")["occurrences"][0]
        expect(abs(origin["transform"]["position"][1]) < 1e-5,
               "undo restored the position")
        # Redo re-applies the move.
        d.call("redo")
        expect(abs(d.call("list_occurrences")["occurrences"][0]
                   ["transform"]["position"][1] - 2.0) < 1e-5, "redo re-moved")

        # EITHER HALF ALONE. `add_occurrence` places a part from {position}
        # by itself, and this call used to throw the raw nlohmann `key
        # 'orientation' not found` for the same object — a message naming
        # neither the RPC nor the field that is optional in its sibling.
        d.call("set_occurrence_transform", id=oid,
               transform={"rotate": {"axis": [0, 0, 1], "angle_deg": 90}})
        turned = d.call("list_occurrences")["occurrences"][0]
        expect(abs(turned["transform"]["position"][1] - 2.0) < 1e-5,
               f"orientation alone left the position: {turned}")
        expect(abs(turned["transform"]["orientation"][2] - 0.7071) < 1e-3,
               f"...and did turn it: {turned}")
        d.call("set_occurrence_transform", id=oid,
               transform={"position": [4, 5, 6]})
        slid = d.call("list_occurrences")["occurrences"][0]
        expect(abs(slid["transform"]["position"][0] - 4.0) < 1e-5,
               f"position alone moved it: {slid}")
        expect(abs(slid["transform"]["orientation"][2] - 0.7071) < 1e-3,
               f"...and kept the orientation: {slid}")
        # Neither half moves nothing, and says so rather than succeeding.
        try:
            d.call("set_occurrence_transform", id=oid, transform={})
            raise AssertionError("an empty transform was accepted")
        except RuntimeError as e:
            expect("omitting both" in str(e), f"the refusal explains: {e}")
        d.call("set_occurrence_transform", id=oid,
               transform={"position": [1, 2, 3], "orientation": [0, 0, 0, 1]})

        # Leave it where it started for the round-trip below.
        d.call("undo")

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
