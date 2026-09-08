#!/usr/bin/env python3
"""Dimensioned clamp: real solids, closed screw loops, edits and persistence."""

import json
import os
import sys
import tempfile
from pathlib import Path

sys.path[:0] = [
    str(Path(__file__).resolve().parents[1] / p) for p in ("tools", "generators")
]
from smoke_app import launch
from designer_sdk import App
import toolmakers_clamp as C

with (
    tempfile.TemporaryDirectory(prefix="clamp-test-") as tmp,
    launch(os.path.abspath(sys.argv[1])) as proc,
):
    app = App(proc.designer)
    out = Path(tmp)
    result = C.run(app, out)
    assert len(result["bom"]["rows"]) == 8
    assert sum(r["quantity"] for r in result["bom"]["rows"]) == 8
    assert len(result["drawings"]) == 8 and all(
        r["dims"] == 2 for r in result["drawings"].values()
    )
    assert (out / "toolmakers-clamp.step").stat().st_size > 1000
    # Exported STEP must import into valid solids with the same total volume.
    original = sum(result["volumes_mm3"].values())
    app.new()
    app.import_step(path=str(out / "toolmakers-clamp.step"))
    measured = app.measure(a={"part": app.parts()[0]["id"]})
    assert measured["solids"] == 8, measured
    assert abs(measured["volume"] - original) < 0.01, (measured, original)
    app.open_project(path=str(out / "toolmakers-clamp.dsgn"))
    rig = app.assembly("Toolmakers clamp", create=False)
    assert rig.solve()["loops_closed"]
    p = app.part("Fixed jaw", create=False)
    ids = [f["id"] for f in p.state()["features"]]
    app.set_variable(name="jaw_l", expression="80", scope="project")
    p.check(solids=1)
    lo, hi = p.bbox()
    assert abs(hi.x - lo.x - 80) < 0.001
    assert ids == [f["id"] for f in p.state()["features"]]
    app.set_variable(name="jaw_l", expression="75", scope="project")
    # Gap frames and both screw loops follow a live assembly dimension.
    app.set_variable(name="gap", expression="20", scope="project")
    assert rig.solve()["loops_closed"]
    lo, hi = rig.occurrence("Moving jaw").bbox()
    assert abs(hi[2] + 20) < 0.001, (lo, hi)
    print(
        "PASS: analytic volumes, screw travel, exact clashes, eight drawings, BOM, STEP round trip, saved project and live edits"
    )
