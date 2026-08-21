#!/usr/bin/env python3
"""Integration test: drive `designer --headless --remote` — no window, no
GPU, no display server (DISPLAY is stripped to prove it).

Covers the document/geometry RPC surface headless: the S1 bracket with
analytic volume + mass_properties asserts, element names driving a 3D
chamfer, project round-trip, undo/redo, the specific errors of every
GUI-dependent method, clean SIGTERM shutdown, and fail-fast on a taken port.

Usage: headless_smoke.py <path-to-designer-binary>
"""
import glob
import math
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


# Phase logging: this suite runs under `ctest -j`, where a rare failure used
# to say only "headless_smoke ... Failed" with no clue which of its ~40 steps
# gave way or how long it had waited. Every phase prints with its elapsed
# time, so the next flake explains itself instead of costing an investigation.
_T0 = time.time()
_PHASE = "startup"


def phase(name):
    global _PHASE
    print(f"  [{time.time() - _T0:6.1f}s] {name}", flush=True)
    _PHASE = name


def expect(condition, message):
    if not condition:
        raise AssertionError(f"[phase: {_PHASE}] {message}")


def wait_for_file(path, timeout, what):
    """Wait for a file, reporting how long it actually took.

    Fixed short deadlines are the classic source of load flakes: this suite
    shares a machine with a GPU topology solve and fifteen other tests. The
    budget is generous and the elapsed time is printed, so a slow machine
    passes and a genuinely broken one still fails.
    """
    deadline = time.time() + timeout
    started = time.time()
    while time.time() < deadline:
        if os.path.exists(path):
            print(f"      ({what} after {time.time() - started:.1f}s)",
                  flush=True)
            return True
        time.sleep(0.1)
    return False


def wait_exit(proc, timeout, what):
    """proc.wait with the elapsed time reported (see wait_for_file)."""
    started = time.time()
    try:
        code = proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        raise AssertionError(
            f"[phase: {_PHASE}] {what} did not exit within {timeout}s")
    print(f"      ({what} exited {code} after {time.time() - started:.1f}s)",
          flush=True)
    return code


def expect_error(d, method, fragment, **params):
    try:
        d.call(method, **params)
        raise AssertionError(f"{method} must error headless")
    except RuntimeError as e:
        expect(fragment in str(e), f"{method} error mentions '{fragment}': {e}")


def main():
    binary = sys.argv[1]
    port = free_port()
    env = dict(os.environ)
    env.pop("DISPLAY", None)  # a display server must NOT be needed
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_headless_")
    env["XDG_STATE_HOME"] = tmp  # keep unsaved-slot autosaves out of $HOME
    # Keep the app's own stderr: when the autosave phase flakes under load,
    # its log is the only witness to what the process was doing.
    app_log = os.path.join(tmp, "app.log")
    app = subprocess.Popen([binary, "--headless", "--remote", str(port),
                            "--autosave", "1"],
                           env=env, stdout=subprocess.DEVNULL,
                           stderr=open(app_log, "w"))
    try:
        d = Designer.wait_for(port)
        expect(d.call("ping")["ok"], "ping")

        phase("Strict params")
        # Strict params: a key no handler reads is a typo, rejected loudly
        # instead of silently ignored (the `named` vs `name` trap). A near
        # miss suggests the intended key; the corrected call still works.
        expect_error(d, "get_state", "unknown parameter", bogus=1)
        expect_error(d, "set_variable", "did you mean 'comment'",
                     name="tw", expression="1", coment="typo")
        d.call("set_variable", name="tw", expression="1", comment="ok")

        phase("The S1 bracket")
        # The S1 bracket: variables -> constrained sketch -> extrude -> fillet
        d.call("new_project")
        d.call("set_variable", name="width", expression="40")
        d.call("set_variable", name="height", expression="width / 2")
        d.call("set_variable", name="thickness", expression="5")
        sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "Base",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [
                {"type": "line", "a": [1, 1], "b": [30, 2]},
                {"type": "line", "a": [30, 2], "b": [31, 15]},
                {"type": "line", "a": [31, 15], "b": [2, 16]},
                {"type": "line", "a": [2, 16], "b": [1, 1]}]})
        bot, right, top, left = sk["entity_ids"]
        for kind, refs, kw in [
                ("coincident", [[bot, 1], [right, 0]], {}),
                ("coincident", [[right, 1], [top, 0]], {}),
                ("coincident", [[top, 1], [left, 0]], {}),
                ("coincident", [[left, 1], [bot, 0]], {}),
                ("horizontal", [[bot, 0]], {}),
                ("horizontal", [[top, 0]], {}),
                ("vertical", [[right, 0]], {}),
                ("vertical", [[left, 0]], {}),
                ("coincident", [[bot, 0], [0xFFFFFF00, 0]], {}),
                ("distance", [[bot, 0], [bot, 1]], {"expr": "width"}),
                ("distance", [[right, 0], [right, 1]], {"expr": "height"})]:
            d.call("add_constraint", sketch_id=sk["id"], kind=kind, refs=refs,
                   **kw)
        d.call("fillet", sketch_id=sk["id"], line1=top, line2=left, radius=4.0)
        pad = d.call("add_feature", feature={
            "type": "Extrude", "name": "Pad", "sketch_id": sk["id"],
            "distance": 1.0, "distance_expr": "thickness",
            "mode": "new_body"})
        expected_volume = 40 * 20 * 5 - (16 - math.pi * 4) * 5
        state = d.call("get_state")
        expect(not any("error" in f for f in state["features"]),
               "no feature errors")
        expect(abs(state["bodies"][0]["volume"] - expected_volume) < 1e-2,
               "bracket volume")

        phase("Mass properties")
        # Mass properties: aluminum bracket, analytic mass/CoM (I2)
        d.call("set_material", feature_id=pad["id"], material="aluminum")
        mp = d.call("mass_properties")
        b = mp["bodies"][0]
        expect(abs(b["volume"] - expected_volume) < 1e-2, "massprops volume")
        expect(abs(b["mass_g"] - 2700 * expected_volume * 1e-6) < 1e-3,
               "aluminum mass in grams")
        expect(abs(b["center_of_mass"][2] - 2.5) < 1e-6, "CoM mid-thickness")
        expect(abs(mp["total"]["mass_g"] - b["mass_g"]) < 1e-9, "total mass")
        surface = b["surface_area"]
        expect(surface > 0, "surface area computed")
        inertia = b["inertia"]
        expect(len(inertia) == 3 and len(inertia[0]) == 3, "inertia is 3x3")

        phase("Unassigned material -> mass is null, never guessed")
        # Unassigned material -> mass is null, never guessed
        d.call("set_material", feature_id=pad["id"], material="")
        mp = d.call("mass_properties")
        expect(mp["bodies"][0]["mass_g"] is None, "unknown density: no mass")
        expect(mp["total"]["mass_g"] is None, "unknown density: no total")

        phase("Measure over element names")
        # Measure over element names (I1): the pad's cap face area, and a
        # top-face edge length.
        pels = d.call("list_elements")["bodies"][0]
        face = next(f["name"] for f in pels["faces"])
        marea = d.call("measure", a={"feature_id": pels["created_by"],
                                     "element": face})
        expect(marea["kind"] == "face" and marea["area"] > 0,
               "measure face area")
        edge = pels["edges"][0]["name"]
        mlen = d.call("measure", a={"feature_id": pels["created_by"],
                                    "element": edge})
        expect(mlen["kind"] == "edge" and mlen["length"] > 0,
               "measure edge length")
        try:
            d.call("measure", a={"feature_id": pels["created_by"],
                                 "element": "nonsense"})
            expect(False, "measuring a dangling ref must error")
        except RuntimeError:
            pass

        phase("Element names drive a 3D chamfer headless")
        # Element names drive a 3D chamfer headless (T5 consumer over RPC)
        els = d.call("list_elements")["bodies"][0]
        expect(els["created_by"] == pad["id"], "created_by is the pad")
        expect(len(els["edges"]) > 0, "edges are named")

        # list_elements enrichment: every face carries kind+area, every edge
        # kind+length; the bracket's sketch fillet extrudes to a cylindrical
        # face and radius-4 arc edges.
        expect(all("kind" in f and f["area"] > 0 for f in els["faces"]),
               "faces carry kind and area")
        expect(all("kind" in e and e["length"] > 0 for e in els["edges"]),
               "edges carry kind and length")
        fkinds = {f["kind"] for f in els["faces"]}
        expect("plane" in fkinds and "cylinder" in fkinds,
               f"bracket faces classify plane + cylinder: {fkinds}")
        arcs = [e for e in els["edges"] if e["kind"] == "arc"]
        expect(arcs and all(abs(e["radius"] - 4.0) < 1e-6 for e in arcs),
               "fillet arc edges report radius 4")
        expect(any(e["kind"] == "line" for e in els["edges"]),
               "straight edges classify as line")
        before = d.call("get_state")["bodies"][0]["volume"]
        d.call("add_feature", feature={
            "type": "Chamfer", "name": "Break", "radius": 0.5,
            "edges": [{"feature_id": els["created_by"],
                       "element": els["edges"][0]["name"]}]})
        state = d.call("get_state")
        expect(not any("error" in f for f in state["features"]),
               "chamfer regenerated clean")
        expect(state["bodies"][0]["volume"] < before, "chamfer removed volume")
        d.call("undo")

        phase("Project round-trip + undo/redo")
        # Project round-trip + undo/redo
        project = os.path.join(tmp, "bracket.json")
        d.call("save_project", path=project)
        d.call("new_project")
        expect(len(d.call("get_state")["features"]) == 0, "cleared")
        d.call("open_project", path=project)
        state = d.call("get_state")
        expect(abs(state["bodies"][0]["volume"] - expected_volume) < 1e-2,
               "reloaded volume")
        d.call("undo")  # camera/view state functions without a GUI
        v = d.call("view", name="front")
        offset = [p - t for p, t in zip(v["position"], v["target"])]
        expect(abs(offset[0]) < 1e-4 and offset[2] > 0, "front view headless")
        # Direct camera control (set_camera) is pure state — works headless.
        v = d.call("set_camera", target=[1.0, 2.0, 3.0], distance=250.0)
        expect(abs(v["distance"] - 250.0) < 1e-3, "set_camera distance")
        expect(abs(v["target"][1] - 2.0) < 1e-6, "set_camera target")
        v = d.call("set_camera", direction=[0, 0, 1])
        expect(v["position"][2] > v["target"][2], "set_camera direction")
        # ...and on an ASSEMBLY tab (viewport state, not part state).
        d.call("add_assembly", name="CamAsm")
        d.call("set_active_part", name="CamAsm")
        v = d.call("set_camera", target=[5.0, 0.0, 0.0], distance=100.0)
        expect(abs(v["distance"] - 100.0) < 1e-3, "set_camera on assembly")
        idx = [pp["index"] for pp in d.call("list_parts")["parts"]
               if pp["name"] == "CamAsm"][0]
        d.call("set_active_part", index=0)
        d.call("delete_part", index=idx)
        d.call("reset_layout")  # harmless no-op headless (flag only)

        phase("Importing a broken STL")
        # Importing a broken STL (zero triangles) names the file and format
        # instead of a generic regeneration failure. (Valid STLs import as
        # sewn shells/solids — covered in the interchange section below.)
        stl_path = os.path.join(tmp, "mesh.stl")
        with open(stl_path, "wb") as fh:
            fh.write(b"\0" * 84)
        r = d.call("add_feature",
                   feature={"type": "Import", "name": "Mesh",
                            "path": stl_path})
        failed = r["regen"]["failed"]
        expect(failed and "as STL" in failed[0]["error"],
               f"STL import error is specific: {failed}")
        d.call("delete_feature", index=len(d.call("get_state")["features"]) - 1)

        phase("A SECOND simultaneous client works")
        # A SECOND simultaneous client works (multi-client remote): both see
        # the same document and responses route to the right connection.
        d2 = Designer(port)
        expect(len(d2.call("get_state")["bodies"]) == 1, "second client reads")
        expect(len(d.call("get_state")["bodies"]) == 1, "first client still ok")
        d2.close()

        phase("Every GUI-dependent method degrades with its specific error")
        # Every GUI-dependent method degrades with its specific error
        # "a rendered frame", not "the GUI": with -DENABLE_TEST_ENGINE=OFF a
        # windowed build has no test engine and still screenshots fine, so the
        # two capabilities are named separately (docs/scripting/mcp.md).
        expect_error(d, "screenshot", "requires a rendered frame",
                     path="/tmp/x.png")
        expect_error(d, "menu_click", "unavailable in --headless",
                     path="Edit/Undo")
        expect_error(d, "viewport_drag", "unavailable in --headless")
        expect_error(d, "key_press", "unavailable in --headless",
                     chord="Ctrl+Z")
        expect_error(d, "select_face", "unavailable in --headless",
                     mesh=0, face=0)
        expect_error(d, "orient_to_face", "unavailable in --headless")
        r = d.call("render_still", width=8, height=8, samples=1,
                   path=os.path.join(tmp, "no.png"))
        expect(not r["ok"] and "not available" in r["error"],
               "render_still reports unavailable")

        # Rib (v36): an open profile above a box grows a stiffener down to it
        # — the generic add_feature path carries the new type.
        d.call("new_project")
        d.call("add_feature", feature={
            "type": "Box", "name": "Base", "position": [0, 0, 0],
            "width": 10, "height": 10, "depth": 10})
        rib_sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "RibProfile",
            "plane": {"origin": [5, 5, 15], "normal": [0, 0, -1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "line", "a": [-3, 0], "b": [3, 0]}]})
        d.call("add_feature", feature={
            "type": "Rib", "name": "Stiffener", "sketch_id": rib_sk["id"],
            "thickness": 1.0})
        state = d.call("get_state")
        expect(not any("error" in f for f in state["features"]),
               "rib regenerated clean")
        expect(state["bodies"][0]["volume"] > 1000.0 + 1.0, "rib added volume")

        phase("Cosmetic thread")
        # Cosmetic thread (v36): annotates a cylindrical face, geometry
        # untouched.
        cyl = d.call("add_feature", feature={
            "type": "Cylinder", "name": "Boss", "center": [30, 0, 0],
            "direction": [0, 0, 1], "radius": 4.0, "height": 20.0})
        els = next(b for b in d.call("list_elements")["bodies"]
                   if b["created_by"] == cyl["id"])
        boss_before = next(b for b in d.call("get_state")["bodies"]
                           if abs(b["volume"] - math.pi * 16 * 20) < 1e-3)
        d.call("add_feature", feature={
            "type": "Thread", "name": "M8", "designation": "M8",
            "pitch": 1.25,
            "face": {"feature_id": cyl["id"],
                     "element": els["faces"][0]["name"]}})
        state = d.call("get_state")
        expect(not any("error" in f for f in state["features"]),
               "thread regenerated clean")
        boss_after = next(b for b in state["bodies"]
                          if abs(b["volume"] - math.pi * 16 * 20) < 1e-3)
        expect(abs(boss_after["volume"] - boss_before["volume"]) < 1e-9,
               "cosmetic thread left the volume untouched")

        phase("Draft")
        # Draft (M8): taper a box's side faces; volume drops
        d.call("new_project")
        d.call("add_feature", feature={
            "type": "Box", "name": "Blk", "position": [0, 0, 0],
            "width": 10, "height": 10, "depth": 10})
        bx = d.call("list_elements")["bodies"][0]
        # A side face: pick the first whose name resolves; the feature rejects
        # caps, so try faces until one drafts. Use get_state face normals via
        # measure is overkill — just try face names, catching the cap error.
        vol0 = d.call("get_state")["bodies"][0]["volume"]
        drafted = False
        for f in bx["faces"]:
            try:
                idx = d.call("add_feature", feature={
                    "type": "Draft", "name": "Taper",
                    "body_id": bx["created_by"], "angle_deg": 8.0,
                    "neutral_plane": "xy",
                    "faces": [{"feature_id": bx["created_by"],
                               "element": f["name"]}]})
                st = d.call("get_state")
                if not any("error" in ff for ff in st["features"]):
                    expect(st["bodies"][0]["volume"] < vol0,
                           "draft reduced the volume")
                    drafted = True
                    break
                d.call("delete_feature", index=idx["index"])
            except RuntimeError:
                pass
        expect(drafted, "at least one side face drafted")

        phase("Hole wizard")
        # Hole wizard (M8): a 4-hole bolt pattern from one point sketch,
        # then interference (I4) between two boxes.
        d.call("new_project")
        d.call("add_feature", feature={
            "type": "Box", "name": "Plate", "position": [0, 0, 0],
            "width": 10, "height": 10, "depth": 4})
        holes = d.call("add_feature", feature={
            "type": "Sketch", "name": "Bolts",
            "plane": {"origin": [0, 0, 4], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "point", "p": [2, 2]},
                         {"type": "point", "p": [8, 2]},
                         {"type": "point", "p": [8, 8]},
                         {"type": "point", "p": [2, 8]}]})
        d.call("add_feature", feature={
            "type": "Hole", "name": "BoltHoles", "sketch_id": holes["id"],
            "hole_kind": "simple", "diameter": 1.0, "depth": 1.0,
            "through": True})
        import math as _m3
        st = d.call("get_state")
        expect(not any("error" in f for f in st["features"]),
               "holes regenerated clean")
        expect(abs(st["bodies"][0]["volume"]
                   - (400.0 - 4.0 * _m3.pi * 0.25 * 4.0)) < 0.5,
               "4-hole plate volume")

        phase("Multi-part project")
        # Multi-part project (assemblies A1): two parts, switch, save/load
        d.call("new_project")
        expect(d.call("list_parts")["parts"][0]["name"] == "Part 1",
               "starts with one part")
        d.call("add_feature", feature={
            "type": "Box", "name": "A", "position": [0, 0, 0],
            "width": 2, "height": 2, "depth": 2})
        d.call("add_part", name="Arm")
        d.call("add_feature", feature={
            "type": "Box", "name": "B", "position": [0, 0, 0],
            "width": 3, "height": 3, "depth": 3})
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 27) < 1e-6,
               "part 2 has its own box")
        d.call("set_active_part", index=0)
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 8) < 1e-6,
               "part 1 intact after switching back")
        # Save + reload the whole multi-part project.
        proj = os.path.join(tmp, "asm.dsgn")
        d.call("save_project", path=proj)
        expect(os.path.exists(os.path.join(proj, "project.json")),
               "multi-part writes project.json")
        d.call("new_project")
        d.call("open_project", path=proj)
        parts = d.call("list_parts")["parts"]
        expect([p["name"] for p in parts] == ["Part 1", "Arm"],
               "both parts reloaded")
        d.call("set_active_part", index=1)
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 27) < 1e-6,
               "reloaded part 2 geometry")
        # #10: rename an existing part (the default "Part 1" isn't permanent).
        # Resolvable by name; occurrences reference parts by stable id, so the
        # rename never breaks a reference.
        pid0 = next(p for p in parts if p["name"] == "Part 1")["id"]
        d.call("rename_part", part="Part 1", new_name="Wheel")
        renamed = d.call("list_parts")["parts"]
        expect(renamed[0]["name"] == "Wheel" and renamed[0]["id"] == pid0,
               "rename_part renames the slot, keeps its stable id")
        # Also resolvable by slot index; bogus part errors.
        d.call("rename_part", part=0, new_name="Hub")
        expect(d.call("list_parts")["parts"][0]["name"] == "Hub",
               "rename_part resolves a part by slot index")
        expect_error(d, "rename_part", "no part", part="ghost", new_name="X")
        d.call("rename_part", part="Hub", new_name="Part 1")  # restore
        # Delete a part; can't delete the last.
        d.call("delete_part", index=1)
        expect(len(d.call("list_parts")["parts"]) == 1, "part deleted")
        try:
            d.call("delete_part", index=0)
            expect(False, "deleting the last part must fail")
        except RuntimeError:
            pass

        phase("Project-wide variables")
        # Project-wide variables (Phase V): ONE variable drives geometry in two
        # parts; editing it changes both; a part-scoped var shadows it locally;
        # an undefined variable is a loud feature error (not a silent literal).
        def rect(w, h):
            return [{"type": "line", "a": [0, 0], "b": [w, 0]},
                    {"type": "line", "a": [w, 0], "b": [w, h]},
                    {"type": "line", "a": [w, h], "b": [0, h]},
                    {"type": "line", "a": [0, h], "b": [0, 0]}]
        d.call("new_project")
        r = d.call("set_variable", name="bore", expression="6")  # default: project
        expect(r["scope"] == "project", "set_variable defaults to project scope")
        sk1 = d.call("add_feature", feature={
            "type": "Sketch", "name": "S1",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]}, "entities": rect(4, 2)})
        # #4: no literal `distance` key at all — only `distance_expr`. The
        # deserializer must default the literal (like Revolve's `angle`) so the
        # expr alone builds a body instead of erroring on a missing key.
        p1 = d.call("add_feature", feature={
            "type": "Extrude", "name": "P1", "sketch_id": sk1["id"],
            "distance_expr": "bore", "mode": "new_body"})
        expect("error" not in p1, f"extrude with only distance_expr built: {p1}")
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 8 * 6) < 1e-3,
               "part 1 driven by the project variable (distance_expr only)")
        d.call("add_part", name="Two")
        sk2 = d.call("add_feature", feature={
            "type": "Sketch", "name": "S2",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]}, "entities": rect(5, 2)})
        d.call("add_feature", feature={
            "type": "Extrude", "name": "P2", "sketch_id": sk2["id"],
            "distance": 1.0, "distance_expr": "bore * 2", "mode": "new_body"})
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 10 * 12) < 1e-3,
               "part 2 driven by the SAME project variable")
        lv = d.call("list_variables")["variables"]
        expect(any(v["name"] == "bore" and v["scope"] == "project" for v in lv),
               "list_variables tags bore as project scope")
        # Edit the project variable: BOTH parts' geometry follows.
        d.call("set_variable", name="bore", expression="10")
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 10 * 20) < 1e-3,
               "part 2 followed the project-variable edit")
        d.call("set_active_part", index=0)
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 8 * 10) < 1e-3,
               "part 1 followed the project-variable edit too")
        # Project-variable edits are undoable across tabs (their own undo stack,
        # routed by the global most-recent-action rule).
        d.call("undo")
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 8 * 6) < 1e-3,
               "undo reverted the project-variable edit")
        d.call("redo")
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 8 * 10) < 1e-3,
               "redo re-applied the project-variable edit")
        # Save + reload preserves the project scope and both geometries.
        pvproj = os.path.join(tmp, "projvars.dsgn")
        d.call("save_project", path=pvproj)
        d.call("new_project")
        d.call("open_project", path=pvproj)
        lv = d.call("list_variables")["variables"]
        expect(any(v["name"] == "bore" and v["scope"] == "project"
                   and abs(v["value"] - 10) < 1e-9 for v in lv),
               "project variable reloaded at project scope")
        d.call("set_active_part", index=1)
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 10 * 20) < 1e-3,
               "part 2 geometry survived the reload")
        # A part-scoped variable of the same name shadows the project one here.
        r = d.call("set_variable", name="bore", expression="1", scope="part")
        expect(r["scope"] == "part", "scope='part' honored")
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 10 * 2) < 1e-3,
               "part-local bore shadows the project bore in THIS part")
        d.call("set_active_part", index=0)
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 8 * 10) < 1e-3,
               "the other part still resolves the project bore")
        # Fail-loud: an expression over an undefined variable errors the feature.
        badsk = d.call("add_feature", feature={
            "type": "Sketch", "name": "S3",
            "plane": {"origin": [0, 0, 20], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]}, "entities": rect(2, 2)})
        d.call("add_feature", feature={
            "type": "Extrude", "name": "Bad", "sketch_id": badsk["id"],
            "distance": 1.0, "distance_expr": "no_such_var", "mode": "new_body"})
        st = d.call("get_state")
        expect(any("error" in f and "unknown variable" in f["error"]
                   for f in st["features"]),
               f"undefined variable is a loud feature error: {st['features']}")

        phase("Section")
        # Section (I3): cross-section area of a plate at a mid-cut
        d.call("new_project")
        d.call("add_feature", feature={
            "type": "Box", "name": "P", "position": [0, 0, 0],
            "width": 4, "height": 5, "depth": 6})
        # set_section just toggles the viewport clip (no visible effect
        # headless), but the RPC must accept and clear it without error.
        d.call("set_section", enabled=True, base_plane="yz", offset=0.0)
        d.call("set_section", enabled=False)
        sec = d.call("section", base_plane="xy", offset=3.0)  # z=3 -> 4x5=20
        expect(abs(sec["area"] - 20.0) < 1e-3, "cross-section area 20")
        miss = d.call("section", base_plane="xy", offset=99.0)
        expect(abs(miss["area"]) < 1e-6, "section miss = 0 area")

        # P2/P3: several cuts, a tilt, a datum source, and persistence.
        d.call("remove_section", all=True)
        a = d.call("add_section", base_plane="xy", offset=2.0)
        b = d.call("add_section", base_plane="yz", offset=1.0, tilt_u=30.0)
        expect(a["id"] != b["id"], "each cut gets its own id")
        lst = d.call("list_sections")["sections"]
        expect(len(lst) == 2, f"two cuts: {lst}")
        expect(lst[0]["source"]["kind"] == "base_plane", "source named")
        # A tilt turns the plane: the cut normal is no longer the base axis.
        tilted = [s for s in lst if s["id"] == b["id"]][0]
        expect(abs(tilted["normal"][0] - 1.0) > 1e-3,
               f"30 deg tilt moved the normal off X: {tilted['normal']}")
        # ...and pivots ABOUT the offset point rather than sliding the cut
        # away. The test is that the tilted plane still passes through the
        # anchor (1,0,0) — dot(anchor, n) == d — NOT that d is unchanged: d
        # is measured along the new normal, so it is cos(30) here. A naive
        # implementation that kept d = offset would slide the cut off the
        # pivot and fail this.
        anchor = [1.0, 0.0, 0.0]
        through = sum(a * n for a, n in zip(anchor, tilted["normal"]))
        expect(abs(through - tilted["d"]) < 1e-3,
               f"tilt pivots about the offset point: {through} vs {tilted['d']}")
        expect(abs(tilted["d"] - 1.0) > 1e-3,
               "d is along the tilted normal, so it moved")

        # A datum plane as the source, resolved live.
        dat = d.call("add_feature", feature={
            "type": "DatumPlane", "name": "D", "kind": "offset",
            "base_plane": "xy", "offset": 1.5})
        upd = d.call("update_section", id=a["id"], plane_id=dat["id"],
                     offset=0.0)
        expect(abs(upd["d"] - 1.5) < 1e-3,
               f"cut sits on the datum: d={upd['d']}")
        # Move the datum (keeping its id) — the cut follows it, which is the
        # whole reason to reference one.
        d.call("modify_feature", index=1, feature={
            "type": "DatumPlane", "id": dat["id"], "name": "D",
            "kind": "offset", "base_plane": "xy", "offset": 4.0})
        after = [s for s in d.call("list_sections")["sections"]
                 if s["id"] == a["id"]][0]
        expect(abs(after["d"] - 4.0) < 1e-3,
               f"the cut followed the datum: d={after['d']}")
        expect(after["source"]["resolved"], "source still resolves")

        # And a source that VANISHES stops the cut rather than silently
        # cutting along some other plane: replacing the datum with a
        # genuinely DIFFERENT feature (a new id) leaves the reference
        # dangling.
        #
        # The id is explicit on purpose. This used to omit it and rely on
        # ReplaceFeature taking the incoming 0 verbatim — which destroyed the
        # identity of every feature the modify_feature RPC touched, wrote it
        # to disk as "0-box.json", and dangled every OTHER reference to it
        # too. Identity is now preserved unless you name a different one, so
        # "this is a different feature" has to be said out loud.
        d.call("modify_feature", index=1, feature={
            "type": "DatumPlane", "id": 9001, "name": "D2", "kind": "offset",
            "base_plane": "xy", "offset": 4.0})
        dangling = [s for s in d.call("list_sections")["sections"]
                    if s["id"] == a["id"]][0]
        expect(not dangling["source"]["resolved"],
               "a dangling datum reference is reported, not guessed")
        d.call("update_section", id=a["id"], base_plane="xy", offset=2.0)

        # Persistence: sections survive save + reopen (they were session
        # state until P3, so a set-up cut vanished on every save).
        with tempfile.TemporaryDirectory() as td:
            path = os.path.join(td, "sections.dsgn")
            d.call("save_project", path=path)
            d.call("new_project")
            expect(not d.call("list_sections")["sections"], "new = no cuts")
            d.call("open_project", path=path)
            back = d.call("list_sections")["sections"]
            expect(len(back) == 2, f"both cuts came back: {back}")
            kinds = sorted(s["source"]["kind"] for s in back)
            expect(kinds == ["base_plane", "base_plane"],
                   f"sources kept: {kinds}")
            tilt = [s for s in back if s["tilt_u"]][0]
            expect(abs(tilt["tilt_u"] - 30.0) < 1e-3, "tilt kept")
        d.call("remove_section", all=True)

        phase("Interference")
        # Interference: two overlapping boxes clash; disjoint ones don't.
        d.call("new_project")
        d.call("add_feature", feature={
            "type": "Box", "name": "A", "position": [0, 0, 0],
            "width": 2, "height": 2, "depth": 2})
        d.call("add_feature", feature={
            "type": "Box", "name": "B", "position": [1, 0, 0],
            "width": 2, "height": 2, "depth": 2})
        clash = d.call("interference", a=0, b=1)
        expect(clash["clash"] and abs(clash["volume"] - 4.0) < 1e-3,
               "overlapping boxes clash with overlap volume 4")
        d.call("modify_feature", index=1, feature={
            "type": "Box", "name": "B", "position": [10, 0, 0],
            "width": 2, "height": 2, "depth": 2})
        expect(not d.call("interference", a=0, b=1)["clash"],
               "disjoint boxes do not clash")

        phase("Assembly interference + cross-part queries")
        # Assembly interference + cross-part queries: two occurrences of one
        # cube part — overlapping placements clash; a flipped fastened mate
        # seats them flush (contact only) and the clash clears.
        d.call("new_project")
        boxf = d.call("add_feature", feature={
            "type": "Box", "name": "Cube", "position": [0, 0, 0],
            "width": 2, "height": 2, "depth": 2})
        d.call("set_material", feature_id=boxf["id"], material="aluminum")
        cube = d.call("list_elements")["bodies"][0]
        # #8: every face reports its outward unit surface normal. A box's six
        # planar faces point along ±X/±Y/±Z; check magnitude ~1 and that the
        # full axis-aligned set is present (not the old all-zero placeholder).
        cnorms = [f["normal"] for f in cube["faces"]]
        expect(all(abs((n[0]**2 + n[1]**2 + n[2]**2) ** 0.5 - 1.0) < 1e-4
                   for n in cnorms), "box face normals are unit length")
        def _has(ax):
            return any(abs(n[ax] - 1) < 1e-4 for n in cnorms) and \
                   any(abs(n[ax] + 1) < 1e-4 for n in cnorms)
        expect(_has(0) and _has(1) and _has(2),
               f"box reports +/-X,Y,Z face normals: {cnorms}")
        d.call("add_assembly", name="asm")  # active tab is now the assembly
        o1 = d.call("add_occurrence", part="Part 1", fixed=True)["id"]
        o2 = d.call("add_occurrence", part="Part 1",
                    transform={"position": [1, 1, 1],
                               "orientation": [0, 0, 0, 1]})["id"]
        clash = d.call("interference", a=o1, b=o2)
        expect(clash["clash"] and abs(clash["volume"] - 1.0) < 1e-3,
               f"overlapping occurrences clash with volume 1: {clash}")
        # Occurrence names resolve too (rename first: two occurrences of one
        # part share a default name).
        d.call("rename_occurrence", id=o2, name="CubeB")
        occs = d.call("list_occurrences")["occurrences"]
        names = {o["id"]: o["name"] for o in occs}
        by_name = d.call("interference", a=names[o1], b="CubeB")
        # No pair named -> sweep every visible occurrence pair.
        sweep = d.call("interference")
        expect(sweep["clash"] and len(sweep["pairs"]) == 1
               and abs(sweep["pairs"][0]["volume"] - 1.0) < 1e-3,
               f"all-pairs sweep finds the clash: {sweep}")
        # Project-level reads work from the assembly tab (were part-gated).
        d.call("history_log")
        d.call("checkpoint_list")
        expect(by_name["clash"], "occurrence-name interference works")

        phase("Assembly mass_properties")
        # Assembly mass_properties (finding #5): aggregate both occurrences in
        # WORLD space. o1 (aluminum 2^3 cube) sits at the origin — local CoM
        # (1,1,1); CubeB is shifted +(1,1,1) — world CoM (2,2,2). The total
        # volume/mass are the sum, the world CoM the volume-weighted midpoint.
        amp = d.call("mass_properties")  # active tab is the assembly
        expect(len(amp["occurrences"]) == 2, f"two occurrence rows: {amp}")
        expect(abs(amp["total"]["volume"] - 16.0) < 1e-3,
               f"assembly total volume 16: {amp['total']}")
        one_mass = 2700 * 8 * 1e-6  # aluminum, 8 mm^3
        expect(abs(amp["total"]["mass_g"] - 2 * one_mass) < 1e-6,
               f"assembly total mass = 2x the part: {amp['total']}")
        com = amp["total"]["center_of_mass"]
        expect(all(abs(c - 1.5) < 1e-4 for c in com),
               f"world CoM at the midpoint (1.5,1.5,1.5): {com}")
        shifted = next(o for o in amp["occurrences"] if o["name"] == "CubeB")
        expect(all(abs(c - 2.0) < 1e-4 for c in shifted["center_of_mass"]),
               f"shifted occurrence world CoM at (2,2,2): {shifted}")
        # Hiding an occurrence drops it from the aggregate (BOM convention).
        d.call("set_occurrence_visible", id=o2, visible=False)
        amp2 = d.call("mass_properties")
        expect(len(amp2["occurrences"]) == 1 and
               abs(amp2["total"]["volume"] - 8.0) < 1e-3,
               f"hidden occurrence excluded: {amp2['total']}")
        d.call("set_occurrence_visible", id=o2, visible=True)

        phase("Revolute axial offset")
        # Revolute axial offset (finding #6a) + sign (finding #6c): a positive
        # offset seats CubeB along +Z of a's connector normal (a's outward face
        # normal), not merely coinciding origins. The 2-cube's top face sits at
        # z=2 with a +Z normal, so a +5 offset places CubeB's origin at z=7 —
        # positive = along the normal, no inversion.
        top_face = max((f for f in cube["faces"] if f["kind"] == "plane"),
                       key=lambda f: f["centroid"][2])
        rev = d.call("add_mate", type="revolute", name="pivot",
                     a={"occurrence": o1,
                        "face": {"feature_id": cube["created_by"],
                                 "element": top_face["name"]}},
                     b={"occurrence": o2, "origin": True},
                     flipped=False, offset=5.0)
        expect(rev.get("errored_mates", 0) == 0,
               f"revolute+offset mate solved: {rev}")
        o2occ = next(o for o in d.call("list_occurrences")["occurrences"]
                     if o["id"] == o2)
        expect(abs(o2occ["transform"]["position"][2] - 7.0) < 1e-3,
               f"positive offset seats CubeB at +Z (z=7): {o2occ}")
        # A negative offset seats it the other way (below the face): z = 2 - 5.
        d.call("set_mate", id=rev["id"], offset=-5.0)
        o2occ = next(o for o in d.call("list_occurrences")["occurrences"]
                     if o["id"] == o2)
        expect(abs(o2occ["transform"]["position"][2] - (-3.0)) < 1e-3,
               f"negative offset seats CubeB at -Z (z=-3): {o2occ}")
        d.call("remove_mate", id=rev["id"])

        phase("Cross-part list_elements/measure from the ASSEMBLY tab")
        # Cross-part list_elements/measure from the ASSEMBLY tab (no
        # set_active_part round-trip): same names, enriched fields.
        pels = d.call("list_elements", part="Part 1")["bodies"][0]
        expect([f["name"] for f in pels["faces"]] ==
               [f["name"] for f in cube["faces"]],
               "cross-part list_elements matches the part's own listing")
        face = pels["faces"][0]
        m = d.call("measure", part="Part 1",
                   a={"feature_id": pels["created_by"],
                      "element": face["name"]})
        expect(abs(m["area"] - face["area"]) < 1e-9 and
               abs(m["area"] - 4.0) < 1e-6,
               "cross-part measure: cube face area 4")
        expect_error(d, "measure", "no part with id or index", part="Nope",
                     a={"feature_id": pels["created_by"],
                        "element": face["name"]})
        # #9: `part=<int>` accepts EITHER the stable id OR the slot index, and a
        # bogus number errors (no silent fall-through to the active part).
        pinfo = next(p for p in d.call("list_parts")["parts"]
                     if p["name"] == "Part 1")
        m_by_index = d.call("measure", part=pinfo["index"],
                            a={"feature_id": pels["created_by"],
                               "element": face["name"]})
        m_by_id = d.call("measure", part=pinfo["id"],
                         a={"feature_id": pels["created_by"],
                            "element": face["name"]})
        expect(abs(m_by_index["area"] - 4.0) < 1e-6 and
               abs(m_by_id["area"] - 4.0) < 1e-6,
               "measure resolves a part by slot index AND by stable id")
        expect_error(d, "measure", "no part with id or index", part=99999,
                     a={"feature_id": pels["created_by"],
                        "element": face["name"]})

        phase("Mate the two cubes face-to-face")
        # Mate the two cubes face-to-face (flipped=True: flush, opposing
        # normals) — mated-apart occurrences no longer clash.
        planar = next(f["name"] for f in pels["faces"]
                      if f["kind"] == "plane")
        mate = d.call("add_mate", type="fastened", name="seat",
                      a={"occurrence": o1,
                         "face": {"feature_id": pels["created_by"],
                                  "element": planar}},
                      b={"occurrence": o2,
                         "face": {"feature_id": pels["created_by"],
                                  "element": planar}},
                      flipped=True)
        expect(mate.get("errored_mates", 0) == 0, f"seat mate solved: {mate}")
        seated = d.call("interference", a=o1, b=o2)
        expect(not seated["clash"] and seated["volume"] < 1e-6,
               f"mated-apart occurrences do not clash: {seated}")

        phase("Mate-connector diagnostics")
        # Mate-connector diagnostics: a stale name that is a prefix of the
        # current names is called out per side, with suggestions.
        bad = d.call("add_mate", type="fastened", name="stale",
                     a={"occurrence": o1,
                        "face": {"feature_id": pels["created_by"],
                                 "element": planar + ";M9"}},
                     b={"occurrence": o2, "origin": True})
        err = bad.get("error", "")
        if not err:  # error may surface on the mate record instead
            lm = d.call("list_mates")["mates"]
            err = next(mm.get("error", "") for mm in lm
                       if mm.get("name") == "stale")
        expect("connector A" in err and names[o1] in err,
               f"error names the failing side + occurrence: {err}")
        expect(planar + ";M9" in err, f"error names the failing face: {err}")
        d.call("remove_mate", id=bad["id"])
        # A stale PREFIX (the real modified-by-later-feature case) suggests
        # the current names. Cube face names share the "<id>/g0f" style
        # prefix; use the planar name minus its last char.
        bad2 = d.call("add_mate", type="fastened", name="stale2",
                      a={"occurrence": o1,
                         "face": {"feature_id": pels["created_by"],
                                  "element": planar[:-1]}},
                      b={"occurrence": o2, "origin": True})
        err2 = bad2.get("error", "")
        expect("similar:" in err2 and planar in err2,
               f"stale-prefix error suggests current names: {err2}")
        d.call("remove_mate", id=bad2["id"])

        phase("Expression-driven sketch entities")
        # Expression-driven sketch entities (Decision 2A, v38): a variable
        # drives a circle radius directly; the edit ripples through the
        # sketch into the extrude volume, and survives save/load.
        d.call("new_project")
        d.call("set_variable", name="bore", expression="6")
        esk = d.call("add_feature", feature={
            "type": "Sketch", "name": "Lobe",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "circle", "center": [0, 0], "radius": 1,
                          "r_expr": "bore / 2"}]})
        d.call("add_feature", feature={
            "type": "Extrude", "name": "Pad", "sketch_id": esk["id"],
            "distance": 2.0, "mode": "new_body"})
        st = d.call("get_state")
        expect(not any("error" in f for f in st["features"]),
               "expr-entity model regenerated clean")
        expect(abs(st["bodies"][0]["volume"] - math.pi * 9 * 2) < 1e-2,
               "circle radius follows bore/2 = 3")
        ent = st["features"][0]["entities"][0]
        expect(ent.get("r_expr") == "bore / 2",
               f"entity expr surfaces in get_state: {ent}")
        d.call("set_variable", name="bore", expression="10")
        st = d.call("get_state")
        expect(abs(st["bodies"][0]["volume"] - math.pi * 25 * 2) < 1e-2,
               "set_variable ripples the entity expr into the volume")
        eproj = os.path.join(tmp, "exprs.json")
        d.call("save_project", path=eproj)
        d.call("new_project")
        d.call("open_project", path=eproj)
        st = d.call("get_state")
        expect(abs(st["bodies"][0]["volume"] - math.pi * 25 * 2) < 1e-2,
               "entity exprs survive save/load")
        d.call("set_variable", name="bore", expression="4")
        st = d.call("get_state")
        expect(abs(st["bodies"][0]["volume"] - math.pi * 4 * 2) < 1e-2,
               "reloaded entity expr still drives the geometry")

        phase("Sweep")
        # Sweep (M8): circle along a straight path = a cylinder
        d.call("new_project")
        pf = d.call("add_feature", feature={
            "type": "Sketch", "name": "Prof",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "circle", "center": [0, 0], "radius": 1}]})
        pa = d.call("add_feature", feature={
            "type": "Sketch", "name": "Path",
            "plane": {"origin": [0, 0, 0], "normal": [0, 1, 0],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "line", "a": [0, 0], "b": [0, 8]}]})
        d.call("add_feature", feature={
            "type": "Sweep", "name": "Pipe", "profile_id": pf["id"],
            "path_id": pa["id"], "mode": "new_body"})
        st = d.call("get_state")
        expect(not any("error" in f for f in st["features"]),
               "sweep regenerated clean")
        import math as _m2
        expect(abs(st["bodies"][0]["volume"] - _m2.pi * 8) < 0.2,
               "swept cylinder volume ~8pi")

        # Loft (M8): frustum between two squares on offset datum planes
        d.call("new_project")
        dp = d.call("add_feature", feature={
            "type": "DatumPlane", "name": "High", "kind": "offset",
            "base_plane": "xy", "offset": 6.0})
        a = d.call("add_feature", feature={
            "type": "Sketch", "name": "Bottom",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "circle", "center": [0, 0], "radius": 3}]})
        b = d.call("add_feature", feature={
            "type": "Sketch", "name": "Top", "plane_ref": dp["id"],
            "plane": {"origin": [0, 0, 6], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "circle", "center": [0, 0], "radius": 1}]})
        d.call("add_feature", feature={
            "type": "Loft", "name": "Cone", "sketch_ids": [a["id"], b["id"]],
            "mode": "new_body"})
        st = d.call("get_state")
        expect(not any("error" in f for f in st["features"]),
               "loft regenerated clean")
        # Conical frustum r=3->1 over h=6: V = pi h/3 (R^2+Rr+r^2) = pi*2*13.
        import math as _m
        expect(abs(st["bodies"][0]["volume"] - _m.pi * 2 * 13) < 1.0,
               "loft frustum volume")

        phase("Shell")
        # Shell (M8): hollow a box into a bin via list_elements face name
        d.call("new_project")
        d.call("add_feature", feature={
            "type": "Box", "name": "Blk", "position": [0, 0, 0],
            "width": 10, "height": 10, "depth": 10})
        bels = d.call("list_elements")["bodies"][0]
        d.call("add_feature", feature={
            "type": "Shell", "name": "Bin", "body_id": bels["created_by"],
            "thickness": 1.0,
            "open_faces": [{"feature_id": bels["created_by"],
                            "element": bels["faces"][0]["name"]}]})
        st = d.call("get_state")
        expect(not any("error" in f for f in st["features"]),
               "shell regenerated clean")
        expect(abs(st["bodies"][0]["volume"] - 424.0) < 5.0, "bin volume ~424")

        phase("Export STEP/STL/BREP")
        # Export STEP/STL/BREP (E1), then re-import the STEP to verify it
        # round-trips to the same volume.
        d.call("new_project")
        d.call("add_feature", feature={
            "type": "Box", "name": "Blk", "position": [0, 0, 0],
            "width": 2, "height": 3, "depth": 4})
        step = os.path.join(tmp, "part.step")
        stl = os.path.join(tmp, "part.stl")
        brep = os.path.join(tmp, "part.brep")
        expect(d.call("export_step", path=step)["bodies"] == 1, "export step")
        stl_rep = d.call("export_stl", path=stl, deflection=0.05)
        expect(stl_rep["watertight"] is True and stl_rep["shells"] == 1 and
               stl_rep["free_edges"] == 0,
               f"exported solid reports watertight: {stl_rep}")
        d.call("export_brep", path=brep)
        for f in (step, stl, brep):
            expect(os.path.getsize(f) > 100, f"{f} written")
        # STL binary header is 80 bytes + 4-byte count; not ASCII "solid".
        with open(stl, "rb") as fh:
            expect(not fh.read(5).startswith(b"solid") or
                   os.path.getsize(stl) > 84, "stl is binary")
        d.call("new_project")
        d.call("import_step", path=step)
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 24.0) < 1e-3,
               "re-imported STEP has the original volume")
        # STL import: the watertight mesh comes back as a closed SOLID (the
        # sewn shell is promoted), so volume works; a box tessellates exactly.
        d.call("new_project")
        d.call("import_step", path=stl)
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 24.0) < 1e-3,
               "re-imported STL is a solid with the original volume")

        phase("Assembly import")
        # Assembly import (I2) into an UNSAVED project: it ADDS to the
        # current project (hand-modeled and imported parts mix); the .brep
        # sidecars stage under the app state dir until the next save adopts
        # them into <project>/imported/.
        d.call("new_project")
        d.call("add_feature", feature={
            "type": "Box", "name": "Mine", "position": [0, 0, 0],
            "width": 1, "height": 1, "depth": 1})
        rep = d.call("import_step", path=step, as_assembly=True)
        expect(len(rep["occurrences"]) == 1,
               "assembly import works without a saved project")
        parts = d.call("list_parts")["parts"]
        expect(len(parts) == 3 and
               [p["type"] for p in parts].count("assembly") == 1,
               f"own part + imported part + assembly tab: {parts}")
        proj = os.path.join(tmp, "miximport.dsgn")
        d.call("save_project", path=proj)
        adopted = glob.glob(os.path.join(proj, "imported", "**", "*.brep"),
                            recursive=True)
        expect(len(adopted) == 1,
               f"staged sidecar adopted into the project on save: {adopted}")
        d.call("open_project", path=proj)
        parts = d.call("list_parts")["parts"]
        imported = [p for p in parts
                    if p["type"] == "part" and p["name"] != "Part 1"]
        expect(len(imported) == 1, f"imported part reloads: {parts}")
        d.call("set_active_part", index=imported[0]["index"])
        expect(abs(d.call("get_state")["bodies"][0]["volume"] - 24.0) < 1e-3,
               "adopted sidecar regenerates with the original volume")
        d.call("new_project")  # empty document
        try:
            d.call("export_step", path=os.path.join(tmp, "empty.step"))
            expect(False, "export with no bodies must fail")
        except RuntimeError:
            pass

        phase("Checkpoints")
        # Checkpoints (F4): create, list, mutate, restore, and error paths
        d.call("new_project")  # fresh document: no project path yet
        d.call("add_feature", feature={
            "type": "Box", "name": "Seed", "position": [0, 0, 0],
            "width": 2, "height": 2, "depth": 2})
        try:
            d.call("checkpoint_create", name="nope")
            expect(False, "checkpoint without a saved project must fail")
        except RuntimeError as e:
            expect("save the project" in str(e), "checkpoint needs a project")
        cp_project = os.path.join(tmp, "cp.json")
        d.call("save_project", path=cp_project)
        d.call("checkpoint_create", name="v1")
        expect(os.path.exists(cp_project + ".checkpoints/v1.json"),
               "checkpoint file written beside the project")
        volume_before = d.call("get_state")["bodies"][0]["volume"]
        d.call("add_feature", feature={
            "type": "Box", "name": "Extra", "position": [90, 0, 0],
            "width": 1, "height": 1, "depth": 1})
        expect(len(d.call("get_state")["bodies"]) == 2, "mutated after cp")
        cps = d.call("checkpoint_list")["checkpoints"]
        expect([c["name"] for c in cps] == ["v1"], "checkpoint listed")
        d.call("checkpoint_restore", name="v1")
        st = d.call("get_state")
        expect(len(st["bodies"]) == 1
               and abs(st["bodies"][0]["volume"] - volume_before) < 1e-9,
               "restore returns the checkpointed state")
        try:
            d.call("checkpoint_restore", name="missing")
            expect(False, "restoring a missing checkpoint must fail")
        except RuntimeError:
            pass

        phase("Directory format + git history")
        # Directory format + git history (F3+F5): saves commit, checkpoints
        # tag, restore rewinds
        d.call("new_project")
        d.call("add_feature", feature={
            "type": "Box", "name": "Base", "position": [0, 0, 0],
            "width": 2, "height": 2, "depth": 2})
        proj = os.path.join(tmp, "gitproj.dsgn")
        d.call("save_project", path=proj, message="first")
        expect(os.path.isdir(proj) and
               os.path.exists(os.path.join(proj, "index.json")),
               "directory project written")
        expect(os.path.isdir(os.path.join(proj, ".git")),
               "save initialized a git repo")
        d.call("add_feature", feature={
            "type": "Box", "name": "More", "position": [5, 0, 0],
            "width": 1, "height": 1, "depth": 1})
        d.call("save_project", path=proj, message="second")
        log = d.call("history_log")["log"]
        expect([e["subject"] for e in log] == ["second", "first"],
               "one commit per save, newest first")
        d.call("checkpoint_create", name="beta")
        d.call("add_feature", feature={
            "type": "Box", "name": "Even more", "position": [10, 0, 0],
            "width": 1, "height": 1, "depth": 1})
        d.call("save_project", path=proj, message="third")
        d.call("history_restore", commit=log[-1]["hash"])  # back to "first"
        expect(len(d.call("get_state")["features"]) == 1,
               "restored the first commit's single feature")
        d.call("history_restore", commit="cp/beta")  # checkpoint tag works
        expect(len(d.call("get_state")["features"]) == 2,
               "restored the checkpoint tag")
        try:
            d.call("history_restore", commit="nonsense")
            expect(False, "unknown commit must fail")
        except RuntimeError:
            pass

        phase("Versions cover the whole project")
        # F6: a named version is the WHOLE project. The F4 checkpoint it
        # replaced snapshotted `state.document` — the ACTIVE PART — so a part
        # that exists in the version but not now could never come back, and
        # on a multi-part project the other tabs were simply not in the file.
        d.call("new_project")
        d.call("add_feature", feature={
            "type": "Box", "name": "Base", "position": [0, 0, 0],
            "width": 4, "height": 4, "depth": 4})
        d.call("add_part", name="Lid")
        d.call("add_feature", feature={
            "type": "Box", "name": "Plate", "position": [0, 0, 0],
            "width": 4, "height": 1, "depth": 4})
        multi = os.path.join(tmp, "multipart.dsgn")
        d.call("save_project", path=multi)
        d.call("checkpoint_create", name="both parts")
        expect([c["name"] for c in d.call("checkpoint_list")["checkpoints"]]
               == ["both parts"], "named version listed")

        # Delete a whole part AND edit the survivor, then go back.
        d.call("set_active_part", index=0)
        d.call("add_feature", feature={
            "type": "Box", "name": "Wart", "position": [20, 0, 0],
            "width": 1, "height": 1, "depth": 1})
        d.call("delete_part", index=1)
        d.call("save_project", path=multi)
        expect(len(d.call("list_parts")["parts"]) == 1, "part deleted")

        d.call("checkpoint_restore", name="both parts")
        names = [pp["name"] for pp in d.call("list_parts")["parts"]]
        expect(names == ["Part 1", "Lid"],
               f"the deleted part came back: {names}")
        expect(len(d.call("get_state")["features"]) == 1,
               "and the survivor's later edit is gone")
        # Linear history: the state we left is still reachable.
        expect(len(d.call("history_log")["log"]) >= 4,
               "the restore is itself a version")

        phase("Autosave + crash recovery")
        # Autosave + crash recovery (F2): save clean, dirty the doc, let the
        # 1s autosave fire, SIGKILL (a real crash — no signal handler), then
        # prove a fresh instance sees and recovers the autosave.
        crash_project = os.path.join(tmp, "crashme.json")
        d.call("save_project", path=crash_project)
        saved_features = len(d.call("get_state")["features"])
        expect(not os.path.exists(crash_project + ".autosave"),
               "clean save leaves no sidecar")
        d.call("add_feature", feature={
            "type": "Box", "name": "Unsaved work", "position": [50, 0, 0],
            "width": 2, "height": 2, "depth": 2})
        # The app's 1 s autosave timer is driven by its main loop, which a
        # loaded machine starves; 60 s of budget, and the real elapsed time is
        # printed so a creeping regression is visible before it fails.
        if not wait_for_file(crash_project + ".autosave", 60.0,
                             "autosave sidecar"):
            # This tripped for months under parallel load and was written off
            # as a flake; the cause was the autosaver treating a version
            # number as a document identity (fixed 2026-08-19). 60 s is
            # generous for a 1 s timer, so if it trips again the interesting
            # question is what the app was doing — answer it here.
            alive = app.poll() is None
            state = {}
            try:
                state = d.call("get_state")
            except Exception as e:  # noqa: BLE001 - diagnostics only
                state = {"get_state failed": str(e)}
            listing = sorted(os.listdir(os.path.dirname(crash_project)))
            tail = ""
            try:
                with open(app_log) as fh:
                    tail = "".join(fh.readlines()[-15:])
            except OSError:
                pass
            expect(False,
                   "autosave sidecar written while dirty\n"
                   f"      app alive: {alive} (exit {app.poll()})\n"
                   f"      features now: {len(state.get('features', []))}"
                   f"  keys: {sorted(state)[:6]}\n"
                   f"      dir: {listing}\n"
                   f"      app log tail:\n{tail}")
        app.send_signal(signal.SIGKILL)
        wait_exit(app, 30, "SIGKILLed app")

        port2 = free_port()
        app2 = subprocess.Popen(
            [binary, "--headless", "--remote", str(port2), "--autosave", "1"],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            d2 = Designer.wait_for(port2)
            r = d2.call("open_project", path=crash_project)
            expect(r["autosave_available"], "recovery offered after crash")
            expect(len(d2.call("get_state")["features"]) == saved_features,
                   "the saved file itself has the pre-crash features")
            d2.call("open_project", path=r["autosave_path"])
            expect(len(d2.call("get_state")["features"]) == saved_features + 1,
                   "the autosave carries the unsaved work")
            d2.call("save_project", path=crash_project)
            expect(not os.path.exists(crash_project + ".autosave"),
                   "saving the recovery clears the sidecar")
        finally:
            app2.terminate()
            wait_exit(app2, 30, "recovery instance")

        phase("Clean shutdown on SIGTERM")
        # Clean shutdown on SIGTERM (fresh instance; the first was SIGKILLed)
        port3 = free_port()
        app3 = subprocess.Popen(
            [binary, "--headless", "--remote", str(port3)],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        Designer.wait_for(port3).close()
        app3.send_signal(signal.SIGTERM)
        expect(wait_exit(app3, 30, "SIGTERMed app") == 0, "exit 0 on SIGTERM")

        phase("A taken port must exit non-zero")
        # A taken port must exit non-zero (CI fail-fast), not serve nothing
        blocker = socket.socket()
        blocker.bind(("127.0.0.1", 0))
        blocker.listen(1)
        p = subprocess.run(
            [binary, "--headless", "--remote",
             str(blocker.getsockname()[1])],
            env=env, capture_output=True, timeout=60)
        expect(p.returncode == 1, "taken port exits 1")
        blocker.close()

        print("headless_smoke: all assertions passed")
    finally:
        if app.poll() is None:
            app.terminate()
    return 0


if __name__ == "__main__":
    sys.exit(main())
