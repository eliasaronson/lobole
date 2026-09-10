#!/usr/bin/env python3
"""Provisional router: real B-Reps, live dimensions, attachment frames and travel."""

import os
import sys

sys.path[:0] = [
    os.path.join(os.path.dirname(__file__), "..", p) for p in ("tools", "generators", "generators/cncest6090")
]
from smoke_app import launch
from designer_sdk import App
from cncest6090_spec import dimensions, manifest
import cncest6090 as C


def main():
    v = dimensions()
    assert manifest(v)["spindle_d"]["status"] == "user_supplied"
    assert all(row["manufacturing_tolerance"] is None for row in manifest(v).values())
    for bad in (
        {"spindel_d": 80},
        {"spindle_d": True},
        {"spindle_d": float("nan")},
        {"spindle_d": 150},
        {"z_travel": 500},
    ):
        try:
            dimensions(bad)
        except ValueError:
            pass
        else:
            raise AssertionError(f"invalid specification accepted: {bad}")
    with launch(os.path.abspath(sys.argv[1])) as proc:
        app = App(proc.designer)
        parts, rig, slide, spindle, machine = C.build(app, v)
        original_ids = [f["id"] for f in parts["clamp"].state()["features"]]
        for d in (82, 80):
            app.set_variable(name="spindle_d", expression=str(d), scope="project")
            parts["clamp"].check(solids=1)
            lo, hi = parts["spindle"].bbox()
            assert abs(hi.x - lo.x - d) < 0.001, (lo, hi, d)
            bores = parts["clamp"].faces(
                kind="cylinder", radius=(d + v["bore_clearance"]) / 2, radius_tol=0.001
            )
            assert bores, "clamp bore did not follow spindle diameter"
            assert original_ids == [f["id"] for f in parts["clamp"].state()["features"]]
        app.set_variable(name="guide_pitch", expression="130", scope="project")
        rig.solve()
        guide = rig.occurrence("Z guide 1")
        assert abs(guide.frame("origin").origin.x - 65) < 0.001
        bearing = rig.occurrence("Z bearing 1 0")
        assert abs(bearing.frame("origin").origin.x - 65) < 0.001
        app.set_variable(
            name="guide_pitch", expression=str(v["guide_pitch"]), scope="project"
        )
        app.set_variable(name="bed_w", expression="850", scope="project")
        machine.solve()
        assert (
            abs(machine.occurrence("Gantry upright 1").frame("origin").origin.x - 455)
            < 0.001
        )
        app.set_variable(name="bed_w", expression=str(v["bed_w"]), scope="project")
        rig.activate()
        standing = app.interference(mode="exact", max_pairs=5000, deadline_s=120)
        assert not standing["clash"] and not standing.get("truncated"), standing
        study = rig.motion(
            slide,
            dof="offset",
            **{"from": 0},
            to=v["z_travel"],
            samples=9,
            clash_mode="exact",
            name="Z regression",
        )
        result = app.run_motion_study(
            study=study["id"], include_poses=True, _timeout=180
        )
        assert result["clean"], result["table"]
        zs = [
            next(
                p["position"][2]
                for p in sample["poses"]
                if p["occurrence"] == spindle.id
            )
            for sample in result["samples"]
        ]
        assert abs(max(zs) - min(zs) - v["z_travel"]) < 0.001, zs
        print(
            "PASS: live spindle bore, guide spacing, bed width, stable feature IDs and exact Z travel"
        )


if __name__ == "__main__":
    main()
