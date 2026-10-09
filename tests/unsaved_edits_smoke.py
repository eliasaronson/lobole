#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: an edit that is saved but regenerates nothing is UNSAVED.

`ping.dirty` is what the close prompt, the title bar and autosave all ask. It
used to compare each document's Version() with the version the last save
took -- and Version() moves only when the model regenerates. So a tolerance,
the print process, the units, the tube rules, a study and a configuration
that was not the active one were written by the next save and changed
nothing about `dirty`: closing the project dropped them without a prompt.

The reproduction: a saved cylinder, then set_tolerance -- `dirty` stayed
False. Every one of those edits is driven here over the RPC, each from a
freshly saved project, and must read dirty until the next save.
docs/versioning/README.md#unsaved-changes.

Usage: unsaved_edits_smoke.py <path-to-designer-binary>
"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402
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
    tmp = tempfile.mkdtemp(prefix="designer_unsaved_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        d.call("new_project")
        cylinder(d, (0, 0, 0), "z", 4, 30, "Pin")
        faces = [(b["created_by"], f["name"])
                 for b in d.call("list_elements")["bodies"]
                 for f in b.get("faces", []) if f.get("kind") == "cylinder"]
        expect(faces, "the cylinder has no round face to tolerance")
        fid, face = faces[0]
        proj = os.path.join(tmp, "pin.dsgn")

        def saved():
            d.call("save_project", path=proj)
            expect(d.call("ping")["dirty"] is False,
                   "a project just saved reads dirty")

        def unsaved(label):
            expect(d.call("ping")["dirty"] is True,
                   f"{label}: the edit is saved with the project, but the "
                   "project reads clean -- closing it would drop the edit "
                   "without asking")

        # --- the reproduction ---------------------------------------------
        saved()
        d.call("set_tolerance", feature_id=fid, element=face,
               **{"class": "h7"})
        unsaved("set_tolerance")

        # --- the rest of the class ----------------------------------------
        saved()
        d.call("set_print_process", process="fdm", material="petg")
        unsaved("set_print_process")

        saved()
        d.call("set_units", length="in")
        unsaved("set_units")

        saved()
        d.call("set_tube_rules", material="steel")
        unsaved("set_tube_rules")

        saved()
        cfg = d.call("add_configuration", name="Short")["id"]
        unsaved("add_configuration")
        saved()
        # Not the active configuration: nothing rebuilds, and it is still an
        # edit the file carries.
        d.call("set_configuration", id=cfg, suppressed_features=[fid])
        unsaved("set_configuration on a configuration that is not active")

        saved()
        study = d.call("add_study", name="Pull")["id"]
        unsaved("add_study")
        saved()
        d.call("set_study", study=study, mesh_size=4.0)
        unsaved("set_study")
        saved()
        d.call("add_load", study=study, type="fixed",
               face={"feature_id": fid, "element": face})
        unsaved("add_load")

        # An undo is an edit too: back past the save, the file differs.
        saved()
        d.call("undo")
        unsaved("undo of an unversioned edit")

        # And reading the model is not an edit.
        saved()
        d.call("get_state")
        d.call("list_elements")
        d.call("list_studies")
        expect(d.call("ping")["dirty"] is False,
               "reading the model made the project unsaved")

        print("unsaved_edits_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    main()
