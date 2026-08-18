"""Blind spot: hand-derived quaternions. The classic slip is forgetting
the half-angle — writing [0, sin(90), 0, cos(90)] for "90 degrees about
Y" and getting a 180-degree rotation. On a symmetric part it is invisible;
on anything else the occurrence lands rotated wrong.

Incident: the dock's right-end posts (v3 build) were first placed with a
hand-derived rotation that put them inside the end joist; caught only by
re-deriving the arithmetic by hand.
"""
import math


def run(d, expect):
    # An asymmetric slab, so a wrong rotation is geometrically visible.
    d.call("add_feature", feature={
        "type": "Box", "name": "Slab", "position": [0, 0, 0],
        "width": 40, "height": 10, "depth": 4})
    part = d.call("list_parts")["parts"][0]["name"]
    d.call("add_assembly", name="Rig")
    d.call("set_active_part", name="Rig")

    # Reference: what "90 degrees about Y" should look like, via rotate{}.
    d.call("add_occurrence", part=part, name="Reference", fixed=True,
           transform={"position": [0, 0, 0],
                      "rotate": {"axis": [0, 1, 0], "angle_deg": 90}})
    # WRONG: sin/cos of the FULL angle -> [0, 1, 0, 0] = 180 degrees.
    d.call("add_occurrence", part=part, name="Hand", fixed=True,
           transform={"position": [0, 0, 0],
                      "orientation": [0, math.sin(math.radians(90)), 0,
                                      math.cos(math.radians(90))]})
    r = d.call("compare_geometry", a="Reference", b="Hand")
    expect(r["iou"] < 0.2,
           f"the half-angle slip must be measurably wrong: iou {r['iou']}")

    # RIGHT: the half-angle by hand matches rotate{} exactly.
    d.call("add_occurrence", part=part, name="Halved", fixed=True,
           transform={"position": [0, 0, 0],
                      "orientation": [0, math.sin(math.radians(45)), 0,
                                      math.cos(math.radians(45))]})
    r = d.call("compare_geometry", a="Reference", b="Halved")
    expect(r["iou"] > 0.999, f"correct quaternion matches: iou {r['iou']}")
