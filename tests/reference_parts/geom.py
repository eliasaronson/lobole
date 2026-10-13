# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Closed forms for the reference parts (tests/reference_parts/).

Standard library only: an expected value must never be measured off the app,
so nothing here talks to it. Every function states its formula. `self_check()`
runs first on every run of the corpus and checks the general routines against
the special cases they must reproduce (Green against the shoelace formula and
against pi r^2, Pappus against a cylinder and a torus, the frustum against a
prism), so a wrong derivation fails before any part is built.

Conventions: millimetres; angles in radians unless a name says `_deg`; a
2D region lies in a plane with coordinates (u, v); a solid's answer is a
`Solid` (volume, first moments, surface area, bounding box) so that solids
can be added and subtracted and the centroid falls out of the moments.
"""
import math

PI = math.pi

# ---------------------------------------------------------------- quadrature

# Gauss-Legendre nodes and weights on [0, 1], computed once by Newton on the
# Legendre recurrence. 32 nodes integrate a polynomial of degree 63 exactly,
# which covers every line term below (degree <= 4), and a trigonometric
# polynomial of degree <= 4 over an arc of at most 2 pi to rounding.
def _gauss_legendre(n):
    nodes, weights = [], []
    for i in range(1, n + 1):
        x = math.cos(PI * (i - 0.25) / (n + 0.5))
        for _ in range(100):
            p0, p1 = 1.0, x
            for k in range(2, n + 1):
                p0, p1 = p1, ((2 * k - 1) * x * p1 - (k - 1) * p0) / k
            dp = n * (x * p1 - p0) / (x * x - 1.0)
            dx = p1 / dp
            x -= dx
            if abs(dx) < 1e-16:
                break
        nodes.append(0.5 * (1.0 - x))
        weights.append(1.0 / ((1.0 - x * x) * dp * dp))
    return list(zip(nodes, weights))


_GL = _gauss_legendre(32)


def _integrate(f):
    """The integral of f(t) over t in [0, 1]."""
    return sum(w * f(t) for t, w in _GL)


# ------------------------------------------------------------- 2D boundaries

class Line:
    """A straight edge from p0 to p1 in (u, v)."""

    def __init__(self, p0, p1):
        self.p0, self.p1 = tuple(map(float, p0)), tuple(map(float, p1))

    def at(self, t):
        (a, b), (c, d) = self.p0, self.p1
        return (a + (c - a) * t, b + (d - b) * t), (c - a, d - b)

    def length(self):
        return math.dist(self.p0, self.p1)

    def extreme_points(self):
        return [self.p0, self.p1]

    def reversed(self):
        return Line(self.p1, self.p0)


class Arc:
    """A circular edge about `center`, radius r, from angle a0 to a1
    (radians; counter-clockwise when a1 > a0)."""

    def __init__(self, center, r, a0, a1):
        self.c = tuple(map(float, center))
        self.r, self.a0, self.a1 = float(r), float(a0), float(a1)

    def at(self, t):
        a = self.a0 + (self.a1 - self.a0) * t
        da = self.a1 - self.a0
        return ((self.c[0] + self.r * math.cos(a),
                 self.c[1] + self.r * math.sin(a)),
                (-self.r * math.sin(a) * da, self.r * math.cos(a) * da))

    def length(self):
        return self.r * abs(self.a1 - self.a0)

    def extreme_points(self):
        """The ends, and every point where the arc crosses an axis
        direction (where u or v is extreme)."""
        lo, hi = sorted((self.a0, self.a1))
        pts = [self.at(0)[0], self.at(1)[0]]
        k = math.ceil(lo / (PI / 2))
        while k * PI / 2 <= hi:
            a = k * PI / 2
            pts.append((self.c[0] + self.r * math.cos(a),
                        self.c[1] + self.r * math.sin(a)))
            k += 1
        return pts

    def reversed(self):
        return Arc(self.c, self.r, self.a1, self.a0)


def circle(center, r):
    """A full circle as one counter-clockwise loop."""
    return [Arc(center, r, 0.0, 2 * PI)]


def polygon(points):
    """A closed polygon as a loop of lines, in the order given."""
    n = len(points)
    return [Line(points[i], points[(i + 1) % n]) for i in range(n)]


def rectangle(u0, v0, u1, v1):
    return polygon([(u0, v0), (u1, v0), (u1, v1), (u0, v1)])


def _loop_integral(loop, f):
    """Sum over a loop of the integral of f(point, tangent) dt."""
    return sum(_integrate(lambda t, e=e: f(*e.at(t))) for e in loop)


class Region:
    """A plane region: one outer loop and any number of holes. Each loop is
    re-oriented here (outer counter-clockwise, holes clockwise) so a caller
    cannot get a sign wrong.

    Green's theorem turns every area integral into a boundary integral:
      A        = 1/2 loop(u dv - v du)
      int u dA = loop(u^2/2 dv)        int v dA  = -loop(v^2/2 du)
      int u^2 dA = loop(u^3/3 dv)      int uv dA = loop(u^2 v/2 dv)
      int v^2 dA = -loop(v^3/3 du)
    and the perimeter-weighted ones (for a surface of revolution):
      loop(ds), loop(u ds), loop(v ds), loop(u v ds).
    """

    def __init__(self, outer, holes=()):
        self.loops = [self._oriented(outer, +1)]
        self.loops += [self._oriented(h, -1) for h in holes]

    @staticmethod
    def _signed_area(loop):
        return 0.5 * _loop_integral(
            loop, lambda p, d: p[0] * d[1] - p[1] * d[0])

    def _oriented(self, loop, sign):
        if self._signed_area(loop) * sign < 0:
            return [e.reversed() for e in reversed(loop)]
        return list(loop)

    def _sum(self, f):
        return sum(_loop_integral(loop, f) for loop in self.loops)

    def area(self):
        return 0.5 * self._sum(lambda p, d: p[0] * d[1] - p[1] * d[0])

    def mu(self):
        return self._sum(lambda p, d: p[0] ** 2 / 2 * d[1])

    def mv(self):
        return -self._sum(lambda p, d: p[1] ** 2 / 2 * d[0])

    def muu(self):
        return self._sum(lambda p, d: p[0] ** 3 / 3 * d[1])

    def muv(self):
        return self._sum(lambda p, d: p[0] ** 2 * p[1] / 2 * d[1])

    def mvv(self):
        return -self._sum(lambda p, d: p[1] ** 3 / 3 * d[0])

    def centroid(self):
        a = self.area()
        return (self.mu() / a, self.mv() / a)

    def perimeter(self):
        return self._sum(lambda p, d: math.hypot(*d))

    def line_moment_u(self):
        """loop(u ds): with 2 pi, the area a full revolution sweeps."""
        return self._sum(lambda p, d: p[0] * math.hypot(*d))

    def bounds(self):
        pts = [q for loop in self.loops for e in loop
               for q in e.extreme_points()]
        return ((min(p[0] for p in pts), min(p[1] for p in pts)),
                (max(p[0] for p in pts), max(p[1] for p in pts)))


# ------------------------------------------------------------------- solids

class Solid:
    """Volume, first moments (sum of V * centroid), surface area and a
    bounding box. Adding two solids adds every one of them (a union of
    disjoint pieces); `minus` takes a piece away (its volume, its moment).
    Surface area does not compose that simply, so a caller that cuts states
    the area itself with `with_area`."""

    def __init__(self, volume, centroid, area, bbox):
        self.volume = float(volume)
        self.moment = [self.volume * c for c in centroid]
        self.area = float(area)
        self.bbox = (tuple(map(float, bbox[0])), tuple(map(float, bbox[1])))

    @property
    def centroid(self):
        return tuple(m / self.volume for m in self.moment)

    def _combine(self, other, sign, area, bbox):
        out = Solid(0.0, (0, 0, 0), area, bbox)
        out.volume = self.volume + sign * other.volume
        out.moment = [a + sign * b for a, b in zip(self.moment, other.moment)]
        return out

    def plus(self, other, area=None):
        """Disjoint union (or a piece glued on): area defaults to the sum,
        which is right only when no faces are shared."""
        lo = tuple(min(a, b) for a, b in zip(self.bbox[0], other.bbox[0]))
        hi = tuple(max(a, b) for a, b in zip(self.bbox[1], other.bbox[1]))
        return self._combine(other, +1,
                             self.area + other.area if area is None else area,
                             (lo, hi))

    def minus(self, other, area=None, bbox=None):
        """Take a piece away; the bbox stays this one's unless given."""
        return self._combine(other, -1,
                             self.area if area is None else area,
                             self.bbox if bbox is None else bbox)

    def with_area(self, area):
        self.area = float(area)
        return self

    def with_bbox(self, lo, hi):
        self.bbox = (tuple(map(float, lo)), tuple(map(float, hi)))
        return self

    def moved(self, dx, dy, dz):
        out = Solid(self.volume, self.centroid, self.area, self.bbox)
        c = self.centroid
        out.moment = [self.volume * (c[0] + dx), self.volume * (c[1] + dy),
                      self.volume * (c[2] + dz)]
        (a, b, cz), (d, e, f) = self.bbox
        out.bbox = ((a + dx, b + dy, cz + dz), (d + dx, e + dy, f + dz))
        return out

    def remapped(self, order, signs=(1, 1, 1)):
        """The same solid in other axes: new coordinate i is
        signs[i] * old coordinate order[i]. A region extruded in its own
        (u, v, w) frame lands in the world this way: on the XZ plane
        (u = x, v = z, normal -y) it is order (0, 2, 1), signs (1, -1, 1)."""
        out = Solid(self.volume, (0, 0, 0), self.area, self.bbox)
        out.moment = [signs[i] * self.moment[order[i]] for i in range(3)]
        lo = [signs[i] * self.bbox[0][order[i]] for i in range(3)]
        hi = [signs[i] * self.bbox[1][order[i]] for i in range(3)]
        out.bbox = (tuple(min(a, b) for a, b in zip(lo, hi)),
                    tuple(max(a, b) for a, b in zip(lo, hi)))
        return out

    def expected(self, bodies=1, faces=None):
        """The dict an expected.py returns."""
        out = {"bodies": bodies, "volume_mm3": self.volume,
               "area_mm2": self.area, "center_of_mass": list(self.centroid),
               "bbox": [list(self.bbox[0]), list(self.bbox[1])]}
        if faces is not None:
            out["faces"] = faces
        return out


def box(x0, y0, z0, x1, y1, z1):
    """A box: V = abc, A = 2(ab + bc + ca), centroid at its centre."""
    a, b, c = x1 - x0, y1 - y0, z1 - z0
    return Solid(a * b * c, ((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2),
                 2 * (a * b + b * c + c * a), ((x0, y0, z0), (x1, y1, z1)))


def cylinder_z(cx, cy, z0, r, h):
    """A cylinder on the z axis through (cx, cy) from z0 up h:
    V = pi r^2 h, A = 2 pi r^2 + 2 pi r h."""
    return Solid(PI * r * r * h, (cx, cy, z0 + h / 2),
                 2 * PI * r * r + 2 * PI * r * h,
                 ((cx - r, cy - r, z0), (cx + r, cy + r, z0 + h)))


def sphere(cx, cy, cz, r):
    """V = 4/3 pi r^3, A = 4 pi r^2."""
    return Solid(4 / 3 * PI * r ** 3, (cx, cy, cz), 4 * PI * r * r,
                 ((cx - r, cy - r, cz - r), (cx + r, cy + r, cz + r)))


def cone_frustum_z(cx, cy, z0, r0, r1, h):
    """A cone frustum on a z axis from radius r0 at z0 to r1 at z0 + h:
    V = pi h/3 (r0^2 + r0 r1 + r1^2), centroid
    h (r0^2 + 2 r0 r1 + 3 r1^2) / (4 (r0^2 + r0 r1 + r1^2)) above z0;
    lateral area pi (r0 + r1) s with slant s = sqrt(h^2 + (r0 - r1)^2)."""
    v = PI * h / 3 * (r0 * r0 + r0 * r1 + r1 * r1)
    zc = h * (r0 * r0 + 2 * r0 * r1 + 3 * r1 * r1) / (
        4 * (r0 * r0 + r0 * r1 + r1 * r1))
    s = math.hypot(h, r0 - r1)
    rm = max(r0, r1)
    return Solid(v, (cx, cy, z0 + zc),
                 PI * (r0 + r1) * s + PI * r0 * r0 + PI * r1 * r1,
                 ((cx - rm, cy - rm, z0), (cx + rm, cy + rm, z0 + h)))


def frustum_volume(a0, a1, h):
    """A frustum (pyramid or cone) between parallel similar sections of area
    a0 and a1, h apart: V = h/3 (a0 + a1 + sqrt(a0 a1))."""
    return h / 3 * (a0 + a1 + math.sqrt(a0 * a1))


def frustum_centroid_height(a0, a1, h):
    """Height of a similar-section frustum's centroid above the a0 end:
    the section scales linearly, s(z) = s0 + (s1 - s0) z/h, area ~ s^2, so
    zbar = h (a0 + 2 sqrt(a0 a1) + 3 a1) / (4 (a0 + sqrt(a0 a1) + a1))."""
    g = math.sqrt(a0 * a1)
    return h * (a0 + 2 * g + 3 * a1) / (4 * (a0 + g + a1))


def spherical_cap(R, h):
    """A cap of height h off a ball of radius R: V = pi h^2 (3R - h)/3, its
    centroid 3 (2R - h)^2 / (4 (3R - h)) from the ball's centre; curved area
    2 pi R h, and the flat a disc of radius sqrt(h (2R - h))."""
    v = PI * h * h * (3 * R - h) / 3
    d = 3 * (2 * R - h) ** 2 / (4 * (3 * R - h))
    return v, d, 2 * PI * R * h, PI * h * (2 * R - h)


def extrude(region, z0, h):
    """A plane region in (x, y) extruded along z from z0 by h: V = A h,
    centroid (ubar, vbar, z0 + h/2), surface 2A + P h."""
    a = region.area()
    cu, cv = region.centroid()
    (u0, v0), (u1, v1) = region.bounds()
    return Solid(a * h, (cu, cv, z0 + h / 2),
                 2 * a + region.perimeter() * h,
                 ((u0, v0, z0), (u1, v1, z0 + h)))


def revolve_z(region, angle=2 * PI):
    """A region in the (r, z) half plane (u = r >= 0, v = z) revolved about
    the z axis by `angle`, starting in the +x half of the XZ plane and
    turning towards +y (Pappus-Guldin, in moments):
      V   = angle int r dA
      M_x = sin(angle) int r^2 dA,  M_y = (1 - cos angle) int r^2 dA,
      M_z = angle int r z dA
      A   = angle loop(r ds), plus the two end faces (2 A2) when partial.
    The bounding box is left to the caller, which knows the profile."""
    a2 = region.area()
    v = angle * region.mu()
    moment = (math.sin(angle) * region.muu(),
              (1 - math.cos(angle)) * region.muu(), angle * region.muv())
    area = angle * region.line_moment_u()
    if abs(angle - 2 * PI) > 1e-12:
        area += 2 * a2
    s = Solid(v, (moment[0] / v, moment[1] / v, moment[2] / v), area,
              ((0, 0, 0), (0, 0, 0)))
    s.moment = list(moment)
    return s


def torus_z(R, r, cz=0.0):
    """A ring torus about the z axis: V = 2 pi^2 R r^2, A = 4 pi^2 R r."""
    return Solid(2 * PI * PI * R * r * r, (0, 0, cz), 4 * PI * PI * R * r,
                 ((-(R + r), -(R + r), cz - r), (R + r, R + r, cz + r)))


def rounded_box(a, b, c, r):
    """A box a x b x c with every edge rounded r (Steiner's formula on the
    inner box a' = a - 2r, ...): V = a'b'c' + 2r(a'b' + b'c' + c'a')
    + pi r^2 (a' + b' + c') + 4/3 pi r^3, A = 2(a'b' + b'c' + c'a')
    + 2 pi r (a' + b' + c') + 4 pi r^2. Centred on the origin."""
    p, q, s = a - 2 * r, b - 2 * r, c - 2 * r
    v = (p * q * s + 2 * r * (p * q + q * s + s * p)
         + PI * r * r * (p + q + s) + 4 / 3 * PI * r ** 3)
    area = 2 * (p * q + q * s + s * p) + 2 * PI * r * (p + q + s) \
        + 4 * PI * r * r
    return Solid(v, (0, 0, 0), area,
                 ((-a / 2, -b / 2, -c / 2), (a / 2, b / 2, c / 2)))


# --------------------------------------------------------------- self-check

def _close(a, b, rel=1e-12, what=""):
    if abs(a - b) > rel * max(1.0, abs(a), abs(b)):
        raise AssertionError(f"geom self-check: {what}: {a!r} != {b!r}")


def _shoelace(points):
    n = len(points)
    return 0.5 * sum(points[i][0] * points[(i + 1) % n][1]
                     - points[(i + 1) % n][0] * points[i][1]
                     for i in range(n))


def self_check():
    """Each general routine against a case it must reproduce. Raises
    AssertionError naming the first that does not."""
    # Green against the shoelace formula, on a non-convex polygon given
    # clockwise (Region re-orients it).
    pts = [(0, 0), (0, 7), (3, 4), (8, 9), (10, 1), (5, 2)]
    reg = Region(polygon(pts))
    _close(reg.area(), abs(_shoelace(pts)), what="Green vs shoelace")
    # ... its centroid against the triangle-fan formula.
    a = _shoelace(pts)
    cx = sum((pts[i][0] + pts[(i + 1) % 6][0])
             * (pts[i][0] * pts[(i + 1) % 6][1]
                - pts[(i + 1) % 6][0] * pts[i][1]) for i in range(6)) / (6 * a)
    _close(reg.centroid()[0], cx, what="Green centroid vs polygon formula")
    # Green against pi r^2 on a circle split into three arcs, off centre.
    r = 7.0
    arcs = [Arc((3, -2), r, 0, 2), Arc((3, -2), r, 2, 4.5),
            Arc((3, -2), r, 4.5, 2 * PI)]
    disc = Region(arcs)
    _close(disc.area(), PI * r * r, what="Green vs pi r^2")
    _close(disc.centroid()[0], 3.0, rel=1e-12, what="disc centroid u")
    _close(disc.perimeter(), 2 * PI * r, what="circle perimeter")
    # A hole subtracts, whichever way it was given.
    ring = Region(circle((0, 0), 10), [circle((2, 0), 3)])
    _close(ring.area(), PI * (100 - 9), what="annulus with a hole")
    # Arc bounds reach the axis crossings.
    (u0, v0), (u1, v1) = Region(circle((1, 1), 2)).bounds()
    _close(u0, -1, what="circle bounds"), _close(v1, 3, what="circle bounds")
    # Pappus against a cylinder: the rectangle r 0..5, z 0..4.
    cyl = revolve_z(Region(rectangle(0, 0, 5, 4)))
    ref = cylinder_z(0, 0, 0, 5, 4)
    _close(cyl.volume, ref.volume, what="Pappus vs cylinder volume")
    _close(cyl.area, ref.area, what="Pappus vs cylinder area")
    _close(cyl.centroid[2], 2.0, what="Pappus vs cylinder centroid")
    # ... and a torus.
    tor = revolve_z(Region(circle((20, 3), 4)))
    _close(tor.volume, torus_z(20, 4).volume, what="Pappus vs torus volume")
    _close(tor.area, torus_z(20, 4).area, what="Pappus vs torus area")
    _close(tor.centroid[2], 3.0, what="Pappus vs torus centroid")
    # A partial revolve's centroid against the annular sector's moments:
    # (R^3 - r^3)/3 * h * (sin t, 1 - cos t) over V.
    t = 1.2
    sec = revolve_z(Region(rectangle(4, 0, 12, 8)), t)
    _close(sec.volume, t / 2 * (144 - 16) * 8, what="sector volume")
    _close(sec.centroid[0],
           (12 ** 3 - 4 ** 3) / 3 * 8 * math.sin(t) / sec.volume,
           what="sector centroid x")
    # The frustum at zero taper is a prism; a cone frustum likewise.
    _close(frustum_volume(9, 9, 5), 45, what="frustum vs prism")
    _close(frustum_centroid_height(9, 9, 5), 2.5, what="frustum centroid")
    _close(cone_frustum_z(0, 0, 0, 3, 3, 5).volume,
           cylinder_z(0, 0, 0, 3, 5).volume, what="cone frustum vs cylinder")
    # ... and a cone frustum against Pappus on its trapezoid.
    trap = revolve_z(Region(polygon([(0, 0), (6, 0), (2, 9), (0, 9)])))
    cf = cone_frustum_z(0, 0, 0, 6, 2, 9)
    _close(trap.volume, cf.volume, what="cone frustum vs Pappus")
    _close(trap.area, cf.area, what="cone frustum area vs Pappus")
    _close(trap.centroid[2], cf.centroid[2], what="cone frustum centroid")
    # An extrude on the XZ plane (u = x, v = z, normal -y) is the box it
    # should be.
    b = extrude(Region(rectangle(0, 0, 2, 3)), 0, 4).remapped((0, 2, 1),
                                                              (1, -1, 1))
    ref = box(0, -4, 0, 2, 0, 3)
    for i in range(3):
        _close(b.centroid[i], ref.centroid[i], what="remapped centroid")
        _close(b.bbox[0][i], ref.bbox[0][i], what="remapped bbox")
        _close(b.bbox[1][i], ref.bbox[1][i], what="remapped bbox")
    # Steiner at r = 0 is the box; a full cap is the ball.
    _close(rounded_box(4, 5, 6, 0).volume, 120, what="Steiner at r = 0")
    _close(rounded_box(4, 6, 8, 2).volume, sphere(0, 0, 0, 2).volume
           + PI * 4 * (0 + 2 + 4) + 2 * 2 * (0 + 8 + 0) + 0,
           what="Steiner on a 0x2x4 core")
    v, d, curved, flat = spherical_cap(5, 10)
    _close(v, sphere(0, 0, 0, 5).volume, what="a full cap is the ball")
    _close(d, 0.0, what="a full cap's centroid is the centre")
    return True
