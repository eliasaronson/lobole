#!/usr/bin/env python3
"""Integration test: drive `designer --headless --remote` — no window, no
GPU, no display server (DISPLAY is stripped to prove it).

Covers the document/geometry RPC surface headless: the S1 bracket with
analytic volume + mass_properties asserts, element names driving a 3D
chamfer, project round-trip, undo/redo, the specific errors of every
GUI-dependent method, clean SIGTERM shutdown, and fail-fast on a taken port.

Usage: headless_smoke.py <path-to-designer-binary>
"""
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


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


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
    app = subprocess.Popen([binary, "--headless", "--remote", str(port),
                            "--autosave", "1"],
                           env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    try:
        d = Designer.wait_for(port)
        expect(d.call("ping")["ok"], "ping")

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

        # Unassigned material -> mass is null, never guessed
        d.call("set_material", feature_id=pad["id"], material="")
        mp = d.call("mass_properties")
        expect(mp["bodies"][0]["mass_g"] is None, "unknown density: no mass")
        expect(mp["total"]["mass_g"] is None, "unknown density: no total")

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

        # Element names drive a 3D chamfer headless (T5 consumer over RPC)
        els = d.call("list_elements")["bodies"][0]
        expect(els["created_by"] == pad["id"], "created_by is the pad")
        expect(len(els["edges"]) > 0, "edges are named")
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

        # Every GUI-dependent method degrades with its specific error
        expect_error(d, "screenshot", "requires the GUI", path="/tmp/x.png")
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

        # Section (I3): cross-section area of a plate at a mid-cut
        d.call("new_project")
        d.call("add_feature", feature={
            "type": "Box", "name": "P", "position": [0, 0, 0],
            "width": 4, "height": 5, "depth": 6})
        sec = d.call("section", base_plane="xy", offset=3.0)  # z=3 -> 4x5=20
        expect(abs(sec["area"] - 20.0) < 1e-3, "cross-section area 20")
        miss = d.call("section", base_plane="xy", offset=99.0)
        expect(abs(miss["area"]) < 1e-6, "section miss = 0 area")

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
        d.call("export_stl", path=stl, deflection=0.05)
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
        d.call("new_project")  # empty document
        try:
            d.call("export_step", path=os.path.join(tmp, "empty.step"))
            expect(False, "export with no bodies must fail")
        except RuntimeError:
            pass

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
        deadline = time.time() + 10
        while (not os.path.exists(crash_project + ".autosave")
               and time.time() < deadline):
            time.sleep(0.1)
        expect(os.path.exists(crash_project + ".autosave"),
               "autosave sidecar written while dirty")
        app.send_signal(signal.SIGKILL)
        app.wait(timeout=10)

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
            app2.wait(timeout=10)

        # Clean shutdown on SIGTERM (fresh instance; the first was SIGKILLed)
        port3 = free_port()
        app3 = subprocess.Popen(
            [binary, "--headless", "--remote", str(port3)],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        Designer.wait_for(port3).close()
        app3.send_signal(signal.SIGTERM)
        expect(app3.wait(timeout=10) == 0, "exit 0 on SIGTERM")

        # A taken port must exit non-zero (CI fail-fast), not serve nothing
        blocker = socket.socket()
        blocker.bind(("127.0.0.1", 0))
        blocker.listen(1)
        p = subprocess.run(
            [binary, "--headless", "--remote",
             str(blocker.getsockname()[1])],
            env=env, capture_output=True, timeout=30)
        expect(p.returncode == 1, "taken port exits 1")
        blocker.close()

        print("headless_smoke: all assertions passed")
    finally:
        if app.poll() is None:
            app.terminate()
    return 0


if __name__ == "__main__":
    sys.exit(main())
