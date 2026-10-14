#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: a press fit is not a clash, in every contact check.

A 6005 bearing in a housing bore modelled at its K7 seat's interference
(docs/tolerances/README.md#press-fits): the ring's ISO 492 band and the
bore's K7 allow 18 um, and the bore is modelled 18 um under the ring. The
named pair, the exact sweep, contact_report and an exact motion study must
each relieve it and list it as a press fit; the mesh checks say they relieve
nothing; and an H7 seat — a clearance fit for the same ring — turns the same
overlap back into a clash that says why, in every one of them, including a
motion result that was stored before the tolerance changed.

Usage: press_fit_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile

from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402

FIT = "Ø47 K7 / 6005 outside (ISO 492 normal)"


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def housing(d, bore):
    """An 80 x 80 x 12 block bored `bore` through on the Z axis."""
    d.call("add_part", name="Housing")
    box(d, (-40, -40, 0), (80, 80, 12), "Block")
    d.call("add_feature", feature={
        "type": "Sketch", "name": "Centre",
        "plane": {"origin": [0, 0, 12], "normal": [0, 0, 1],
                  "uAxis": [1, 0, 0]},
        "entities": [{"type": "circle", "center": [0, 0], "radius": 5}]})
    sk = {f["name"]: f["id"] for f in d.call("get_state")["features"]}["Centre"]
    d.call("add_feature", feature={
        "type": "Hole", "name": "Seat", "hole_kind": "simple",
        "sketch_id": sk, "diameter": bore, "through": True})
    found = [(b["created_by"], f["name"])
             for b in d.call("list_elements")["bodies"]
             for f in b.get("faces", []) if f.get("kind") == "cylinder"]
    expect(len(found) == 1, f"one bore: {found}")
    return found[0]


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_press_fit_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        d.call("new_project")
        hf, hname = housing(d, 46.982)
        d.call("set_tolerance", feature_id=hf, element=hname,
               **{"class": "K7"})
        d.call("add_part", name="Arm")
        box(d, (2, -2, 30), (28, 4, 4), "Arm")

        d.call("add_assembly", name="Rig")
        oh = d.call("add_occurrence", part="Housing", fixed=True)["id"]
        ob = d.call("add_bearing", designation="6005",
                    position=[0, 0, 0], orientation=[0, 0, 0, 1])["occurrence"]
        arm = d.call("add_occurrence", part="Arm")["id"]
        axle = d.call("add_mate", type="revolute", name="axle",
                      a={"occurrence": oh, "origin": True},
                      b={"occurrence": arm, "origin": True},
                      flipped=False)["id"]
        names = {o["id"]: o["name"]
                 for o in d.call("list_occurrences")["occurrences"]}
        bearing = names[ob]

        # --- The named pair ------------------------------------------------
        r = d.call("interference", a="Housing", b=bearing)
        expect(r["clash"] is False and r["volume"] < 1e-9,
               f"a press fit is not a clash: {r}")
        expect(r["state"] == "touching", f"pressed together: {r}")
        expect(r["press_fit_pairs"] == 1, f"one press fit: {r}")
        fit = r["press_fits"][0]
        expect(fit["fit"] == FIT, f"named by its bands: {fit}")
        expect(abs(fit["interference_mm"] - 0.018) < 1e-9 and
               abs(fit["max_interference_mm"] - 0.018) < 1e-9,
               f"at the top of the band: {fit}")
        # The ring's outside between its two 0.6 chamfers.
        expect(fit["volume_mm3"] > 0 and abs(fit["engaged_length_mm"] -
                                             10.8) < 1e-6,
               f"relieved over the ring's width: {fit}")
        m = d.request("interference", {"a": "Housing", "b": bearing,
                                       "mode": "mesh"})
        expect("relieves" in m, f"mesh says what it does not relieve: {m}")

        # --- The sweep -----------------------------------------------------
        r = d.call("interference")
        expect(r["clash"] is False and r["clashes"] == [],
               f"the sweep is clean: {r}")
        expect(len(r["press_fits"]) == 1 and
               {r["press_fits"][0]["a"], r["press_fits"][0]["b"]} ==
               {"Housing", bearing}, f"and lists the fit: {r}")
        expect(r["press_fits"][0]["fits"][0]["fit"] == FIT, f"{r}")
        m = d.request("interference", {"mode": "mesh"})
        expect("relieves" in m and "press_fits" not in m,
               f"a mesh sweep relieves nothing, and says so: {m}")

        # --- contact_report ------------------------------------------------
        r = d.call("contact_report")
        expect(r["clashes"] == [], f"no clash: {r['clashes']}")
        expect(len(r["press_fits"]) == 1, f"one press fit: {r['press_fits']}")
        pair = [p for p in r["pairs"]
                if {p["a"], p["b"]} == {"Housing", bearing}]
        expect(len(pair) == 1 and pair[0]["state"] == "touching" and
               pair[0]["press_fits"][0]["fit"] == FIT,
               f"the pair is in contact, by its fit: {pair}")

        # --- An exact motion study -----------------------------------------
        sid = d.call("add_motion_study", driver=axle, **{"from": 0}, to=90,
                     samples=4, name="turn", clash_mode="exact")["id"]
        r = d.call("run_motion_study", study=sid)
        expect(r["collision_status"] == "passed", f"clean: {r['collision_status']}"
               f" {r['rigid_clashes']} {r['samples'][0]}")
        expect(len(r["press_fits"]) == 1 and
               r["press_fits"][0]["fit"] == FIT and
               {r["press_fits"][0]["a"], r["press_fits"][0]["b"]} == {oh, ob},
               f"the run lists the press fit: {r['press_fits']}")

        # --- An H7 seat is a clearance fit: the same overlap clashes -------
        d.call("set_tolerance", part="Housing", feature_id=hf, element=hname,
               **{"class": "H7"})
        r = d.call("interference", a="Housing", b=bearing)
        expect(r["clash"] is True and r["press_fit_pairs"] == 0,
               f"a clearance fit's overlap is a clash: {r}")
        refused = r["press_fit_refused"]
        expect(len(refused) == 1 and "clearance" in refused[0]["reason"],
               f"and says why: {refused}")
        r = d.call("contact_report")
        expect(len(r["clashes"]) == 1 and r["press_fits"] == [],
               f"contact_report agrees: {r['clashes']} {r['press_fits']}")
        expect("press_fit_refused" in r["clashes"][0], f"{r['clashes']}")
        r = d.call("interference")
        expect(r["clash"] is True and
               "press_fit_refused" in r["clashes"][0], f"the sweep too: {r}")
        # A tolerance is an input of the stored run: it is not reused.
        r = d.call("run_motion_study", study=sid)
        expect(not r.get("reused"), "a tolerance edit invalidates the run")
        expect(r["collision_status"] == "failed" and r["press_fits"] == [],
               f"and the run now fails: {r['collision_status']}")
        print("press_fit_smoke: all checks passed")
        return 0
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
