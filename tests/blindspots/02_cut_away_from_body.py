# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Blind spot: extrude follows +plane-normal, so a cut sketched on a top
face whose plane normal points OUT of the body extrudes into empty air.
The profile is exactly where the agent thinks it is — only the direction
is wrong — which made this invisible until digests/warnings existed.

Incident: the strict-inputs smoke's own first draft cut upward off the
block (this session); same class as countless "cut did nothing" loops.
"""
from smoke_shapes import box  # noqa: E402  (tests/ is on the path)


def run(d, expect):
    box(d, (0, 0, 0), (20, 20, 20), "Block")
    sk = d.call("add_feature", feature={
        "type": "Sketch", "name": "Top",
        "plane": {"origin": [0, 0, 20], "normal": [0, 0, 1],
                  "uAxis": [1, 0, 0]},
        "entities": [{"type": "circle", "center": [10, 10], "radius": 4}]})

    # WRONG: cut along +normal, away from the body.
    r = d.call("add_feature", feature={
        "type": "Extrude", "name": "Cut", "sketch_id": sk["id"],
        "distance": 10, "mode": "cut"})
    expect(abs(r["digest"]["delta"]["volume_mm3"]) < 1e-6,
           f"away-cut must show a zero delta: {r['digest']['delta']}")
    expect(r["regen"]["warnings"], f"away-cut must warn: {r['regen']}")
    d.call("undo")

    # RIGHT: reversed drives the cut into the body.
    r = d.call("add_feature", feature={
        "type": "Extrude", "name": "Cut", "sketch_id": sk["id"],
        "distance": 10, "mode": "cut", "reversed": True})
    import math
    expect(abs(r["digest"]["delta"]["volume_mm3"] + math.pi * 16 * 10) < 1.0,
           f"reversed cut must remove the bore: {r['digest']['delta']}")
