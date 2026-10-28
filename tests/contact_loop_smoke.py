#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Cam followers that drive mate loops, headless: the woodcutter
automaton's mechanism (generators/woodcutter), built by the generator's own
code, run over one crank turn in mesh clash mode and checked by
check_followers (docs/assemblies/relations.md#the-follower-check).

What it holds the app to, each a bug the woodcutter found:
- the study is clean: a cam and its follower that the mesh calls
  overlapping at their tangency are judged by the follower check's cuts
  (the bird's cam pierced its knife on the concave flanks at any
  deflection), and the axe's drop and landing, which its loop inherits,
  are not branch jumps (nor is the loop solve's leftover where the rise
  starts from rest);
- check_followers counts what moves with each follower: the rod, the link
  and the lever, and not the figure fastened to the box, which here has no
  material and was refused as "moving" for the loop's leftover;
- the contact tables are built once, not again at samples where the loop
  leaves the follower a micrometre off;
- the bird's liftoff is the closed form's neighbourhood, not a second
  difference's noise in the middle of its rise.
"""
import importlib.util
import os
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402

GEN = os.path.join(os.path.dirname(HERE), "generators", "woodcutter")


def load(name):
    spec = importlib.util.spec_from_file_location(
        f"woodcutter_{name}", os.path.join(GEN, f"{name}.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="lobole_contact_loop_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    sys.path.insert(0, GEN)
    sys.path.insert(0, os.path.dirname(GEN))
    wc = load("build")
    S, P = wc.S, wc.P
    app = launch_app(binary, env=env)
    t0 = time.perf_counter()
    try:
        d = Client.wait_for(app.port)
        d.call("new_project")
        proc = {"process": "fdm", "material": "pla"}
        s = S.solve(120.0, 0.5)
        S.layout(s, s["hub_floor"], s["bird_base_pressure"])
        for name, fn, *a in (("box", P.build_box), ("camshaft", P.build_shaft),
                             ("axe cam", P.build_cam, "axe", s["axe_base"]),
                             ("bird cam", P.build_cam, "bird",
                              s["bird_base"]),
                             ("axe rod", P.build_rod, "axe"),
                             ("bird rod", P.build_rod, "bird"),
                             ("axe link", P.build_link, "axe"),
                             ("bird link", P.build_link, "bird"),
                             ("arms", P.build_arms), ("woodcutter",
                                                      P.build_woodcutter),
                             ("bird", P.build_bird), ("perch", P.build_perch),
                             ("log", P.build_log)):
            wc.build_part(d, name, proc, "pla", fn, s, *a)
        # The figure stands fastened to the box: it does not move with the
        # axe's rod, so it needs no mass.
        d.call("set_active_part", part="woodcutter")
        d.call("set_material", feature_id=d.call(
            "list_elements")["bodies"][0]["created_by"], material="")
        o, m, rel, _ = wc.build_assembly(d)
        turn = wc.add_study(d, m, "one crank turn", 0.0, 360.0, 49,
                            s["g"] / 5.0)
        res = d.call("run_motion_study", study=turn)
        res = res.get("result", res)
        bad = [(x["value"], [(c["a_name"], c["b_name"])
                             for c in x["clashes"]], x.get("branch_jump"))
               for x in res["samples"]
               if x.get("clashes") or x.get("branch_jump")]
        expect(res["clean"] and not bad and res["branch_jumps"] == 0,
               f"one crank turn is clean: {bad}")
        touching = {tuple(sorted((c["a_name"], c["b_name"])))
                    for x in res["samples"] for c in x.get("touching", [])}
        expect(("bird cam", "bird rod") in touching,
               f"the bird's rod touches its cam: {touching}")
        out = d.call("check_followers", study=turn, speed_rpm=120.0)
        by = {r["follower"]: r for r in out["relations"]}
        for k, lever in (("axe", "arms"), ("bird", "bird")):
            r = by[o[f"{k} rod"]]
            moving = sorted(x["name"] for x in r["gravity"]["moving"])
            expect(moving == sorted([f"{k} rod", f"{k} link", lever]),
                   f"what moves with the {k} rod: {moving}")
            expect(r["touching"], f"the {k} rod on its cam: {r['failures']}")
        bird = by[o["bird rod"]]["gravity"]
        # Rod alone: sqrt(g b^2 / (2 pi h)) = 197 rpm at b 85 deg, h 8 mm;
        # the bird's lever takes some of gravity's hold, never half of it.
        closed = S.liftoff_rod_rpm(s["h_bird"], 85.0 * 3.141592653589793 /
                                   180.0)
        expect(0.7 * closed < bird["liftoff_rpm"] <= closed * 1.01,
               f"the bird's liftoff {bird['liftoff_rpm']} against the bare "
               f"rod's {closed}")
        expect(not (S.BIRD_PECK_FROM_DEG < bird["liftoff_at_cam_deg"] <
                    S.BIRD_PECK_FROM_DEG + S.BIRD_PECK_DEG / 2.0),
               f"no liftoff while the peck speeds up: "
               f"{bird['liftoff_at_cam_deg']}")
        drop = by[o["axe rod"]]["gravity"]["drops"][0]
        expect(drop["lands"], f"the axe lands in its dwell at 120: {drop}")
        d.close()
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()
    print(f"contact_loop_smoke: the woodcutter's followers and their loops, "
          f"checked ({time.perf_counter() - t0:.0f} s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
