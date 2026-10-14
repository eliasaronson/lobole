#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: A4 exploded views, headless over JSON-RPC.

Checks the RPC surface (add/remove/list/set_explode_step/auto_explode/
set_explode), the pure-view-state contract (applying a view never changes
occurrence transforms), undo, and that views persist through save/reopen
while the applied view + factor stay transient.

Usage: exploded_smoke.py <path-to-designer-binary>
"""
import os
import subprocess
import sys
import tempfile

from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402




def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_exploded_")
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
        d.call("add_part", name="base")
        box(d, (0, 0, 0), (10, 10, 10), "Blk")
        d.call("add_part", name="cap")
        box(d, (0, 0, 0), (10, 10, 2), "Blk")
        d.call("add_assembly", name="stack")
        o1 = d.call("add_occurrence", part="base", fixed=True)["id"]
        o2 = d.call("add_occurrence", part="cap",
                    transform={"position": [0, 0, 10],
                               "orientation": [0, 0, 0, 1]})["id"]

        def transforms():
            return {o["id"]: o["transform"]["position"]
                    for o in d.call("list_occurrences")["occurrences"]}

        before = transforms()

        # --- author a view: manual step + radial auto-seed -------------------
        view = d.call("add_exploded_view", name="service")["id"]
        expect(view > o2, f"view id from the shared counter: {view}")
        v = d.call("set_explode_step", view=view, occurrence=o2,
                   offset=[0, 0, 25])
        expect(v["steps"] == [{"occurrence": o2, "offset": [0.0, 0.0, 25.0]}],
               f"manual step: {v}")
        v = d.call("auto_explode", view=view)
        expect(len(v["steps"]) == 2, f"auto seeded both occurrences: {v}")

        # --- applying the view is PURE display state -------------------------
        d.call("set_explode", view=view, factor=1.0)
        expect(transforms() == before,
               "exploding never touches occurrence transforms")
        st = d.call("list_exploded_views")
        expect(st["active"] == view and abs(st["factor"] - 1.0) < 1e-6,
               f"display state readable: {st}")
        d.call("set_explode", view=0)
        expect(d.call("list_exploded_views")["active"] == 0, "view off")

        # --- undo: the auto-seed and the manual step are separate steps ------
        d.call("undo")  # undoes auto_explode
        st = d.call("list_exploded_views")["views"][0]
        expect(st["steps"] == [{"occurrence": o2, "offset": [0.0, 0.0, 25.0]}],
               f"undo restored the manual-step state: {st}")

        # --- persistence: views survive; applied state does not --------------
        d.call("set_explode", view=view, factor=0.5)
        proj = os.path.join(tmp, "stack.dsgn")
        d.call("save_project", path=proj)
        expect(os.path.isdir(os.path.join(proj, "assemblies", "stack",
                                          "exploded")),
               "exploded/ directory written")
        d.call("new_project")
        d.call("open_project", path=proj)
        parts = {p["name"]: p for p in d.call("list_parts")["parts"]}
        d.call("set_active_part", index=parts["stack"]["index"])
        st = d.call("list_exploded_views")
        expect(len(st["views"]) == 1 and st["views"][0]["name"] == "service",
               f"view reopened: {st}")
        expect(st["views"][0]["steps"][0]["offset"] == [0.0, 0.0, 25.0],
               f"step survived: {st}")
        expect(st["active"] == 0 and abs(st["factor"] - 1.0) < 1e-6,
               f"applied view + factor are transient: {st}")

        # --- removal ----------------------------------------------------------
        vid = st["views"][0]["id"]
        d.call("remove_exploded_view", id=vid)
        expect(d.call("list_exploded_views")["views"] == [], "view removed")

        # #7: auto_explode with NO view no longer faults on a missing view — it
        # auto-creates a fresh exploded view, seeds it, and returns its id.
        expect(d.call("list_exploded_views")["views"] == [], "no views yet")
        auto = d.call("auto_explode")  # note: no `view` param
        expect("view" in auto and auto["view"] > 0,
               f"auto_explode auto-created a view: {auto}")
        expect(len(auto["steps"]) == 2, "auto-created view seeded both")
        views = d.call("list_exploded_views")["views"]
        expect(len(views) == 1 and views[0]["id"] == auto["view"],
               "auto_explode with no view added exactly one view")

        print("exploded_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
