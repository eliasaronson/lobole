#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""`--file`, or a lone path, opens a project, and imports anything else.

It used to import whatever it was given: a saved project passed on the
command line became an Import feature that failed with "not a file the import
reads", and the project was never opened. Each project form (a flat .json, a
.lob, a container's project.json, and a .dsgn -- the extension before the
rename, still read) must open as the project, and a STEP must still arrive as
an Import. A lone path is what the desktop entry's `Exec=lobole %f` passes. The import
runs as the import job, so the test waits for it to land.
And `save_project` refuses a path that is not a project's, writing nothing,
since nothing would open it as one; saved back to the .dsgn it was opened
from, a project goes to the .lob beside it and the reply says so.

Usage: startup_file_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile
import time

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
        lob = os.path.join(tmp, "plate.lob")
        step = os.path.join(tmp, "plate.step")
        # What 0.1.5 left: a project folder named .dsgn (written as a .lob,
        # the only form the app writes, and renamed).
        dsgn = os.path.join(tmp, "old.dsgn")
        with launch(binary) as app:
            d = app.client
            box(d, [0, 0, 0], [30, 20, 4], name="Plate")
            d.call("save_project", path=flat)
            d.call("save_project", path=lob)
            d.call("save_project", path=os.path.join(tmp, "old.lob"))
            os.rename(os.path.join(tmp, "old.lob"), dsgn)
            d.call("export_step", path=step)
            for odd in (os.path.join(tmp, "plate.lobole"),
                        os.path.join(tmp, "new.dsgn")):
                try:
                    d.call("save_project", path=odd)
                    refused = ""
                except Exception as e:  # noqa: BLE001 — the message is it
                    refused = str(e)
                expect(".lob" in refused,
                       f"save_project to {odd}: {refused or 'saved'}, not a "
                       "refusal naming .lob")
                expect(not os.path.exists(odd),
                       f"the refused save_project wrote {odd}")
            expect(not os.path.exists(os.path.join(tmp, "new.lob")),
                   "the refused save_project to new.dsgn wrote new.lob")

        cases = [(flat, flat), (lob, lob),
                 (os.path.join(lob, "project.json")
                  if os.path.exists(os.path.join(lob, "project.json"))
                  else os.path.join(lob, "index.json"), lob),
                 (dsgn, dsgn)]
        for path, root in cases:
            for args in (["--file", path], [path]):
                with launch(binary, args) as app:
                    d = app.client
                    ping = d.call("ping")
                    expect(ping.get("project") == root,
                           f"{args}: ping names project "
                           f"{ping.get('project')!r}, not {root!r}")
                    feats = d.call("get_state")["features"]
                    names = [f["name"] for f in feats]
                    expect("Plate" in names and not any(
                        f.get("type") == "Import" for f in feats),
                        f"{args}: the project's features are {names}, "
                        "not the saved Plate")

        # Saved back to the .dsgn it came from, the project goes to the .lob
        # beside it; the .dsgn stays.
        before = sorted(os.listdir(dsgn))
        with launch(binary, [dsgn]) as app:
            d = app.client
            reply = d.call("save_project", path=dsgn)
            moved = os.path.join(tmp, "old.lob")
            expect(reply.get("path") == moved and
                   reply.get("migrated_from") == dsgn,
                   f"save_project back to {dsgn}: {reply}, not path "
                   f"{moved} migrated_from {dsgn}")
            expect(d.call("ping").get("project") == moved,
                   "after the save the project is not the .lob")
        expect(os.path.isdir(moved), f"{moved} was not written")
        expect(sorted(os.listdir(dsgn)) == before,
               f"{dsgn} changed: {before} -> {sorted(os.listdir(dsgn))}")

        # The import is a job the loop runs (docs/ui/long-operations.md#import),
        # so it has landed once background_jobs no longer lists it.
        with launch(binary, ["--file", step]) as app:
            d = app.client
            deadline = time.monotonic() + 60
            while any(j["kind"] == "import"
                      for j in d.call("background_jobs")["jobs"]):
                expect(time.monotonic() < deadline,
                       f"--file {step}: the import never landed")
                time.sleep(0.05)
            feats = d.call("get_state")["features"]
            expect([f.get("type") for f in feats] == ["Import"],
                   f"--file {step}: features {feats}, not one Import")
    print("startup_file_smoke: ok")


if __name__ == "__main__":
    main()
