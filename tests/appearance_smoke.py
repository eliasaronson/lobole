#!/usr/bin/env python3
"""Integration test: the v54 appearance override — set_appearance RPC,
inheritance from the material, persistence, clearing, validation, and the
no-geometry-rebuild guarantee (appearance is stripped from the regen hash).

Usage: appearance_smoke.py <path-to-designer-binary>
"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402




def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_appearance_")
    env["XDG_STATE_HOME"] = tmp
    # ...and its own CONFIG dir, or the run edits the developer's real
    # preferences: set_settings persists immediately (headless runs no GUI
    # loop to debounce it), so a suite reset their font size to 120%.
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        d.call("new_project")
        r = d.call("add_feature", feature={
            "type": "Box", "name": "Barrelish", "position": [0, 0, 0],
            "width": 10, "height": 10, "depth": 10})
        fid = r["id"]
        d.call("set_material", feature_id=fid, material="plastic_white")

        # Override color+roughness; the body echoes it in get_state.
        d.call("set_appearance", feature_id=fid, color=[0.05, 0.2, 0.6],
               roughness=0.4)
        body = d.call("get_state")["bodies"][0]
        expect("appearance" in body, f"body carries the override: {body}")
        expect(abs(body["appearance"]["color"][2] - 0.6) < 1e-6 and
               abs(body["appearance"]["roughness"] - 0.4) < 1e-6 and
               "metallic" not in body["appearance"],
               f"only set fields serialize: {body['appearance']}")
        expect(body["material"] == "plastic_white",
               "physical material untouched")

        # The recolor must rebuild NOTHING (hash-stripped): regen executed 0.
        stats = d.call("get_state")["regen"]
        expect(stats.get("executed", -1) == 0,
               f"recolor must not rebuild geometry: {stats}")

        # Mass is untouched (appearance never reaches physics).
        mp = d.call("mass_properties")
        expect(abs(mp["total"]["mass_g"] - 1.04) < 1e-3,
               f"plastic_white density governs mass: {mp['total']}")

        # Persistence: save, reopen, still overridden.
        path = os.path.join(tmp, "appearance.json")
        d.call("save_project", path=path)
        d.call("open_project", path=path)
        body = d.call("get_state")["bodies"][0]
        expect("appearance" in body and
               abs(body["appearance"]["color"][2] - 0.6) < 1e-6,
               f"override survives reopen: {body}")

        # Clearing: no optional fields = back to pure material.
        r = d.call("set_appearance", feature_id=fid)
        expect(r["cleared"], f"clear reported: {r}")
        body = d.call("get_state")["bodies"][0]
        expect("appearance" not in body, f"cleared: {body}")

        # Validation: 0..255 colors are the classic slip.
        try:
            d.call("set_appearance", feature_id=fid, color=[30, 60, 120])
            raise AssertionError("0..255 color should be rejected")
        except RuntimeError as e:
            expect("0..255" in str(e) and "0..1" in str(e),
                   f"names the convention: {e}")

        print("appearance_smoke: all checks passed")
        return 0
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
