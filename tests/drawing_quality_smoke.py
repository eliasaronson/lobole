#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A drawing of an assembly is affordable, or it says why not
(docs/drawings/README.md#what-a-view-costs).

Exact HLR is priced by SURFACE COMPLEXITY, not by part count, and that is
worth stating carefully because the obvious reading is the wrong one:

    16 boxes with a bore, one view       3.1 ms   <- linear in bodies
    ONE box + a 4-turn helical spring    6.4 s
    the single-cylinder engine (19)     85.1 s
    ...the same engine, 2 valves hidden  0.0 s    <- the springs WERE the bill

So it is not "assemblies are slow". Hide() intersects every projected edge
against every face, and a swept B-spline surface is orders of magnitude
dearer to intersect than a plane or a cylinder. `show_hidden: false` does not
help — it only filters the output. Caching between views is not available
either: HLR runs ONE pass across all bodies so occlusion is global, and every
view looks a different way.

So two levers, and this test holds both:

* a view's `quality: "fast"` runs polygonal HLR over the triangulations —
  orders of magnitude cheaper, and a LOOK (polylines, no source refs, so
  nothing can be dimensioned against them). Never substituted for exact
  silently: an unknown quality is refused by name.
* `update_drawing {budget_s}` bounds the whole update. OCC's Hide() has no
  cancel hook, so this rests on driving its PAIRWISE form ourselves — which
  must produce byte-identical output, and that is asserted here.

Usage: drawing_quality_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, HERE)
from designer_sdk import Plane, connect  # noqa: E402
from smoke_app import launch_app  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_dwgq_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        a = connect(app.port)
        a.new()
        p = a.part("blk")
        p.box("Top", width=40, depth=30, height=20, at=(0, 0), centered=False,
              name="B")
        # A hole, so there is a curved edge with hidden segments to classify.
        s = p.sketch(Plane.xy(-5), name="Bore")
        s.circle((20, 15), d=10.0)
        p.extrude(s.add(), 30.0, mode="cut", name="Bore")

        a.create_drawing(name="sheet")
        exact = a.add_drawing_view(source="blk", orientation="iso",
                                   position=[100, 150], name="exact")
        fast = a.add_drawing_view(source="blk", orientation="iso",
                                  position=[220, 150], name="fast",
                                  quality="fast")
        a.update_drawing(_timeout=300)
        views = {v["name"]: v for v in a.list_drawing_views()["views"]}

        # Both project the same solid; both must produce a real drawing.
        for name in ("exact", "fast"):
            expect(views[name]["curves"] > 0,
                   f"the {name} view projected nothing: {views[name]}")
            expect(not views[name].get("error"),
                   f"the {name} view failed: {views[name].get('error')}")
        # Same object, same size on the sheet — the fast one is a coarser
        # description of the same geometry, not a different one.
        eb, fb = views["exact"]["bbox"], views["fast"]["bbox"]
        for i in range(2):
            expect(abs((eb[1][i] - eb[0][i]) - (fb[1][i] - fb[0][i])) < 1.0,
                   f"fast and exact disagree about the object's size on axis "
                   f"{i}: {eb} vs {fb}")
        # And the quality each was made with is on the record.
        expect(views["exact"]["quality"] == "exact", views["exact"])
        expect(views["fast"]["quality"] == "fast", views["fast"])

        # ---- An unknown quality is REFUSED, naming what is accepted -------
        try:
            a.set_drawing_view(id=exact["id"], quality="quick")
            raise AssertionError("'quick' was accepted as a view quality")
        except RuntimeError as e:
            expect("exact" in str(e) and "fast" in str(e),
                   f"the refusal names both qualities: {e}")
        try:
            a.add_drawing_view(source="blk", orientation="top",
                               position=[100, 60], quality="")
            raise AssertionError("an empty quality was accepted")
        except RuntimeError as e:
            expect("unknown view quality" in str(e), str(e))

        # ---- The pairwise Hide is the SAME projection --------------------
        # It has to be: it is what makes budget_s possible, and if it differed
        # every dimension anchored to a curve would move. Re-projecting from
        # scratch must reproduce the curve count exactly.
        before = views["exact"]["curves"]
        a.set_drawing_view(id=exact["id"], position=[101, 150])
        a.update_drawing(_timeout=300)
        after = {v["name"]: v
                 for v in a.list_drawing_views()["views"]}["exact"]["curves"]
        expect(before == after,
               f"the exact projection is not reproducible: {before} then "
               f"{after} curves")

        # ---- A budget that cannot be met stops and SAYS SO ----------------
        # The SOURCE has to move, not the sheet: set_drawing_view re-projects
        # on the spot, so a sheet edit leaves every view fresh and the next
        # update_drawing has nothing to spend a budget on.
        a.set_drawing_view(id=fast["id"], quality="exact")
        p.activate()
        p.box(p.datum_plane("xy", 40, name="Boss plane"), width=10, depth=10,
              height=10, at=(0, 0), centered=False, name="Boss")
        a.set_active_part(part="sheet")
        t = time.time()
        r = a.update_drawing(budget_s=0.0000001, _timeout=300)
        elapsed = time.time() - t
        errs = [v.get("error", "") for v in r["views"]]
        expect(any("time budget" in e for e in errs),
               f"a budget of ~0 stops the update and says so: {errs}")
        expect(any("fast" in e for e in errs),
               f"and names the way through: {errs}")
        expect(elapsed < 60.0, f"the budget bounded the call: {elapsed:.1f} s")
        print("drawing_quality_smoke: ok")
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
