#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: C2 units algebra, driven headless over JSON-RPC.

Checks unit-suffixed expressions resolve end-to-end through the document:
a variable defined in cm/in, an extrude distance in mm, the resulting volume,
that a variable named after a unit is refused, and that a saved file reopens
with the unit text intact (format v28).

Usage: units_smoke.py <path-to-designer-binary>
"""
import os
import subprocess
import sys
import tempfile

from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402




def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_units_")
    env["XDG_STATE_HOME"] = tmp
    # ...and its own CONFIG dir, or the run edits the developer's real
    # preferences: set_settings persists immediately (headless runs no GUI
    # loop to debounce it), so a suite reset their font size to 120%.
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        expect(d.call("ping")["ok"], "ping")
        d.call("new_project")

        # --- a variable in cm resolves to mm --------------------------------
        r = d.call("set_variable", name="w", expression="2 cm")
        expect(abs(r["value"] - 20.0) < 1e-6, f"2 cm -> 20 mm: {r}")
        # mixed-unit arithmetic in a variable
        r = d.call("set_variable", name="pitch", expression="1 in + 6 mm")
        expect(abs(r["value"] - 31.4) < 1e-6, f"1in + 6mm -> 31.4: {r}")

        # --- a unit-named variable is refused -------------------------------
        try:
            d.call("set_variable", name="mm", expression="1")
            raise AssertionError("a variable named 'mm' should be refused")
        except Exception as e:
            expect("invalid variable name" in str(e).lower() or "mm" in str(e),
                   f"clear refusal for a unit name: {e}")

        # --- a unit expression drives real geometry -------------------------
        sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "S",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "rect", "c1": [0, 0], "c2": [10, 4]}]})
        d.call("add_feature", feature={
            "type": "Extrude", "name": "Pad", "sketch_id": sk["id"],
            "distance": 5.0, "distance_expr": "0.5 cm", "mode": "new_body"})
        st = d.call("get_state")
        expect(len(st["bodies"]) == 1, f"one body: {st['bodies']}")
        vol = st["bodies"][0]["volume_mm3"]
        expect(abs(vol - 10 * 4 * 5) < 1e-3,
               f"0.5cm extrude -> 5mm, volume 200: {vol}")

        # --- save/reopen keeps the unit text (v28) --------------------------
        proj = os.path.join(tmp, "units.dsgn")
        d.call("save_project", path=proj)
        d.call("new_project")
        d.call("open_project", path=proj)
        st = d.call("get_state")
        pad = [f for f in st["features"] if f.get("name") == "Pad"][0]
        expect(pad.get("distance_expr") == "0.5 cm",
               f"unit expr text survived the round-trip: {pad}")
        expect(abs(st["bodies"][0]["volume_mm3"] - 200.0) < 1e-3,
               "reopened volume still 200")

        # --- DISPLAY units (v64) --------------------------------------
        # A different thing from everything above, and worth saying so: the
        # expressions tested so far carry units on the way IN ("2 cm"), and
        # resolve to millimetres. This is the way OUT — what the document is
        # read in. Storage stays mm either way, which is the invariant the
        # volume check below is here to hold.
        expect(d.call("get_units") == {"length": "mm", "mass": "kg",
                                       "angle": "deg"},
               "a fresh document is mm/kg/deg")

        d.call("set_units", length="in", mass="lb")
        expect(d.call("get_units")["length"] == "in", "length is inches")
        expect(d.call("get_units")["mass"] == "lb", "mass is pounds")
        expect(d.call("get_units")["angle"] == "deg", "angle untouched")

        st = d.call("get_state")
        expect(abs(st["bodies"][0]["volume_mm3"] - 200.0) < 1e-3,
               f"a display unit did NOT convert the geometry: {st['bodies'][0]}")

        # Refused, not substituted: a document that said "inches" and loaded
        # as millimetres would be wrong in every dimension and look fine.
        try:
            d.call("set_units", length="inches")
            expect(False, "set_units accepted 'inches'")
        except RuntimeError as e:
            expect("expected one of" in str(e), f"names what it accepts: {e}")
        expect(d.call("get_units")["length"] == "in",
               "a refused set_units changed nothing")

        d.call("save_project", path=proj)
        d.call("new_project")
        expect(d.call("get_units")["length"] == "mm", "new project is mm")
        d.call("open_project", path=proj)
        expect(d.call("get_units") == {"length": "in", "mass": "lb",
                                       "angle": "deg"},
               "display units survived the round-trip")

        print("units_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
