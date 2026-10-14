#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A file handle to cut on the 6090's 4th axis.

The dogfood for continuous 4-axis (docs/cam/rotary.md#continuous-surface_rotary). It is a real
part -- a handle for a bastard file, in beech, turned and carved on the
rotary -- and it is chosen because it cannot be made on a lathe, which is the
whole point of having the axis.

A body of revolution is a LATHE job. What makes this one need a milling
spindle and a trunnion together is the grip: five dimples spiralling down the
swell, each turned 43 degrees from the last. The radius varies with theta as
well as with x, so there is no single cross-section and no way to cut it by
spinning the work against a stationary tool.

Geometry, in the profile's own (x along the axis, radius) half-plane:

      18 |     ,--.__            the swell your palm sits in
         |   ,'      `--.__
      13 |  |               `--.___        the waist, for two fingers
         | |                       `--.
       6 ||                             `.   the ferrule end, Phi 12
         +----------------------------------
         0                                130

Run against a running instance:  python3 generators/rotary/handle.py --port N
"""
import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from lobole.sdk import Plane  # noqa: E402
from _lib import arc_through, check, connect, line  # noqa: E402

# --- the part, in millimetres ---------------------------------------------
LENGTH = 130.0        # tip to ferrule
BUTT_R = 7.0          # the rounded end, before the dome
SWELL_R = 18.0        # the widest point: Phi 36, a comfortable palm
SWELL_X = 45.0
WAIST_R = 13.0
WAIST_X = 88.0
FERRULE_R = 9.0       # Phi 18 where the ferrule goes
DIMPLE_N = 5
DIMPLE_R = 9.0        # the ball that makes each grip
DIMPLE_TWIST = 43.0   # degrees between one dimple and the next
DIMPLE_X0 = 26.0
DIMPLE_STEP = 13.0
# How deep each dimple bites into the surface. Derived, not chosen: a fifth
# of the swell radius is a grip you can feel and not a hole you can see
# through, and it scales if the handle does.
DIMPLE_BITE = SWELL_R / 5.0


def profile():
    """The half-section, as sketch entities in the (x, radius) plane.

    Every arc is drawn through three points rather than by centre and angle,
    because what a handle has to be is a shape your hand agrees with, and
    three points on the curve is how a person states that. The one number
    that is derived rather than drawn is the butt dome's mid point, which is
    placed on the circle through the tip and the swell so the end is round
    instead of a cone with a cap on it.
    """
    butt_mid = (BUTT_R * 0.55, BUTT_R + (SWELL_R - BUTT_R) * 0.35)
    return [
        line((0.0, 0.0), (0.0, BUTT_R)),                  # the flat butt
        arc_through((0.0, BUTT_R), butt_mid, (14.0, SWELL_R - 2.0)),
        arc_through((14.0, SWELL_R - 2.0), (SWELL_X, SWELL_R),
                    (66.0, SWELL_R - 2.5)),               # the swell
        arc_through((66.0, SWELL_R - 2.5), (WAIST_X, WAIST_R),
                    (112.0, FERRULE_R + 2.5)),            # into the waist
        line((112.0, FERRULE_R + 2.5), (LENGTH, FERRULE_R)),
        line((LENGTH, FERRULE_R), (LENGTH, 0.0)),         # the ferrule face
        line((LENGTH, 0.0), (0.0, 0.0)),                  # the axis
    ]


def dimples(d, solid):
    """Five spheres spiralling down the swell, subtracted.

    Spheres and not a swept groove: a dimple is what a thumb wants, and it
    also keeps the part honest about WHY it needs four axes rather than
    clever about how it was modelled. Each is sunk `DIMPLE_BITE` into the
    surface at its own station, so the bite is constant while the handle's
    radius is not.

    Each ball is a Sphere that CUTS, so the handle keeps its one body. It
    stands on a frame at its own centre whose X is the handle's axis: a
    sphere is a half disc revolved about the frame's X, and with X along the
    handle its poles sit beside the dimple, not in it. A ball on a YZ plane
    revolves about Y instead, which puts a pole inside the dimple on the
    stations where the twist turns it towards Y -- and the kernel's cut
    through a pole is off by tens of mm3 (31 low at the second dimple, where
    a slice integral of ball and profile gives 263.4); placed this way all
    five agree with that integral to 0.01 mm3. The old stored Sphere was
    off there too, 17.6 high, for the same reason.
    """
    for i in range(DIMPLE_N):
        x = DIMPLE_X0 + i * DIMPLE_STEP
        theta = math.radians(i * DIMPLE_TWIST)
        # The surface radius at this station, read off the same profile the
        # revolve used rather than guessed: the dimples sit on the swell,
        # which is the arc from x=14 to x=66.
        r = surface_radius(x)
        centre = r + DIMPLE_R - DIMPLE_BITE
        ball_at = check(d.call("add_feature", feature={
            "type": "Frame", "name": f"Grip {i + 1} centre",
            "kind": "explicit",
            "origin": [x, centre * math.sin(theta), centre * math.cos(theta)],
            "z_axis": [0.0, math.cos(theta), -math.sin(theta)],
            "x_axis": [1.0, 0.0, 0.0]}), f"grip {i + 1} centre")
        check(d.call("add_feature", feature={
            "type": "Sphere", "name": f"Grip {i + 1}", "mode": "cut",
            "on": {"feature_id": ball_at["id"]},
            "radius": DIMPLE_R}), f"grip {i + 1}")
    return solid


def surface_radius(x):
    """The handle's radius at `x`, by the same three-point arcs the sketch
    uses. Written out rather than measured off the solid because the dimples
    have to be placed BEFORE the solid exists, and a number taken from one
    description and used against another is how a part ends up almost
    right."""
    def arc_r(p0, pm, p1, at):
        # Circle through three points, then the y of it at x = `at`.
        ax, ay = p0
        bx, by = pm
        cx, cy = p1
        d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by))
        ux = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) *
              (cy - ay) + (cx * cx + cy * cy) * (ay - by)) / d
        uy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) *
              (ax - cx) + (cx * cx + cy * cy) * (bx - ax)) / d
        rad = math.hypot(ax - ux, ay - uy)
        under = rad * rad - (at - ux) ** 2
        if under < 0.0:
            raise SystemExit(f"x={at} is off this arc")
        # A vertical line meets a circle TWICE, and which root is the profile
        # depends on whether the arc bulges up (the swell, centre below) or
        # down (the waist, centre above). Picking one by hand gets the other
        # wrong: the first version took the upper root always, which put the
        # waist's dimples several millimetres outside the part, and the app
        # said so -- "subtract removed no material". Choose the root that
        # agrees with the points the arc was drawn through.
        lo, hi = uy - math.sqrt(under), uy + math.sqrt(under)
        want = (ay + by + cy) / 3.0
        return lo if abs(lo - want) < abs(hi - want) else hi

    if 14.0 <= x <= 66.0:
        return arc_r((14.0, SWELL_R - 2.0), (SWELL_X, SWELL_R),
                     (66.0, SWELL_R - 2.5), x)
    if 66.0 < x <= 112.0:
        return arc_r((66.0, SWELL_R - 2.5), (WAIST_X, WAIST_R),
                     (112.0, FERRULE_R + 2.5), x)
    raise SystemExit(f"x={x} is not on the part of the handle we dimple")


def build(d, name="Handle"):
    sk = check(d.call("add_feature", feature={
        "type": "Sketch", "name": "Handle profile",
        "plane": Plane.xz(0.0).json(), "entities": profile()}), "profile sketch")
    body = check(d.call("add_feature", feature={
        "type": "Revolve", "name": name, "sketch_id": sk["id"],
        # About X, because the rotary axis is setup-space X and a part
        # modelled across it would have to be laid onto it by a fixture
        # frame -- one more transform between the model and the trunnion,
        # for nothing.
        "axis": {"kind": "x"}, "angle_deg": 360.0,
        "mode": "new_body"}), "revolve")
    return dimples(d, body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=None)
    args = ap.parse_args()
    d = connect(args.port)
    build(d)
    mp = d.call("mass_properties")
    print(f"  volume {mp['total']['volume'] / 1000.0:.1f} cm3")
    print(f"  check_geometry: {d.call('check_geometry')}")


if __name__ == "__main__":
    main()
