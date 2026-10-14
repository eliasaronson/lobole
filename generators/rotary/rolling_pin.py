#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A barley-twist rolling pin, to cut on the 4th axis.

Six helical flutes down a Phi 50 pin: the rope twist you see on Jacobean
table legs, and an embossing pin that rolls a diagonal rope into dough. It is
here because it is the clearest thing a 4-axis router can make and a LATHE
CANNOT -- a lathe spins the work against a still tool, so every shape it can
cut is a body of revolution, and a twist is precisely not one. The work has
to turn WHILE the cutter travels, which is the whole definition of the axis.

  (//////////////////////////////////////////////////////)
   six flutes, one 45-degree turn every 157 mm, 300 mm of Phi 50

A 45-degree lead, which is what makes the twist read as a rope rather than as
stripes or as rings: at that angle the flute travels as far round the pin as
it does along it. Cut from a Phi 54 dowel in maple or
beech: the surface takes 2 mm off the radius and the flutes 4 mm more, so
nothing here asks a finishing pass to remove more than it should -- which
matters, because rotary roughing is layers, not constant engagement
(docs/cam/rotary.md#limits).

Run against a running instance:
  python3 generators/rotary/rolling_pin.py --port N
"""
import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from lobole.sdk import Plane  # noqa: E402
from _lib import arc_through, check, connect, line  # noqa: E402

# --- the pin, in millimetres ----------------------------------------------
LENGTH = 300.0
R = 25.0              # Phi 50: a pin you can get both hands round
DOME = 10.0           # the rounded ends, so it has no corner to bruise dough
GROOVE_D = 8.0        # a Phi 8 pipe on the surface is a 4 mm round-bottomed
                      # groove -- and a 6 mm ball can reach the bottom of it
STARTS = 6            # flutes

# A 45-degree lead makes the diamonds square, and that is arithmetic rather
# than taste: a helix at radius R advances its pitch in one turn while
# travelling 2*pi*R round, so pitch = 2*pi*R is the pitch at which the two
# distances are equal.
PITCH = 2.0 * math.pi * R


def twist(d, solid):
    """Six helical flutes, all the same hand: a barley twist.

    ONE hand, and that is the whole design decision. The first version was a
    diamond lattice -- six right and six left -- which is prettier on paper
    and asks the kernel for thirty-six boolean crossings where two B-spline
    pipes meet the cylinder TANGENTIALLY, because a pipe whose centreline
    lies on the surface has its equator in that surface. It came back
    "self-intersecting wire, unorientable shape", and lifting the grooves
    clear to make the crossings transversal traded that for a part with
    0.2 cm3 left in it. A single hand has no crossings at all, is valid
    first time, and is the shape everybody recognises as turned-and-twisted
    anyway.

    A circular `Pattern` of the flute would say what this means -- six
    copies of one thing at six angles -- and is refused: a Pipe that CUTS
    consumes its tool body, so there is nothing left to repeat, and the
    refusal says so by name ("an Extrude that cuts or adds patterns its tool
    instead"). Extrude is special-cased for this; Pipe is not.

    So the six are written out, each phased by starting its helix a sixth of
    a pitch further BACK -- the same thing as a sixth of a turn round, and
    the only lever a helix offers, since it takes an origin and no phase.
    That lead-in is why the twist runs the WHOLE length: off the end it is
    air, and inside a smooth grip it would be a flute where the design says
    there is none.
    """
    for k in range(STARTS):
        # A sixth of a pitch further back per flute, and no more. Backing
        # every helix off a WHOLE pitch as well -- to run each flute clean
        # through the end face rather than starting its pipe there -- cost a
        # flute: the first one came back "cut removed no material" on a path
        # that plainly crosses the pin. Where the flutes die into the domed
        # end is what a barley twist looks like anyway.
        x0 = -PITCH * k / STARTS
        curve = check(d.call("add_feature", feature={
            "type": "Curve3D", "name": f"Helix {k + 1}", "kind": "helix",
            "origin": [x0, 0.0, 0.0], "axis": [1.0, 0.0, 0.0],
            "radius": R, "pitch": PITCH, "turns": (LENGTH - x0) / PITCH}),
            f"helix {k + 1}")
        # A solid pipe, not a tube: wall_thickness 0 means the whole section
        # is the cutter.
        check(d.call("add_feature", feature={
            "type": "Pipe", "name": f"Flute {k + 1}", "path_id": curve["id"],
            "outer_diameter": GROOVE_D, "wall_thickness": 0.0,
            "mode": "cut"}), f"flute {k + 1}")
    return solid


def round_ends(d):
    """The two domed ends, cut AFTER the lattice.

    Revolving the finished profile and cutting the grooves into it is the
    obvious order and it does not survive: twelve B-spline pipe booleans
    against a body that already has toroidal ends left a 583 cm3 pin
    weighing one, with four "cut removed no material" warnings on grooves
    that plainly cross it. Cutting the same grooves into a plain CYLINDER
    works, so the domes go on last -- as the waste outside each corner,
    revolved away.

    The other orders were tried: intersecting the latticed cylinder with a
    domed revolve came back with BOTH bodies and their volumes added, which
    is a boolean that did nothing and said nothing. The part is shaped
    around it rather than waiting for it.
    """
    c45 = math.cos(math.radians(45))
    s45 = math.sin(math.radians(45))
    ends = {
        "Front": [line((0.0, R - DOME), (0.0, R)),
                  line((0.0, R), (DOME, R)),
                  arc_through((DOME, R),
                              (DOME * (1.0 - c45), R - DOME + DOME * s45),
                              (0.0, R - DOME))],
        "Back": [line((LENGTH, R - DOME), (LENGTH, R)),
                 line((LENGTH, R), (LENGTH - DOME, R)),
                 arc_through((LENGTH - DOME, R),
                             (LENGTH - DOME + DOME * c45,
                              R - DOME + DOME * s45),
                             (LENGTH, R - DOME))],
    }
    for name, ents in ends.items():
        sk = check(d.call("add_feature", feature={
            "type": "Sketch", "name": f"{name} corner",
            "plane": Plane.xz(0.0).json(), "entities": ents}), f"{name} sketch")
        check(d.call("add_feature", feature={
            "type": "Revolve", "name": f"{name} round", "sketch_id": sk["id"],
            "axis": {"kind": "x"}, "angle_deg": 360.0,
            "mode": "cut"}), f"{name} round")


def build(d, name="Rolling pin"):
    # A plain cylinder, not the finished profile revolved: see round_ends.
    body = check(d.call("add_feature", feature={
        "type": "Cylinder", "name": name,
        # On Right, so about X: the rotary axis is setup-space X and a part
        # modelled across it would need a fixture frame to lay it down --
        # one more transform between the model and the trunnion, for nothing.
        "on": "Right", "radius": R, "height": LENGTH}), "barrel")
    twist(d, body)
    round_ends(d)
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
    print(f"  valid: {geo['valid']}  bodies: {geo['body_count']}")
    for e in geo.get("feature_errors", []):
        print(f"  ! {e}")


if __name__ == "__main__":
    main()
