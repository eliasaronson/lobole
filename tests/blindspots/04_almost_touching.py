# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Blind spot: a sub-millimetre gap is invisible in any render and passes
every clash-only check — the checks pass EXACTLY BECAUSE nothing touches.
Members meant to be bolted end up floating next to each other.

Incident: the dock's v2 barrel stops, left hanging in mid-air when the
rails moved but the stops' height didn't; caught by human eyes only.
"""
from smoke_shapes import box  # noqa: E402  (tests/ is on the path)


def run(d, expect):
    box(d, (0, 0, 0), (20, 20, 20), "B")
    part = d.call("list_parts")["parts"][0]["name"]
    d.call("add_assembly", name="Rig")
    d.call("set_active_part", name="Rig")
    d.call("add_occurrence", part=part, name="Base", fixed=True)
    # WRONG: 0.5 mm shy of touching — an arithmetic slip, not a design.
    r = d.call("add_occurrence", part=part, name="Slipped", fixed=True,
               transform={"position": [20.5, 0, 0],
                          "orientation": [0, 0, 0, 1]})
    slipped = r["id"]

    r = d.call("interference", a="Base", b="Slipped")
    expect(not r["clash"], "clash-only checking is blind here")
    rep = d.call("contact_report")
    expect(not rep["connected"], "the graph must show TWO components")
    expect(len(rep["near_misses"]) == 1 and
           abs(rep["near_misses"][0]["distance"] - 0.5) < 1e-6,
           f"the 0.5 mm slip must be a named near-miss: {rep['near_misses']}")

    # RIGHT: close the gap; the joint asserts touching, the graph connects.
    d.call("set_occurrence_transform", id=slipped,
           transform={"position": [20, 0, 0], "orientation": [0, 0, 0, 1]})
    r = d.call("interference", a="Base", b="Slipped")
    expect(r["state"] == "touching", f"joint must touch: {r}")
    rep = d.call("contact_report")
    expect(rep["connected"] and rep["near_misses"] == [],
           f"one component, no near-misses: {rep['components']}")
