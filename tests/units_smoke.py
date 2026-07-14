#!/usr/bin/env python3
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

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402


def free_port():
    import socket
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]
    port = free_port()
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_units_")
    env["XDG_STATE_HOME"] = tmp
    app = subprocess.Popen([binary, "--headless", "--remote", str(port)],
                           env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    try:
        d = Designer.wait_for(port)
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
        vol = st["bodies"][0]["volume"]
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
        expect(abs(st["bodies"][0]["volume"] - 200.0) < 1e-3,
               "reopened volume still 200")

        print("units_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
