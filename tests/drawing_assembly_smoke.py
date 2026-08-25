#!/usr/bin/env python3
"""Integration test: drawing views of an assembly, and projected-child folding.

Covers the D2b additions on top of drawing_smoke's part views: a view whose
source is an ASSEMBLY (every occurrence's bodies projected at its solved
transform), and a projected child view folded from a base view (its position
locked to the base along the shared sheet axis, regardless of what it was
stored as). Headless — no display.

Usage: drawing_assembly_smoke.py <path-to-designer-binary>
"""
import os
import re
import socket
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402




def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_dwgasm_")
    env["XDG_STATE_HOME"] = tmp
    # ...and its own CONFIG dir, or the run edits the developer's real
    # preferences: set_settings persists immediately (headless runs no GUI
    # loop to debounce it), so a suite reset their font size to 120%.
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        expect(d.call("ping")["ok"], "ping")

        # A part + an assembly of two copies of it, 40 mm apart.
        d.call("new_project")
        d.call("add_part", name="block")
        d.call("add_feature", feature={
            "type": "Box", "name": "B", "position": [0, 0, 0],
            "width": 20, "height": 20, "depth": 20})
        d.call("add_assembly", name="asm")
        d.call("add_occurrence", part="block", fixed=True)
        d.call("add_occurrence", part="block",
               transform={"position": [40, 0, 0], "orientation": [0, 0, 0, 1]})
        d.call("solve_assembly")
        d.call("create_drawing", name="dwg", sheet={"size": "a3", "landscape": True})

        # --- A view sourced from the ASSEMBLY projects both occurrences ------
        av = d.call("add_drawing_view", source="asm", orientation="front",
                    position=[150, 150])
        upd = {v["name"]: v for v in d.call("update_drawing")["views"]}
        fv = upd["front"]
        expect(fv["has_geometry"] and not fv.get("error"),
               f"assembly view projected: {fv}")
        # A single box front view is 4 visible + 4 hidden = 8 curves; two
        # occurrences give ~twice that.
        expect(fv["curves"] >= 14, f"two occurrences ~2x a single box: {fv}")
        d.call("remove_drawing_view", id=av["id"])

        # --- Projected-child folding ----------------------------------------
        base = d.call("add_drawing_view", source="block", orientation="front",
                      position=[120, 200])
        # Store the child with a deliberately wrong X; folding must override it.
        child = d.call("add_drawing_view", source="block", orientation="top",
                       base_view=base["id"], position=[999, 120])
        upd2 = {v["id"]: v for v in d.call("update_drawing")["views"]}
        expect(upd2[base["id"]]["has_geometry"] and upd2[child["id"]]["has_geometry"],
               "base + child both projected")

        # Compare the rendered X-extents via the exported SVG (per-view groups).
        svg_path = os.path.join(tmp, "fold.svg")
        d.call("export_drawing", path=svg_path)
        svg = open(svg_path).read()
        xext = {}
        for vid, body in re.findall(r'<g id="view-(\d+)">(.*?)</g>', svg, re.S):
            xs = [float(pt.split(",")[0])
                  for pl in re.findall(r'points="([^"]+)"', body)
                  for pt in pl.split()]
            if xs:
                xext[int(vid)] = (min(xs), max(xs))
        bx, cx = xext[base["id"]], xext[child["id"]]
        # front and top share sheet-X (both map world +X -> sheet +X), so the
        # folded child aligns in X with its base despite the stored X=999.
        expect(abs(bx[0] - cx[0]) < 1.0 and abs(bx[1] - cx[1]) < 1.0,
               f"folded child aligns in X with base: base={bx} child={cx}")

        print("drawing_assembly_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
