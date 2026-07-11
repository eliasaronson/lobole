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

        # Autosave + crash recovery (F2): save clean, dirty the doc, let the
        # 1s autosave fire, SIGKILL (a real crash — no signal handler), then
        # prove a fresh instance sees and recovers the autosave.
        crash_project = os.path.join(tmp, "crashme.json")
        d.call("save_project", path=crash_project)
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
            expect(len(d2.call("get_state")["features"]) == 2,
                   "the saved file itself has the pre-crash features")
            d2.call("open_project", path=r["autosave_path"])
            expect(len(d2.call("get_state")["features"]) == 3,
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
