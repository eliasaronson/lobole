#!/usr/bin/env python3
"""Integration test: a mutating RPC that BREAKS the model says so.

RemoteHelpers.h states the contract: the regeneration summary is "appended as
'regen' to every mutating RPC's response ... so clients learn about failures
from the call that caused them instead of having to poll".

Several handlers did not honour it, and the failure mode was worse than a
missing field. Activating a configuration whose override broke a fillet
returned:

    {"active": 3, "bodies": 1, "volumes": [7999.999999999998]}

A success, one body, a plausible volume — and 7999.999... is exactly 20^3,
i.e. the UNFILLETED box. Nothing in the response said the fillet had failed, so
a script would build its next edit on a broken model. `undo` had the same hole.

This pins the contract for the handlers that can leave a FEATURE broken.
Studies are deliberately excluded: a study edit cannot break a feature, so
regen there would always report ok and tell a caller nothing.

Usage: regen_honesty_smoke.py <path-to-designer-binary>
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


def broken(response, label):
    """The response must carry regen AND name the failure."""
    expect("regen" in response, f"{label}: response carries no 'regen': {response}")
    reg = response["regen"]
    expect(reg["ok"] is False, f"{label}: regen.ok should be False: {reg}")
    expect(len(reg["failed"]) >= 1, f"{label}: regen.failed is empty: {reg}")
    names = [f["name"] for f in reg["failed"]]
    expect("F" in names, f"{label}: the broken fillet is not named: {reg}")
    # Decision-complete: the error says what went wrong, not just that it did.
    err = next(f["error"] for f in reg["failed"] if f["name"] == "F")
    expect(len(err) > 10, f"{label}: error is not explanatory: {err}")


def healthy(response, label):
    expect("regen" in response, f"{label}: response carries no 'regen': {response}")
    expect(response["regen"]["ok"] is True,
           f"{label}: regen.ok should be True: {response['regen']}")


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_regen_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        d.call("new_project")

        # A 20 mm box with a variable-driven fillet: small enough to build,
        # and an override can make the radius impossible for the geometry.
        d.call("set_variable", name="r", expression="2")
        d.call("add_feature", feature={
            "type": "Box", "name": "B", "position": [0, 0, 0],
            "width": 20, "height": 20, "depth": 20})
        edges = d.call("list_elements", body=0)["bodies"][0]["edges"]
        r = d.call("add_feature", feature={
            "type": "Fillet", "name": "F", "radius": 2, "radius_expr": "r",
            "edges": [{"feature_id": 1, "element": edges[0]["name"]}]})
        healthy(r, "the fillet builds to start with")

        cfg = d.call("add_configuration", name="huge")["id"]
        d.call("set_configuration", id=cfg,
               parameter_overrides=[{"name": "r", "expression": "50"}])

        # --- the reproduction ---------------------------------------------
        act = d.call("set_active_configuration", id=cfg)
        broken(act, "set_active_configuration")
        # The legacy hand-rolled keys stay (configuration_smoke reads them),
        # but they are exactly what made this look healthy: one body, and a
        # volume equal to the unfilleted box.
        expect(act["bodies"] == 1, f"legacy 'bodies' key kept: {act}")
        expect(abs(sum(act["volumes"]) - 8000.0) < 1e-3,
               f"the misleading volume is still 20^3: {act}")
        expect("digest" in act, f"and a digest is now carried too: {act}")

        # --- undo/redo had the same hole ----------------------------------
        healthy(d.call("undo"), "undo back to the working configuration")
        broken(d.call("redo"), "redo into the broken one")
        healthy(d.call("set_active_configuration", id=0), "back to default")

        # --- a variable edit that breaks a feature -------------------------
        broken(d.call("set_variable", name="r", expression="50"),
               "set_variable driving the radius impossible")
        healthy(d.call("set_variable", name="r", expression="2"),
                "and back")

        # --- loading is where a broken model most often ARRIVES ------------
        proj = os.path.join(tmp, "broken.dsgn")
        d.call("set_variable", name="r", expression="50")
        d.call("save_project", path=proj)
        d.call("new_project")
        opened = d.call("open_project", path=proj)
        broken(opened, "open_project on a project whose feature does not build")

        print("regen_honesty_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    main()
