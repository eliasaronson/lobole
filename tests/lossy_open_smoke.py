#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: a project that opens without some of what is in it is
not saved back over itself without being asked to.

A project whose index lost an entry by hand -- the feature file is still in
features/, the index no longer lists it -- opens: the load keeps going past
the file it cannot place and says so in open_project's own reply,
`dropped`. A save back to the same path would delete that file for good,
so save_project there is refused, naming it, until the call says
{overwrite_lossy: true}; then it saves and the reply's `warning` repeats
what was lost. A save to another path is never blocked.
docs/versioning/README.md#a-lossy-load.

Usage: lossy_open_smoke.py <path-to-designer-binary>
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

from lobole.client import Client  # noqa: E402
from lobole.errors import Refused  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import cylinder  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_lossy_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        d.call("new_project")
        cylinder(d, (0, 0, 0), "z", 4, 30, "Pin")
        cylinder(d, (20, 0, 0), "z", 3, 10, "Peg")
        proj = os.path.join(tmp, "pin.dsgn")
        d.call("save_project", path=proj)

        # A clean open drops nothing.
        out = d.call("open_project", path=proj)
        expect(out.get("dropped") == [],
               f"a project that loads whole reports drops: {out}")

        # The index entry of the last feature removed by hand; its file
        # stays.
        index_path = os.path.join(proj, "index.json")
        with open(index_path) as f:
            index = json.load(f)
        removed = index["features"].pop()
        with open(index_path, "w") as f:
            json.dump(index, f, indent=2)
        kept = [n for n in os.listdir(os.path.join(proj, "features"))
                if n.startswith(f"{removed}-")]
        expect(len(kept) == 1, f"no feature file for id {removed}")
        orphan = os.path.join(proj, "features", kept[0])

        out = d.call("open_project", path=proj)
        dropped = out.get("dropped", [])
        expect(any(e["path"] == f"features/{kept[0]}" and not e["derived"]
                   for e in dropped),
               f"open_project did not report the unlisted file: {dropped}")

        # Saving over it is refused, naming the file; nothing is written.
        try:
            d.call("save_project", path=proj)
            raise AssertionError("save_project over a lossy open was not "
                                 "refused")
        except Refused as e:
            expect(kept[0] in str(e),
                   f"the refusal does not name the file: {e}")
        expect(os.path.exists(orphan),
               "a refused save deleted the file it was protecting")

        # Saving elsewhere is never blocked.
        copy = os.path.join(tmp, "copy.dsgn")
        out = d.call("save_project", path=copy)
        expect("warning" not in out, f"a save elsewhere warned: {out}")
        expect(os.path.exists(orphan), "a save elsewhere touched the original")

        # With overwrite_lossy it saves, and says what it lost.
        d.call("open_project", path=proj)
        out = d.call("save_project", path=proj, overwrite_lossy=True)
        expect(kept[0] in out.get("warning", ""),
               f"the overwrite did not repeat what was lost: {out}")
        expect(not os.path.exists(orphan),
               "an overwrite left the unlisted file behind")
        # Saved over, the project is whole again: a save goes straight on.
        out = d.call("open_project", path=proj)
        expect(out.get("dropped") == [], f"still dropping: {out}")
        d.call("save_project", path=proj)
        print("lossy_open_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except subprocess.TimeoutExpired:
            app.kill()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
