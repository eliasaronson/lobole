#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Placing a fastener, bearing or retaining ring is one undo step
(docs/parts/README.md#placing-is-one-undo-step).

None of `add_fastener`, `fit_fastener`, `add_bearing` or
`add_retaining_ring` took an assembly snapshot, so `undo` skipped the
placement and took back whatever came before it — here, the plate placed
just before. Each placement below is undone and must leave exactly what was
there before it; a fitted joint (screw, nut and the mate between the plates)
goes in one step.

Usage: placement_undo_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from lobole.sdk import Plane, connect  # noqa: E402
from smoke_app import launch_app  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def plate(app, name, thickness):
    """A square plate with an M10 clearance hole through it, and frames on
    both faces."""
    p = app.part(name)
    p.box("Top", width=80, depth=80, height=thickness, at=(-40, -40),
          centered=False, name="P")
    s = p.sketch(Plane.xy(-5.0), name="Hole")
    s.circle((0, 0), d=11.0)
    p.extrude(s.add(), thickness + 10.0, mode="cut", name="Hole")
    p.frame("top", origin=(0, 0, thickness), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("bottom", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    return p


def contents(app):
    """What the assembly holds: its occurrences' names and its mate count."""
    occs = sorted(o["name"]
                  for o in app.list_occurrences()["occurrences"])
    return occs, len(app.call("list_mates")["mates"])


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_placement_undo_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        a = connect(app.port)
        a.new()
        lower = plate(a, "lower", 20.0)
        upper = plate(a, "upper", 20.0)
        rig = a.assembly("rig")
        lo = rig.place(lower, fixed=True)
        up = rig.place(upper, at=(0, 0, 20))
        a.solve_assembly()
        base = contents(a)
        expect(base == (["lower", "upper"], 0), f"the rig: {base}")

        placements = [
            ("add_fastener", dict(standard="iso4762", size="M10",
                                  length=30.0, position=[0, 0, 60])),
            ("add_bearing", dict(designation="6200",
                                 position=[50, 0, 0])),
            ("add_retaining_ring", dict(standard="din471", size=25,
                                        position=[-50, 0, 0])),
        ]
        for method, params in placements:
            placed = a.call(method, **params)
            expect(placed["occurrence"] != 0, f"{method} placed: {placed}")
            expect(len(contents(a)[0]) == 3, f"{method} added one")
            a.undo()
            expect(contents(a) == base,
                   f"undo after {method} takes back exactly it: "
                   f"{contents(a)} (was {base})")

        # A fitted joint: screw, nut and the mate between the plates, ONE
        # step.
        rims = [e for e in upper.body().edges(circular=True, radius=5.5)
                if e.center.z > 19.0]
        expect(len(rims) == 1, f"one rim on the upper plate: {rims}")
        fit = rig.bolt(rims[0], up, standard="iso4762", size="M10",
                       nut="iso4032")
        expect(len(fit["placed"]) == 2 and len(fit["mates"]) == 1,
               f"a screw, a nut and a mate: {fit}")
        a.undo()
        expect(contents(a) == base,
               f"undo after fit_fastener takes back the whole joint: "
               f"{contents(a)} (was {base})")
        a.redo()
        occs, mates = contents(a)
        expect(len(occs) == 4 and mates == 1,
               f"redo puts the joint back: {occs}, {mates} mate(s)")
        del lo
        print("placement_undo_smoke: ok")
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
