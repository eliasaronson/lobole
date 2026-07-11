#!/usr/bin/env python3
"""Model a bracket through the pythonic client (scripting S3).

Usage: pyclient_smoke.py <path-to-designer-binary>
"""
import math
import os
import socket
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer import Designer, ORIGIN  # noqa: E402


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
    app = subprocess.Popen([binary, "--headless", "--remote", str(port)],
                           env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    try:
        d = Designer(port)
        d.new().var("w", 40).var("h", "w / 2").var("t", 5)

        sk = (d.sketch(plane="xy", name="Base")
              .line((1, 1), (30, 2)).line((30, 2), (31, 15))
              .line((31, 15), (2, 16)).line((2, 16), (1, 1))
              .add())
        bot, right, top, left = sk.entity_ids
        (sk.constrain("coincident", [[bot, 1], [right, 0]])
           .constrain("coincident", [[right, 1], [top, 0]])
           .constrain("coincident", [[top, 1], [left, 0]])
           .constrain("coincident", [[left, 1], [bot, 0]])
           .constrain("horizontal", [[bot, 0]])
           .constrain("horizontal", [[top, 0]])
           .constrain("vertical", [[right, 0]])
           .constrain("vertical", [[left, 0]])
           .constrain("coincident", [[bot, 0], [ORIGIN, 0]])
           .constrain("distance", [[bot, 0], [bot, 1]], expr="w")
           .constrain("distance", [[right, 0], [right, 1]], expr="h"))

        d.extrude(sk, depth="t", name="Pad")
        expect(abs(d.volume() - 40 * 20 * 5) < 1e-2, "fluent bracket volume")

        # Parameter edit flows through the expression-driven extrude.
        d.var("t", 8)
        expect(abs(d.volume() - 40 * 20 * 8) < 1e-2, "param edit rebuilds")

        # Inspection sugar.
        mp = d.mass_properties()
        expect(abs(mp["bodies"][0]["volume"] - 40 * 20 * 8) < 1e-2,
               "mass_properties via client")
        els = d.elements()["bodies"][0]
        area = d.measure({"feature_id": els["created_by"],
                          "element": els["faces"][0]["name"]})
        expect(area["kind"] == "face" and area["area"] > 0,
               "measure via client")

        # box() helper + multi-body state.
        d.box(position=(60, 0, 0), width=2, height=2, depth=2, name="Blk")
        expect(len(d.bodies()) == 2, "box helper added a body")
        d.close()
        print("pyclient_smoke: all assertions passed")
    finally:
        app.terminate()
        app.wait(timeout=10)
    return 0


if __name__ == "__main__":
    sys.exit(main())
