#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""`--file` opens a project, and imports anything else.

It used to import whatever it was given: a saved project passed on the
command line became an Import feature that failed with "not a file the import
reads", and the project was never opened. Each project form (a flat .json, a
.dsgn, a container's project.json) must open as the project, and a STEP must
still arrive as an Import. And `save_project` refuses a path that is not a
project's, writing nothing, since nothing would open it as one.

Usage: startup_file_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from smoke_app import launch  # noqa: E402
from smoke_shapes import box  # noqa: E402


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="lobole-startup-") as tmp:
        flat = os.path.join(tmp, "plate.json")
        dsgn = os.path.join(tmp, "plate.dsgn")
        step = os.path.join(tmp, "plate.step")
        with launch(binary) as app:
            d = app.client
            box(d, [0, 0, 0], [30, 20, 4], name="Plate")
            d.call("save_project", path=flat)
            d.call("save_project", path=dsgn)
            d.call("export_step", path=step)
            odd = os.path.join(tmp, "plate.lobole")
            try:
                d.call("save_project", path=odd)
                refused = ""
            except Exception as e:  # noqa: BLE001 — the message is the result
                refused = str(e)
            expect(".dsgn" in refused and ".json" in refused,
                   f"save_project to {odd}: {refused or 'saved'}, not a "
                   "refusal naming .dsgn and .json")
            expect(not os.path.exists(odd),
                   f"the refused save_project wrote {odd}")

        for path in (flat, dsgn, os.path.join(dsgn, "project.json")
                     if os.path.isdir(dsgn) else dsgn):
            with launch(binary, ["--file", path]) as app:
                d = app.client
                ping = d.call("ping")
                root = dsgn if path.startswith(dsgn) else flat
                expect(ping.get("project") == root,
                       f"--file {path}: ping names project "
                       f"{ping.get('project')!r}, not {root!r}")
                names = [f["name"] for f in d.call("get_state")["features"]]
                expect("Plate" in names and not any(
                    f.get("type") == "Import"
                    for f in d.call("get_state")["features"]),
                    f"--file {path}: the project's features are {names}, "
                    "not the saved Plate")

        with launch(binary, ["--file", step]) as app:
            feats = app.client.call("get_state")["features"]
            expect([f.get("type") for f in feats] == ["Import"],
                   f"--file {step}: features {feats}, not one Import")
    print("startup_file_smoke: ok")


if __name__ == "__main__":
    main()
