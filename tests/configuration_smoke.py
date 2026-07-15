#!/usr/bin/env python3
"""Integration test: part configurations, driven headless.

Covers: adding configurations, editing their parameter overrides + feature
suppression, switching the active configuration (geometry follows), undo/redo,
the family-export capability (a STEP per configuration), and a save -> reopen
round-trip preserving configs + the active id.

Usage: configuration_smoke.py <path-to-designer-binary>
"""
import os
import socket
import subprocess
import sys
import tempfile
import json as jsonlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def main():
    binary = sys.argv[1]
    port = free_port()
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_cfg_")
    env["XDG_STATE_HOME"] = tmp
    app = subprocess.Popen([binary, "--headless", "--remote", str(port)],
                           env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    try:
        d = Designer.wait_for(port)
        expect(d.call("ping")["ok"], "ping")

        # A parametric part: a 40x20 plate `depth` tall, plus a boss prism.
        d.call("new_project")
        d.call("set_variable", name="depth", expression="10")
        sk1 = d.call("add_feature", feature={
            "type": "Sketch", "name": "Plate",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1], "uAxis": [1, 0, 0]},
            "entities": [{"type": "rect", "c1": [0, 0], "c2": [40, 20]}]})
        d.call("add_feature", feature={
            "type": "Extrude", "name": "Pad", "sketch_id": sk1["id"],
            "distance": 10.0, "distance_expr": "depth", "mode": "new_body"})
        sk2 = d.call("add_feature", feature={
            "type": "Sketch", "name": "BossProfile",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1], "uAxis": [1, 0, 0]},
            "entities": [{"type": "circle", "center": [20, 10], "radius": 6}]})
        boss = d.call("add_feature", feature={
            "type": "Extrude", "name": "Boss", "sketch_id": sk2["id"],
            "distance": 25.0, "mode": "new_body"})
        boss_id = boss["id"]

        # --- Configurations ------------------------------------------------
        tall = d.call("add_configuration", name="Tall")["id"]
        plain = d.call("add_configuration", name="Plain")["id"]
        cfgs = d.call("list_configurations")
        expect(len(cfgs["configurations"]) == 2, f"two configs: {cfgs}")
        expect(cfgs["active"] == 0, "base is active initially")

        # Tall overrides `depth`; Plain suppresses the boss.
        d.call("set_configuration", id=tall,
               parameter_overrides=[{"name": "depth", "expression": "30"}])
        d.call("set_configuration", id=plain, suppressed_features=[boss_id])
        # Read back: the edits round-trip through set/list.
        by_id = {c["id"]: c for c in d.call("list_configurations")["configurations"]}
        expect(by_id[tall]["parameter_overrides"] == [{"name": "depth", "expression": "30"}],
               f"Tall override stored: {by_id[tall]}")
        expect(by_id[plain]["suppressed_features"] == [boss_id],
               f"Plain suppression stored: {by_id[plain]}")

        # Switching active regenerates; geometry follows.
        base = d.call("set_active_configuration", id=0)
        tallr = d.call("set_active_configuration", id=tall)
        plainr = d.call("set_active_configuration", id=plain)
        expect(base["bodies"] >= 1 and tallr["bodies"] >= 1,
               f"base + tall have bodies: {base} {tallr}")
        expect(sum(tallr["volumes"]) > sum(base["volumes"]) + 1.0,
               f"Tall (depth 30) > base (depth 10): {tallr} vs {base}")
        expect(plainr["bodies"] < base["bodies"],
               f"Plain has fewer bodies (boss suppressed): {plainr} vs {base}")

        # Undo the activation returns to Tall; redo re-applies Plain.
        d.call("undo")
        expect(d.call("list_configurations")["active"] == tall, "undo -> Tall active")
        d.call("redo")
        expect(d.call("list_configurations")["active"] == plain, "redo -> Plain active")

        # --- Family export: a STEP per configuration -----------------------
        for cid, label in [(0, "base"), (tall, "tall"), (plain, "plain")]:
            d.call("set_active_configuration", id=cid)
            path = os.path.join(tmp, f"bracket-{label}.step")
            d.call("export_step", path=path)  # exports the active configuration
            expect(os.path.exists(path) and os.path.getsize(path) > 0,
                   f"exported {label}: {path}")

        # --- Save, reopen, verify configs + active survive -----------------
        d.call("set_active_configuration", id=tall)
        proj = os.path.join(tmp, "bracket.dsgn")
        d.call("save_project", path=proj)
        with open(os.path.join(proj, "index.json")) as f:
            idx = jsonlib.load(f)
        expect(idx["version"] == 31, f"format v31: {idx.get('version')}")
        expect(idx["active_configuration"] == tall,
               f"active config persisted: {idx.get('active_configuration')}")
        expect(len(idx["configurations"]) == 2, "config id list persisted")
        expect(os.path.isdir(os.path.join(proj, "configurations")),
               "configurations dir written")

        d.call("new_project")
        d.call("open_project", path=proj)
        reop = d.call("list_configurations")
        expect(reop["active"] == tall, f"active config survived reopen: {reop}")
        names = {c["name"] for c in reop["configurations"]}
        expect(names == {"Tall", "Plain"}, f"configs survived reopen: {names}")

        print("configuration_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
