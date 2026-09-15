"""The cockpit: bars, stem, grips, lever, bell, seatpost and saddle.

Everything here is drawn in WORLD coordinates and carries a "seat" frame at
the origin, so one fastened mate puts it where it was drawn. The ones that
STEER fasten to the fork rather than the frame, and follow it round: the bar,
the stem, and everything hanging off them.

The two exceptions are the parts there are two of. A grip is built on its own
axis, local Z running out of the bar's end, and the handlebar carries a frame
at each end for it — one part, two occurrences, and no mirrored copy to keep
in step.
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
from bike_shop import (axle_frame, bowed_tube, cut, disc,  # noqa: E402
                       seat_frame, tube, union)


def _stem_points(S):
    """Where the stem's quill, its bend and its bar clamp are."""
    up = V(S["HEAD_UP"])
    top = V(S["HT_TOP"])
    quill = tuple(top + up * 12.0)                     # down inside the steerer
    bend = tuple(top + up * float(S["STEM_QUILL_UP"]))
    clamp = (bend[0] + float(S["STEM_REACH"]), 0.0, bend[2] + 6.0)
    return quill, bend, clamp


def stem(app, S):
    part = app.part("Stem")
    quill, bend, clamp = _stem_points(S)
    ids = [tube(part, quill, bend, S["STEM_D"], 0.0, "Quill"),
           tube(part, bend, clamp, 24.0, 0.0, "Extension"),
           tube(part, (clamp[0], -22, clamp[2]), (clamp[0], 22, clamp[2]),
                34.0, 0.0, "Bar clamp")]
    body = union(part, ids, "Stem build")
    # A HAIR under the bar, so the clamp grips it. Bored to exactly the
    # bar's diameter the two touch nowhere, and the bars, the grips and the
    # lever come back as a group floating in front of the bicycle.
    bore = tube(part, (clamp[0], -24, clamp[2]), (clamp[0], 24, clamp[2]),
                float(S["BAR_D"]) - 0.3, 0.0, "Bar bore")
    cut(part, body, bore, "Clamp bore")
    shop.finish(part, "steel", "chrome")
    seat_frame(part)
    shop.verified(part, 1)
    return part


def handlebar(app, S):
    """North-road bars: straight across the clamp, then swept back and up to
    the grips — which is the shape that puts the rider upright, and the
    reason the saddle in the photographs sits well above the bars."""
    part = app.part("Handlebar")
    _, _, clamp = _stem_points(S)
    d = float(S["BAR_D"])
    half = float(S["BAR_WIDTH"]) / 2.0
    ids = [tube(part, (clamp[0], -80, clamp[2]), (clamp[0], 80, clamp[2]),
                d, 1.6, "Centre", weld=(20, 20))]
    ends = {}
    for side in (+1, -1):
        a = (clamp[0], side * 80.0, clamp[2])
        b = (clamp[0] - float(S["BAR_SWEEP"]), side * half,
             clamp[2] + float(S["BAR_RISE"]))
        ids.append(bowed_tube(part, a, b, (0, 0, 1), 26.0, d, 1.6,
                              f"Sweep {side:+d}", weld=(20, 20)))
        ends[side] = (a, b)
    union(part, ids, "Bar build")
    shop.finish(part, "steel", "chrome")
    seat_frame(part)
    for side, name in ((+1, "grip left"), (-1, "grip right")):
        a, b = ends[side]
        # The grip slides on along the bar's own direction at the end, not
        # along Y: these bars arrive at the hand pointing back and up.
        direction = (V(b) - V(a)).unit() * -1.0
        axle_frame(part, name, b, tuple(direction), x=(0, 0, 1))
    # The lever and the bell go INBOARD of the grip, not on top of it: at
    # exactly GRIP_LEN their clamps straddled the grip's inner end and sat
    # 3300 mm3 inside it.
    axle_frame(part, "lever", tuple(V(ends[-1][1])
                                    + (V(ends[-1][0]) - V(ends[-1][1])).unit()
                                    * (float(S["GRIP_LEN"]) + 22.0)),
               (0, 0, -1), x=(1, 0, 0))
    axle_frame(part, "bell", tuple(V(ends[+1][1])
                                   + (V(ends[+1][0]) - V(ends[+1][1])).unit()
                                   * (float(S["GRIP_LEN"]) + 34.0)),
               (0, 0, 1), x=(1, 0, 0))
    shop.verified(part, 1)
    return part


def grip(app, S):
    """One grip, on its own axis: local Z runs from the bar's end inward, so
    the same part fits both ends."""
    part = app.part("Grip")
    length = float(S["GRIP_LEN"])
    body = shop.revolved(part, [(float(S["BAR_D"]) / 2.0 - 0.2, 0),
                                (float(S["GRIP_D"]) / 2.0, 6.0),
                                (float(S["GRIP_D"]) / 2.0, length - 8.0),
                                (float(S["GRIP_D"]) / 2.0 + 2.5, length - 4.0),
                                (float(S["GRIP_D"]) / 2.0 + 2.5, length),
                                (float(S["BAR_D"]) / 2.0 - 0.2, length)],
                         (0, 0, 0), (0, 0, 1), "Grip")
    del body
    shop.finish(part, "rubber", "white")
    axle_frame(part, "axis", (0, 0, 0), (0, 0, 1), x=(1, 0, 0))
    shop.verified(part, 1)
    return part


def brake_lever(app, S):
    """A lever on its own axis, hanging under the right-hand bar."""
    part = app.part("Brake lever")
    ids = [shop.revolved(part, [(16, -14), (16, 14), (11, 14), (11, -14)],
                         (0, 0, 0), (0, 0, 1), "Clamp")]
    sk = part.sketch(Plane.xy(-6.0), name="Blade profile")
    sk.polyline([(8, -5), (float(S["LEVER_LEN"]), -16),
                 (float(S["LEVER_LEN"]), -24), (6, -14)], close=True)
    ids.append(part._add({"type": "Extrude", "name": "Blade",
                          "sketch_id": _id(sk.add()), "mode": "new_body",
                          "distance": 12.0})["id"])
    union(part, ids, "Lever build")
    shop.finish(part, "aluminum", "alloy")
    axle_frame(part, "axis", (0, 0, 0), (0, 0, 1), x=(1, 0, 0))
    shop.verified(part, 1)
    return part


def bell(app, S):
    """A pressed-steel dome on a clamp — a SHELL, not a solid dome: solid, a
    54 mm bell weighs 320 g, which is more than the chainguard."""
    part = app.part("Bell")
    r = float(S["BELL_D"]) / 2.0
    wall, n = 1.2, 10

    def arc(radius, height, shift):
        return [(radius * math.sin(math.pi / 2 * k / n),
                 shift + height * (1 - math.cos(math.pi / 2 * k / n)))
                for k in range(n + 1)]

    dome = ([(0.0, 0.0)] + arc(r, 14.0, 0.0)[1:] + [(r, 18.0), (r - wall, 18.0)]
            + list(reversed(arc(r - wall, 14.0, wall)))[:-1] + [(0.0, wall)])
    ids = [shop.revolved(part, dome, (0, 0, 0), (0, 0, 1), "Dome"),
           # The clamp runs UP INTO the dome: ending level with its base
           # leaves the two tangent, and a bell in two pieces.
           shop.revolved(part, [(11, -22), (16, -22), (16, 6), (11, 6)],
                         (0, 0, 0), (0, 0, 1), "Clamp")]
    union(part, ids, "Bell build")
    shop.finish(part, "steel", "brass")
    axle_frame(part, "axis", (0, 0, 0), (0, 0, 1), x=(1, 0, 0))
    shop.verified(part, 1)
    return part


def seatpost(app, S):
    part = app.part("Seatpost")
    sd = V(S["SEAT_DIR"])
    top = V(S["ST_TOP"])
    body = tube(part, tuple(top - sd * 110.0),
                tuple(top + sd * float(S["SEATPOST_OUT"])),
                S["SEATPOST_D"], 1.6, "Post", weld=(0, 18))
    del body
    shop.finish(part, "steel", "chrome")
    seat_frame(part)
    shop.verified(part, 1)
    return part


def saddle_top(S):
    """Where the saddle's underside sits: on the post, level, nose forward."""
    top = V(S["ST_TOP"]) + V(S["SEAT_DIR"]) * float(S["SEATPOST_OUT"])
    return (top[0] - 12.0, 0.0, top[2] + 14.0)


def saddle(app, S):
    """A lofted shell on a clamp, with its rails under it.

    Three sections, not two. Lofted straight from the rails' outline to a
    smaller one at the top, a saddle comes out as a truncated pyramid — a
    slab with sloped sides, which is exactly what it looked like. A saddle is
    widest at its TOP and tucks under at both the rail plane and the crown,
    so the MIDDLE section is the big one.
    """
    part = app.part("Saddle")
    seat = saddle_top(S)
    length = float(S["SADDLE_LEN"])
    wide = float(S["SADDLE_W"]) / 2.0
    high = float(S["SADDLE_H"])
    plan = [(0.55, 0.0), (0.42, 0.10), (0.18, 0.22), (-0.06, 0.55),
            (-0.28, 0.95), (-0.41, 1.0), (-0.45, 0.62), (-0.45, 0.0)]
    outline = [(length * u, wide * v) for u, v in plan] + \
              [(length * u, -wide * v) for u, v in reversed(plan[1:-1])]
    sketches = []
    for scale, z in ((0.74, seat[2]), (1.00, seat[2] + 0.62 * high),
                     (0.66, seat[2] + high)):
        sk = part.sketch(Plane.xy(z), name=f"Saddle {scale:.2f}")
        sk.polyline([(seat[0] + x * scale, y * scale) for x, y in outline],
                    close=True)
        sketches.append(_id(sk.add()))
    shell = part._add({"type": "Loft", "name": "Saddle shell",
                       "sketch_ids": sketches, "mode": "new_body"})["id"]
    rails = [shell]
    for side in (+1, -1):
        # The rails hang under the shell and BITE into it — level with its
        # underside they only touch it, and the saddle comes out as three
        # solids: a shell and two loose rods.
        rails.append(tube(part, (seat[0] + length * 0.30, side * 28.0,
                                 seat[2] - 2.0),
                          (seat[0] - length * 0.36, side * 28.0,
                           seat[2] - 2.0), 8.0, 0.0, f"Rail {side:+d}"))
    # The clamp that holds it on the post. Without it the saddle sits in the
    # air above the post touching nothing, and comes back as its own floating
    # group — which, being a saddle on nothing, it is.
    post_top = V(S["ST_TOP"]) + V(S["SEAT_DIR"]) * float(S["SEATPOST_OUT"])
    sd = V(S["SEAT_DIR"])
    rails.append(tube(part, tuple(post_top - sd * 42.0),
                      tuple(post_top + sd * 22.0),
                      float(S["SEATPOST_D"]) + 8.0, 0.0, "Clamp"))
    union(part, rails, "Saddle build")
    shop.finish(part, "rubber", "saddle")
    seat_frame(part)
    shop.verified(part, 1)
    return part
