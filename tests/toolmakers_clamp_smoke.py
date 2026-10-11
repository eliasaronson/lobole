#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Dimensioned clamp: real solids, closed screw loops, edits and persistence."""

import json
import math
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
    app.set_active_part(part="Fixed jaw - drawing")
    app.update_drawing()
    notes = app.list_drawing_notes()["notes"]
    assert any("80 x 12 x 12" in n["resolved_text"] for n in notes), notes
    assert all(n["resolved"] for n in notes)
    app.export_drawing(path=str(out / "edited.svg"))
    assert "80 x 12 x 12" in (out / "edited.svg").read_text()
    app.set_variable(name="jaw_l", expression="75", scope="project")
    # Gap frames and both screw loops follow a live assembly dimension.
    app.set_variable(name="gap", expression="20", scope="project")
    assert rig.solve()["loops_closed"]
    lo, hi = rig.occurrence("Moving jaw").bbox()
    assert abs(hi[2] + 20) < 0.001, (lo, hi)
    # The exported/reopened bore is the 5 mm tap drill, not a 6 mm envelope.
    jaw = app.part("Moving jaw", create=False)
    assert len(jaw.faces(kind="cylinder", radius=2.5, radius_tol=1e-5)) == 2
    # A separate M8 fixture exercises two standards actually in the native table.
    app.new()
    shaft = app.part("Pitch fixture shaft")
    shaft.cylinder("Top", d=8, height=12)
    shaft.frame("origin")
    handle = shaft.thread(shaft.faces(kind="cylinder", radius=4)[0], "M8")
    fixture = app.assembly("Pitch fixture")
    ground = fixture.place(shaft, fixed=True, name="Ground")
    moving = fixture.place(shaft, name="Shaft")
    mate = fixture.cylindrical((ground, "origin"), (moving, "origin"), flipped=False)
    fixture.screw(mate, handle)
    fixture.solve()
    assert (
        abs(app.list_relations()["relations"][0]["ratio"] - 1.25 / (2 * math.pi)) < 1e-9
    )
    shaft.activate()
    spec = next(f for f in shaft.state()["features"] if f["type"] == "Thread")
    spec["designation"] = "M8x1"
    app.modify_feature(feature_id=spec["id"], feature=spec)
    fixture.solve()
    assert abs(app.list_relations()["relations"][0]["ratio"] - 1 / (2 * math.pi)) < 1e-9
    # A custom thread drives the screw by its own pitch: every printed
    # thread is a custom one (docs/assemblies/relations.md).
    shaft.activate()
    spec["designation"] = "custom"
    spec["pitch"] = 2.5
    app.modify_feature(feature_id=spec["id"], feature=spec)
    fixture.solve()
    assert abs(app.list_relations()["relations"][0]["ratio"] - 2.5 / (2 * math.pi)) < 1e-9
    # A thread the tables refuse is the feature's error, and the screw it
    # backs errors with it rather than keeping the last pitch.
    shaft.activate()
    spec["designation"] = "M7.3"
    app.modify_feature(feature_id=spec["id"], feature=spec)
    assert fixture.solve()["errored_mates"] > 0
    print(
        "PASS: analytic volumes, screw travel, exact clashes, eight drawings, BOM, STEP round trip, saved project and live edits"
    )
