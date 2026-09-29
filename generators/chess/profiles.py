"""Staunton profiles for the turned pieces, as expressions.

A profile is a closed loop in the (x, r) half-plane: x along the piece's
axis from the underside (x = 0) to the top (x = piece_h), r the radius. Every
point is written against one of two anchors, so a configuration that changes
the piece's height or base moves the right half of it:

    base point  (a, b)  ->  x = a * base_r,            r = b * base_r
    head point  (a, b)  ->  x = piece_h - a * u,       r = b * u

`u` is king_h / 85: the heads are drawn in millimetres at the nominal 85 mm
king and scale with it. Within one anchor the scale is uniform, so an arc
through three points stays an arc at any size.
"""
import math

# The shared foot, bead and cove, in units of the base radius. It ends where
# the stem starts.
BASE = [
    ("L", (0.0, 1.0)),
    ("L", (0.10, 1.0)),
    ("L", (0.10, 0.95)),
    ("A", (0.22, 0.99), (0.34, 0.90)),
    ("L", (0.36, 0.82)),
    ("A", (0.46, 0.70), (0.62, 0.64)),
]

# Heads, measured DOWN from the top in mm at the nominal size. Each starts
# where the stem ends (its first point) and finishes on the axis at the top.
HEADS = {
    "pawn": [
        ("L", (17.0, 5.2)),
        ("L", (17.0, 9.0)),
        ("L", (15.5, 9.4)),
        ("L", (14.2, 9.0)),
        ("L", (14.2, 5.0)),
        ("L", (13.6, 5.0)),
        ("A", (8.0, 7.6), (0.0, 0.0)),
    ],
    "rook": [
        ("L", (12.0, 9.5)),
        ("L", (11.0, 10.2)),
        ("L", (10.0, 11.8)),
        ("L", (0.0, 11.8)),
        ("L", (0.0, 0.0)),
    ],
    "bishop": [
        ("L", (26.0, 5.5)),
        ("L", (26.0, 9.5)),
        ("L", (24.5, 10.0)),
        ("L", (23.0, 9.5)),
        ("L", (23.0, 5.2)),
        ("L", (21.5, 5.2)),
        ("A", (13.0, 8.6), (5.5, 3.4)),
        ("L", (4.6, 1.8)),
        ("A", (2.4, 2.6), (0.0, 0.0)),
    ],
    "queen": [
        ("L", (32.0, 6.5)),
        ("L", (32.0, 11.0)),
        ("L", (30.5, 11.5)),
        ("L", (29.0, 11.0)),
        ("L", (29.0, 6.0)),
        ("L", (24.0, 5.8)),
        ("A", (15.0, 8.0), (9.0, 11.0)),
        ("L", (7.5, 11.0)),
        ("L", (7.5, 7.0)),
        ("A", (5.5, 6.0), (4.2, 2.2)),
        ("A", (2.2, 2.8), (0.0, 0.0)),
    ],
    "king": [
        ("L", (36.0, 6.8)),
        ("L", (36.0, 12.0)),
        ("L", (34.5, 12.5)),
        ("L", (33.0, 12.0)),
        ("L", (33.0, 6.3)),
        ("L", (28.0, 6.0)),
        ("A", (20.0, 8.5), (15.0, 11.5)),
        ("L", (13.5, 11.5)),
        ("L", (13.5, 8.0)),
        ("A", (12.5, 7.4), (11.5, 6.0)),
        ("L", (0.0, 6.0)),
        ("L", (0.0, 0.0)),
    ],
    # The knight's turned part stops at its collar; the head is a separate
    # extruded outline (knight.py) that starts inside the collar.
    "knight": [
        ("L", (38.0, 9.5)),
        ("L", (38.0, 12.5)),
        ("L", (35.0, 12.5)),
        ("L", (35.0, 0.0)),
    ],
}


# how far off the axis a head's last arc stops (mm at the nominal size)
TOP_R = 0.6


def fmt(c):
    s = f"{c:.4f}".rstrip("0").rstrip(".")
    return s if s not in ("-0", "") else "0"


def base_pt(p):
    a, b = p
    return (f"{fmt(a)} * base_r", f"{fmt(b)} * base_r")


def head_pt(p):
    a, b = p
    x = "piece_h" if a == 0 else f"piece_h - {fmt(a)} * u"
    r = "0" if b == 0 else f"{fmt(b)} * u"
    return (x, r)


def circle3(p, q, s):
    (ax, ay), (bx, by), (cx, cy) = p, q, s
    d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by))
    ux = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) * (cy - ay)
          + (cx * cx + cy * cy) * (ay - by)) / d
    uy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx)
          + (cx * cx + cy * cy) * (bx - ax)) / d
    return (ux, uy), math.hypot(ax - ux, ay - uy)


def segments(kind):
    """[(type, anchor, points...)] from the axis at the underside, round the
    profile, back along the axis."""
    out, prev = [], ("base", (0.0, 0.0))
    for seg in BASE:
        out.append((seg[0], "base", prev[1]) + seg[1:])
        prev = ("base", seg[-1])
    # The stem: from the base's last point to the head's first.
    head = HEADS[kind]
    out.append(("S", None, prev[1], head[0][1]))
    prev = ("head", head[0][1])
    for seg in head[1:]:
        if seg[0] == "A" and seg[-1][1] == 0.0:
            # An arc may not END on the axis: its endpoint is computed from
            # a centre, a radius and an angle in single precision, lands a
            # hair either side of the axis, and one size in several makes
            # the revolve refuse a profile that crosses it. Stop the arc
            # TOP_R off the axis and finish with a flat.
            (cx, cy), r = circle3(prev[1], seg[1], seg[2])
            a = cx - math.sqrt(max(r * r - (TOP_R - cy) ** 2, 0.0))
            if abs(a - seg[2][0]) > abs(cx + (cx - a) - seg[2][0]):
                a = 2 * cx - a
            end = (a, TOP_R)
            out.append(("A", "head", prev[1], seg[1], end))
            out.append(("L", "head", end, (a, 0.0)))
            prev = ("head", (a, 0.0))
            continue
        out.append((seg[0], "head", prev[1]) + seg[1:])
        prev = ("head", seg[-1])
    out.append(("C", None, prev, ("base", (0.0, 0.0))))
    return out


def numeric(kind, piece_h, base_r, u):
    """The same profile in millimetres, for checks and plots: a list of
    polyline points (arcs sampled)."""
    def at(anchor, p):
        a, b = p
        if anchor == "base":
            return (a * base_r, b * base_r)
        return (piece_h - a * u, b * u)

    pts = [(0.0, 0.0)]
    for seg in segments(kind):
        t, anchor = seg[0], seg[1]
        if t == "L":
            pts.append(at(anchor, seg[3]))
        elif t == "A":
            p, q, s = at(anchor, seg[2]), at(anchor, seg[3]), at(anchor, seg[4])
            (cx, cy), r = circle3(p, q, s)
            a0 = math.atan2(p[1] - cy, p[0] - cx)
            a1 = math.atan2(q[1] - cy, q[0] - cx)
            a2 = math.atan2(s[1] - cy, s[0] - cx)
            tau = 2 * math.pi
            ccw = (a1 - a0) % tau < (a2 - a0) % tau
            span = (a2 - a0) % tau if ccw else -((a0 - a2) % tau)
            for i in range(1, 17):
                a = a0 + span * i / 16
                pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
        elif t == "S":
            pts.append(at("head", seg[3]))
    return pts


# --- inside corners a ball can finish -------------------------------------
# A turned profile's inside corners are where the finishing ball cannot go:
# a line meeting a line at an angle leaves a ring the ball rounds over, and
# the rest pass that cleans those rings was most of the pawn blank's ten
# hours. Each concave line-to-line corner is rounded to FILLET_R (mm at the
# nominal size) -- more than the 3 mm finishing ball's radius -- or to what
# its two lines have room for, which `tight` lists.
FILLET_R = 1.6


def _num(anchor, p, dims):
    piece_h, base_r, u = dims
    a, b = p
    return (a * base_r, b * base_r) if anchor == "base" else \
        (piece_h - a * u, b * u)


def _back(anchor, q, dims):
    piece_h, base_r, u = dims
    x, r = q
    return (x / base_r, r / base_r) if anchor == "base" else \
        ((piece_h - x) / u, r / u)


def filleted(kind, dims, radius=FILLET_R, tight=None):
    """segments(kind), with every concave corner between two straight
    segments rounded. `dims` = (piece_h, base_r, u) at the nominal size; the
    fillet's points are written in the anchor of the corner they round, so
    they scale with it."""
    segs = segments(kind)
    # vertices between segments i and i+1
    out = [list(s) for s in segs]
    for i in range(len(out) - 1):
        s1, s2 = out[i], out[i + 1]
        if s1[0] not in ("L", "S") or s2[0] not in ("L", "S"):
            continue
        # the corner's anchor: the segment that is not the stem
        a1 = "head" if s1[0] == "S" else s1[1]
        a2 = "base" if s2[0] == "S" else s2[1]
        anchor = a1 if s1[0] != "S" else a2
        p0 = _num("base" if s1[0] == "S" else s1[1], s1[2], dims)
        pc = _num(a1, s1[-1], dims)
        p2 = _num("head" if s2[0] == "S" else s2[1], s2[-1], dims)
        d1 = (pc[0] - p0[0], pc[1] - p0[1])
        d2 = (p2[0] - pc[0], p2[1] - pc[1])
        l1, l2 = math.hypot(*d1), math.hypot(*d2)
        if l1 < 1e-9 or l2 < 1e-9:
            continue
        d1 = (d1[0] / l1, d1[1] / l1)
        d2 = (d2[0] / l2, d2[1] / l2)
        cross = d1[0] * d2[1] - d1[1] * d2[0]
        if cross <= 1e-6:
            continue  # straight on, or a convex corner: the ball rolls over
        # never round onto the axis (the closing segment)
        if abs(pc[1]) < 1e-9:
            continue
        phi = math.acos(max(-1.0, min(1.0, d1[0] * d2[0] + d1[1] * d2[1])))
        t = radius * math.tan(phi / 2)
        room = 0.45 * min(l1, l2)
        rho = radius
        if t > room:
            t = room
            rho = t / math.tan(phi / 2)
            if tight is not None:
                tight.append((kind, round(pc[0], 2), round(pc[1], 2),
                              round(rho, 2)))
        q1 = (pc[0] - d1[0] * t, pc[1] - d1[1] * t)
        q2 = (pc[0] + d2[0] * t, pc[1] + d2[1] * t)
        n1 = (-d1[1], d1[0])  # left of travel: the concave side
        c = (q1[0] + n1[0] * rho, q1[1] + n1[1] * rho)
        m = (pc[0] - c[0], pc[1] - c[1])
        lm = math.hypot(*m)
        mid = (c[0] + m[0] / lm * rho, c[1] + m[1] / lm * rho)
        # s1 now ends at q1, s2 starts at q2, an arc between, all in the
        # corner's anchor (the stem keeps its other end in its own)
        s1[-1] = _back(a1, q1, dims)
        s2[2] = _back(a2, q2, dims)
        out[i] = s1
        out[i + 1] = s2
        out.insert(i + 1, ["A", anchor, _back(anchor, q1, dims),
                           _back(anchor, mid, dims),
                           _back(anchor, q2, dims)])
    return [tuple(s) for s in out]
