"""The two internal braces.

**These are re-drawn to a rule, not transcribed.** Everything else in this
generator reproduces the Onshape design; the braces do not, and the reason is
worth stating rather than hiding in a volume that nearly matches.

The exported ring brace is not a prism — a section through it runs 18,057 mm2
at the faces and 25,133 mm2 at mid-thickness, because every edge where a wall
meets a face carries one of twenty R8 fillets, and the four spokes blend into
the ring tangentially rather than meeting it. Recovering that shape from a
STEP body, exactly, is a job for the CAD that drew it. What the export does
pin down, and what is reproduced here:

    outer                    210 x 320 x 16, centred
    bore                     r 52.5 = port_tube_od/2 — the tube is a fit,
                             not a clearance
    ring outer               r 60.5 — an 8 mm wall round the tube
    frame rail               8 mm — the window's inner edge is at x +/- 97
    fixings                  4 x d5.5 = m4_t_nutt_dia, one at each spoke
    fillets                  R8 throughout

so the brace here has the same outline, the same bore, the same ring, the same
rail and the same fixings, drawn from the parameters — and square spokes where
the original blends. `verify.py` reports the IoU against
`reference/ring_brace.step`; it does not assert a match it cannot make.

If the exact mouldings matter more than following the parameters, the two
reference STEPs are already exported and `bought.py` will import them the way
it imports the drivers.
"""
import math

from designer_sdk import Plane

MDF = "mdf"

RAIL = 8.0          # the frame rail, and the ring's wall — measured, above
SPOKE = 24.0        # square here; the export blends this into the ring


def ring_brace(app, p):
    """The panel that closes the port bay and holds the tube on its axis."""
    part = app.part("ring_brace")
    t = p["wall_thickness"]
    half_w = p["cabinet_internal_w"] / 2
    half_d = p["cabinet_internal_d"] / 2

    sk = part.sketch(Plane.xy(-t / 2), name="outline")
    sk.centered_rect((0, 0), half_w * 2, half_d * 2)
    part.extrude(sk.add(), distance=t, name="outline")
    part.material(MDF)

    # One window, then the ring and its spokes put back inside it. Cheaper to
    # reason about than four windows clipped by a circle, and it is the same
    # solid.
    sk = part.sketch(Plane.xy(t / 2), name="window")
    sk.centered_rect((0, 0), (half_w - RAIL) * 2, (half_d - RAIL) * 2)
    part.cut(sk.add(), distance=t, reversed=True)

    sk = part.sketch(Plane.xy(-t / 2), name="ring")
    sk.circle((0, 0), d=float(p["port_tube_od"]) + 2 * RAIL)
    sk.circle((0, 0), d=p["port_tube_od"])
    part.extrude(sk.add(), distance=t, mode="add", name="ring")

    r_ring = float(p["port_tube_od"]) / 2 + RAIL
    sk = part.sketch(Plane.xy(-t / 2), name="spokes")
    sk.rect((-SPOKE / 2, -float(half_d)), (SPOKE / 2, float(half_d)))
    sk.rect((-float(half_w), -SPOKE / 2), (float(half_w), SPOKE / 2))
    part.extrude(sk.add(), distance=t, mode="add", name="spokes")

    # Bore the tube's hole back through: the spokes just crossed it.
    sk = part.sketch(Plane.xy(t / 2), name="bore")
    sk.circle((0, 0), d=p["port_tube_od"])
    part.cut(sk.add(), distance=t, reversed=True)

    # The fixings the upper port flare bolts down through. On the flare's
    # own lug circle, because that is what they are for — drawn to a rail
    # width instead they landed 0.5 mm off it by luck.
    sk = part.sketch(Plane.xy(t / 2), name="fixings")
    n = int(float(p["port_brace_bolt_count"]))
    for i in range(n):
        a = 2 * math.pi * i / n
        r = float(p["port_brace_bolt_circle"]) / 2
        sk.circle((r * math.cos(a), r * math.sin(a)), d=p["m4_t_nutt_dia"])
    part.cut(sk.add(), distance=t, reversed=True)

    # The export rounds every edge at R8. Not reproduced, and not only
    # because the brace is a re-draw: asking for that set of blends here
    # SEGFAULTS the app — nine of these edges fillet, the tenth takes the
    # process down inside OpenCASCADE's blend builder
    # (scratch/smart_speaker/fillet_crash_repro.py). A router cutting this
    # part leaves the tool's own radius in the corners anyway.
    return part


def ladder(app, p):
    """The window brace under the divider, and its three support studs.

    Measured off the export rather than invented: a section runs 34,466 mm2
    through the panel and a flat 1,200 mm2 above it, which is three 20 x 20
    studs and nothing else. The panel's four slots at `window_brace_width`
    rails come to 34,344 mm2 of the same 210 x 320, so the rail width in the
    parameter table is the one the part was drawn with.

    The studs stand `brace_stud_gap` off the wall each braces — two on the
    side walls at mid depth, one on the back wall at mid width. That is what
    they are for: breaking the unsupported span of the big panels.

    The slots' R8 corners are not reproduced. They are 55 mm2 between them,
    and asking this app for that set of blends takes the process down
    (scratch/smart_speaker/fillet_crash_repro.py).
    """
    part = app.part("brace_ladder")
    t = p["wall_thickness"]
    half_w = p["cabinet_internal_w"] / 2
    half_d = p["cabinet_internal_d"] / 2
    h = float(p["brace_ladder_h"])
    rail = p["window_brace_width"]
    stud = p["brace_stud"]
    gap = p["brace_stud_gap"]

    # The window brace, at the BOTTOM of the assembly.
    base = -h / 2
    sk = part.sketch(Plane.xy(base), name="window brace")
    sk.centered_rect((0, 0), half_w * 2, half_d * 2)
    part.extrude(sk.add(), distance=t, name="window brace")
    part.material(MDF)

    # Four windows: a rail round the outside and a rail each way through the
    # middle, so the slot is what is left of each quarter.
    sk = part.sketch(Plane.xy(base + float(t)), name="windows")
    sw = (half_w - rail * 1.5) / 2
    sd = (half_d - rail * 1.5) / 2
    for sx in (-1, 1):
        for sy in (-1, 1):
            cx = sx * (rail / 2 + sw)
            cy = sy * (rail / 2 + sd)
            sk.centered_rect((cx, cy), sw * 2, sd * 2)
    part.cut(sk.add(), distance=t, reversed=True, name="windows")

    # Three studs, standing on it.
    rise = h - float(t)
    for at in ((-(half_w - stud / 2 - gap), 0.0),
               (half_w - stud / 2 - gap, 0.0),
               (0.0, half_d - stud / 2 - gap)):
        sk = part.sketch(Plane.xy(base + float(t)), name="stud")
        sk.centered_rect(at, stud, stud)
        part.extrude(sk.add(), distance=rise, mode="add", name="stud")
    return part


def placements(p):
    """The ladder hangs under the divider; the ring brace sits where the port
    passes through it."""
    divider_under = -(float(p["alpair_chamber_h"])
                      + 2 * float(p["wall_thickness"]))
    from port import RING_BRACE_MID
    return [
        ("brace_ladder", "brace ladder",
         (0, 0, divider_under - float(p["brace_ladder_h"]) / 2), None),
        ("ring_brace", "port ring brace",
         (0, 0, -float(p["carcass_h"]) + RING_BRACE_MID), None),
    ]
