"""What is bolted on: mudguards and their stays, the rear rack, the rear
reflector and the front brake.

The mudguards are the one thing here not drawn in world coordinates. A guard
is a partial surface of revolution about its wheel's axle, and a Revolve
turns about the PART's own axis — so each is built on the axle, local Z
pointing RIGHT, and fastens to the "front mudguard" / "rear mudguard" frame
the fork and the frame carry for exactly that. The angles are the bicycle's
own: 0 straight forward from the axle, 90 over the crown, 180 level behind.

Everything else is drawn where it goes and fastened by its "seat" frame.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
from designer_sdk import Plane, V, _id  # noqa: E402
import bike_frame as frame_mod  # noqa: E402
import bike_shop as shop  # noqa: E402
from bike_shop import (axle_frame, bowed_tube, cut, seat_frame,  # noqa: E402
                       tube, union)


def _fender_section(S):
    """The guard's section across its width: a crowned band, drawn as a
    closed outline in (radius, along-the-axle)."""
    R = float(S["WHEEL_R"]) + float(S["FENDER_GAP"])
    half = float(S["FENDER_W"]) / 2.0
    crown = float(S["FENDER_CROWN"])
    t = float(S["FENDER_T"])
    n = 8
    us = [-half + 2 * half * k / n for k in range(n + 1)]
    outer = [(R + crown * (1 - (u / half) ** 2), u) for u in us]
    inner = [(r - t, u) for r, u in reversed(outer)]
    return outer + inner


def _fender(app, S, name, from_deg, to_deg, stay_deg, bolt=None):
    part = app.part(name)
    R = float(S["WHEEL_R"]) + float(S["FENDER_GAP"])
    start = math.radians(float(from_deg))
    ids = [shop.revolved(part, _fender_section(S), (0, 0, 0), (0, 0, 1), name,
                         ref=(math.cos(start), math.sin(start), 0.0),
                         angle_deg=float(to_deg) - float(from_deg))]
    # The stays: from the guard's edge down to the dropout it bolts to, one
    # each side. In this part's coordinates the dropout IS on the axis, so a
    # stay is simply a rod from the rim's distance to nearly the centre.
    a = math.radians(float(stay_deg))
    foot = math.radians(float(stay_deg) + 35.0)
    dz = (float(S["REAR_OLD"] if name.startswith("Rear") else S["FRONT_OLD"])
          + float(S["DROPOUT_T"])) / 2.0
    half = float(S["FENDER_W"]) / 2.0
    u = half - 8.0
    # The stay runs THROUGH the guard and stands a millimetre proud of it,
    # which is both what the bolt does and the robust way to meet a 1.2 mm
    # shell: a tube ending inside that wall is a cap wedged between two
    # surfaces, and the guard came out as three solids — a band and two
    # rods lying beside it.
    top = R + float(S["FENDER_CROWN"]) * (1 - (u / half) ** 2) + 1.5
    for side in (+1, -1):
        ids.append(tube(part,
                        (top * math.cos(a), top * math.sin(a), side * u),
                        (24.0 * math.cos(foot), 24.0 * math.sin(foot),
                         side * dz),
                        float(S["STAY_D"]), 0.0, f"Stay {side:+d}"))
    body = union(part, ids, f"{name} build")
    if bolt is not None:
        # The hole the brake's centre bolt passes through. Every roadster
        # mudguard has one, and this model needs it for the same reason the
        # real one does: without it the caliper's bolt and the guard's crown
        # are in the same place.
        at, along = bolt
        cut(part, body, tube(part, tuple(V(at) - V(along) * 30.0),
                             tuple(V(at) + V(along) * 30.0), 16.0, 0.0,
                             "Brake bolt hole"), "Brake bolt hole")
    shop.finish(part, "aluminum", "alloy")
    axle_frame(part, "axle", (0, 0, 0), (0, 0, 1), x=(1, 0, 0))
    shop.verified(part, 1)
    return part


def front_fender(app, S):
    # The guard is built on its own axle with local X = world X and local
    # Y = world Z, so a world point in the bicycle's plane goes in as
    # (dx, dz) — which is the whole reason the mudguards are built this way.
    crown, axle = V(S["FORK_CROWN"]), V(S["FRONT_AXLE"])
    up = (crown - axle).unit()
    return _fender(app, S, "Front mudguard", S["FRONT_FENDER_FROM_DEG"],
                   S["FRONT_FENDER_TO_DEG"], 186.0,
                   bolt=((crown[0] - axle[0], crown[2] - axle[2], 0.0),
                         (up[0], up[2], 0.0)))


def rear_fender(app, S):
    return _fender(app, S, "Rear mudguard", S["REAR_FENDER_FROM_DEG"],
                   S["REAR_FENDER_TO_DEG"], 196.0)


def rear_rack(app, S):
    """A tubular carrier over the rear guard: a slatted platform, two legs
    down to the dropouts and two forward to the seat stays."""
    part = app.part("Rear rack")
    geom = frame_mod.rear_triangle(S)
    ra = S["REAR_AXLE"]
    top = ra[2] + float(S["WHEEL_R"]) + float(S["RACK_ABOVE_TYRE"])
    half = float(S["RACK_W"]) / 2.0
    back = ra[0] - float(S["RACK_LEN"]) * 0.5
    front = back + float(S["RACK_LEN"])
    d = float(S["RACK_TUBE_D"])
    ids = []
    for side in (+1, -1):
        ids.append(tube(part, (back, side * half, top),
                        (front, side * half, top), d, 1.0,
                        f"Rail {side:+d}", weld=(6, 6)))
        # Down to the dropout, and forward to the seat stay. Both END ON the
        # frame: a carrier is bolted to it, and `asm.check` is told that
        # those two contacts are meant.
        ids.append(tube(part, (back + 10, side * half, top),
                        (ra[0] - 18, side * geom["dy"], ra[2] + 16),
                        float(S["STAY_D"]) + 1.0, 0.0, f"Leg {side:+d}"))
        stay = frame_mod.seat_stay_at(S, 0.42)
        ids.append(tube(part, (front - 10, side * half, top),
                        (stay[0], side * stay[1], stay[2]),
                        float(S["STAY_D"]) + 1.0, 0.0, f"Strut {side:+d}"))
    for k in range(4):
        x = back + (front - back) * (k + 0.5) / 4.0
        ids.append(tube(part, (x, -half, top), (x, half, top), 7.0, 0.8,
                        f"Slat {k}", weld=(5, 5)))
    # The bar across the back: the reflector hangs on it, and without one it
    # hangs on nothing.
    ids.append(tube(part, (back, -half, top), (back, half, top), 9.0, 1.0,
                    "Tail bar", weld=(5, 5)))
    union(part, ids, "Rack build")
    shop.finish(part, "aluminum", "dull_alloy")
    seat_frame(part)
    axle_frame(part, "reflector", (back - 3.0, 0.0, top - 12.0), (-1, 0, 0),
               x=(0, 0, 1))
    shop.verified(part, 1)
    return part


def reflector(app, S):
    """The red rear reflector, on its own axis so it can hang off whatever
    carries it — here the back of the rack."""
    part = app.part("Reflector")
    w, h = float(S["REFLECTOR_W"]) / 2.0, float(S["REFLECTOR_H"]) / 2.0
    sk = part.sketch(Plane.xy(0.0), name="Lens profile")
    sk.centered_rect((0, 0), 2 * w, 2 * h)
    body = part._add({"type": "Extrude", "name": "Lens",
                      "sketch_id": _id(sk.add()), "mode": "new_body",
                      "distance": 9.0})["id"]
    del body
    shop.finish(part, "plastic_black", "red")
    axle_frame(part, "axis", (0, 0, 0), (0, 0, 1), x=(1, 0, 0))
    shop.verified(part, 1)
    return part


def front_brake(app, S):
    """A side-pull caliper on the fork crown: a centre bolt, two arms and two
    pads on the rim's braking track.

    Its reach is not a number of its own — it is where the crown is and where
    the rim's sidewall is, and the model would rather that came out of the
    layout than be asserted.
    """
    part = app.part("Front brake")
    crown = V(S["FORK_CROWN"])
    axle = V(S["FRONT_AXLE"])
    # The braking track: the rim's outer wall, either side.
    track_r = float(S["RIM_BED_D"]) / 2.0 - 4.0
    up = (crown - axle).unit()
    pad = axle + up * track_r
    ids = [tube(part, tuple(crown - up * 8.0), tuple(crown + up * 26.0),
                12.0, 0.0, "Centre bolt")]
    # The arms straddle the MUDGUARD on their way to the rim: a caliper whose
    # arms run straight down is a caliper through the mudguard, and the
    # sweep found it at 289 mm3.
    # Each arm goes OUT first and only then down. The mudguard is 52 wide and
    # its crown reaches almost to the fork crown the caliper bolts to, so an
    # arm that starts downward is an arm through the mudguard — which is
    # where 425 mm3 of it was.
    clear = float(S["FENDER_W"]) / 2.0 + 8.0
    for side in (+1, -1):
        y = side * (float(S["RIM_WIDTH"]) / 2.0 + 1.0)
        shoulder = tuple(crown + up * 4.0 + V(0, side * clear, 0))
        waist = (pad[0] + 22.0, side * clear, pad[2] + 46.0)
        ids.append(tube(part, tuple(crown + up * 4.0), shoulder, 9.0, 0.0,
                        f"Arm {side:+d} shoulder"))
        ids.append(tube(part, shoulder, waist, 9.0, 0.0,
                        f"Arm {side:+d} upper"))
        ids.append(tube(part, waist, (pad[0], y, pad[2] + 5.0), 9.0, 0.0,
                        f"Arm {side:+d} lower"))
        # The pad KISSES the braking track; it does not sink into it.
        ids.append(part._add({
            "type": "Box", "name": f"Pad {side:+d}",
            "position": [pad[0] - 24.0,
                         (float(S["RIM_WIDTH"]) / 2.0) if side > 0
                         else -(float(S["RIM_WIDTH"]) / 2.0 + 6.0),
                         pad[2] - 4.0],
            "width": 48.0, "height": 6.0, "depth": 10.0})["id"])
    union(part, ids, "Brake build")
    shop.finish(part, "steel", "chrome")
    seat_frame(part)
    shop.verified(part, 1)
    return part
