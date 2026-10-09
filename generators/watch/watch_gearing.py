# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Cycloidal watch gearing: epicycloidal addenda on radial flanks.

Why not involute (generators/gear.py): the gates in watch_spec proved it
cannot work at watch pinion counts. Unshifted, EIGHT of the nine meshes
interfere — below ~17 teeth the wheel's tip passes the interference point and
cuts into a pinion flank drawn radial below its base circle. Topping the wheel
clears it but leaves the 6-leaf escape mesh at a contact ratio of 0.92. A
profile shift clears it but the 6-leaf pinion is POINTED from x 0.42 and still
interferes below x 0.53: no shift is both. That is why watches are cut with
cycloidal teeth, and this is that.

The rule: two gears mesh conjugately when each one's ADDENDUM is the
epicycloid, and the other's DEDENDUM the hypocycloid, traced by the SAME
rolling circle. Choose the rolling circle as half a gear's pitch radius and
that gear's hypocycloid degenerates to a straight RADIAL line — the classic
watch pinion leaf. So every gear here has:

    rho_ded  the circle its own dedendum is traced by (default r/2: radial)
    rho_add  the circle its addendum is traced by — its MATE's rho_ded

and a pair is conjugate at the exact pitch centre distance m(z1 + z2)/2,
with the backlash in the teeth (a pinion leaf is thinner than half a pitch),
not in the centre distance, which cycloidal action does not forgive.
"""
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                ".."))
from _lib import arc_through, line  # noqa: E402


def _epi(r, rho, t):
    """Epicycloid: a point on a circle of radius rho rolling OUTSIDE radius r,
    starting at (r, 0)."""
    k = (r + rho) / rho
    return ((r + rho) * math.cos(t) - rho * math.cos(k * t),
            (r + rho) * math.sin(t) - rho * math.sin(k * t))


def _hypo(r, rho, t):
    """Hypocycloid: the same circle rolling INSIDE radius r, from (r, 0).
    With rho = r/2 this is the straight segment along +X to the centre."""
    k = (r - rho) / rho
    return ((r - rho) * math.cos(t) + rho * math.cos(k * t),
            (r - rho) * math.sin(t) - rho * math.sin(k * t))


def _solve(f, target, lo, hi):
    """t in [lo, hi] with f(t) == target, f monotone on the interval."""
    flo = f(lo) - target
    for _ in range(80):
        mid = (lo + hi) / 2
        if (f(mid) - target) * flo > 0:
            lo, flo = mid, f(mid) - target
        else:
            hi = mid
    return (lo + hi) / 2


def addendum_height(module, z, rho_add, frac, cap):
    """How far the addendum can rise: to `cap` modules, or to where the two
    epicycloidal flanks meet on the tooth's centre line (an ogival point),
    whichever comes first."""
    r = module * z / 2.0
    beta = frac * math.pi / z                  # half the tooth, on the pitch
    t_top = math.pi * rho_add / r * 0.999      # the epicycloid's highest point
    ang = lambda t: math.atan2(*reversed(_epi(r, rho_add, t)))  # noqa: E731
    rad = lambda t: math.hypot(*_epi(r, rho_add, t))  # noqa: E731
    if ang(t_top) >= beta:
        t_meet = _solve(ang, beta, 1e-9, t_top)
        meet = rad(t_meet) - r
    else:
        meet = rad(t_top) - r
    return min(cap * module, meet)


def flank_points(r, rho_add, rho_ded, h_a, h_f, samples=7):
    """One RIGHT flank as (dedendum root->pitch, addendum pitch->tip), with
    the pitch point at angle 0 and the tooth's centre at +beta — the frame
    profile() places its teeth from.

    Exposed because a part may need the curve itself rather than the drawing:
    the watch's click is fitted to the ratchet's real flank, so its pawl
    drops as deep into a gap as its own width allows and never touches
    (watch_spec.click_table).
    """
    ra, rf = r + h_a, r - h_f
    t_a = _solve(lambda t: math.hypot(*_epi(r, rho_add, t)), ra, 1e-9,
                 math.pi * rho_add / r * 0.999)
    add = [_epi(r, rho_add, t_a * i / (samples - 1)) for i in range(samples)]
    if abs(rho_ded - r / 2) < 1e-12:
        return [(rf, 0.0), (r, 0.0)], add
    # The hypocycloid leans AWAY from the tooth centre going down (the tooth
    # thickens toward its root); pick the sign that does that.
    t_lim = math.pi * rho_ded / (r - rho_ded) * 0.999 if rho_ded < r else 1
    f = lambda t: math.hypot(*_hypo(r, rho_ded, t))  # noqa: E731
    sign = -1.0 if math.atan2(*reversed(_hypo(r, rho_ded, 1e-3))) > 0 else 1.0
    t_f = _solve(lambda t: f(sign * t), rf, 1e-9, t_lim)
    ded = [_hypo(r, rho_ded, sign * t_f * (samples - 1 - i) / (samples - 1))
           for i in range(samples)]
    return ded, add


def profile(module, z, rho_add, rho_ded, frac, h_a, h_f, samples=7):
    """Closed outline of a whole cycloidal gear, as sketch entities.

    frac  the tooth's share of the pitch on the pitch circle (0.5 = half)
    h_a   addendum height (mm), from addendum_height()
    h_f   dedendum depth (mm): the MATE's addendum plus clearance
    Tooth 0 points along +X, as generators/gear.py's do, so the relation
    phases in watch.py apply unchanged.
    """
    r = module * z / 2.0
    beta = frac * math.pi / z
    ra, rf = r + h_a, r - h_f
    if rf <= 0:
        raise ValueError(f"a {z}-tooth gear's root is not positive: "
                         f"dedendum {h_f:.3f} on a {r:.3f} pitch radius")

    ded, add = flank_points(r, rho_add, rho_ded, h_a, h_f, samples)

    def place(pts, c, mirror):
        out = []
        for x, y in pts:
            rr, th = math.hypot(x, y), math.atan2(y, x)
            th = (c + beta - th) if mirror else (c - beta + th)
            out.append((rr * math.cos(th), rr * math.sin(th)))
        return out

    def curve(pts):
        """Arcs through consecutive triples; a straight run stays a line."""
        if len(pts) == 2:
            return [line(pts[0], pts[1])]
        ents = []
        for i in range(0, len(pts) - 2, 2):
            ents.append(arc_through(pts[i], pts[i + 1], pts[i + 2]))
        return ents

    ents, starts, ends = [], [], []
    for k in range(z):
        c = 2 * math.pi * k / z
        rd, ra_ = place(ded, c, False), place(add, c, False)       # root->tip
        la, ld = place(add[::-1], c, True), place(ded[::-1], c, True)
        ents += curve(rd) + curve(ra_)
        tip_gap = math.dist(ra_[-1], la[0])
        if tip_gap > 1e-6:
            ents.append(arc_through(ra_[-1], (ra * math.cos(c),
                                              ra * math.sin(c)), la[0]))
        ents += curve(la) + curve(ld)
        starts.append(rd[0])
        ends.append(ld[-1])
    for k in range(z):
        cm = 2 * math.pi * (k + 0.5) / z
        ents.append(arc_through(ends[k], (rf * math.cos(cm), rf * math.sin(cm)),
                                starts[(k + 1) % z]))
    return ents, {"rp": r, "ra": ra, "rf": rf}


# --- racks: contrate teeth ------------------------------------------------------
# A contrate tooth on the face of a stem pinion, seen along the axis of the
# flat wheel it drives, passes the contact as a RACK tooth does — so its
# profile is the cycloidal rack's. A circle rolling along a straight pitch line
# traces a common cycloid; the wheel's dedendum circle rolled on the TIP side
# traces the rack's addendum, and the rack's own dedendum circle, rolled on the
# body side, traces its flank and (on the wheel) the wheel's epicycloidal
# addendum. So a rack has a rho_ded of its own choosing and takes its
# rho_add from the wheel, exactly as a wheel does.

def _cycloid(rho, t):
    """A point on a circle of radius rho rolling along the pitch line, from the
    pitch point: (along the line, away from the line)."""
    return (rho * (t - math.sin(t)), rho * (1.0 - math.cos(t)))


def rack_addendum_height(module, rho_add, frac, cap):
    """How far a rack tooth's addendum can rise: to `cap` modules, or to where
    its two cycloidal flanks meet on the tooth's centre line."""
    half = frac * math.pi * module / 2.0
    t_meet = _solve(lambda t: _cycloid(rho_add, t)[0], half, 1e-9, math.pi)
    return min(cap * module, _cycloid(rho_add, t_meet)[1])


def rack_arc(rho_add, h_a):
    """The pitch-line length the contact rolls while it climbs a rack tooth's
    addendum of height h_a — the rack's share of the arc of action."""
    t = _solve(lambda t: _cycloid(rho_add, t)[1], h_a, 1e-9, math.pi)
    return rho_add * t


def rack_profile(module, rho_add, rho_ded, frac, h_a, h_f, samples=7,
                 sink=0.0):
    """One rack tooth as sketch entities, in (u, v): u along the pitch line
    with the tooth centred on u = 0, v away from the tooth's root, which is
    the line v = 0; the pitch line is v = h_f and the tip v = h_f + h_a. The
    flanks run on straight for `sink` below the root, so a tooth extruded
    from a face is sunk INTO it rather than touching it on a shared plane."""
    half = frac * math.pi * module / 2.0

    def pts(rho, h, up):
        t_end = _solve(lambda t: _cycloid(rho, t)[1], h, 1e-9, math.pi)
        out = []
        for i in range(samples):
            x, y = _cycloid(rho, t_end * i / (samples - 1))
            # The addendum leans in toward the centre going up; the flank
            # leans out going down, so the tooth thickens toward its root.
            out.append((-half + x, h_f + y) if up else (-half - x, h_f - y))
        return out

    add = pts(rho_add, h_a, True)          # pitch point -> tip, right to left
    ded = pts(rho_ded, h_f, False)         # pitch point -> root
    left = ded[::-1] + add[1:]             # root -> pitch -> tip, u < 0 side
    right = [(-u, v) for u, v in left][::-1]

    def curve(p):
        return [arc_through(p[i], p[i + 1], p[i + 2])
                for i in range(0, len(p) - 2, 2)]
    ents = curve(left)
    if math.dist(left[-1], right[0]) > 1e-6:
        ents.append(line(left[-1], right[0]))           # the tip land
    ents += curve(right)
    if sink > 0.0:
        a, b = (left[0][0], -sink), (right[-1][0], -sink)
        ents = [line(a, left[0])] + ents + [line(right[-1], b), line(b, a)]
    else:
        ents.append(line(right[-1], left[0]))           # the root line
    return ents
