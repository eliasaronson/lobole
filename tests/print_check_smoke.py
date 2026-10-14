#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: check_print over RPC, headless.

docs/tolerances/README.md#checking-a-print. The negative controls a check
needs to be one: a 60 degree overhang fails where a 45 degree flank passes
(and reads 45, off the B-rep, not a facet's 47), a print-in-place pair that
touches fails as one piece, a pair closer than the gap fails, and a part off
the bed fails. Then the limits it takes from the part's print, what an
unprinted part skips, and the refusals.

Usage: print_check_smoke.py <path-to-designer-binary>
"""
import math
import os
import sys
import tempfile

from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402

PLA = {"process": "fdm", "material": "pla"}


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def refused(d, needle, **kw):
    before = d.call("get_state")
    try:
        d.call("check_print", **kw)
    except RuntimeError as e:
        expect(needle in str(e),
               f"check_print refused, but not naming '{needle}': {e}")
        after = d.call("get_state")
        expect(before.get("features") == after.get("features"),
               "a refused check_print changed the part")
        return
    raise AssertionError(f"check_print was accepted: {kw}")


def wedge(d, name, deg):
    """A block 10 high whose +X side leans out over the bed at `deg` from
    vertical: a sketch in the XZ plane (V = +Z) extruded along -Y."""
    run = 10.0 * math.tan(math.radians(deg))
    pts = [(0, 0), (10, 0), (10 + run, 10), (0, 10)]
    sk = d.call("add_feature", feature={
        "type": "Sketch", "name": f"{name} profile",
        "plane": {"origin": [0, 0, 0], "normal": [0, -1, 0],
                  "uAxis": [1, 0, 0]},
        "entities": [{"type": "line", "a": list(pts[i]),
                      "b": list(pts[(i + 1) % 4])} for i in range(4)]})
    d.call("add_feature", feature={
        "type": "Extrude", "name": name, "sketch_id": sk["id"],
        "distance": 8.0, "mode": "new_body"})


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_print_check_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        d.call("new_project")

        # --- Overhangs: 45 reads 45 and passes, 60 fails ------------------
        d.call("add_part", name="flank 45")
        d.call("set_print_process", **PLA)
        wedge(d, "Wedge", 45.0)
        r = d.call("check_print")
        oh = r["overhang"]
        expect(abs(oh["worst_deg"] - 45.0) < 1e-6,
               f"a 45 degree flank reads 45, off the B-rep: {oh}")
        expect(oh["status"] == "pass" and oh["area_mm2"] == 0.0,
               f"45 passes: {oh}")
        expect(oh["limit_deg"] == 45.0 and oh["limit_from"] == "process",
               f"the limit is the process's: {oh}")
        expect(r["bed"]["status"] == "pass", f"on the bed: {r['bed']}")
        expect(r["ok"], f"the 45 degree wedge prints: {r}")
        expect(r["receipt"]["standing"] == "verified",
               f"a passing receipt: {r['receipt']}")

        d.call("add_part", name="flank 60")
        d.call("set_print_process", **PLA)
        wedge(d, "Wedge", 60.0)
        r = d.call("check_print")
        oh = r["overhang"]
        expect(oh["status"] == "fail", f"a 60 degree overhang fails: {oh}")
        expect(abs(oh["worst_deg"] - 60.0) < 1e-6, f"reads 60: {oh}")
        # The leaning face: 8 deep, 10 / cos(60) = 20 long.
        expect(abs(oh["area_mm2"] - 160.0) < 1.0, f"its area: {oh}")
        expect(oh["worst"]["face"] >= 0 and "element" in oh["worst"],
               f"where, as an element: {oh['worst']}")
        expect(not r["ok"] and r["receipt"]["standing"] == "failed",
               f"the part fails: {r['receipt']}")
        # A stated limit past it passes.
        r = d.call("check_print", max_overhang_deg=61.0)
        expect(r["overhang"]["status"] == "pass"
               and r["overhang"]["limit_from"] == "asked",
               f"a stated limit: {r['overhang']}")

        # --- Print-in-place: a gap, a fused pair, a near pair ---------------
        d.call("add_part", name="pair")
        d.call("set_print_process", **PLA)
        box(d, (0, 0, 0), (10, 10, 10), "A")
        box(d, (10.5, 0, 0), (10, 10, 10), "B")
        r = d.call("check_print")
        cl = r["clearance"]
        expect(cl["required_mm"] == 0.25 and cl["required_from"] == "process",
               f"the gap a side of fdm pla is 0.25: {cl}")
        expect(cl["status"] == "pass" and abs(cl["min_mm"] - 0.5) < 1e-6,
               f"0.5 apart keeps 0.25: {cl}")
        expect(r["shells"]["pieces"] == 2 and r["shells"]["status"] == "pass",
               f"two pieces: {r['shells']}")
        expect(r["chords"]["status"] == "pass"
               and abs(r["chords"]["deflection_mm"] - 0.025) < 1e-9
               and r["chords"]["deflection_from"] == "gap",
               f"export_stl's chords, a tenth of the gap: {r['chords']}")
        expect(r["ok"], f"the pair prints: {r}")
        # Coarser chords than a fifth of the gap fail.
        r = d.call("check_print", stl_deflection_mm=0.1)
        expect(r["chords"]["status"] == "fail", f"0.1 mm chords: {r['chords']}")

        d.call("add_part", name="fused pair")
        d.call("set_print_process", **PLA)
        box(d, (0, 0, 0), (10, 10, 10), "A")
        box(d, (10, 0, 0), (10, 10, 10), "B")
        r = d.call("check_print")
        expect(r["shells"]["status"] == "fail" and r["shells"]["pieces"] == 1
               and r["shells"]["expected"] == 2,
               f"a fused print-in-place pair is one piece: {r['shells']}")
        expect(r["shells"]["joined"] == [["A", "B"]]
               or r["shells"]["joined"] == [["B", "A"]],
               f"which bodies joined: {r['shells']}")
        expect(r["clearance"]["status"] == "fail", f"and no gap: {r}")

        d.call("add_part", name="near pair")
        d.call("set_print_process", **PLA)
        box(d, (0, 0, 0), (10, 10, 10), "A")
        box(d, (10.1, 0, 0), (10, 10, 10), "B")
        r = d.call("check_print")
        expect(r["clearance"]["status"] == "fail"
               and abs(r["clearance"]["min_mm"] - 0.1) < 1e-6,
               f"0.1 apart is under 0.25: {r['clearance']}")
        expect(r["shells"]["status"] == "pass", f"still two: {r['shells']}")
        # Only one body asked about: nothing to keep apart.
        r = d.call("check_print", bodies=["A"])
        expect(len(r["bodies"]) == 1 and r["clearance"]["status"] == "pass",
               f"one body: {r}")

        # --- Off the bed -----------------------------------------------------
        d.call("add_part", name="lifted")
        d.call("set_print_process", **PLA)
        box(d, (0, 0, 2), (10, 10, 10), "Up")
        r = d.call("check_print")
        expect(r["bed"]["status"] == "fail"
               and abs(r["bed"]["lowest_mm"] - 2.0) < 1e-6,
               f"2 mm off the bed: {r['bed']}")
        # Only the bed item fails: the faces are judged from the part's own
        # lowest point, so its underside is what it would stand on.
        expect(not r["bridges"]["faces"] and r["overhang"]["status"] == "pass",
               f"its underside is not a ceiling: {r}")
        # Printed along +X, its x = 0 face is the bed.
        r = d.call("check_print", direction=[2, 0, 0])
        expect(r["direction"] == [1.0, 0.0, 0.0], f"normalised: {r}")
        expect(r["bed"]["status"] == "pass", f"on x = 0: {r['bed']}")

        # --- An unprinted part ---------------------------------------------
        d.call("add_part", name="unprinted")
        box(d, (0, 0, 0), (10, 10, 10), "Block")
        r = d.call("check_print")
        expect(r["process"] is None, f"no process: {r}")
        for k in ("overhang", "bridges", "clearance", "chords"):
            expect(r[k]["status"] == "skipped"
                   and "set_print_process" in r[k]["note"],
                   f"{k} skipped, saying why: {r[k]}")
        expect(r["receipt"]["standing"] == "unverified",
               f"skipped items are unverified: {r['receipt']}")
        r = d.call("check_print", max_overhang_deg=45, max_bridge_mm=10,
                   min_gap_mm=0.2)
        for k in ("overhang", "bridges", "clearance"):
            expect(r[k]["status"] == "pass", f"{k} stated, run: {r[k]}")

        # --- Refusals --------------------------------------------------------
        refused(d, "direction", direction=[0, 0, 0])
        refused(d, "direction", direction=[0, 1])
        refused(d, "max_overhang_deg", max_overhang_deg=90)
        refused(d, "max_overhang_deg", max_overhang_deg=-5)
        refused(d, "min_gap_mm", min_gap_mm=0)
        refused(d, "no body", bodies=["Nope"])
        refused(d, "part", part="no such part")
        print("print_check_smoke: OK")
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    main()
