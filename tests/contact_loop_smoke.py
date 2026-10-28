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
- the cam turns on one fixed panel (the left wall) and each follower's
  loop is grounded on another (the top): the relation is a row in the
  loop's solve, which once moved the rod back from where it set it;
- check_followers counts what moves with each follower: the rod, the link
  and the lever with the pins pressed into them, and not the figure
  fastened to the top, which here has no material and was refused as
  "moving" for the loop's leftover;
- the contact tables are built once, not again at samples where the loop
  leaves the follower a micrometre off;
- the bird's liftoff is the closed form's neighbourhood, not a second
  difference's noise in the middle of its rise.
"""
import importlib.util
import os
import subprocess
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
        # The generator prints with a profile (the parts are weighed as they
        # print): the P1S's, seeded into this instance's own config.
        seed = subprocess.run(
            [sys.executable, os.path.join(os.path.dirname(GEN),
                                          "print_coupon.py"),
             "--port", str(app.port), "--seed"], capture_output=True,
            text=True, timeout=300)
        expect(seed.returncode == 0, f"the profiles seed: {seed.stderr}")
        proc = {"profile": "K5 PLA on P1S"}
        s0 = S.solve(120.0, 0.0)
        gap = d.call("print_allowance", kind="gap", size_mm=s0["d_rod"],
                     **proc)["allowance_mm"]
        press = d.call("print_allowance", kind="press", size_mm=s0["d_pin"],
                       **proc)["allowance_mm"]
        s = S.solve(120.0, gap, press)
        S.layout(s, s["hub_floor"], s["bird_base_pressure"])
        for name, fn, *a in wc.PARTS:
            if fn is P.build_cam:
                a = a + [s[f"{a[0]}_base"]]
            wc.build_part(d, name, proc, "pla", fn, s, *a)
        # The figure stands fastened to the top: it does not move with the
        # axe's rod, so it needs no mass.
        d.call("set_active_part", part="woodcutter")
        d.call("set_material", feature_id=d.call(
            "list_elements")["bodies"][0]["created_by"], material="")
        o, m, rel, _ = wc.build_assembly(d, s)
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
            expect(moving == sorted([f"{k} rod", f"{k} link", lever,
                                     f"{k} rod pin", f"{k} lever pin"]),
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
