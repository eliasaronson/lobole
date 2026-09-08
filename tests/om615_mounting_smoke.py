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
    for directory in ("tools", "generators")
]
from designer_sdk import App, Plane, SdkError
from designer_remote import Designer
from smoke_app import launch_app
import om615
import om615_accessories as A


def refused(fn, message):
    try:
        fn()
    except (SdkError, RuntimeError):
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
            measured = A.check_mountings(app, rig)
            assert len(measured["contacts"]) == 28, measured
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
            missing = rig.occurrence("alternator mounting bracket pivot_bolt")
            app.remove_occurrence(id=missing.id)
            refused(
                lambda: A.check_mountings(app, rig),
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
                lambda: A.check_mountings(app, rig),
                "a thin flange ring passed as an enclosed pump pocket",
            )
            print(
                "PASS: mounting contacts, copper windings, standing clearance, "
                "missing bolt and open-pocket rejection"
            )
        finally:
            proc.terminate()
            proc.wait(timeout=10)


if __name__ == "__main__":
    main()
