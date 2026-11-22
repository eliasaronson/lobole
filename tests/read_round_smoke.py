#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""read_round through the Python client, whose returns check holds each
answer to the row's contract (docs/modeling/README.md#reading-a-round-off-a-scan).

A round's answer carries radius_mm and sigma_mm, a chamfer's chamfer_mm, a
sharp edge's neither: the row declared all three always present, so the
strict client refused every answer the reader gave. Here a scanned round
and a scanned chamfer are each read back, strict, and the keys that are not
the verdict's are absent.

Usage: read_round_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile

from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import scanned_corner  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]
    tmp = tempfile.mkdtemp(prefix="lobole_read_round_")
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    env["LOBOLE_REMOTE_STRICT"] = "1"
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        expect(d.strict, "the client checks answers against the contract")
        for chamfer, size in ((False, 2.0), (True, 1.5)):
            stl = os.path.join(tmp, f"corner_{int(chamfer)}.stl")
            scanned_corner(stl, size, chamfer=chamfer)
            d.call("new_project")
            d.call("import_step", path=stl, representation="mesh",
                   keep_origin=True, up_axis="none")
            fid = d.call("get_state")["features"][0]["id"]
            r = d.call("read_round", feature_id=fid, point_mm=[0, 0, 20])
            if chamfer:
                expect(r["verdict"] == "chamfer", f"a chamfer: {r}")
                expect(all(abs(c - size) < 0.1 for c in r["chamfer_mm"]),
                       f"its two distances, {size} each: {r}")
                expect("radius_mm" not in r and "sigma_mm" not in r,
                       f"no radius for a chamfer: {r}")
            else:
                expect(r["verdict"] == "round", f"a round: {r}")
                expect(abs(r["radius_mm"] - size) < 0.1,
                       f"its radius, {size}: {r}")
                expect("chamfer_mm" not in r, f"no chamfer for a round: {r}")
            expect(abs(r["angle_deg"] - 90.0) < 1.0,
                   f"the faces meet square: {r}")
        print("read_round_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except Exception:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
