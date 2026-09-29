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
# The groove between foot and bead is a cove of 0.125 base radii -- 1.8 mm
# on the smallest foot, the pawn's -- because the 3 mm finishing ball cannot
# reach into anything narrower; the step it replaced left a ring of stock.
BASE = [
    ("L", (0.0, 1.0)),
    ("L", (0.10, 1.0)),
    ("A", (0.20, 0.95), (0.27, 0.971)),
    ("A", (0.33, 0.99), (0.40, 0.90)),
    ("L", (0.42, 0.82)),
    ("A", (0.50, 0.70), (0.62, 0.64)),
]

# Heads, measured DOWN from the top in mm at the nominal size. Each starts
# where the stem ends (its first point) and finishes on the axis at the top.
HEADS = {
    "pawn": [
        ("L", (17.0, 5.2)),
        ("L", (17.0, 9.0)),
        ("L", (15.5, 9.4)),
        ("L", (14.2, 9.0)),
        # the neck: a 1.8 mm cove from the collar's face into the ball, so
        # the finishing ball reaches its bottom
        ("L", (14.2, 6.8)),
        ("A", (12.4, 5.0), (10.62, 7.07)),
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
        ("L", (23.0, 7.0)),
        ("A", (21.2, 5.2), (19.4, 7.07)),
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
# two segments meeting at an angle leave a ring the ball rounds over, and the
# rest pass that cleans those rings was most of the pawn blank's ten hours.
# Every concave junction -- line or arc on either side -- is rounded to
# FILLET_R (mm at the nominal size), more than the 3 mm finishing ball's
# radius, or to what its segments have room for, which `tight` lists. A
# concave ARC tighter than that, or a groove narrower than the ball, is the
# profile's to fix; `reach` in tests says where.
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


TAU = 2 * math.pi


class _Line:
    """A straight segment in mm. `anchors` = the anchor each END is written
    in (the stem's differ)."""

    def __init__(self, kind, anchors, p, q):
        self.kind, self.anchors, self.p, self.q = kind, anchors, p, q

    def d(self):
        dx, dy = self.q[0] - self.p[0], self.q[1] - self.p[1]
        n = math.hypot(dx, dy)
        return (dx / n, dy / n)

    def start_dir(self):
        return self.d()

    def end_dir(self):
        return self.d()

    def length(self):
        return math.dist(self.p, self.q)

    def param(self, t):
        """0..1 of point t along the segment."""
        dx, dy = self.q[0] - self.p[0], self.q[1] - self.p[1]
        return ((t[0] - self.p[0]) * dx + (t[1] - self.p[1]) * dy) / \
            (dx * dx + dy * dy)

    def offset(self, rho):
        dx, dy = self.d()
        n = (-dy, dx)  # left of travel: the air
        return ("line", (self.p[0] + n[0] * rho, self.p[1] + n[1] * rho),
                (dx, dy))

    def foot(self, c, rho):
        dx, dy = self.d()
        return (c[0] + dy * rho, c[1] - dx * rho)


class _Arc:
    """An arc in mm: centre, radius, start angle, signed sweep."""

    def __init__(self, anchor, p, q, s):
        self.anchor = anchor
        (cx, cy), self.r = circle3(p, q, s)
        self.c = (cx, cy)
        a0 = math.atan2(p[1] - cy, p[0] - cx)
        a1 = math.atan2(q[1] - cy, q[0] - cx)
        a2 = math.atan2(s[1] - cy, s[0] - cx)
        ccw = (a1 - a0) % TAU < (a2 - a0) % TAU
        self.a0 = a0
        self.sweep = (a2 - a0) % TAU if ccw else -((a0 - a2) % TAU)

    def at(self, a):
        return (self.c[0] + self.r * math.cos(a), self.c[1] + self.r * math.sin(a))

    @property
    def p(self):
        return self.at(self.a0)

    @property
    def q(self):
        return self.at(self.a0 + self.sweep)

    def _tan(self, a):
        sg = 1.0 if self.sweep > 0 else -1.0
        return (-math.sin(a) * sg, math.cos(a) * sg)

    def start_dir(self):
        return self._tan(self.a0)

    def end_dir(self):
        return self._tan(self.a0 + self.sweep)

    def length(self):
        return abs(self.sweep) * self.r

    def param(self, t):
        a = math.atan2(t[1] - self.c[1], t[0] - self.c[0])
        if self.sweep > 0:
            return ((a - self.a0) % TAU) / self.sweep
        return ((self.a0 - a) % TAU) / -self.sweep

    def offset(self, rho):
        # turning left (ccw) the air is toward the centre: a concave arc
        return ("circle", self.c,
                self.r - rho if self.sweep > 0 else self.r + rho)

    def foot(self, c, rho):
        dx, dy = c[0] - self.c[0], c[1] - self.c[1]
        n = math.hypot(dx, dy)
        return (self.c[0] + dx / n * self.r, self.c[1] + dy / n * self.r)

    def trim(self, start=None, end=None):
        """Move an end onto the circle's point nearest `start` / `end`."""
        a1 = self.a0 + self.sweep
        if start is not None:
            a = math.atan2(start[1] - self.c[1], start[0] - self.c[0])
            self.a0 = a
        if end is not None:
            a1 = math.atan2(end[1] - self.c[1], end[0] - self.c[0])
        span = (a1 - self.a0) % TAU
        self.sweep = span if self.sweep > 0 else span - TAU
        if abs(self.sweep) > TAU - 1e-9:
            self.sweep = 0.0


def _meet(o1, o2, near):
    """The point where two offset curves meet nearest `near`, or None."""
    if o1[0] == "circle" and o2[0] == "line":
        o1, o2 = o2, o1
    if o1[0] == "line" and o2[0] == "line":
        (p, d), (q, e) = o1[1:], o2[1:]
        den = d[0] * e[1] - d[1] * e[0]
        if abs(den) < 1e-12:
            return None
        t = ((q[0] - p[0]) * e[1] - (q[1] - p[1]) * e[0]) / den
        return (p[0] + d[0] * t, p[1] + d[1] * t)
    if o1[0] == "line":
        (p, d), (c, r) = o1[1:], o2[1:]
        if r <= 0:
            return None
        fx, fy = p[0] - c[0], p[1] - c[1]
        b = fx * d[0] + fy * d[1]
        disc = b * b - (fx * fx + fy * fy - r * r)
        if disc < 0:
            return None
        pts = [(p[0] + d[0] * t, p[1] + d[1] * t)
               for t in (-b + math.sqrt(disc), -b - math.sqrt(disc))]
        return min(pts, key=lambda x: math.dist(x, near))
    (c1, r1), (c2, r2) = o1[1:], o2[1:]
    if r1 <= 0 or r2 <= 0:
        return None
    dd = math.dist(c1, c2)
    if dd < 1e-12 or dd > r1 + r2 or dd < abs(r1 - r2):
        return None
    a = (r1 * r1 - r2 * r2 + dd * dd) / (2 * dd)
    h = math.sqrt(max(r1 * r1 - a * a, 0.0))
    ex, ey = (c2[0] - c1[0]) / dd, (c2[1] - c1[1]) / dd
    mx, my = c1[0] + ex * a, c1[1] + ey * a
    pts = [(mx - ey * h, my + ex * h), (mx + ey * h, my - ex * h)]
    return min(pts, key=lambda x: math.dist(x, near))


def _fillet(s1, s2, rho, lim1, lim2):
    """Centre and tangent points of a radius-rho fillet between the end of s1
    and the start of s2, with each tangent point no further back along its
    segment than lim1 / lim2 (0..1 params); None if it does not fit."""
    c = _meet(s1.offset(rho), s2.offset(rho), s1.q)
    if c is None:
        return None
    t1, t2 = s1.foot(c, rho), s2.foot(c, rho)
    if math.dist(c, t1) - rho > 1e-6 or math.dist(c, t2) - rho > 1e-6:
        return None
    u1, u2 = s1.param(t1), s2.param(t2)
    if not (lim1 <= u1 <= 1.0 + 1e-9 and -1e-9 <= u2 <= lim2):
        return None
    return c, t1, t2


def filleted(kind, dims, radius=FILLET_R, tight=None):
    """segments(kind), with every concave junction rounded. `dims` =
    (piece_h, base_r, u) at the nominal size; the fillet's points are written
    in the anchor of the corner they round, so they scale with it."""
    segs = segments(kind)
    closing = segs[-1]
    prims = []
    for s in segs[:-1]:
        if s[0] == "A":
            prims.append(_Arc(s[1], *(_num(s[1], q, dims) for q in s[2:])))
        elif s[0] == "S":
            prims.append(_Line("S", ("base", "head"), _num("base", s[2], dims),
                               _num("head", s[3], dims)))
        else:
            prims.append(_Line("L", (s[1], s[1]), _num(s[1], s[2], dims),
                               _num(s[1], s[3], dims)))

    def concave(i):
        a, b = prims[i], prims[i + 1]
        if abs(a.q[1]) < 1e-9:
            return False  # never round onto the axis
        d1, d2 = a.end_dir(), b.start_dir()
        return d1[0] * d2[1] - d1[1] * d2[0] > 1e-6

    def anchor_at(pr, end):
        if isinstance(pr, _Arc):
            return pr.anchor
        return pr.anchors[1 if end else 0]

    fillets = {}  # junction i -> (centre, rho)
    starts = [0.0] * len(prims)  # how far each prim's start was trimmed
    for i in range(len(prims) - 1):
        if not concave(i):
            continue
        s1, s2 = prims[i], prims[i + 1]
        # the far end of s2 keeps half for its own fillet, if it has one
        lim2 = 0.5 if i + 1 < len(prims) - 1 and concave(i + 1) else 0.95
        fit = _fillet(s1, s2, radius, starts[i], lim2)
        rho = radius
        if fit is None:
            lo, hi = 0.0, radius
            for _ in range(40):
                mid = (lo + hi) / 2
                if _fillet(s1, s2, mid, starts[i], lim2) is None:
                    hi = mid
                else:
                    lo = mid
            rho = lo
            fit = _fillet(s1, s2, rho, starts[i], lim2) if rho > 1e-3 else None
            if tight is not None:
                pc = s1.q
                tight.append((kind, round(pc[0], 2), round(pc[1], 2),
                              round(rho, 2)))
            if fit is None:
                continue
        c, t1, t2 = fit
        if isinstance(s1, _Arc):
            s1.trim(end=t1)
        else:
            s1.q = t1
        if isinstance(s2, _Arc):
            s2.trim(start=t2)
        else:
            s2.p = t2
        starts[i + 1] = 0.0
        fillets[i] = (c, rho, t1, t2)

    out = []
    for i, pr in enumerate(prims):
        if isinstance(pr, _Arc):
            m = pr.at(pr.a0 + pr.sweep / 2)
            out.append(("A", pr.anchor, _back(pr.anchor, pr.p, dims),
                        _back(pr.anchor, m, dims),
                        _back(pr.anchor, pr.q, dims)))
        elif pr.kind == "S":
            out.append(("S", None, _back("base", pr.p, dims),
                        _back("head", pr.q, dims)))
        else:
            a = pr.anchors[0]
            out.append(("L", a, _back(a, pr.p, dims), _back(a, pr.q, dims)))
        if i in fillets:
            c, rho, t1, t2 = fillets[i]
            # the corner's anchor: the side that is not the stem
            anchor = anchor_at(prims[i], True) if not (
                isinstance(prims[i], _Line) and prims[i].kind == "S") \
                else anchor_at(prims[i + 1], False)
            a1 = math.atan2(t1[1] - c[1], t1[0] - c[0])
            a2 = math.atan2(t2[1] - c[1], t2[0] - c[0])
            am = a1 + ((a2 - a1) % TAU) / 2  # concave: counter-clockwise
            mid = (c[0] + rho * math.cos(am), c[1] + rho * math.sin(am))
            out.append(("A", anchor, _back(anchor, t1, dims),
                        _back(anchor, mid, dims), _back(anchor, t2, dims)))
    return [tuple(x) for x in out] + [closing]
