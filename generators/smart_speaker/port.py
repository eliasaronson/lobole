"""The reflex port: a straight tube with a flare on each end.

The tube is exactly what the parameters say — pi/4 (105^2 - 100^2) x 250 is
201,278 mm3 against the export's 201,258, so `port_tube_od`,
`port_internal_dia` and `port_length_total` describe it and nothing here is
transcribed.

The flares are IMPORTED, not modelled. They were revolved here first, from the
throat and mouth diameters the table names, and came out at 0.245 and 0.192 of
the export's volume: a swept 2.5 mm shell is not what those parts are. They
are mouldings — a flange, ribs, the little feet on the upper one — and
reproducing that from a STEP body would be drawing a bought part badly. They
go through `bought.py` with the drivers.

The tube stays here, and it is the one that matters: `port_length_total` and
`port_internal_dia` are what tune the box, the run prints the Helmholtz fb
they give, and a bought solid could not follow either. Its volume matches the
export to four figures.

## Where the parts sit

Anchored to the cabinet's outer floor, because that is what the port is bolted
through. Each offset below says what it seats against; none of them is a
coordinate read off the model and left unexplained.
"""
from designer_sdk import Plane

import seating

ABS = "plastic_black"   # the flares and tube are moulded plastic, not MDF

# The stack, bottom up, all measured from the cabinet's OUTER floor
# (z = -carcass_h) because that is the face the port is bolted to:
#
#   the lower flare hangs BELOW the floor, in the plinth, mouth down, its
#   flange bolted up through the bottom wall;
#   the tube starts at the floor's underside and passes up through the
#   115 hole and on through the ring brace;
#   the upper flare sits on the tube's top end, mouth up into the bay.
#
# Both flares are imported and arrive CENTRED on their own origin, so they are
# placed by their middles; the tube is built here from z = 0 upward and is
# placed by its bottom. That difference is worth the sentence — mixing the two
# up put the lower flare 25 mm into the floor and mouth-first into the room.
TUBE_SEAT = 0.0             # the tube's bottom, on the floor's underside
RING_BRACE_MID = 236.8      # the brace the tube passes through


def _floor(p):
    return -float(p["carcass_h"])


def tube(app, p):
    """The straight section: `port_length_total` of `port_tube_od` over
    `port_internal_dia`."""
    part = app.part("port_tube")
    sk = part.sketch(Plane.xy(0), name="tube")
    sk.circle((0, 0), d=p["port_tube_od"])
    sk.circle((0, 0), d=p["port_internal_dia"])
    part.extrude(sk.add(), distance=p["port_length_total"], name="tube")
    part.material(ABS)
    return part


def acoustic_length(p):
    """The air path, end to end — the tube plus what each flare adds beyond
    where it swallows the tube. This, not `port_length_total`, is what tunes
    the box, and it is what `helmholtz_fb` should be asked about."""
    return (float(p["port_length_total"])
            + 2 * float(p["port_flare_h"])
            - (FLARE_BOTTOM_TOP - TUBE_SEAT))


def placements(p):
    z0 = _floor(p)
    half = float(p["port_flare_h"]) / 2
    tube_top = z0 + TUBE_SEAT + float(p["port_length_total"])
    return [
        # TURNED OVER, mouth down. The Onshape assembly places this
        # occurrence at quat [1,0,0,0] and it reads unambiguously in a
        # section: the trumpet hangs below the cabinet floor and opens
        # downward into the plinth, throat up through the hole. Identity
        # points the mouth back into the cabinet.
        ("port_flare_bottom", "port flare (floor)",
         (0, 0, z0 - half), seating.quat("-z")),
        ("port_tube", "port tube", (0, 0, z0 + TUBE_SEAT), None),
        ("port_flare_top", "port flare (bay)",
         (0, 0, tube_top + half), None),
    ]
