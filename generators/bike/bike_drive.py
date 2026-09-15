"""The drivetrain: crankset, pedals, chain and chainguard.

The crankset turns, so it is built on ITS OWN AXIS — local Z along the bottom
bracket — and mated by an "axis" frame. Its local -Z is the bicycle's RIGHT
(the mate takes local Z to the frame's "bb" Z, which is world +Y, and +Y is
left), so the chainring sits at local -CHAINLINE and lands on the drive side
without anything having to be flipped later.

The chain and the guard are drawn in WORLD coordinates and fastened to the
frame. They are the same outline — `tangent_hull` round the chainring and the
cog — because a chainguard is drawn round the chain.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
from designer_sdk import Plane, V, _id  # noqa: E402
import bike_shop as shop  # noqa: E402
from bike_shop import (axle_frame, cut, disc, seat_frame,  # noqa: E402
                       sprocket_outline, tangent_hull, tube, union)


def _extrude(part, plane, loops, distance, name, holes=()):
    """A sketch of closed loops, extruded. The first loop is the outline and
    any others are holes in it."""
    sk = part.sketch(plane, name=f"{name} profile")
    sk.polyline([(float(u), float(v)) for u, v in loops], close=True)
    for hole in holes:
        if isinstance(hole, dict):
            sk.circle(hole["center"], d=hole["d"])
        else:
            sk.polyline([(float(u), float(v)) for u, v in hole], close=True)
    return part._add({"type": "Extrude", "name": name,
                      "sketch_id": _id(sk.add()), "mode": "new_body",
                      "distance": float(distance)})["id"]


def crankset(app, S):
    """Both arms, the spindle and the chainring, as one turning part.

    One part and not three because they turn as one: a separate chainring
    would need a mate whose only job is to say "it does not move relative to
    the crank", and a bolt circle to hang it on that no photograph shows.
    """
    part = app.part("Crankset")
    # The arms sweep OUTSIDE the chainguard, which sits just outside the
    # chain: that is the chain of clearances that fixes this number, and it
    # is why a bicycle with a full guard has a wide crank.
    arm_z = float(S["CRANK_OFFSET"])
    arm_t = float(S["CRANK_T"])
    ring_z = -float(S["CHAINLINE"])
    length = float(S["CRANK_LEN"])
    ids = []

    ids.append(tube(part, (0, 0, -62), (0, 0, 62), S["SPINDLE_D"], 0.0,
                    "Spindle"))
    # The bottom bracket's cups. Like the headset, they are what makes the
    # crank TOUCH the frame: a 17 mm spindle in a 34.8 mm shell touches
    # nothing, and a bottom bracket held on by the chain alone is not one.
    for side in (+1, -1):
        ids.append(shop.revolved(
            part, [(7.5, side * 26.0), (17.6, side * 26.0),
                   (17.6, side * 38.0), (7.5, side * 38.0)],
            (0, 0, 0), (0, 0, 1), f"Cup {side:+d}"))
    for side, direction in ((-1, +1), (+1, -1)):
        # The two arms point opposite ways: that is what a crank IS. And
        # each TAPERS — 32 mm across at the axle, 22 at the pedal — which is
        # not decoration: a parallel arm 28 wide fouled the chainstay as it
        # came round, and a real crank is narrow at the end for that reason.
        ids.append(_extrude(
            part, Plane.xy(side * arm_z - arm_t / 2.0),
            tangent_hull((0, 0), float(S["CRANK_W"]) / 2.0 + 2.0,
                         (direction * length, 0),
                         float(S["CRANK_W"]) / 2.0 - 3.0,
                         steps=16),
            arm_t, f"Arm {side:+d}"))

    # The spider carries the ring off the right arm, and is thick enough to
    # overlap it: 0.5 mm of overlap is a boolean waiting to fail.
    # The spider reaches from the ring's plane out to the arm's, so it has
    # to pass THROUGH the guard's eye — which is why the eye is 132 and not
    # 96.
    spider_z = -arm_z + arm_t / 2.0
    for k in range(int(S["CHAINRING_ARMS"])):
        # Turned half a spacing off the crank arm: in line with it, one
        # spider arm lies entirely inside the arm and its union "added no
        # material", which is the app telling you a feature is doing nothing.
        a = 2 * math.pi * (k + 0.5) / int(S["CHAINRING_ARMS"])
        sk = part.sketch(Plane.xy(spider_z), name=f"Spider {k} profile")
        sk.slot((0, 0), (58.0 * math.cos(a), 58.0 * math.sin(a)), 15.0)
        ids.append(part._add({"type": "Extrude", "name": f"Spider {k}",
                              "sketch_id": _id(sk.add()), "mode": "new_body",
                              "distance": abs(spider_z - ring_z) + 1.0})["id"])

    ids.append(_extrude(
        part, Plane.xy(ring_z - float(S["CHAINRING_T_MM"]) / 2.0),
        sprocket_outline(int(S["CHAINRING_T"]), float(S["CHAIN_PITCH"]),
                         float(S["CHAIN_ROLLER_D"])),
        float(S["CHAINRING_T_MM"]), "Chainring",
        holes=[{"center": (0, 0), "d": 124.0}]))

    body = union(part, ids, "Crank build")
    del body
    shop.finish(part, "steel", "chrome")
    axle_frame(part, "axis", (0, 0, 0), (0, 0, 1), x=(1, 0, 0))
    # The pedal eyes. Each frame's Z points OUTBOARD, so one pedal part
    # placed on either frame comes out the right way round on both sides.
    axle_frame(part, "pedal right", (length, 0, -arm_z - arm_t / 2.0),
               (0, 0, -1), x=(1, 0, 0))
    axle_frame(part, "pedal left", (-length, 0, arm_z + arm_t / 2.0),
               (0, 0, 1), x=(1, 0, 0))
    shop.verified(part, 1)
    return part


def pedal(app, S):
    """A rubber-block pedal, on its own axis: local Z runs OUTBOARD from the
    crank's face, so the same part serves both sides.

    The body runs ALONG the axle, which is worth saying because the first
    version drew it across one: a flat plate normal to Z, 62 x 92, standing
    up in front of the crank like a paddle.
    """
    part = app.part("Pedal")
    length = float(S["PEDAL_L"])
    width = float(S["PEDAL_W"])
    high = float(S["PEDAL_H"]) / 2.0
    rail = width / 2.0 - 2.0
    steel = [tube(part, (0, 0, -14), (0, 0, length + 18), 12.0, 0.0, "Axle")]
    for x in (-rail, rail):                    # the cage's side rails
        steel.append(part._add({
            "type": "Box", "name": f"Rail {x:+.0f}",
            "position": [x - 2.5, -high, 12.0],
            "width": 5.0, "height": 2 * high, "depth": length})["id"])
    for z in (14.0, length + 8.0):             # webs tying them to the axle
        steel.append(tube(part, (-rail, 0, z), (rail, 0, z), 5.0, 0.0,
                          f"Web {z:.0f}"))
    union(part, steel, "Cage")
    # The rubber, which is most of what a foot stands on and a tenth of what
    # this pedal weighed when the blocks were steel too.
    for side in (+1, -1):
        part._add({"type": "Box", "name": f"Block {side:+d}",
                   "position": [-(rail + 1.0), 4.0 if side > 0 else -high,
                                14.0],
                   "width": 2 * (rail + 1.0), "height": high - 4.0,
                   "depth": length - 4.0})
    part.app.set_material(body=0, material="steel")
    shop.look(part, "chrome", body=0)
    for index in (1, 2):
        part.app.set_material(body=index, material="rubber")
        shop.look(part, "black", body=index)
    axle_frame(part, "axis", (0, 0, 0), (0, 0, 1), x=(1, 0, 0))
    shop.verified(part, 3)
    return part


def _chainstay_at(S, x):
    """The DRIVE-SIDE chainstay's axis at a given x, in world coordinates —
    from the frame's own geometry, not from a guess about it.

    Its y matters and is easy to drop: the stay necks in over its first half
    (for the crank) and flares to the dropout, so a bracket aimed at the
    guard's own plane misses it by 27 mm in the middle and lands on it at
    the ends.
    """
    import bike_frame
    g = bike_frame.rear_triangle(S)
    t = (float(x) - g["chain_a"][0]) / (g["drop"][0] - g["chain_a"][0])
    p = bike_frame._stay_at(g["chain_a"], g["drop"], (0, 1, 0),
                            g["chain_bow"], max(0.0, min(1.0, t)))
    return (p[0], -p[1], p[2])


def _drive_circles(S):
    """The chainring and the cog, as circles in the bicycle's own plane."""
    ra = S["REAR_AXLE"]
    return ((0.0, 0.0), float(S["CHAINRING_R"]), (ra[0], ra[2]),
            float(S["COG_R"]))


def _drive_plane(S, y):
    """A sketch plane in the bicycle's plane of symmetry, offset to `y`, with
    U = world X and V = world Z — so a profile drawn in (x, z) goes in as it
    is read off the layout."""
    return Plane((0.0, float(y), 0.0), (0, -1, 0), (1, 0, 0))


def chain(app, S):
    """The chain as a closed band on the two pitch circles.

    Not a hundred and six links: what a chain contributes to this model is
    where it runs and what it fouls, and a band of the right section on the
    right path answers both. The links are named in "not drawn".
    """
    part = app.part("Chain")
    c1, r1, c2, r2 = _drive_circles(S)
    half = 3.75                      # the plates' height, either side of pitch
    y = -float(S["CHAINLINE"])
    outer = tangent_hull(c1, r1 + half, c2, r2 + half)
    inner = tangent_hull(c1, r1 - half, c2, r2 - half)
    # The plane's normal is -Y, so an extrude runs to the RIGHT of it: start
    # half a chain's width to the LEFT to end up centred on the chainline.
    body = _extrude(part, _drive_plane(S, y + float(S["CHAIN_WIDTH"]) / 2.0),
                    outer, float(S["CHAIN_WIDTH"]), "Chain", holes=[inner])
    del body
    shop.finish(part, "steel", "oiled")
    seat_frame(part)
    shop.verified(part, 1)
    return part


def chainguard(app, S):
    """The teardrop guard: a disc over the chainring with an eye the crank
    shows through, a lip turned in over the teeth, and a tapering tail along
    the chain's TOP run.

    The tail is a strip and not the other half of the chain's own outline,
    which is what it was first drawn as. A guard shaped like the whole chain
    loop has to pass through the chainstay — the stay runs between the two
    runs of the chain for its whole length — and it did, 3.5 cm3 of it. What
    a roadster's guard actually covers is the top run and the ring; the
    bottom run is open, and that is why.
    """
    part = app.part("Chainguard")
    c1, r1, c2, r2 = _drive_circles(S)
    y = -(float(S["CHAINLINE"]) + float(S["GUARD_CLEAR"]))
    disc_r = r1 + float(S["GUARD_DISC_MARGIN"])
    plane = _drive_plane(S, y)

    # Where the tail ends, and the line it runs along: the common tangent
    # over the top of the ring and the cog, which is the chain's own top run.
    short = float(S["GUARD_TAIL_SHORT"]) / math.hypot(c2[0] - c1[0],
                                                      c2[1] - c1[1])
    tail = (c2[0] + (c1[0] - c2[0]) * short, c2[1] + (c1[1] - c2[1]) * short)
    span = math.hypot(tail[0] - c1[0], tail[1] - c1[1])
    base = math.atan2(tail[1] - c1[1], tail[0] - c1[0])
    off = math.acos((disc_r - float(S["GUARD_TAIL_R"])) / span)
    up = base - off                      # the tangent over the TOP
    n = (math.cos(up), math.sin(up))
    w = float(S["GUARD_TAIL_W"])
    a = (c1[0] + disc_r * n[0], c1[1] + disc_r * n[1])
    b = (tail[0] + float(S["GUARD_TAIL_R"]) * n[0],
         tail[1] + float(S["GUARD_TAIL_R"]) * n[1])
    strip = [a, b, (b[0] - w * n[0], b[1] - w * n[1]),
             (a[0] - w * n[0], a[1] - w * n[1])]

    sk = part.sketch(plane, name="Guard plate profile")
    sk.circle((0, 0), d=2 * disc_r)
    sk.circle((0, 0), d=float(S["GUARD_EYE_D"]))
    plate = part._add({"type": "Extrude", "name": "Guard plate",
                       "sketch_id": _id(sk.add()), "mode": "new_body",
                       "distance": float(S["GUARD_T"])})["id"]
    tail_id = _extrude(part, plane, strip, float(S["GUARD_T"]), "Guard tail")
    # The lip that turns in over the teeth. Three things about it are not
    # free:
    #   its AXIS is -Y, not +Y — with +Y the sketch's V comes out as -Z
    #   (V = normal x u), the angles run DOWNWARD, and the lip wrapped the
    #   bottom of the ring, straight through the chain's lower run;
    #   it stops INBOARD at the crank arm's outer face, because the arm
    #   sweeps this radius every revolution and passes between the ring and
    #   the guard;
    #   and it stops at the angle where the CHAIN leaves the ring, because
    #   from there the chain runs outside this radius, not inside it.
    # The lip turns IN over the teeth — toward the bicycle, into the space
    # between the guard and the chain, which is where a pressed guard's
    # rolled edge goes.
    lip = shop.arc_tube(part, (0, y + 2.5, 0), (0, -1, 0), disc_r - 1.0,
                        -100.0, math.degrees(up) - 4.0, 7.0, 1.0,
                        ref=(1, 0, 0), name="Lip")
    # The bracket that bolts the tail to the chainstay. Without it the guard
    # is held on by nothing — it came back as its own connected component,
    # which is exactly what a chainguard lying beside a bicycle is.
    where = 0.72                     # of the way along the tail
    top = (a[0] + (b[0] - a[0]) * where, a[1] + (b[1] - a[1]) * where)
    foot = (top[0] - w * n[0], top[1] - w * n[1])
    stay = _chainstay_at(S, top[0])   # the same x, or it is a diagonal strut
    bracket = tube(part, stay, (top[0], y, foot[1] + 3.0), 6.0, 0.0,
                   "Guard bracket")
    union(part, [plate, tail_id, lip, bracket], "Guard")
    shop.finish(part, "aluminum", "dull_alloy")
    seat_frame(part)
    shop.verified(part, 1)
    return part
