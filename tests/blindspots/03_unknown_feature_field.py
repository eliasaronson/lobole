# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Blind spot: a plausible-but-wrong feature field used to be silently
ignored — Box accepted mode:"add", dropped it, and created a separate
body; the failure surfaced three calls later as a null-mass BOM. (A Box
takes a mode now -- it is a sketch and an extrude -- so the trap is shown
with a key it still does not have: `length`, which would have left the box
at a size nobody asked for.)

Incident: the dock's corner-mooring bracket (v3 build), where the 'mixed
material' BOM row was the first visible symptom.
"""
from smoke_shapes import box  # noqa: E402  (tests/ is on the path)


def run(d, expect):
    # WRONG: Box has no length (width, depth, height). Must error AND name
    # the accepted vocabulary (the full-template principle: don't reveal one
    # constraint per trip) -- the WHOLE dialect, including the fields this
    # call left out, which a caller cannot list from what it sent.
    try:
        d.call("add_feature", feature={
            "type": "Box", "name": "B", "on": "Top", "centered": False,
            "width": 10, "depth": 10, "length": 10})
        raise AssertionError("Box length must be rejected")
    except RuntimeError as e:
        expect("unknown parameter 'feature/length'" in str(e),
               f"names the field: {e}")
        expect("width" in str(e) and "height" in str(e),
               f"lists the fields it does hold: {e}")
        expect("mode" in str(e) and "offset" in str(e),
               f"and the rest of the vocabulary: {e}")

    # RIGHT: to merge prisms, extrude with mode:"add" (Extrude has one).
    box(d, (0, 0, 0), (10, 10, 10), "B")
    sk = d.call("add_feature", feature={
        "type": "Sketch", "name": "S",
        "plane": {"origin": [0, 0, 10], "normal": [0, 0, 1],
                  "uAxis": [1, 0, 0]},
        "entities": [{"type": "rect", "c1": [0, 0], "c2": [10, 10]}]})
    r = d.call("add_feature", feature={
        "type": "Extrude", "name": "Riser", "sketch_id": sk["id"],
        "distance": 5, "mode": "add"})
    expect(abs(r["digest"]["delta"]["volume_mm3"] - 500.0) < 1e-6,
           f"the add merges into the body: {r['digest']['delta']}")
    expect(len(r["digest"]["bodies"]) == 1,
           f"still ONE body: {r['digest']['bodies']}")
