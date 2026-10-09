#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Designer's own icon font: CAD glyphs Font Awesome has no vocabulary for.

The icons are GEOMETRY, defined below with shapely: strokes are buffered into
outlines and unioned, so what lands in the font is a clean filled contour
(a TrueType glyph cannot hold an SVG stroke). Every icon is drawn on the
14 x 14 grid it renders at -- one unit is one pixel at 100% UI scale -- with
y pointing DOWN and the baseline at y = 12, the box Font Awesome Solid's
glyphs occupy at the same size, so the two fonts merge on one baseline.

Outputs, both committed so a build needs no Python:
  deps/fonts/designer-icons.ttf   merged after fa-solid-900.ttf (Theme.cpp)
  deps/fonts/IconsDesigner.h      ICON_DS_<NAME> as UTF-8, from U+E900

  tools/icons/build_icon_font.py            write both
  tools/icons/build_icon_font.py --check    fail if either is out of date
  tools/icons/build_icon_font.py --sheet OUT.png [--compare]   review sheet

Run through nix: nix-shell -p "python3.withPackages(p: [p.fonttools
p.shapely p.pillow])" --run "python3 tools/icons/build_icon_font.py"
(docs/ui/README.md#icons).
"""

import argparse
import io
import math
import os
import sys

from shapely import affinity
from shapely.geometry import LineString, Point, Polygon, box as _box
from shapely.geometry.polygon import orient
from shapely.ops import unary_union

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TTF = os.path.join(ROOT, "deps/fonts/designer-icons.ttf")
HEADER = os.path.join(ROOT, "deps/fonts/IconsDesigner.h")
FIRST_CODEPOINT = 0xE900  # Font Awesome 6 uses nothing in U+E700..U+EFFF
GRID = 14                 # the icon box, in pixels at 100% UI scale
BASELINE = 12             # grid row the text baseline runs along
UNIT = 64                 # font units per grid unit
W = 2.0                   # the default stroke: two pixels, like FA Solid

# --------------------------------------------------------------------------
# Drawing primitives. Coordinates are grid units, y down.


def U(*shapes):
    return unary_union([s for s in shapes if s is not None and not s.is_empty])


def rect(x, y, w, h):
    return _box(x, y, x + w, y + h)


def poly(*pts):
    return Polygon(pts)


def disc(cx, cy, r):
    return Point(cx, cy).buffer(r, quad_segs=24)


def ellipse(cx, cy, rx, ry):
    unit = Point(0, 0).buffer(1.0, quad_segs=32)
    return affinity.translate(affinity.scale(unit, rx, ry, origin=(0, 0)), cx, cy)


def ring(cx, cy, r, w=W):
    """A circle stroked on radius r."""
    return disc(cx, cy, r + w / 2) - disc(cx, cy, r - w / 2)


def ering(cx, cy, rx, ry, w=W):
    return ellipse(cx, cy, rx + w / 2, ry + w / 2) - ellipse(cx, cy, rx - w / 2,
                                                            ry - w / 2)


def line(*pts, w=W, cap="flat", join="mitre"):
    return LineString(pts).buffer(w / 2, cap_style=cap, join_style=join,
                                  mitre_limit=3.0, quad_segs=12)


def arc_pts(cx, cy, rx, a0, a1, ry=None, n=48):
    """Points along an arc, angles in degrees, 0 = +x, 90 = UP (y down)."""
    ry = rx if ry is None else ry
    return [(cx + rx * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
             cy - ry * math.sin(math.radians(a0 + (a1 - a0) * i / n)))
            for i in range(n + 1)]


def arc(cx, cy, r, a0, a1, w=W, ry=None, cap="flat"):
    return line(*arc_pts(cx, cy, r, a0, a1, ry), w=w, cap=cap, join="round")


def head(tip, deg, length=4.0, width=5.0):
    """An arrowhead whose TIP is at `tip`, pointing along `deg` (90 = up)."""
    dx, dy = math.cos(math.radians(deg)), -math.sin(math.radians(deg))
    bx, by = tip[0] - dx * length, tip[1] - dy * length
    px, py = -dy * width / 2, dx * width / 2
    return poly(tip, (bx + px, by + py), (bx - px, by - py))


def arc_arrow(cx, cy, r, a0, a1, w=W, length=4.0, width=5.5):
    """An arc from a0 to a1 with an arrowhead ending exactly at a1."""
    sense = 1 if a1 > a0 else -1
    back = math.degrees(length * 0.8 / r) * sense
    tip = arc_pts(cx, cy, r, a1, a1)[0]
    return U(arc(cx, cy, r, a0, a1 - back, w=w),
             head(tip, a1 + 90 * sense, length, width))


def dashed(p0, p1, dash, gap, w=W, phase=0.0):
    L = math.dist(p0, p1)
    ux, uy = (p1[0] - p0[0]) / L, (p1[1] - p0[1]) / L
    out, t = [], -phase
    while t < L:
        a, b = max(t, 0.0), min(t + dash, L)
        if b > a:
            out.append(line((p0[0] + ux * a, p0[1] + uy * a),
                            (p0[0] + ux * b, p0[1] + uy * b), w=w))
        t += dash + gap
    return U(*out)


def outline(shape, w=W):
    """The shape's own boundary, stroked INSIDE it."""
    return shape - shape.buffer(-w, join_style="mitre")


def pt(x, y, s=3.0):
    """A sketch point: a small filled square."""
    return rect(x - s / 2, y - s / 2, s, s)


def rot(shape, deg, about=(7, 7)):
    return affinity.rotate(shape, -deg, origin=about)  # y down: flip sense


def mirror_x(shape):
    return affinity.scale(shape, -1, 1, origin=(7, 7))


def bezier(p0, p1, p2, p3, n=40):
    out = []
    for i in range(n + 1):
        t = i / n
        a, b, c, d = (1 - t) ** 3, 3 * (1 - t) ** 2 * t, 3 * (1 - t) * t * t, t ** 3
        out.append((a * p0[0] + b * p1[0] + c * p2[0] + d * p3[0],
                    a * p0[1] + b * p1[1] + c * p2[1] + d * p3[1]))
    return out


# --------------------------------------------------------------------------
# The icons. Order is the codepoint order: APPEND, never reorder, so a
# glyph's codepoint is stable across rebuilds.

ICONS = {}


def icon(fn):
    ICONS[fn.__name__.upper()] = fn
    return fn


# ---- part ----------------------------------------------------------------

@icon
def extrude():
    base = poly((0, 14), (9, 14), (13, 11), (4, 11))
    return U(base, rect(6, 4.5, 2, 5.5), head((7, 0), 90, 4.5, 8))


@icon
def revolve():
    # A profile beside its axis, and the turn it is swept through.
    turn = U(line(*arc_pts(7, 9.5, 6, 195, 330, 3.5), w=2),
             head((13.6, 7.2), 70, 4, 5.5))
    return U(line((2, 0), (2, 14), w=2) - rect(0, 8.5, 4, 2.5),
             rect(4, 0, 5, 6), turn)


@icon
def sweep():
    path = bezier((1.5, 13), (1.5, 5), (9, 10), (9, 3.5))
    return U(line(*path, w=2), rect(4.5, 0, 9, 2.5))


@icon
def loft():
    return U(rect(0, 11, 14, 3), rect(4, 0, 6, 3),
             line((1, 11), (4.5, 3), w=2), line((13, 11), (9.5, 3), w=2))


@icon
def pipe():
    path = arc_pts(13, 13, 9, 180, 90)
    tube = line(*path, w=7) - line(*path, w=3)
    return U(tube, rect(1, 12, 6, 2) - rect(2.5, 12, 3, 2),
             rect(12, 1, 2, 6) - rect(12, 2.5, 2, 3))


@icon
def helix():
    pts = [(7 + 5.5 * math.sin(t / 60 * 6 * math.pi), 1 + 12 * t / 60)
           for t in range(61)]
    return line(*pts, w=1.8, join="round")


@icon
def cylinder():
    body = U(rect(1, 3, 12, 8), ellipse(7, 11, 6, 2.5), ellipse(7, 3, 6, 2.5))
    rim = line(*arc_pts(7, 3, 6, 180, 360, 2.5), w=1.2)
    return body - rim


@icon
def sphere():
    return disc(7, 7, 6.5) - line(*arc_pts(7, 7, 6.5, 180, 360, 2.2), w=1.2) \
        - line(*arc_pts(7, 7, 2.2, 90, 270, 6.5), w=1.2)


@icon
def datum_plane():
    return outline(poly((0, 12), (9, 12), (14, 3), (5, 3)), 2)


@icon
def datum_axis():
    return U(dashed((1, 13), (11.5, 2.5), 4, 1.6, w=2), head((13.5, 0.5), 45, 4.5, 6))


@icon
def boolean():
    a, b = disc(5, 9, 4.8), disc(9, 5, 4.8)
    return U(a - b.buffer(1.2), ring(9, 5, 4.0, 1.6))


@icon
def mirror():
    return U(poly((0, 12), (5, 12), (5, 2)),
             outline(poly((14, 12), (9, 12), (9, 2)), 1.6),
             dashed((7, 0), (7, 14), 2.5, 1.5, w=1.4))


@icon
def fillet():
    r = 8
    return U(rect(1, 1, 12 - r, 12), rect(1, 1 + r, 12, 12 - r),
             disc(13 - r, 1 + r, r).intersection(rect(1, 1, 12, 12)))


@icon
def chamfer():
    return rect(1, 1, 12, 12) - poly((6, 0), (14, 0), (14, 8))


@icon
def rib():
    return U(rect(0, 0, 2.5, 14), rect(0, 11.5, 14, 2.5),
             poly((4, 3.5), (4, 10), (10.5, 10)))


@icon
def thread():
    bars = [line((2, y + 1.5), (12, y - 1.5), w=2) for y in (5, 8.5, 12)]
    return U(rect(0, 0, 14, 2.5), *bars) - rect(0, 13.5, 14, 2)


@icon
def shell():
    return rect(1, 1, 12, 12) - rect(3.5, 1, 7, 9.5)


@icon
def draft():
    return U(poly((4, 13), (13, 13), (13, 1), (7, 1)),
             dashed((1.5, 1), (1.5, 13), 2.5, 1.5, w=1.6))


@icon
def split():
    body = rect(2, 2, 10, 10) - line((0, 14), (14, 0), w=2)
    return U(body, line((0, 14), (1.2, 12.8), w=1.4), line((12.8, 1.2), (14, 0), w=1.4))


@icon
def section():
    sq = rect(1, 1, 12, 12)
    left = rect(1, 1, 5, 12)
    hatch = U(*[line((x, 14), (x + 14, 0), w=1.2) for x in range(-8, 14, 3)])
    return U(left, hatch.intersection(rect(7, 1, 6, 12)), outline(sq, 1.2))


# ---- sheet metal ---------------------------------------------------------

@icon
def base_flange():
    top = poly((0, 9), (8, 9), (14, 3), (6, 3))
    return U(top, poly((0, 10.3), (8, 10.3), (8, 12.8), (0, 12.8)),
             poly((9.2, 10.1), (14, 5.3), (14, 7.8), (9.2, 12.6)))


@icon
def edge_flange():
    pts = [(0, 12)] + arc_pts(8, 8, 4, 270, 360) + [(12, 1)]
    return line(*pts, w=2.2, join="round")


@icon
def hem():
    pts = [(0, 4)] + arc_pts(9, 7.5, 3.5, 90, -90) + [(3, 11)]
    return line(*pts, w=2.2, join="round")


@icon
def jog():
    return line((0, 11.5), (4.5, 11.5), (9.5, 3.5), (14, 3.5), w=2.2, join="round")


@icon
def corner_relief():
    return rect(1, 1, 12, 12) - rect(8.5, 0, 6, 5.5) - disc(8.5, 5.5, 2.3)


@icon
def normal_cut():
    return U(rect(0, 8, 5.8, 3), rect(8.2, 8, 5.8, 3), rect(6, 0, 2, 3),
             head((7, 7), -90, 3.5, 6))


@icon
def flat_pattern():
    return U(rect(4, 4, 6, 6), rect(4, 0, 6, 3), rect(4, 11, 6, 3),
             rect(0, 4, 3, 6), rect(11, 4, 3, 6))


@icon
def unfold():
    # The bent flange (thin, where it was) swung down flat (solid).
    return U(line((0, 12.5), (14, 12.5), w=2.2), line((3, 12), (3, 1), w=1),
             arc(3, 12.5, 8.5, 82, 30, w=2), head((11.2, 7.6), -55, 4, 5.5))


@icon
def refold():
    # The flat flange (thin, where it was) swung back up (solid).
    return U(line((0, 12.5), (4, 12.5), w=2.2), line((3, 13.6), (3, 0), w=2.2),
             line((4, 12.5), (14, 12.5), w=1),
             arc(3, 12.5, 8.5, 5, 60, w=2), head((7.3, 5.1), 150, 4, 5.5))


# ---- sketch --------------------------------------------------------------

@icon
def circle():
    return U(ring(7, 7, 5.5, 2), disc(7, 7, 1.3))


@icon
def ellipse_tool():
    return U(ering(7, 7, 6, 3.8, 2), disc(7, 7, 1.3))


@icon
def rectangle():
    return U(outline(rect(1.5, 3, 11, 8), 1.8), pt(1.5, 3), pt(12.5, 3),
             pt(1.5, 11), pt(12.5, 11))


@icon
def centre_rectangle():
    return U(outline(rect(0.5, 2, 13, 10), 1.8), pt(7, 7, 3))


@icon
def arc_3point():
    return U(arc(7, 12, 6, 180, 0, w=2), pt(1, 12), pt(13, 12), pt(7, 6))


@icon
def arc_centre():
    return U(arc(3, 11, 8.5, 0, 90, w=2), pt(3, 11), dashed((3, 11), (11.5, 11), 1.5, 1.2, w=1))


@icon
def ellipse_arc():
    return U(arc(7, 10, 6, 180, 0, w=2, ry=7.5), pt(7, 10))


@icon
def horizontal():
    return U(line((1, 7), (13, 7), w=2), line((1, 3), (1, 11), w=2),
             line((13, 3), (13, 11), w=2))


@icon
def vertical():
    return rot(horizontal(), 90)


@icon
def coincident():
    return U(line((0.5, 13), (7, 7), w=2), line((13.5, 13), (7, 7), w=2),
             disc(7, 6.5, 3))


@icon
def parallel():
    return U(line((1, 13), (7, 1), w=2), line((7, 13), (13, 1), w=2))


@icon
def perpendicular():
    return U(line((0, 12.5), (14, 12.5), w=2), line((5, 13), (5, 0), w=2),
             line((6, 8.5), (9.5, 8.5), (9.5, 11.5), w=1.2))


@icon
def construction():
    return U(dashed((1, 13), (13, 1), 3, 1.8, w=2), pt(1, 13), pt(13, 1))


@icon
def offset():
    return U(outline(rect(0, 0, 14, 14).buffer(-0.01).buffer(0), 1.6)
             - rect(0, 0, 0, 0),
             outline(rect(4, 4, 6, 6), 1.8), dashed((1.6, 1.6), (4, 4), 1, 1, w=1),
             dashed((12.4, 12.4), (10, 10), 1, 1, w=1))


@icon
def project():
    base = outline(poly((0, 14), (9, 14), (14, 9), (5, 9)), 1.6)
    return U(rect(4, 0, 6, 4), base, dashed((5, 4), (5, 10), 1.5, 1.2, w=1.2),
             dashed((9, 4), (9, 10), 1.5, 1.2, w=1.2))


# ---- assembly mates ------------------------------------------------------

@icon
def mate_fastened():
    plates = U(rect(0, 3, 14, 3.5), rect(0, 7.5, 14, 3.5))
    return U(plates - rect(4.5, 0, 5, 14), rect(5.5, 0, 3, 14), rect(3.5, 0, 7, 2))


@icon
def mate_revolute():
    return U(disc(7, 7, 2.5), arc_arrow(7, 7, 5.5, 120, 400, w=2, length=4, width=5.5))


@icon
def mate_slider():
    return U(rect(0, 11.5, 14, 2.5), rect(4, 5, 6, 5),
             head((0, 7.5), 180, 3, 5), head((14, 7.5), 0, 3, 5))


@icon
def mate_cylindrical():
    return U(rect(0, 6, 14, 2), ering(7, 7, 2.2, 5.5, 1.6) - rect(6.1, 5.2, 1.8, 3.6),
             head((0, 7), 180, 3, 5.5), head((14, 7), 0, 3, 5.5))


@icon
def mate_planar():
    return U(outline(poly((0, 13), (9, 13), (14, 7), (5, 7)), 1.6),
             rect(4.5, 1, 5, 5))


@icon
def mate_ball():
    return U(disc(7, 5.5, 4), arc(7, 6, 6.2, 200, 340, w=2), rect(6, 11.5, 2, 2.5))


# ---- drawing dimensions --------------------------------------------------

@icon
def dim_x():
    return U(line((1, 1), (1, 13), w=2), line((13, 1), (13, 13), w=2),
             line((5, 7), (9, 7), w=1.6), head((2.2, 7), 180, 3.5, 5),
             head((11.8, 7), 0, 3.5, 5))


@icon
def dim_y():
    return rot(dim_x(), 90)


@icon
def dim_aligned():
    return rot(dim_x(), 45)


@icon
def dim_radius():
    return U(arc(1.5, 12.5, 11, 0, 90, w=2), pt(1.5, 12.5, 3),
             line((1.5, 12.5), (7.5, 6.5), w=1.6), head((9.3, 4.7), 45, 4, 5))


@icon
def dim_diameter():
    return U(ring(7, 7, 5, 2), line((1, 13), (13, 1), w=2))


@icon
def dim_angle():
    return U(line((0.5, 13), (14, 13), w=2), line((0.5, 13), (8, 1), w=2),
             arc(0.5, 13, 9, 3, 50, w=1.6), head((9.5, 12), -90, 3, 4.5))


# ---- CAM operations: the cutter over the work ----------------------------

def _cutter(x, y, h=6):
    return rect(x, y, 3, h)


@icon
def cam_pocket():
    return U(rect(0, 8, 14, 6) - rect(3.5, 8, 7, 3.5), _cutter(5.5, 1, 7))


@icon
def cam_profile():
    return U(rect(0, 6, 7.5, 8), _cutter(9.5, 2, 9), dashed((13.5, 6), (13.5, 14), 1.5, 1.2, w=1))


@icon
def cam_face():
    return U(rect(0, 10, 14, 4), _cutter(6, 1, 6),
             dashed((0, 8.5), (5, 8.5), 1.5, 1, w=1), head((14, 8.5), 0, 3, 4))


@icon
def cam_walls():
    m = poly((1, 14), (4, 3), (10, 3), (13, 14))
    return U(m - rect(0, 6, 14, 1.2) - rect(0, 9.5, 14, 1.2), _cutter(12, 0, 5) - m.buffer(1))


@icon
def cam_surface_rough():
    steps = U(rect(0, 11, 14, 3), rect(2, 8, 10, 3), rect(4, 5, 6, 3))
    return U(steps - rect(0, 10.6, 14, 0.8) - rect(0, 7.6, 14, 0.8), _cutter(5.5, 0, 3.5))


@icon
def cam_surface():
    dome = disc(7, 17, 9.5).intersection(rect(0, 0, 14, 14))
    return U(dome, line(*arc_pts(7, 17, 12, 145, 35), w=1.4))


def _axis():
    """The A axis the part turns on: a centre line out past both ends."""
    return U(rect(0, 6.2, 1.2, 1.6), rect(12.8, 6.2, 1.2, 1.6))


@icon
def cam_rotary_rough():
    # A turned part lying along the rotary axis, stepped as roughing leaves
    # it, under the turn the A axis gives it.
    return U(rect(2, 5, 10, 4), rect(4, 2.5, 3, 9), rect(8.5, 1, 3, 12), _axis())


@icon
def cam_rotary_finish():
    prof = [(2 + x / 4, 7 - (3.8 + 2.2 * math.cos(x / 40 * 2.6 * math.pi)))
            for x in range(41)]
    body = Polygon(prof + [(q[0], 14 - q[1]) for q in reversed(prof)])
    return U(body, _axis())


# --------------------------------------------------------------------------


def cleaned(shape):
    """Snap near-grid coordinates onto the pixel grid, so axis-aligned edges
    that should be crisp are not a hair off."""
    s = shape.buffer(0)
    if s.geom_type == "Polygon":
        return [s]
    return [g for g in s.geoms if g.geom_type == "Polygon" and g.area > 1e-3]


def centred(shape):
    """Moves the glyph's INK to the middle of the 14 x 14 box -- the box
    fills the 14 px icon slot exactly (Theme.cpp), and its middle row sits on
    the middle of Font Awesome's glyph box -- by whole pixels, so edges drawn
    on the grid stay on it. An odd ink width cannot centre on whole pixels;
    it lands half a pixel left, and centre_report() lists those."""
    x0, y0, x1, y1 = shape.bounds
    dx = math.floor(GRID / 2 - (x0 + x1) / 2 + 0.5)
    dy = math.floor(GRID / 2 - (y0 + y1) / 2 + 0.5)
    return affinity.translate(shape, dx, dy)


def contours(fn):
    polys = cleaned(centred(fn()))
    out = []
    for p in polys:
        p = orient(p, sign=-1.0)  # y DOWN here; the flip below makes it CW
        for ring_ in [p.exterior, *p.interiors]:
            pts = list(ring_.coords)[:-1]
            out.append([(round(x * UNIT), round((BASELINE - y) * UNIT)) for x, y in pts])
    return out


def build_font():
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.ttGlyphPen import TTGlyphPen

    names = list(ICONS)
    order = [".notdef"] + names
    cmap = {FIRST_CODEPOINT + i: n for i, n in enumerate(names)}
    glyphs, metrics = {}, {}
    empty = TTGlyphPen(None)
    glyphs[".notdef"] = empty.glyph()
    metrics[".notdef"] = (GRID * UNIT, 0)
    for n in names:
        pen = TTGlyphPen(None)
        for c in contours(ICONS[n]):
            pen.moveTo(c[0])
            for q in c[1:]:
                pen.lineTo(q)
            pen.closePath()
        g = pen.glyph()
        g.recalcBounds(None)
        metrics[n] = (GRID * UNIT, getattr(g, "xMin", 0))
        glyphs[n] = g
    fb = FontBuilder(GRID * UNIT, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap(cmap)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics(metrics)
    asc, desc = BASELINE * UNIT, -(GRID - BASELINE) * UNIT
    fb.setupHorizontalHeader(ascent=asc, descent=desc)
    fb.setupNameTable({"familyName": "Designer Icons", "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=asc, sTypoDescender=desc, usWinAscent=asc,
                usWinDescent=-desc)
    fb.setupPost()
    fb.font["head"].created = fb.font["head"].modified = 0  # reproducible bytes
    buf = io.BytesIO()
    fb.save(buf)
    return buf.getvalue()


def sha256(data):
    import hashlib
    return hashlib.sha256(data).hexdigest()


def build_header(ttf):
    """The header also records what it was built FROM and the font it was
    built WITH, so tests/icon_font_gate.py can tell a stale pair apart with
    nothing but the standard library (ctest and CI have no fontTools)."""
    lines = [
        "// Generated by tools/icons/build_icon_font.py -- do not edit.",
        "// Designer's own icon glyphs, merged after Font Awesome (Theme.cpp).",
        f"// source-sha256: {sha256(open(os.path.abspath(__file__), 'rb').read())}",
        f"// ttf-sha256: {sha256(ttf)}",
        "#pragma once",
        "",
        f"#define ICON_MIN_DS 0x{FIRST_CODEPOINT:04x}",
        f"#define ICON_MAX_DS 0x{FIRST_CODEPOINT + len(ICONS) - 1:04x}",
    ]
    for i, n in enumerate(ICONS):
        cp = FIRST_CODEPOINT + i
        utf8 = "".join(f"\\x{b:02x}" for b in chr(cp).encode("utf-8"))
        lines.append(f'#define ICON_DS_{n} "{utf8}" // U+{cp:04X}')
    return "\n".join(lines) + "\n"


def sheet(path, compare):
    """The review sheet, placed the way the app places an icon: a 14 px slot,
    Font Awesome's glyph box centred in it, each glyph centred by advance."""
    from PIL import Image, ImageDraw, ImageFont

    font_bytes = build_font()
    names = list(ICONS)
    fa = os.path.join(ROOT, "deps/fonts/fa-solid-900.ttf")
    replaced = compare_map() if compare else {}
    cols, cw, ch = 6, 208, 64
    rows = (len(names) + cols - 1) // cols
    img = Image.new("RGB", (cols * cw, rows * ch), (27, 29, 35))
    d = ImageDraw.Draw(img)
    ds14 = ImageFont.truetype(io.BytesIO(font_bytes), 14)
    fa14 = ImageFont.truetype(fa, 14)
    fa_mid = (448 - 64) / 2 * 14 / 534  # FA's box centre above the baseline

    def button(ch_, font, size=26):
        """A square button with the glyph where ImGui puts it."""
        tile = Image.new("RGB", (size, size), (41, 44, 53))
        adv = font.getlength(ch_)
        x = (size - GRID) / 2 + (GRID - adv) / 2
        base = size / 2 + fa_mid
        ImageDraw.Draw(tile).text((x, base), ch_, font=font, anchor="ls",
                                  fill=(232, 234, 239))
        return tile

    for i, n in enumerate(names):
        x, y = (i % cols) * cw, (i // cols) * ch
        ch_ = chr(FIRST_CODEPOINT + i)
        tile = button(ch_, ds14, 18)
        img.paste(tile.resize((54, 54), Image.NEAREST), (x + 2, y + 4))
        img.paste(button(ch_, ds14), (x + 64, y + 6))
        if n in replaced:
            old = button(chr(replaced[n]), fa14)
            img.paste(Image.eval(old, lambda v: int(v * 0.7)), (x + 96, y + 6))
        d.text((x + 60, y + 40), n.lower()[:26], fill=(150, 200, 255))
    img.save(path)


def compare_map():
    """Which FA glyph each new icon replaces, for the review sheet only."""
    import re
    hdr = open(os.path.join(ROOT, "deps/fonts/IconsFontAwesome6.h")).read()
    cp = {m[0][8:]: int(m[1], 16) for m in re.findall(
        r'#define (ICON_FA_\w+)\s+"[^"]*"\s*// U\+(\w+)', hdr)}
    was = {
        "EXTRUDE": "UP_LONG", "REVOLVE": "ROTATE", "SWEEP": "ROUTE",
        "LOFT": "CUBES_STACKED", "PIPE": "ROAD", "HELIX": "WAVE_SQUARE",
        "CYLINDER": "DATABASE", "SPHERE": "CIRCLE",
        "DATUM_PLANE": "TABLE_CELLS_LARGE", "DATUM_AXIS": "ARROWS_UP_DOWN",
        "BOOLEAN": "OBJECT_GROUP", "MIRROR": "LEFT_RIGHT",
        "FILLET": "BORDER_TOP_LEFT", "CHAMFER": "CROP_SIMPLE",
        "RIB": "BARS_STAGGERED", "THREAD": "LINES_LEANING", "SHELL": "BOX_OPEN",
        "DRAFT": "MOUNTAIN", "SPLIT": "TABLE_COLUMNS", "SECTION": "SWATCHBOOK",
        "BASE_FLANGE": "LAYER_GROUP", "EDGE_FLANGE": "ARROW_TURN_UP",
        "HEM": "ROTATE_LEFT", "JOG": "STAIRS", "CORNER_RELIEF": "OBJECT_UNGROUP",
        "NORMAL_CUT": "CIRCLE_MINUS", "FLAT_PATTERN": "FILE_LINES",
        "UNFOLD": "MAP", "REFOLD": "DOWN_LEFT_AND_UP_RIGHT_TO_CENTER",
        "CIRCLE": "RING", "ELLIPSE_TOOL": "EGG", "RECTANGLE": "VECTOR_SQUARE",
        "CENTRE_RECTANGLE": "EXPAND", "ARC_3POINT": "BEZIER_CURVE",
        "ARC_CENTRE": "BEZIER_CURVE", "ELLIPSE_ARC": "BEZIER_CURVE",
        "COINCIDENT": "ARROWS_TO_DOT", "PARALLEL": "GRIP_LINES_VERTICAL",
        "PERPENDICULAR": "ANGLE_RIGHT", "CONSTRUCTION": "MINUS",
        "OFFSET": "CLONE", "PROJECT": "DIAGRAM_PROJECT",
        "MATE_FASTENED": "LOCK", "MATE_REVOLUTE": "ARROWS_SPIN",
        "MATE_SLIDER": "ARROWS_LEFT_RIGHT",
        "MATE_CYLINDRICAL": "ARROWS_UP_DOWN_LEFT_RIGHT",
        "MATE_PLANAR": "TABLE_CELLS_LARGE", "MATE_BALL": "GLOBE",
        "DIM_X": "RULER_HORIZONTAL", "DIM_Y": "UP_DOWN",
        "DIM_ALIGNED": "ARROWS_LEFT_RIGHT_TO_LINE", "DIM_RADIUS": "CIRCLE_NOTCH",
        "DIM_DIAMETER": "CIRCLE_HALF_STROKE", "DIM_ANGLE": "ANGLE_LEFT",
        "CAM_POCKET": "SQUARE_MINUS", "CAM_PROFILE": "DRAW_POLYGON",
        "CAM_FACE": "GRIP_LINES", "CAM_WALLS": "MOUNTAIN_SUN",
        "CAM_SURFACE_ROUGH": "CHART_AREA", "CAM_SURFACE": "WATER",
        "CAM_ROTARY_ROUGH": "DRUM", "CAM_ROTARY_FINISH": "SCROLL",
    }
    return {k: cp[v] for k, v in was.items() if v in cp}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--sheet")
    ap.add_argument("--compare", action="store_true")
    a = ap.parse_args()
    if a.sheet:
        sheet(a.sheet, a.compare)
        return 0
    ttf = build_font()
    hdr = build_header(ttf)
    if a.check:
        stale = [p for p, want in ((TTF, ttf), (HEADER, hdr.encode()))
                 if not os.path.exists(p) or open(p, "rb").read() != want]
        for p in stale:
            print(f"out of date: {os.path.relpath(p, ROOT)} -- run "
                  "tools/icons/build_icon_font.py", file=sys.stderr)
        return 1 if stale else 0
    open(TTF, "wb").write(ttf)
    open(HEADER, "w").write(hdr)
    print(f"{len(ICONS)} icons -> {os.path.relpath(TTF, ROOT)}, "
          f"{os.path.relpath(HEADER, ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
