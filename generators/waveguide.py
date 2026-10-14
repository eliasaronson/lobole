#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A constant-directivity waveguide for the Alpair 7MS, to cut in 3D.

The dogfood for 3-axis surfacing (docs/cam/operations.md#3-axis-surfacing). It is a real part
-- it bolts to the smart_speaker baffle on the driver's own bolt circle and
tightens the Alpair's directivity -- and it is chosen because ONE part
exercises everything the workstream added:

  * a STEEP throat flare, which is waterline's case: a raster's passes crowd
    together on a near-vertical wall;
  * a SHALLOW mouth, which is the raster's case: waterline's laps spread out
    as a surface flattens;
  * TWO curvature scales an order apart -- a 6 mm rim roundover against a
    ~60 mm flare -- so no single stepover suits the whole part;
  * five bolt holes THROUGH the flare, which the region has to work around;
  * a flat back and a cylindrical rim, which the 2.5D ops still own.

Geometry, in the profile's own (radius, height) half-plane, revolved about Z:

      96 ---.__  <- rim roundover, R6
     100 |      `-. (94,25)
         |         \\__ front flat
         |            `--.__
         |                  `--.__   the flare: steep at the throat,
         |                        `--.  shallow at the mouth
      (100,0) ------------------- (50,0)   back face / throat

Run against a running instance:  python3 generators/waveguide.py --port N
"""
import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lobole.sdk import Plane  # noqa: E402
from _lib import arc_through, check, circle, connect, line  # noqa: E402

# --- the part, in millimetres ---------------------------------------------
#
# Every one of these is READ from the speaker project's own parameters
# (parts/front_wall: alpair_cutout_dia 100, alpair_bolt_circle 112,
# alpair_bolt_count 5) rather than guessed, so the waveguide bolts to the
# baffle that is already cut.
THROAT_R = 50.0      # = alpair_cutout_dia / 2, so the throat meets the driver
BLANK_R = 100.0      # 200 mm disc out of 25 mm MDF
DEPTH = 25.0
RIM_R = 6.0          # the roundover on the outer edge
MOUTH_R = 92.0       # where the flare meets the front flat
BOLT_PCD = 112.0
BOLT_N = 5
BOLT_D = 3.4         # M3 clearance


def flare_arc():
    """The flare, as (mid_point, radius), DERIVED rather than chosen.

    An arc through three eyeballed points is not a waveguide: the first
    attempt peaked at v=25.79 and came back down, putting a 0.79 mm ridge at
    r=84.5 and leaving the mouth in a shallow dip. Two conditions fix it and
    between them they determine the arc completely:

      * TANGENT to the front flat at the mouth, so the surface arrives at the
        rim level instead of crossing it. The centre is then directly below
        the mouth point;
      * THROUGH the throat, which sets the radius.

    Solving (THROAT_R - MOUTH_R)^2 + (DEPTH - R)^2 = R^2 for R gives a flare
    that is steep where it meets the driver and flat where it meets the rim --
    which is the whole point of the part, and is now arithmetic rather than
    taste.
    """
    dr = MOUTH_R - THROAT_R
    radius = (dr * dr + DEPTH * DEPTH) / (2.0 * DEPTH)
    centre = (MOUTH_R, DEPTH - radius)
    a_mouth = math.atan2(DEPTH - centre[1], 0.0)
    a_throat = math.atan2(0.0 - centre[1], THROAT_R - centre[0])
    a_mid = (a_mouth + a_throat) / 2.0
    mid = (centre[0] + radius * math.cos(a_mid),
           centre[1] + radius * math.sin(a_mid))

    # It must not rise above the front flat anywhere, which is what went
    # wrong before. The tangency puts the arc's highest point exactly AT the
    # mouth, so this is an equality, not a margin.
    assert centre[1] + radius <= DEPTH + 1e-9, "the flare crests above the rim"
    # ...and it must still be steep at the throat and shallow at the mouth,
    # or it is not exercising both finishing strategies.
    slope_throat = abs((THROAT_R - centre[0]) / (0.0 - centre[1]))
    assert slope_throat > 1.0, "the throat end must be steep"
    return mid, radius


def build(d, name="Waveguide"):
    mid, flare_r = flare_arc()
    rim_c = (BLANK_R - RIM_R, DEPTH - RIM_R)   # (94, 19)
    rim_mid = (rim_c[0] + RIM_R * math.cos(math.radians(45)),
               rim_c[1] + RIM_R * math.sin(math.radians(45)))

    # `Plane.xz` and not a bare XZ plane: that helper exists because the XZ
    # plane's V axis is -Z (V = normal x uAxis, normal +Y), so a profile drawn
    # with +v up comes out upside down and the revolve is a mirror of the part
    # (docs, and the sketch-plane gotcha this project has paid for twice).
    ents = [
        line((THROAT_R, 0.0), (BLANK_R, 0.0)),            # back face
        line((BLANK_R, 0.0), (BLANK_R, DEPTH - RIM_R)),   # outer wall
        arc_through((BLANK_R, DEPTH - RIM_R), rim_mid,
                    (rim_c[0], DEPTH)),                   # rim roundover, R6
        line((rim_c[0], DEPTH), (MOUTH_R, DEPTH)),        # front flat
        arc_through((MOUTH_R, DEPTH), mid, (THROAT_R, 0.0)),  # the flare
    ]
    sk = check(d.call("add_feature", feature={
        "type": "Sketch", "name": "Waveguide profile",
        "plane": Plane.xz(0.0).json(), "entities": ents}), "profile sketch")
    body = check(d.call("add_feature", feature={
        "type": "Revolve", "name": name, "sketch_id": sk["id"],
        "axis": {"kind": "z"}, "angle_deg": 360.0,
        "mode": "new_body"}), "revolve")

    # The driver's own bolt circle, straight through. The holes land ON the
    # flare near the throat -- which is where they belong on the real part,
    # since the waveguide is sandwiched between driver and baffle, and is
    # exactly the internal feature a surfacing region has to work around.
    holes = [circle((BOLT_PCD / 2.0 * math.cos(2 * math.pi * i / BOLT_N),
                     BOLT_PCD / 2.0 * math.sin(2 * math.pi * i / BOLT_N)),
                    BOLT_D / 2.0)
             for i in range(BOLT_N)]
    hsk = check(d.call("add_feature", feature={
        "type": "Sketch", "name": "Driver bolts",
        "plane": Plane.xy(DEPTH + 1.0, flip=True).json(), "entities": holes}),
        "bolt sketch")
    check(d.call("add_feature", feature={
        "type": "Extrude", "name": "Driver bolts", "sketch_id": hsk["id"],
        "distance": DEPTH + 2.0, "mode": "cut"}), "bolt holes")
    return body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=None)
    args = ap.parse_args()
    d = connect(args.port)
    build(d)

    mp = d.call("mass_properties")
    print(f"  volume {mp['total']['volume'] / 1000.0:.1f} cm3")
    geo = d.call("check_geometry")
    print(f"  check_geometry: {geo}")


if __name__ == "__main__":
    main()
