"""T-nuts and dowels, seated in the holes they belong to.

Both are drawn from the holes they go in, so a change to `plug_dia` or
`m4_t_nutt_dia` moves the fixing and the hole together, and both are PLACED
through `seating.seat` rather than by a hand-written quaternion — see that
module for the three orientation bugs that cost.

A T-nut here is built along local +Z from its origin: flange first, then the
barrel. `seat(..., proud=FLANGE_T)` therefore puts the flange proud of the
face and drives the barrel into the material behind it.
"""
from designer_sdk import Plane

import seating

STEEL = "steel"
FLANGE_T = 1.5


def t_nut(app, p):
    """A flanged T-nut: a disc, a barrel that fits the clearance hole, and a
    bore for the thread."""
    part = app.part("t_nut")
    # The barrel is the CLEARANCE hole's diameter, because that is the hole it
    # goes into; the bore is the thread. Drawn wider than the hole it reported
    # 222 mm3 of interference per nut, and "fixing" that by turning the nut
    # over lifted it out of the wood altogether.
    sk = part.sketch(Plane.xy(0), name="flange")
    sk.circle((0, 0), d=p["t_nut_size"])
    part.extrude(sk.add(), distance=FLANGE_T, name="flange")
    sk = part.sketch(Plane.xy(FLANGE_T), name="barrel")
    sk.circle((0, 0), d=p["m4_t_nutt_dia"])
    part.extrude(sk.add(), distance=p["t_nut_length"] / 3, mode="add",
                 name="barrel")
    sk = part.sketch(Plane.xy(0), name="bore")
    sk.circle((0, 0), d=p["w170s_bolt_dia"])
    part.cut(sk.add(), distance=p["t_nut_length"], name="bore")
    part.material(STEEL)
    return part


def dowel(app, p):
    """A plain glue dowel, `plug_dia` by `plug_length`."""
    part = app.part("dowel")
    sk = part.sketch(Plane.xy(0), name="dowel")
    sk.circle((0, 0), d=p["plug_dia"])
    part.extrude(sk.add(), distance=p["plug_length"], name="dowel")
    part.material("wood_oak")   # beech, really; oak is the nearest slug
    return part


def placements(p):
    """The three bolt circles the T-nuts serve, each seated on its own face.

    Every ring comes from `seating.ring` with the same start angle the panel's
    holes were drawn at, so the fixings cannot drift off the holes.
    """
    out = []
    half_h = float(p["carcass_half_h"])
    carcass = float(p["carcass_h"])
    wall = float(p["wall_thickness"])

    # Behind the baffle: flange on the cabinet side, barrel into the panel.
    baffle_inside = -(float(p["cabinet_internal_d"]) / 2)
    z_w170s = -2 * half_h + float(p["w170s_center_from_floor"])
    for i, (x, dz) in enumerate(seating.ring(p["w170s_bolt_circle"],
                                             p["w170s_bolt_count"])):
        pos, q = seating.seat(on=baffle_inside, axis="-y",
                              at=(x, z_w170s + dz), proud=FLANGE_T)
        out.append(("t_nut", f"T-nut W170S {i+1}", pos, q))

    # Under the port-ring brace: the upper flare bolts DOWN onto the brace
    # from above, so the nuts take those bolts underneath. On the flare's own
    # lug circle — the same parameter the brace is drilled to.
    from port import RING_BRACE_MID
    brace_under = -carcass + RING_BRACE_MID - wall / 2
    for i, (x, y) in enumerate(seating.ring(p["port_brace_bolt_circle"],
                                            p["port_brace_bolt_count"],
                                            start_deg=0.0)):
        pos, q = seating.seat(on=brace_under, axis="+z", at=(x, y),
                              proud=FLANGE_T)
        out.append(("t_nut", f"T-nut brace {i+1}", pos, q))

    # INSIDE the bay floor: the lower flare hangs underneath and its bolts
    # come UP through the wall, so the nuts sit on the floor's top face.
    # Underneath is where the flange is.
    floor_inside = -carcass + wall
    for i, (x, y) in enumerate(seating.ring(p["outer_port_bolt_circle"],
                                            p["outer_port_bolt_count"])):
        pos, q = seating.seat(on=floor_inside, axis="-z", at=(x, y),
                              proud=FLANGE_T)
        out.append(("t_nut", f"T-nut port {i+1}", pos, q))
    return out
