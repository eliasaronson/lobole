#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The regeneration cache remembers more than one history, on real parts.

tests/test_regen_cache.cpp walks the golden corpus; this walks generator
output (a gear, the slider crank's four parts) over the RPC: edit an early, a
middle and a late numeric field of each part, set it back, undo and redo,
and require that every return to a state seen before executes NOTHING and
reads back exactly as it did — bodies, element names, every feature's error
and warning (docs/architecture.md#the-document-model-and-regeneration).

Usage: regen_cache_smoke.py <path-to-designer-binary>
"""
import copy
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "generators"))
sys.path.insert(0, HERE)
from smoke_app import launch_app  # noqa: E402

NUMERIC = ("distance", "depth", "radius", "height", "width", "angle_deg",
           "spacing", "thickness", "offset")


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def signature(d):
    s = d.call("get_state")
    bodies = [(b.get("name"), round(b.get("volume", 0.0), 6))
              for b in s["bodies"]]
    feats = [(f.get("name"), f.get("error", ""), f.get("warning", ""))
             for f in s["features"]]
    elements = d.call("list_elements")
    names = [sorted(f.get("name", "") for f in b.get("faces", []))
             for b in elements.get("bodies", [])]
    expect(any(names), f"list_elements named no face: {elements}"[:400])
    return {"bodies": bodies, "features": feats, "elements": names}


def executed(d):
    return d.call("get_state")["regen"]["executed"]


def numeric_key(f):
    for k in NUMERIC:
        if isinstance(f.get(k), (int, float)) and not isinstance(
                f.get(k), bool) and not f.get(k + "_expr"):
            return k
    return None


def walk_part(d, label):
    feats = d.call("get_state")["features"]
    picks = []
    for start in (0, len(feats) // 2, len(feats) - 1):
        for j in range(start, len(feats)):
            if numeric_key(feats[j]) and j not in picks:
                picks.append(j)
                break
    expect(picks, f"{label}: no numeric field to edit")
    original = signature(d)
    for idx in picks:
        spec = d.call("get_state")["features"][idx]
        key = numeric_key(spec)
        edited = copy.deepcopy(spec)
        edited[key] = spec[key] * 1.05 + 0.1
        where = f"{label} feature {idx} ({spec['type']} {key})"
        d.call("modify_feature", index=idx, feature=edited)
        changed = signature(d)
        d.call("modify_feature", index=idx, feature=spec)
        expect(executed(d) == 0,
               f"{where}: set back executed {executed(d)} features")
        expect(signature(d) == original, f"{where}: set back reads differently")
        d.call("undo")
        expect(executed(d) == 0, f"{where}: undo executed {executed(d)}")
        expect(signature(d) == changed, f"{where}: undo reads differently")
        d.call("undo")
        expect(executed(d) == 0, f"{where}: second undo executed {executed(d)}")
        expect(signature(d) == original, f"{where}: undone reads differently")
        d.call("redo")
        expect(executed(d) == 0, f"{where}: redo executed {executed(d)}")
        expect(signature(d) == changed, f"{where}: redo reads differently")
        d.call("redo")
        expect(executed(d) == 0, f"{where}: second redo executed {executed(d)}")
        expect(signature(d) == original, f"{where}: redone reads differently")
    print(f"{label}: {len(picks)} edits, every return executed nothing")


def main():
    binary = sys.argv[1]
    state = tempfile.mkdtemp(prefix="lobole-regen-cache-")
    env = dict(os.environ, XDG_STATE_HOME=state,
               XDG_CONFIG_HOME=os.path.join(state, "config"))
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    app = launch_app(binary, env=env)
    try:
        from lobole.client import Client
        Client.wait_for(app.port).close()
        out = subprocess.run(
            [sys.executable, os.path.join(ROOT, "generators", "gear.py"),
             "--port", str(app.port)],
            capture_output=True, text=True, env=env, timeout=120)
        expect(out.returncode == 0, f"gear.py failed:\n{out.stderr}")
        d = Client(app.port)
        walk_part(d, "gear")

        from lobole.sdk import connect
        from slider_crank import build
        build(connect(app.port))
        tabs = d.call("list_parts")
        rows = tabs.get("parts", tabs) if isinstance(tabs, dict) else tabs
        parts = [t for t in rows if t.get("type") == "part"]
        expect(len(parts) >= 4, f"slider crank parts: {parts}")
        for t in parts:
            d.call("set_active_part", part=t["name"])
            if d.call("get_state")["features"]:
                walk_part(d, t["name"])
    finally:
        app.kill()


if __name__ == "__main__":
    main()
