#!/usr/bin/env python3
"""Integration test: the drawings D1 sheet spine, driven headless.

Covers: creating a drawing tab (a third document kind) with an initial sheet
size + title block, list_drawings/list_parts reporting it, the clear error
when a part-only RPC runs on an active drawing tab, and a full save -> reopen
round-trip that preserves the drawing document and its sheet through the v2
project container.

Usage: drawing_smoke.py <path-to-designer-binary>
"""
import os
import subprocess
import sys
import tempfile
import json as jsonlib

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
    env.pop("DISPLAY", None)  # prove no display server is needed
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_dwg_")
    env["XDG_STATE_HOME"] = tmp
    app = subprocess.Popen([binary, "--headless", "--remote", str(port)],
                           env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    try:
        d = Designer.wait_for(port)
        expect(d.call("ping")["ok"], "ping")

        # --- A project with a part + a drawing -----------------------------
        d.call("new_project")  # starts with one part ("Part 1")
        d.call("add_feature", feature={
            "type": "Box", "name": "Blk", "position": [0, 0, 0],
            "width": 40, "height": 20, "depth": 20})

        dwg = d.call("create_drawing", name="bracket-dwg", sheet={
            "size": "a3", "landscape": True,
            "title_block": {"project": "Gearbox", "document": "Bracket",
                            "author": "E. Aronsson", "date": "2026-07-14"}})
        expect(dwg["id"] > 0, "drawing got a stable id")
        expect(dwg["size"] == "a3", f"sheet size applied: {dwg}")

        # list_drawings reports it; list_parts types it as a drawing.
        ld = d.call("list_drawings")["drawings"]
        expect(len(ld) == 1, f"one drawing: {ld}")
        expect(ld[0]["name"] == "bracket-dwg", f"named: {ld}")
        expect(ld[0]["views"] == 0 and ld[0]["dims"] == 0,
               f"empty of views/dims in D1: {ld}")

        lp = {p["name"]: p["type"] for p in d.call("list_parts")["parts"]}
        expect(lp.get("bracket-dwg") == "drawing", f"typed as drawing: {lp}")

        # The drawing tab is active. A part-only RPC must fail clearly.
        try:
            d.call("get_state")
            raise AssertionError("get_state must fail on an active drawing")
        except RuntimeError as e:
            expect("part" in str(e).lower(), f"clear error: {e}")

        # --- Save, reopen, verify the drawing survives ---------------------
        proj = os.path.join(tmp, "gearbox.dsgn")
        d.call("save_project", path=proj)

        # On disk: project.json v2 with a typed "drawing" document + its dir.
        with open(os.path.join(proj, "project.json")) as f:
            pj = jsonlib.load(f)
        types = {docu["name"]: docu["type"] for docu in pj["documents"]}
        expect(types.get("bracket-dwg") == "drawing", f"drawing typed: {types}")
        dwg_dir = os.path.join(proj, "drawings", "bracket-dwg")
        expect(os.path.isdir(dwg_dir), "drawing directory written")
        with open(os.path.join(dwg_dir, "index.json")) as f:
            idx = jsonlib.load(f)
        expect(idx["kind"] == "drawing", f"kind discriminator: {idx.get('kind')}")
        expect(idx["version"] == 30, f"format v30: {idx.get('version')}")
        expect(idx["sheet"]["size"] == "a3", f"sheet persisted: {idx['sheet']}")
        expect(idx["sheet"]["title_block"]["project"] == "Gearbox",
               "title block persisted")

        # Reopen in a fresh instance and re-check.
        d.call("new_project")
        d.call("open_project", path=proj)
        lp2 = {p["name"]: p for p in d.call("list_parts")["parts"]}
        expect(set(lp2) == {"Part 1", "bracket-dwg"},
               f"all tabs reopened: {set(lp2)}")
        expect(lp2["bracket-dwg"]["type"] == "drawing",
               "bracket-dwg still a drawing after reopen")
        ld2 = d.call("list_drawings")["drawings"]
        expect(len(ld2) == 1 and ld2[0]["name"] == "bracket-dwg",
               f"drawing survived reopen: {ld2}")

        print("drawing_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
