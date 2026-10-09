#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: V1 contact & connectivity over RPC, driven headless.

Covers: interference's distance/state fields on part and assembly tabs, and
contact_report — components (a floating occurrence is a second component),
near-miss detection, clash listing, and the touch/near tolerances. This is
the RPC-level gate for the class of defect the dock dogfooding round hit:
checks that passed BECAUSE nothing touched.

Usage: contact_smoke.py <path-to-designer-binary>
"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box, cylinder  # noqa: E402




def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def refused(d, method, needle, **kw):
    try:
        d.call(method, **kw)
    except RuntimeError as e:
        expect(needle in str(e),
               f"{method} refused, but not naming '{needle}': {e}")
        return
    raise AssertionError(f"{method} was accepted: {kw}")


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_contact_")
    env["XDG_STATE_HOME"] = tmp
    # ...and its own CONFIG dir, or the run edits the developer's real
    # preferences: set_settings persists immediately (headless runs no GUI
    # loop to debounce it), so a suite reset their font size to 120%.
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        d.call("new_project")

        # --- Part tab: two bodies, touching then apart ---------------------
        box(d, (0, 0, 0), (10, 10, 10), "A")
        box(d, (10, 0, 0), (10, 10, 10), "B")
        r = d.call("interference", a=0, b=1)
        expect(r["state"] == "touching", f"tangent boxes: {r}")
        expect(abs(r["distance"]) < 1e-6, f"tangent distance: {r}")
        expect(not r["clash"], f"tangent clash: {r}")

        box(d, (25, 0, 0), (10, 10, 10), "C")
        r = d.call("interference", a=1, b=2)
        expect(r["state"] == "clear", f"apart boxes: {r}")
        expect(abs(r["distance"] - 5.0) < 1e-6, f"apart distance: {r}")

        # Round 7: BODY-level connectivity on a part tab — the class the
        # occurrence sweep cannot see (a bracket floating inside one part).
        rep = d.call("contact_report")
        expect(rep["scope"] == "bodies", f"part scope: {rep}")
        expect(not rep["connected"], "C floats 5 mm off: two components")
        sizes = sorted(len(c) for c in rep["components"])
        expect(sizes == [1, 2], f"components: {rep['components']}")

        # ...and check_geometry warns about it BY DEFAULT (round-8 retro:
        # the floating-plate class must be opt-out, not opt-in).
        cg = d.call("check_geometry")
        expect(cg["ok"], "geometry itself is valid")
        expect(not cg["connected"], f"floating body flagged: {cg}")
        expect(any("touch nothing" in w for w in cg.get("warnings", [])),
               f"warning names the island: {cg.get('warnings')}")

        # Bodies whose BOUNDING BOXES overlap completely — a rod inside a
        # sleeve — are what the bbox prefilter cannot separate, so they are
        # the pairs the connectivity pass actually pays for. It used to pay
        # an exact boolean plus BRepExtrema for each: on a 36-spoke bicycle
        # wheel (rim, spoked hub, tyre — all concentric) check_geometry took
        # 96 s, of which 0.2 was the B-Rep check it is named after. The
        # triangles decide it now, and these two cases are what that must
        # keep getting right.
        d.call("add_part", name="Sleeve")
        d.call("set_active_part", name="Sleeve")
        cylinder(d, (0, 0, 0), "z", 10, 40, "Sleeve")
        cylinder(d, (0, 0, -1), "z", 6, 42, "Bore")
        d.call("add_feature", feature={
            "type": "Boolean", "name": "Tube", "target_id":
            d.call("get_state")["features"][0]["id"], "tool_id":
            d.call("get_state")["features"][1]["id"], "op": "subtract"})
        cylinder(d, (0, 0, 5), "z", 6, 30, "Rod")
        cg = d.call("check_geometry")
        expect(cg["connected"], f"a rod touching its sleeve's bore: {cg}")

        cylinder(d, (0, 0, 8), "z", 3, 10, "Free")
        cg = d.call("check_geometry")
        expect(not cg["connected"],
               f"a 3 mm rod floating inside the same bore: {cg}")

        # --- Assembly: bolted / floating / near-miss / clash ---------------
        part = d.call("list_parts")["parts"][0]["name"]
        d.call("add_assembly", name="Rig")
        d.call("set_active_part", name="Rig")

        def place(name, x):
            return d.call("add_occurrence", part=part, name=name, fixed=True,
                          transform={"position": [x, 0, 0],
                                     "orientation": [0, 0, 0, 1]})
        # The part spans x 0..35 (three boxes). Chain: P1 at 0, P2 at 35
        # (tangent), P3 at 70.5 (0.5 near-miss), P4 at 200 (floating).
        place("P1", 0)
        place("P2", 35)
        place("P3", 70.5)
        p4_id = place("P4", 200)["id"]

        r = d.call("interference", a="P1", b="P2")
        expect(r["state"] == "touching", f"P1/P2: {r}")
        r = d.call("interference", a="P1", b="P4")
        expect(r["state"] == "clear" and r["distance"] > 100, f"P1/P4: {r}")

        rep = d.call("contact_report")
        expect(rep["occurrences"] == 4, f"count: {rep['occurrences']}")
        expect(not rep["connected"], "P3/P4 float, must not be connected")
        comps = sorted(sorted(c) for c in rep["components"])
        expect(["P1", "P2"] in comps, f"components: {comps}")
        expect(["P3"] in comps and ["P4"] in comps, f"components: {comps}")
        near = [(m["a"], m["b"]) for m in rep["near_misses"]]
        expect(("P2", "P3") in near or ("P3", "P2") in near,
               f"near_misses: {rep['near_misses']}")
        expect(rep["clashes"] == [], f"clashes: {rep['clashes']}")
        expect("unmated_contacts" not in rep,
               "mate diff must be absent without mates")

        # Overlap P4 onto P3's tail: one clash, and the graph unites them.
        d.call("set_occurrence_transform", id=p4_id,
               transform={"position": [100, 0, 0],
                          "orientation": [0, 0, 0, 1]})
        rep = d.call("contact_report")
        expect(len(rep["clashes"]) == 1, f"clashes: {rep['clashes']}")
        comps = sorted(sorted(c) for c in rep["components"])
        expect(["P3", "P4"] in comps, f"clash unites: {comps}")

        # WHERE: every clash says where its overlap is, in world mm. P3 spans
        # x 70.5..105.5 and P4 100..135, so the overlap lies in x 100..105.5
        # — in contact_report, the named pair and the standing sweep alike.
        def inside_overlap(c, what):
            lo, hi, mid = c["bbox_min_mm"], c["bbox_max_mm"], c["centroid_mm"]
            expect(lo[0] > 100 - 1e-3 and hi[0] < 105.5 + 1e-3,
                   f"{what}: the overlap's x bounds are P3's tail: {c}")
            expect(all(lo[i] - 1e-6 <= mid[i] <= hi[i] + 1e-6
                       for i in range(3)),
                   f"{what}: the centroid lies in the bounds: {c}")
        inside_overlap(rep["clashes"][0], "contact_report")
        inside_overlap(d.call("interference", a="P3", b="P4"), "named pair")
        sweep = d.call("interference")
        expect(len(sweep["pairs"]) == 1, f"one standing clash: {sweep}")
        inside_overlap(sweep["pairs"][0], "sweep")
        # A mesh sweep knows only THAT, and says where it could be — the
        # region both boxes share — rather than a site it does not know.
        mesh = next(p for p in d.call("interference", mode="mesh")["pairs"]
                    if {p["a"], p["b"]} == {"P3", "P4"})
        expect("centroid_mm" not in mesh and "volume" not in mesh,
               f"a mesh hit invents no site: {mesh}")
        expect(abs(mesh["within_min_mm"][0] - 100) < 1e-3
               and abs(mesh["within_max_mm"][0] - 105.5) < 1e-3,
               f"the shared box is P3's tail: {mesh}")

        # A generous near_tol turns the 0.5 gap into a connection? No —
        # near_tol only widens REPORTING; connectivity stays touch_tol.
        rep = d.call("contact_report", near_tol=50.0)
        expect(not rep["connected"], "near_tol must not fake connectivity")
        # ...but touch_tol does: 1.0 swallows the 0.5 near-miss gap.
        rep = d.call("contact_report", touch_tol=1.0)
        comps = sorted(sorted(c) for c in rep["components"])
        expect(["P1", "P2", "P3", "P4"] in comps,
               f"touch_tol widens contact: {comps}")

        # --- The sweeps are BOUNDED and say what they covered --------------
        # Both used to be O(occurrences^2) exact OpenCASCADE checks with no
        # bound: on a 253-occurrence imported arm neither returned, and the
        # server could not answer `ping` while one ran.
        # Six copies overlapping each other (the part spans x 0..35), six
        # far away: the far ones must cost a box test, not a boolean.
        d.call("add_assembly", name="grid")
        d.call("set_active_part", part="grid")
        for i in range(6):
            d.call("add_occurrence", part="Part 1", fixed=True,
                   transform={"position": [i * 5.0, 0, 0]})
        for i in range(6):
            d.call("add_occurrence", part="Part 1", fixed=True,
                   transform={"position": [1000.0 + i * 500.0, 0, 0]})
        rep = d.call("interference")
        pairs = 12 * 11 // 2
        expect(rep["pairs_box_cleared"] + rep["pairs_checked"] +
               rep.get("pairs_unchecked", 0) == pairs,
               f"every pair accounted for: {rep}")
        expect(rep["pairs_box_cleared"] >= 51,
               f"parts far apart are box-cleared, not booleaned: {rep}")
        expect(rep["clash"], f"the overlapping six clash: {rep}")
        expect(not rep.get("truncated"), f"a 12-part grid completes: {rep}")
        # A budget of one: the answer says so instead of pretending.
        rep = d.call("interference", max_pairs=1)
        expect(rep["pairs_checked"] == 1 and rep.get("truncated"),
               f"max_pairs bounds the sweep: {rep}")
        expect("mm apart" in rep["truncation"],
               f"truncation names what was left: {rep['truncation']}")
        rep = d.call("contact_report", max_pairs=1)
        expect(rep["pairs_checked"] == 1 and rep.get("truncated"),
               f"contact_report honours max_pairs: {rep}")

        # mode: mesh (G3) — the same verdict, from triangles, saying which
        # mode answered and to what deflection. It never reports a
        # volume: one measured off a tessellation is the tessellation's.
        exact = d.call("interference")
        mesh = d.call("interference", mode="mesh")
        expect(mesh["mode"] == "mesh" and exact["mode"] == "exact",
               f"each answer names the mode that decided it: {mesh}, {exact}")
        expect(mesh["clash"] == exact["clash"],
               f"mesh agrees with exact on this grid: {mesh} vs {exact}")
        expect({(p["a"], p["b"]) for p in mesh["pairs"]} ==
               {(p["a"], p["b"]) for p in exact["pairs"]},
               f"pair for pair: {mesh['pairs']} vs {exact['pairs']}")
        expect(all("volume" not in p for p in mesh["pairs"]),
               f"and reports no volume: {mesh['pairs'][:2]}")
        expect(mesh.get("deflection_mm", 0) > 0,
               f"but says how close 'clear' was decided: {mesh}")
        refused(d, "interference", "must be one of: exact, mesh", mode="wibble")

        print("contact_smoke: all checks passed")
        return 0
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
