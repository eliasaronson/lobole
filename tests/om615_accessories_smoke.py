#!/usr/bin/env python3
"""Real accessory B-Reps and staged appearance updates, without an engine sweep.

Run through nix: python3 tests/om615_accessories_smoke.py build/lobole
The generator's --stage full remains the whole-engine 49-pose release gate.
"""

import math
import os
import sys
import tempfile
import time

sys.path[:0] = [
    os.path.join(os.path.dirname(__file__), "..", directory)
    for directory in ("tools", "generators", "generators/om615")
]
from designer_sdk import App
from designer_remote import Designer
from designer_components import claim, plan
from designer_validation import validate
from smoke_app import launch_app
import om615_accessories as A


def main():
    spans, wraps, length = A.belt_geometry()
    assert 800 < length < 825
    assert abs(sum(w["angle"] for w in wraps) - math.tau) < 1e-10
    # Each span leaves one pulley for the next one round, and together they
    # visit all three whichever side the alternator is on.
    assert sorted(span["a"] for span in spans) == [0, 1, 2]
    for span in spans:
        dx = span["end"][0] - span["start"][0]
        dz = span["end"][1] - span["start"][1]
        for point, circle in (
            (span["start"], A.PULLEYS[span["a"]]),
            (span["end"], A.PULLEYS[span["b"]]),
        ):
            rx, rz = point[0] - circle[0], point[1] - circle[1]
            assert abs(math.hypot(rx, rz) - circle[2]) < 1e-10
            assert abs(rx * dx + rz * dz) < 1e-8
    with tempfile.TemporaryDirectory(prefix="om615_accessory_test_") as tmp:
        proc = launch_app(
            sys.argv[1],
            env=dict(os.environ, XDG_STATE_HOME=tmp, XDG_CONFIG_HOME=tmp + "/config"),
        )
        try:
            app = App(Designer.wait_for(proc.port))
            app.new()
            started = time.perf_counter()
            belt = A.belt(app)
            fan = A.fan(app)
            A.vacuum_parts(app)
            # What hangs on it: each builder refuses anything but one valid
            # solid (A.verify), and a hose is a bored tube, not a rod.
            hanging = {name: build(app) for name, build, *_ in A.HANGING}
            bores = {"upper coolant hose": A.COOLANT_HOSE_OD / 2.0
                     - A.COOLANT_HOSE_WALL,
                     "lower coolant hose": A.COOLANT_HOSE_OD / 2.0
                     - A.COOLANT_HOSE_WALL,
                     "heater pipe": A.HEATER_PIPE[0] / 2.0 - A.HEATER_PIPE[1],
                     "dipstick tube": A.DIPSTICK_BORE_D / 2.0}
            for name, radius in bores.items():
                assert hanging[name].faces(kind="cylinder", radius=radius,
                                           radius_tol=0.001), name
            app.delete_part(part="Part 1")
            print(f"accessory build: {time.perf_counter() - started:.3f}s", flush=True)
            lo, hi = belt.bbox()
            assert abs((hi.y - lo.y) - 9.5) < 0.001
            claim(fan, namespace="accessory-test", key="fan", fingerprint="steel")
            rig = app.assembly("fan fixture")
            occurrence = rig.place(fan, fixed=True)
            old_features = [f["id"] for f in fan.state()["features"]]
            before = validate(app, assembly=rig)
            assert before.data["status"] == "passed"
            assert not before.data["accepted"]

            def recolour(a):
                return A.fan(a).material("aluminum")

            started = time.perf_counter()
            with plan(
                app,
                namespace="accessory-test",
                key="fan",
                fingerprint="aluminum",
                builder=recolour,
            ) as update:
                assert update.apply().id == fan.id
                assert update.report["geometry_reused"], update.report
            print(
                f"staged fan recolour: {time.perf_counter() - started:.3f}s", flush=True
            )
            assert [f["id"] for f in fan.state()["features"]] == old_features
            assert fan.state()["bodies"][0]["material"] == "aluminum"
            assert rig.occurrences()[0].id == occurrence.id
            assert before.current(app)["status"] == "stale"

            def never(a):
                raise AssertionError("unchanged builder executed")

            started = time.perf_counter()
            with plan(
                app,
                namespace="accessory-test",
                key="fan",
                fingerprint="aluminum",
                builder=never,
            ) as update:
                assert update.report["action"] == "reuse"
                update.apply()
            print(f"unchanged fan: {time.perf_counter() - started:.3f}s", flush=True)
            after = validate(app, assembly=rig)
            assert after.data["status"] == "passed"
            assert after.data["motion"]["status"] == "not_checked"
        finally:
            proc.terminate()
            proc.wait(timeout=10)
    print("PASS: belt tangency, strict accessory solids and stable staged fan update")


if __name__ == "__main__":
    main()
