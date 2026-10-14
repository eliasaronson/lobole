# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The frame set: the green lugged frame, and the fork.

Both are built in WORLD coordinates — the bicycle's own, with the bottom
bracket at the origin — and carry a "seat" frame there. Everything that does
not move relative to them (fenders, rack, saddle, bars) is drawn in the same
coordinates and fastened by that one frame, so a part lands where it was
drawn instead of where a chain of offsets says it should.

The three frames that DO move are the joints: "bb" (the crank's axis), "rear
axle", and "head axis" — the last of which the fork carries too, at the same
world point, which is what makes the revolute between them the steering.

Tubes are PIPES, not rods. A solid steel frame of these dimensions weighs
8.6 kg; this one weighs 2.8, which is what a 1970s gas-pipe roadster frame
actually weighs, and the mass check is the reason the wall thickness is in
the spec at all.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
from lobole.sdk import V  # noqa: E402
import bike_shop as shop  # noqa: E402
from bike_shop import (Weldment, axle_frame, cut, disc,  # noqa: E402
                       seat_frame, tube)


# How far a brazed joint runs SOLID, at an end that is welded into another
# tube. Not a spec dimension: it is a consequence of how a lugged frame is
# made, and a NEGATIVE value means the opposite — a bore that runs out past
# the end, for a tube that has to stay open (the seat tube, the head tube,
# the bottom bracket shell).
WELD = 28.0


def _along(p, direction, distance):
    return tuple(V(p) + V(direction).unit() * float(distance))


def _short(a, b, back):
    """`b`, pulled back toward `a` by `back` mm.

    Both stays are drawn to the dropout's CENTRE, which is also the axle's
    axis — so both of their flat end caps pass through the line the axle hole
    is drilled along, and the drill met two coincident cap faces on one axis.
    The cut came back "empty or invalid solid" on one side and, with a
    slightly smaller drill, silently produced a body with bad wire nesting.
    Stopping the stays 12 mm inside the dropout plate (which is 52 across)
    keeps them brazed into it and keeps their caps off the axle's line.
    """
    a, b = V(a), V(b)
    return tuple(b - (b - a).unit() * float(back))


def _stay_at(a, b, bulge, sagitta, t):
    """A point on a bowed tube's axis, `t` of the way along it.

    The bow is the reason this exists: a chainstay leaves the bottom bracket
    at y = 28 and arrives at the dropout at y = 60, and bows 7 mm further out
    in between, so nothing that has to touch it can be placed on the straight
    line between its ends.
    """
    a, b = V(a), V(b)
    u = (b - a).unit()
    v = V(bulge) - u * u.dot(V(bulge))
    v = v.unit() if v.length() > 1e-9 else V(0, 0, 0)
    return tuple(a + (b - a) * float(t)
                 + v * (4.0 * float(sagitta) * t * (1.0 - t)))


def rear_triangle(S):
    """Where the rear triangle's tubes run, as points.

    The frame builds its stays from this and the rack and mudguard bolt to
    the same points — a rack leg written to its own idea of where a seat stay
    is ends up in the air beside it.
    """
    half = float(S["BB_SHELL_W"]) / 2.0
    dy = (float(S["REAR_OLD"]) + float(S["DROPOUT_T"])) / 2.0
    ra = S["REAR_AXLE"]
    stay_top = _along(S["ST_TOP"], S["SEAT_DIR"], -34.0)
    seat_a = (stay_top[0], 11.0, stay_top[2])
    chain_a = (0.0, float(S["CHAINSTAY_FOOT_Y"]), float(S["CHAINSTAY_FOOT_Z"]))
    drop = (ra[0], dy, ra[2])
    return {
        "dy": dy, "drop": drop, "seat_a": seat_a, "chain_a": chain_a,
        # The chainstay bows outward around the tyre. It once had to NECK IN
        # instead, to dodge a crank whose arms ran 46 mm off the centre line
        # — the motion study caught the left pedal striking it 16 degrees
        # past top dead centre. The crank is 62.5 mm out now, for the
        # chainguard's sake, and the stay is clear of it either way.
        "seat_bow": 4.0, "chain_bow": 5.0,
        # The seat-stay bridge: the rear brake and the mudguard hang on it.
        "brake": _stay_at(seat_a, drop, (0, 1, 0), 4.0, 0.30),
        "chain_bridge": _stay_at(chain_a, drop, (0, 1, 0), 7.0, 0.18),
    }


def seat_stay_at(S, t):
    g = rear_triangle(S)
    return _stay_at(g["seat_a"], g["drop"], (0, 1, 0), g["seat_bow"], t)


def frame(app, S):
    """The frame: seven tubes, a bottom-bracket shell and two dropouts,
    welded solid and then bored."""
    part = app.part("Frame")
    w = float(S["TUBE_WALL"])
    bb, st_top, ra = S["BB"], S["ST_TOP"], S["REAR_AXLE"]
    sd = S["SEAT_DIR"]
    half = float(S["BB_SHELL_W"]) / 2.0
    geom = rear_triangle(S)
    dy = geom["dy"]
    weld = Weldment(part, "Frame weld")

    # The bottom bracket shell: bored right through and a little past, since
    # its bore is what the crank's bearings sit in.
    weld.tube((0, -half, 0), (0, half, 0), S["BB_SHELL_D"],
              (float(S["BB_SHELL_D"]) - 34.8) / 2.0, "BB shell", weld=(-4, -4))
    # Both tubes run PAST the bottom bracket's centre, not up to it — which
    # is what a mitred tube brazed round a shell does.
    # Far enough past the centre that the two tubes' solids OVERLAP there
    # rather than meet at a point, and not so far that either shows outside
    # the shell.
    foot = 10.0
    weld.tube(_along(bb, sd, -foot), st_top, S["SEAT_TUBE_D"], w, "Seat tube",
              weld=(WELD, -6))            # open at the top, for the seatpost
    weld.tube(_along(bb, V(S["DT_HEAD"]).unit(), -foot), S["DT_HEAD"],
              S["DOWN_TUBE_D"], w, "Down tube")
    weld.tube(S["TT_REAR"], S["TT_FRONT"], S["TOP_TUBE_D"], w, "Top tube")
    weld.tube(S["HT_BOT"], S["HT_TOP"], S["HEAD_TUBE_D"], float(S["LUG_T"]),
              "Head tube", weld=(-6, -6))  # open both ends, for the steerer

    # The rear triangle, twice. The stays bow outward around the tyre.
    for side in (+1, -1):
        def flip(p):
            return (p[0], side * p[1], p[2])
        for key, dia, bow in (("chain_a", S["CHAIN_STAY_D"], geom["chain_bow"]),
                              ("seat_a", S["SEAT_STAY_D"], geom["seat_bow"])):
            start = flip(geom[key])
            weld.tube(start, _short(start, flip(geom["drop"]), 12.0),
                      dia, w,
                      f"{'Chain' if key == 'chain_a' else 'Seat'}stay "
                      f"{side:+d}", bulge=(0, side, 0), sagitta=bow)
        # The dropout: a plate on the axle, which is also what the stays land
        # on — they end at its face, not in mid-air.
        weld.solid(disc(part, flip(geom["drop"]), (0, 1, 0),
                        2 * float(S["DROPOUT_R"]), S["DROPOUT_T"],
                        f"Dropout {side:+d}"))

    # Bridges: the rear brake and the mudguard hang on the seat-stay one, the
    # carrier's legs on the chainstay one. They have to LAND on the stays,
    # and the stays bow — `rear_triangle` is where they actually are.
    for key, od in (("brake", 12.0), ("chain_bridge", 12.0)):
        at = geom[key]
        weld.tube((at[0], -at[1], at[2]), at, od, 1.0, f"{key} bridge",
                  weld=(8, 8))

    body = weld.weld()
    # The axle's hole, drilled in each dropout SEPARATELY. One tool through
    # both of them crosses 130 mm of open air between the plates and enters
    # and leaves material four times; OCC fused that into a body with "bad
    # wire nesting" — valid enough to pass a solid count and a feature-error
    # check, and wrong enough that every interference against it came back
    # with the TOOL's volume (a 78 mm disc 40 mm outside the frame's own
    # bounding box read as 98% buried in it). check_geometry is what caught
    # it, which is why every part here now gates on its receipt.
    for side in (+1, -1):
        slot = tube(part, (ra[0], side * (dy - 14.0), ra[2]),
                    (ra[0], side * (dy + 14.0), ra[2]),
                    float(S["AXLE_D"]) + float(S["AXLE_FIT"]), 0.0,
                    f"Axle bore {side:+d}")
        body = cut(part, body, slot, f"Rear axle bore {side:+d}")

    shop.finish(part, "steel", "green")
    seat_frame(part)
    axle_frame(part, "bb", (0, 0, 0), (0, 1, 0), x=(1, 0, 0))
    axle_frame(part, "rear axle", ra, (0, 1, 0), x=(1, 0, 0))
    axle_frame(part, "head axis", S["HT_BOT"], S["HEAD_UP"],
               x=S["STEER_DOWN"])
    # A mudguard is built on its own axle with local Z to the RIGHT, so the
    # frame offers it an axle frame that way round rather than making the
    # mate turn it.
    axle_frame(part, "rear mudguard", ra, (0, -1, 0), x=(1, 0, 0))
    shop.verified(part, 1)
    return part


def fork(app, S):
    """The fork: a steerer, a crown, two bowed blades and two dropouts.

    Its "head axis" frame is at the SAME world point as the frame's, which is
    what the steering revolute is between. The rake is not drawn into the
    blade as a curve and hoped for — the axle's position came out of the
    layout, and the blade is the tube that reaches it.
    """
    part = app.part("Fork")
    w = float(S["TUBE_WALL"])
    fa, crown = S["FRONT_AXLE"], S["FORK_CROWN"]
    dy = (float(S["FRONT_OLD"]) + float(S["DROPOUT_T"])) / 2.0
    weld = Weldment(part, "Fork weld")

    steerer_top = _along(S["HT_TOP"], S["HEAD_UP"], 58.0)
    steerer_bot = _along(crown, S["STEER_DOWN"], 6.0)
    weld.tube(steerer_bot, steerer_top, 25.4, 1.6, "Steerer", weld=(WELD, -6))
    weld.solid(disc(part, crown, S["HEAD_UP"], 54.0, 18.0, "Crown"))
    # The headset. Without it the steerer hangs in a 1.7 mm-larger head tube
    # touching nothing, and the fork, the front wheel, the bars and the front
    # mudguard come back as a group FLOATING beside the bicycle — which they
    # would be. A headset is what joins a fork to a frame, and its cups are
    # a fifth of a millimetre into the head tube's bore.
    bore = float(S["HEAD_TUBE_D"]) - 2 * float(S["LUG_T"]) + 0.4
    for at, into in ((S["HT_BOT"], 1.0), (S["HT_TOP"], -1.0)):
        weld.solid(disc(part, _along(at, S["HEAD_UP"], into * 7.0),
                        S["HEAD_UP"], bore, 14.0,
                        f"Headset cup {'lower' if into > 0 else 'upper'}"))
    for side in (+1, -1):
        top = (crown[0], side * 23.0, crown[2])
        weld.tube(top, _short(top, (fa[0], side * dy, fa[2]), 10.0), 22.0, w,
                  f"Blade {side:+d}", bulge=S["STEER_DOWN"], sagitta=-13.0)
        weld.solid(disc(part, (fa[0], side * dy, fa[2]), (0, 1, 0), 40.0,
                        S["DROPOUT_T"], f"Fork end {side:+d}"))
    body = weld.weld()
    for side in (+1, -1):
        slot = tube(part, (fa[0], side * (dy - 12.0), fa[2]),
                    (fa[0], side * (dy + 12.0), fa[2]),
                    float(S["AXLE_D"]) + float(S["AXLE_FIT"]), 0.0,
                    f"Axle bore {side:+d}")
        body = cut(part, body, slot, f"Front axle bore {side:+d}")

    shop.finish(part, "steel", "green")
    seat_frame(part)
    axle_frame(part, "head axis", S["HT_BOT"], S["HEAD_UP"],
               x=S["STEER_DOWN"])
    axle_frame(part, "front axle", fa, (0, 1, 0), x=(1, 0, 0))
    axle_frame(part, "stem", steerer_top, S["HEAD_UP"], x=S["STEER_DOWN"])
    axle_frame(part, "front mudguard", fa, (0, -1, 0), x=(1, 0, 0))
    shop.verified(part, 1)
    return part
