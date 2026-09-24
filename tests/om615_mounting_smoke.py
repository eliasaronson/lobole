#!/usr/bin/env python3
"""Physical accessory connections: contact, pocket section and missing-hardware gates.

Run through nix: python3 tests/om615_mounting_smoke.py build/designer
Uses a private lower-engine fixture; the generator's full stage checks 720° motion.
"""

import os
import sys
import tempfile

sys.path[:0] = [
    os.path.join(os.path.dirname(__file__), "..", directory)
    for directory in ("tools", "generators", "generators/om615")
]
from designer_sdk import App, Part, Plane, SdkError
from designer_remote import Designer
from smoke_app import launch_app
import om615
import om615_accessories as A
import om615_bottom as B


def refused(fn, message, reason=None):
    try:
        fn()
    except (SdkError, RuntimeError) as error:
        if reason is not None:
            assert reason in str(error), str(error)
        return
    raise AssertionError(message)


def main():
    with tempfile.TemporaryDirectory(prefix="om615_mounting_test_") as tmp:
        proc = launch_app(
            sys.argv[1],
            env=dict(os.environ, XDG_STATE_HOME=tmp, XDG_CONFIG_HOME=tmp + "/config"),
        )
        try:
            app = App(Designer.wait_for(proc.port))
            app.new()
            rig = app.assembly("engine")
            occ, main = om615.bottom_end(app, rig)
            om615.front_and_rear(app, rig, occ, main)
            measured = A.check_mountings(app, rig, hanging=False)
            assert len(measured["contacts"]) == 27, measured
            stator = app.assembly("alternator stator assembly", create=False)
            assert (
                len(
                    [
                        o
                        for o in stator.occurrences()
                        if o.name.startswith("stator winding ")
                    ]
                )
                == 12
            )
            winding = app.part("alternator winding pack", create=False)
            assert winding.state()["bodies"][0]["material"] == "copper"
            A.check_subassemblies(app)
            rig.activate()
            result = app.interference(mode="exact", max_pairs=20000, deadline_s=120)
            assert not result.get("truncated") and not result["clash"], result
            # A valid body can contain two disconnected solids. Then leave a
            # thin bridge: topology passes, but the minimum neck must not.
            bracket = app.part("alternator mounting bracket", create=False)
            # The arm is on the manifold side: 144..146 out from the block.
            ms = A.MANIFOLD_SIDE
            B.slab(bracket, ms * 144, -300, -50, ms * 146, -140, 50,
                   "Sever support", mode="cut")
            refused(lambda: bracket.check(solids=1), "a severed support passed",
                    "expected 1 solid(s), got 2")
            app.undo()
            B.slab(bracket, ms * 144, -300, -50, ms * 146, -140, -5.5,
                   "Thin support below", mode="cut")
            B.slab(bracket, ms * 144, -300, -4.5, ms * 146, -140, 50,
                   "Thin support above", mode="cut")
            bracket.check(solids=1)
            refused(lambda: A.check_mounting_structure(app, rig),
                    "a connected but thin support passed", "insufficient support section")
            bracket.activate()
            # Each slab creates a sketch and extrusion.
            for _ in range(4):
                app.undo()
            rig.activate()
            bolt = rig.occurrence("alternator mounting bracket foot_bolt_0")
            short = Part(app, next(p for p in app.parts() if p["id"] == bolt.part_id))
            B.slab(short, -20, -20, 10, 20, 20, 50, "Shorten bolt", mode="cut")
            refused(lambda: A.check_mounting_structure(app, rig),
                    "an under-engaged block bolt passed", "block engagement")
            short.activate()
            app.undo()
            app.undo()
            bracket.activate()
            B.axis_solid(bracket, (ms * 99, A.MOUNT_BOLT_Y, A.BRACKET_Z + 20),
                         (ms * 111, A.MOUNT_BOLT_Y, A.BRACKET_Z + 20), 9,
                         "Fill mounting hole")
            refused(lambda: A.check_mounting_structure(app, rig),
                    "a datum without a drilled hole passed", "not aligned with a drilled hole")
            bracket.activate()
            app.undo()
            app.undo()
            A.check_mounting_structure(app, rig)
            missing = rig.occurrence("alternator mounting bracket pivot_bolt")
            app.remove_occurrence(id=missing.id)
            refused(
                lambda: A.check_mountings(app, rig, hanging=False),
                "a missing alternator pivot bolt passed the mounting check",
            )
            # Reproduce the previous thin-ring mount: it touches a pump flange
            # but has no enclosure behind it. It must fail the section gate.
            app.new()
            cover = app.part("timing cover")
            sk = cover.sketch(Plane.xz(-260), name="Old thin mounting ring")
            sk.circle((A.VAC_X, A.VAC_Z), r=59)
            sk.circle((A.VAC_X, A.VAC_Z), r=49)
            cover.extrude(sk.add(), 7, name="Old thin mounting ring")
            rig = app.assembly("old mounting")
            refused(
                lambda: A.check_mountings(app, rig, hanging=False),
                "a thin flange ring passed as an enclosed pump pocket",
            )
            print(
                "PASS: mounting contacts, copper windings, standing clearance, "
                "severed/thin support, short bolt, absent hole, missing bolt "
                "and open-pocket rejection"
            )
        finally:
            proc.terminate()
            proc.wait(timeout=10)


if __name__ == "__main__":
    main()
