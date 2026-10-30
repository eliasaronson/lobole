# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

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
from lobole.sdk import Plane, SdkError, V, ref_of  # noqa: E402
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


def _bar_path(S):
    """A north-road bar's centreline, in the plane it is BENT in.

    A handlebar is one pipe with two bends in it, and it has to be built as
    one: three tubes welded end to end show their seams and a step at every
    joint, which is what the first one did. So the path is a chain — line,
    arc, line, arc, line — in a single sketch, and one `Pipe` sweeps it.

    The plane is the bar's own: across the bicycle, and tilted back and up so
    the grips end up BAR_SWEEP behind the clamp and BAR_RISE above it. The
    bend ANGLE is then not a free number — it is whatever puts the grips at
    that reach for the given width and bend radius — so it is solved for
    rather than guessed, and the spec keeps the three dimensions a rider
    would actually measure.
    """
    half = float(S["BAR_WIDTH"]) / 2.0
    centre = float(S["BAR_CENTRE"]) / 2.0
    radius = float(S["BAR_BEND_R"])
    sweep = math.hypot(float(S["BAR_SWEEP"]), float(S["BAR_RISE"]))

    def reach(theta):
        """How far back the grip lands, if the bend turns through theta."""
        run = (half - centre - radius * math.sin(theta)) / math.cos(theta)
        return radius * (1 - math.cos(theta)) + run * math.sin(theta)

    lo, hi = math.radians(1.0), math.radians(75.0)
    if reach(hi) < sweep:
        raise SdkError(f"a {S['BAR_WIDTH']} mm bar bent to "
                       f"{S['BAR_BEND_R']} mm cannot reach {sweep:.0f} mm "
                       "back — widen it, or bend it tighter")
    for _ in range(60):                      # bisection: reach() is monotone
        mid = (lo + hi) / 2.0
        lo, hi = (mid, hi) if reach(mid) < sweep else (lo, mid)
    theta = (lo + hi) / 2.0
    tangent = (centre + radius * math.sin(theta),
               radius * (1 - math.cos(theta)))
    return theta, centre, radius, half, sweep, tangent


def _bar_plane(S):
    """The plane the bar is bent in: U across the bicycle, V back and UP."""
    _, _, _, _, _, _ = _bar_path(S)
    back = V(-float(S["BAR_SWEEP"]), 0.0, float(S["BAR_RISE"])).unit()
    u = V(0, 1, 0)
    return Plane(V(_stem_points(S)[2]), u.cross(back), u), back


def handlebar(app, S):
    """North-road bars: straight across the clamp, then bent back and up to
    the grips — the shape that puts the rider upright, and the reason the
    saddle in the photographs sits above the bars."""
    part = app.part("Handlebar")
    theta, centre, radius, half, sweep, tangent = _bar_path(S)
    plane, _ = _bar_plane(S)
    sk = part.sketch(plane, name="Bar path")
    deg = math.degrees(theta)
    sk.line((-half, sweep), (-tangent[0], tangent[1]))
    sk.arc((-centre, radius), radius, -90.0 - deg, -90.0)
    sk.line((-centre, 0.0), (centre, 0.0))
    sk.arc((centre, radius), radius, -90.0, -90.0 + deg)
    sk.line((tangent[0], tangent[1]), (half, sweep))
    shop.path_tube(part, sk.add(), S["BAR_D"], 1.6, "Handlebar")

    shop.finish(part, "steel", "chrome")
    seat_frame(part)
    # Where things clamp ON the bar. Each frame's Z runs ALONG the bar, so a
    # grip, a lever or a bell is a ring round it rather than a ring on some
    # other axis perched beside it.
    for side, name in ((+1, "left"), (-1, "right")):
        tip = plane.world(side * half, sweep)
        inward = (V(plane.world(side * tangent[0], tangent[1])) - V(tip)).unit()
        axle_frame(part, f"grip {name}", tip, tuple(inward), x=(0, 0, 1))
        # The bell's dome is as wide ALONG the bar as it is across, so it
        # needs more room past the grip than the lever's clamp does.
        clear = 18.0 if side < 0 else float(S["BELL_D"]) / 2.0 + 10.0
        at = tuple(V(tip) + inward * (float(S["GRIP_LEN"]) + clear))
        # The lever hangs off the right bar pointing FORWARD and down; the
        # bell stands UP off the left one. Both are the frame's X.
        want = V(1, 0, -0.45) if side < 0 else V(0, 0, 1)
        across = (want - inward * inward.dot(want)).unit()
        axle_frame(part, "lever" if side < 0 else "bell", at, tuple(inward),
                   x=tuple(across))
    shop.verified(part, 1)
    return part


def grip(app, S):
    """One grip, on its own axis: local Z runs from the bar's end inward, so
    the same part fits both ends."""
    part = app.part("Grip")
    length = float(S["GRIP_LEN"])
    bore = float(S["BAR_D"]) / 2.0 - 0.2
    fat = float(S["GRIP_D"]) / 2.0
    # CLOSED at its outer end. A grip bored right through is a piece of pipe,
    # and it looks like one: the bar stops at the grip's end, so you see
    # straight down the hole.
    body = shop.revolved(part, [(0.0, 0.0), (fat - 4.0, 0.0), (fat, 4.0),
                                (fat, length - 8.0), (fat + 2.5, length - 4.0),
                                (fat + 2.5, length), (bore, length),
                                (bore, 7.0), (0.0, 7.0)],
                         (0, 0, 0), (0, 0, 1), "Grip")
    del body
    shop.finish(part, "rubber", "white")
    axle_frame(part, "axis", (0, 0, 0), (0, 0, 1), x=(1, 0, 0))
    shop.verified(part, 1)
    return part


def brake_lever(app, S):
    """A lever whose clamp goes ROUND the bar.

    Its local Z is the bar's own axis, so the clamp is a ring on it and the
    blade reaches out along local X. Built on a vertical axis instead — which
    is where this started — the clamp is a ring standing on the bar rather
    than round it, and it shows.
    """
    part = app.part("Brake lever")
    bore = float(S["BAR_D"]) / 2.0 - 0.1        # gripping it, not floating
    ids = [shop.revolved(part, [(bore, -14), (16, -14), (16, 14), (bore, 14)],
                         (0, 0, 0), (0, 0, 1), "Clamp")]
    sk = part.sketch(Plane.xy(-6.0), name="Blade profile")
    sk.slot((14.0, 0.0), (float(S["LEVER_LEN"]), 0.0), 13.0)
    ids.append(part._add({"type": "Extrude", "name": "Blade",
                          "sketch_id": ref_of(sk.add()), "mode": "new_body",
                          "distance": 12.0})["id"])
    union(part, ids, "Lever build")
    shop.finish(part, "aluminum_6061_t6", "alloy")
    axle_frame(part, "axis", (0, 0, 0), (0, 0, 1), x=(1, 0, 0))
    shop.verified(part, 1)
    return part


def bell(app, S):
    """A pressed-steel dome on a clamp that goes ROUND the bar.

    Local Z is the bar; the dome is turned about local X, which is the one
    other axis a Revolve will take — and it is why the part is built on the
    bar's axis rather than the dome's.
    """
    part = app.part("Bell")
    r = float(S["BELL_D"]) / 2.0
    bore = float(S["BAR_D"]) / 2.0 - 0.1
    wall, n, base = 1.2, 10, 11.0

    def arc(radius, height, shift, inset=0.0):
        """A quarter dome, WIDEST AT ITS BASE and closed at the crown.

        Written the other way up — starting on the axis at the base and
        flaring away from the bar — it is a brass bowl bolted to the
        handlebar, which is what the first one rendered as.
        """
        return [(max(radius - inset, 0.0) * math.cos(math.pi / 2 * k / n),
                 shift + (height - inset) * math.sin(math.pi / 2 * k / n))
                for k in range(n + 1)]

    outer = arc(r, 17.0, base)
    # The inner surface is the outer one offset inward by `wall` — SAME
    # base plane, one wall less of height. Shifted up by a wall instead, the
    # crown closes to a point and the shell is degenerate there.
    inner = list(reversed(arc(r, 17.0, base, wall)))
    dome = outer + inner
    ids = [shop.revolved(part, dome, (0, 0, 0), (1, 0, 0), "Dome"),
           shop.revolved(part, [(bore, -9), (15, -9), (15, 9), (bore, 9)],
                         (0, 0, 0), (0, 0, 1), "Clamp"),
           # The post the dome is held on. The dome is a SHELL and its base
           # is open, so it touches the clamp nowhere — without this the bell
           # is a cap hovering over a ring, and comes back as two solids.
           tube(part, (10.0, 0, 0), (base + 16.0, 0, 0), 9.0, 0.0, "Post")]
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


def _smooth(points, per_span=4):
    """A Catmull-Rom pass over a closed outline.

    Ten points describe a saddle's plan; ten SEGMENTS render as ten facets,
    and a lofted shell built on them is visibly cut from flats. This puts
    four points where there was one, which is the cheapest way to a curve the
    shading believes — the sketch entity for a real spline is another matter.
    """
    n = len(points)
    out = []
    for i in range(n):
        p0, p1 = points[(i - 1) % n], points[i]
        p2, p3 = points[(i + 1) % n], points[(i + 2) % n]
        for k in range(per_span):
            t = k / float(per_span)
            t2, t3 = t * t, t * t * t
            out.append(tuple(
                0.5 * ((2 * p1[c])
                       + (-p0[c] + p2[c]) * t
                       + (2 * p0[c] - 5 * p1[c] + 4 * p2[c] - p3[c]) * t2
                       + (-p0[c] + 3 * p1[c] - 3 * p2[c] + p3[c]) * t3)
                for c in (0, 1)))
    return out


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
    plan = [(0.55, 0.0), (0.46, 0.09), (0.24, 0.17), (0.02, 0.33),
            (-0.10, 0.62), (-0.22, 0.92), (-0.34, 1.0), (-0.44, 0.86),
            (-0.47, 0.45), (-0.47, 0.0)]
    half_outline = [(length * u, wide * v) for u, v in plan]
    outline = _smooth(half_outline
                      + [(x, -y) for x, y in reversed(half_outline[1:-1])])
    sketches = []
    for scale, z in ((0.74, seat[2]), (1.00, seat[2] + 0.62 * high),
                     (0.66, seat[2] + high)):
        sk = part.sketch(Plane.xy(z), name=f"Saddle {scale:.2f}")
        sk.polyline([(seat[0] + x * scale, y * scale) for x, y in outline],
                    close=True)
        sketches.append(ref_of(sk.add()))
    shell = part._add({"type": "Loft", "name": "Saddle shell",
                       "sketch_ids": sketches, "mode": "new_body"})["id"]
    rails = [shell]
    for side in (+1, -1):
        # The rails hang under the shell and BITE into it — level with its
        # underside they only touch it, and the saddle comes out as three
        # solids: a shell and two loose rods. They also have to stay UNDER
        # it: run from 0.30 of the length forward to 0.36 back, at 28 mm
        # out, they came through the nose and out of the tail, because the
        # shell is only 14 mm wide that far forward.
        rails.append(tube(part, (seat[0] + length * 0.04, side * 21.0,
                                 seat[2] - 2.0),
                          (seat[0] - length * 0.30, side * 21.0,
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
