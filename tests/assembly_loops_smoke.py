#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: A6 mate LOOPS, driven headless over JSON-RPC.

The A3 pass reported every loop as "over-constrained (mate loop conflict)" and
stopped there. A6 closes them, and this covers what the wire now says about
one: how many mates closed a loop, how many degrees of freedom are left,
whether the loops closed and by how much they missed, and which mates were
redundant BUT CONSISTENT.

The mechanism maths (four-bar branches, rank-based DOF) lives in the C++
test_assembly_loops.cpp; this exercises the RPC layer, so every connector is an
origin anchor or a named Frame -- no toponaming knowledge needed.

Usage: assembly_loops_smoke.py <path-to-designer-binary>
"""
import math
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


def two_parts(d, name):
    """A fresh assembly holding one grounded box and one parked far away."""
    d.call("add_assembly", name=name)
    o1 = d.call("add_occurrence", part="base", fixed=True)["id"]
    o2 = d.call("add_occurrence", part="arm",
                transform={"position": [50, 50, 50],
                           "orientation": [0, 0, 0, 1]})["id"]
    return o1, o2


def origin_mate(d, kind, a, b, **kw):
    return d.call("add_mate", type=kind,
                  a={"occurrence": a, "origin": True},
                  b={"occurrence": b, "origin": True},
                  flipped=False, **kw)["id"]


def link_part(d, name, frames):
    """A part holding nothing but named frames: {name: (origin, z, x)}."""
    d.call("add_part", name=name)
    for fname, (origin, z, x) in frames.items():
        d.call("add_feature", feature={
            "type": "Frame", "name": fname, "kind": "explicit",
            "origin": origin, "z_axis": z, "x_axis": x})


def four_bar(d):
    z, x = [0, 0, 1], [1, 0, 0]
    t = math.radians(30.0)
    link_part(d, "ground", {"p0": ([0, 0, 0], z, x),
                            "p1": ([100, 0, 0], z, x)})
    link_part(d, "crank", {"p0": ([0, 0, 0], z, x),
                           "p1": ([30, 0, 0], z, x)})
    link_part(d, "crank_bent", {
        "p0": ([0, 0, 0], z, x),
        "p1": ([30, 0, 0], [math.sin(t), 0, math.cos(t)],
               [math.cos(t), 0, -math.sin(t)])})
    link_part(d, "coupler", {"p0": ([0, 0, 0], z, x),
                             "p1": ([90, 0, 0], z, x)})
    link_part(d, "rocker", {"p0": ([0, 0, 0], z, x),
                            "p1": ([60, 0, 0], z, x)})

    def build(crank_part):
        d.call("add_assembly", name="linkage_" + crank_part)
        g = d.call("add_occurrence", part="ground", fixed=True)["id"]
        c = d.call("add_occurrence", part=crank_part)["id"]
        k = d.call("add_occurrence", part="coupler")["id"]
        r = d.call("add_occurrence", part="rocker")["id"]

        def joint(name, a, fa, b, fb, rot):
            return d.call("add_mate", type="revolute", name=name,
                          a={"occurrence": a, "frame": fa},
                          b={"occurrence": b, "frame": fb},
                          flipped=False, rotation_deg=math.degrees(rot))
        joint("main bearing", g, "p0", c, "p0", 1.2)
        pin = joint("crank pin", c, "p1", k, "p0", -0.8)["id"]
        joint("rocker pin", k, "p1", r, "p1", 1.0)
        last = joint("rocker pivot", g, "p1", r, "p0", 1.0)
        return pin, last

    pin, last = build("crank")
    expect(last["errored_mates"] == 0, f"the correct linkage closes: {last}")
    r = d.call("solve_assembly")
    expect(r["loops_closed"] is True and r["dof"] == 1,
           f"a four-bar with one DOF: {r}")
    expect("suspects" not in r, f"nothing to suspect when it closes: {r}")

    pin, last = build("crank_bent")
    cut = last["id"]
    # add_mate's own response already carries the diagnosis: the mate that
    # broke the loop is reported, and its error names the one to fix.
    errored = {e["id"]: e for e in last.get("errored", [])}
    expect(cut in errored, f"the closing mate is the one flagged: {last}")
    msg = errored[cut]["error"]
    expect("does not close" in msg, f"the plain words stay: {msg}")
    expect("'crank pin' connector A (crank_bent" in msg,
           f"...and it names the tilted connector: {msg}")
    print(f"  add_mate error: {msg}")

    r = d.call("solve_assembly")
    expect(r["loops_closed"] is False, f"it cannot close: {r}")
    suspects = r["suspects"]
    expect(len(suspects) == 1, f"one suspect: {suspects}")
    s = suspects[0]
    expect(s["mate"] == pin and s["name"] == "crank pin", f"the pin: {s}")
    expect(s["connector"] == "a", f"its crank-side connector: {s}")
    expect(abs(s["tilt_deg"] - 30.0) < 0.5, f"by 30 degrees: {s}")
    expect(s["sole"] is True and s["closes"] == [cut], f"of that loop: {s}")
    mates = {m["id"]: m for m in r["mates"]}
    expect(abs(mates[cut]["residual_mm"] - r["residual_mm"]) < 1e-9,
           f"the cut mate carries its own residual: {mates[cut]}")
    expect("residual_mm" not in mates[pin],
           f"a tree mate has no closure residual: {mates[pin]}")
    listed = {m["id"]: m for m in d.call("list_mates")["mates"]}
    expect(abs(listed[cut]["residual_mm"] - r["residual_mm"]) < 1e-9,
           f"list_mates reports it too: {listed[cut]}")
    print(f"  suspect: {s}, residual {r['residual_mm']:.3f} mm")


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)  # prove no display server is needed
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_loops_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        expect(d.call("ping")["ok"], "ping")

        d.call("new_project")
        d.call("add_part", name="base")
        box(d, (0, 0, 0), (4, 4, 4), "Blk")
        d.call("add_part", name="arm")
        box(d, (0, 0, 0), (2, 6, 2), "Blk")

        # --- Three bolts through a flange ----------------------------------
        # Three identical fastened mates between the same pair. Two of them
        # close loops; both are satisfied and say nothing new. Before A6 this
        # was two mates in an "over-constrained" error state, which is why
        # add_fastener had to DROP the mates for bolts two and three.
        o1, o2 = two_parts(d, "flange")
        origin_mate(d, "fastened", o1, o2)
        m2 = origin_mate(d, "fastened", o1, o2)
        m3 = origin_mate(d, "fastened", o1, o2)

        r = d.call("solve_assembly")
        expect(r["loop_mates"] == 2, f"two mates closed a loop: {r}")
        expect(r["loops_closed"] is True, f"consistent, so closed: {r}")
        expect(r["dof"] == 0, f"a bolted flange has no freedom left: {r}")
        expect(r["errored_mates"] == 0,
               f"a consistent loop is NOT an error: {r}")
        expect(r["residual_mm"] < 1e-3, f"closes exactly: {r}")
        expect(sorted(r["redundant_mates"]) == sorted([m2, m3]),
               f"names WHICH mates are redundant: {r}")
        for mate in r["mates"]:
            expect("error" not in mate, f"no mate is errored: {mate}")

        # --- Two rails under one carriage ----------------------------------
        # Two sliders along the same axis. The loop closes only where the
        # offsets agree, leaving one DOF: slide them together. The cut mate is
        # only PARTLY dependent, so it must not be called redundant.
        o1, o2 = two_parts(d, "rails")
        a = origin_mate(d, "slider", o1, o2, offset=0.0)
        b = origin_mate(d, "slider", o1, o2, offset=10.0)

        r = d.call("solve_assembly")
        expect(r["loop_mates"] == 1, f"one cut mate: {r}")
        expect(r["loops_closed"] is True, f"solvable by moving the DOF: {r}")
        expect(r["dof"] == 1, f"one freedom left on the rails: {r}")
        expect(r["errored_mates"] == 0, f"nothing errored: {r}")
        expect(r["redundant_mates"] == [],
               f"a partly-dependent mate is not redundant: {r}")
        mates = {m["id"]: m for m in d.call("list_mates")["mates"]}
        expect(abs(mates[a]["offset"] - mates[b]["offset"]) < 1e-3,
               f"the solve moved the offsets into agreement: {mates}")

        # --- A loop that genuinely conflicts -------------------------------
        # Two fastened mates demanding placements 7 mm apart. This one IS an
        # error -- and it says by how much, because 7 mm is a modelling
        # mistake and 0.02 mm is a part tolerance.
        o1, o2 = two_parts(d, "conflict")
        origin_mate(d, "fastened", o1, o2, offset=0.0)
        bad = origin_mate(d, "fastened", o1, o2, offset=7.0)

        r = d.call("solve_assembly")
        expect(r["loop_mates"] == 1, f"one cut mate: {r}")
        expect(r["loops_closed"] is False, f"it cannot close: {r}")
        expect(abs(r["residual_mm"] - 7.0) < 1e-3,
               f"reports how far off, in mm: {r}")
        expect(r["errored_mates"] == 1, f"exactly the one mate: {r}")
        expect(r["redundant_mates"] == [],
               f"a conflict is never a redundancy: {r}")
        errored = [m for m in r["mates"] if "error" in m]
        expect(len(errored) == 1 and errored[0]["id"] == bad,
               f"the SECOND mate is the one flagged: {errored}")
        expect("does not close" in errored[0]["error"],
               f"the message says what happened: {errored[0]}")
        expect("7" in errored[0]["error"],
               f"...and by how much: {errored[0]}")

        # --- A tilted connector is named, not the mate that fails ----------
        # A crank-rocker built from Frame features (ground pivots 100 apart,
        # crank 30, coupler 90, rocker 60). Correct, it closes with nothing
        # to suspect. With the crank pin's frame tilted 30 degrees, the loop
        # fails on the LAST mate added -- the rocker pivot -- and the error
        # names the crank pin, which is where the fault is.
        four_bar(d)

        # --- A loop-free assembly says nothing about loops -----------------
        o1, o2 = two_parts(d, "tree")
        origin_mate(d, "fastened", o1, o2)
        r = d.call("solve_assembly")
        expect("loop_mates" not in r,
               f"no loop keys at all on a mate tree: {r}")
        expect(r["errored_mates"] == 0, f"clean: {r}")

        print("assembly_loops_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
