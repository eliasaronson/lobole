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
from smoke_app import launch_app  # noqa: E402




def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_cfg_")
    env["XDG_STATE_HOME"] = tmp
    # ...and its own CONFIG dir, or the run edits the developer's real
    # preferences: set_settings persists immediately (headless runs no GUI
    # loop to debounce it), so a suite reset their font size to 120%.
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
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
        expect(idx["version"] >= 31, f"format >= v31 (configs): {idx.get('version')}")
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

        # --- Assembly configurations: occurrence suppression (C4) ----------
        d.call("new_project")
        d.call("add_part", name="widget")
        d.call("add_feature", feature={
            "type": "Box", "name": "W", "position": [0, 0, 0],
            "width": 5, "height": 5, "depth": 5})
        d.call("add_configuration", name="Small")
        d.call("add_assembly", name="rig")
        o1 = d.call("add_occurrence", part="widget", fixed=True)["id"]
        o2 = d.call("add_occurrence", part="widget",
                    transform={"position": [10, 0, 0], "orientation": [0, 0, 0, 1]})["id"]
        # A config that suppresses the second occurrence.
        acfg = d.call("add_configuration", name="Half")["id"]
        # A part's configurations, read from the assembly tab by naming it;
        # without `part` the tab's own answer.
        pc = d.call("list_configurations", part="widget")
        expect([c["name"] for c in pc["configurations"]] == ["Small"],
               f"widget's configurations from the assembly tab: {pc}")
        ac = d.call("list_configurations")
        expect([c["name"] for c in ac["configurations"]] == ["Half"],
               f"the assembly's own configurations: {ac}")
        d.call("set_configuration", id=acfg, suppressed_occurrences=[o2])
        cur = {c["id"]: c for c in d.call("list_configurations")["configurations"]}
        expect(cur[acfg]["suppressed_occurrences"] == [o2],
               f"assembly config stores suppressed occurrence: {cur}")

        # Base: BOM counts both occurrences; Half: only one.
        base_bom = d.call("bom")["rows"]
        base_qty = sum(r["quantity"] for r in base_bom)
        r = d.call("set_active_configuration", id=acfg)
        expect(r["visible_occurrences"] == 1, f"Half hides one occurrence: {r}")
        half_qty = sum(r2["quantity"] for r2 in d.call("bom")["rows"])
        expect(half_qty == base_qty - 1,
               f"Half BOM drops the suppressed occurrence: {half_qty} vs {base_qty}")

        # Save/reopen preserves the assembly config + active id.
        aproj = os.path.join(tmp, "rig.dsgn")
        d.call("save_project", path=aproj)
        d.call("new_project")
        d.call("open_project", path=aproj)
        # Reactivate the rig tab to read its configs.
        rig_idx = [p for p in d.call("list_parts")["parts"]
                   if p["name"] == "rig"][0]["index"]
        d.call("set_active_part", index=rig_idx)
        areop = d.call("list_configurations")
        expect(areop["active"] == acfg, f"assembly active config survived: {areop}")
        expect(areop["configurations"][0]["suppressed_occurrences"] == [o2],
               f"assembly suppression survived reopen: {areop}")

        # --- Overriding a PROJECT variable re-derives its project-level
        # dependents (the C-clamp split-brain bug): `h = w * 2` must follow
        # an override of `w` for BOTH a constraint expr and an entity expr.
        d.call("new_project")
        d.call("set_variable", name="w", expression="10")   # project scope
        d.call("set_variable", name="h", expression="w * 2")
        skc = d.call("add_feature", feature={
            "type": "Sketch", "name": "S",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [
                {"type": "line", "a": [0, 0], "b": [9, 1]},
                {"type": "line", "a": [9, 1], "b": [10, 19]},
                {"type": "line", "a": [10, 19], "b": [1, 20]},
                {"type": "line", "a": [1, 20], "b": [0, 0]},
                # entity expr referencing the DERIVED project var:
                {"type": "circle", "center": [5, 5], "radius": 2,
                 "construction": True, "r_expr": "h / 8"}]})
        l1, l2, l3, l4, circ = skc["entity_ids"]
        for kind, refs, kw in [
                ("coincident", [[l1, 1], [l2, 0]], {}),
                ("coincident", [[l2, 1], [l3, 0]], {}),
                ("coincident", [[l3, 1], [l4, 0]], {}),
                ("coincident", [[l4, 1], [l1, 0]], {}),
                ("horizontal", [[l1, 0]], {}), ("horizontal", [[l3, 0]], {}),
                ("vertical", [[l2, 0]], {}), ("vertical", [[l4, 0]], {}),
                ("coincident", [[l1, 0], [0xFFFFFF00, 0]], {}),
                ("distance", [[l1, 0], [l1, 1]], {"expr": "w"}),
                # constraint expr referencing the DERIVED project var:
                ("distance", [[l2, 0], [l2, 1]], {"expr": "h"})]:
            d.call("add_constraint", sketch_id=skc["id"], kind=kind,
                   refs=refs, **kw)
        d.call("add_feature", feature={
            "type": "Extrude", "name": "P", "sketch_id": skc["id"],
            "distance": 5.0, "mode": "new_body"})
        b = d.call("get_state")["bodies"][0]
        base_h = b["bbox_max"][1] - b["bbox_min"][1]
        expect(abs(base_h - 20.0) < 1e-3, f"base h=20: {base_h}")
        cfg2 = d.call("add_configuration", name="Wide")["id"]
        d.call("set_configuration", id=cfg2,
               parameter_overrides=[{"name": "w", "expression": "15"}])
        d.call("set_active_configuration", id=cfg2)
        st = d.call("get_state")
        b = st["bodies"][0]
        got_w = b["bbox_max"][0] - b["bbox_min"][0]
        got_h = b["bbox_max"][1] - b["bbox_min"][1]
        expect(abs(got_w - 15.0) < 1e-3, f"override drives w: {got_w}")
        expect(abs(got_h - 30.0) < 1e-3,
               f"override re-derives dependent h (constraint expr): {got_h}")
        ent = next(e for e in st["features"][0]["entities"]
                   if e["type"] == "circle")
        expect(abs(ent["radius"] - 30.0 / 8) < 1e-3,
               f"override re-derives dependent h (entity expr): {ent}")
        d.call("set_active_configuration", id=0)
        b = d.call("get_state")["bodies"][0]
        expect(abs((b["bbox_max"][1] - b["bbox_min"][1]) - 20.0) < 1e-3,
               "base config restores h=20")

        print("configuration_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
