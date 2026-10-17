# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The 20 mm cube less a teardrop prism along x through it: in (y, z) the
round r 3 about (10, 10) from 135 degrees round the bottom to 45 (three
quarters of it), and two lines from its tangent points to the point
3 sqrt 2 above the centre (+z, the part printed as modelled). Green's
theorem gives the section's area and centroid; the prism is that region
extruded 20 along x. The cube loses the section twice, at its ends, and
gains the prism's walls, perimeter x 20. Six faces and three of the hole's
(the round and the two flanks)."""
import math

import geom


def expected():
    r, c = 3.0, (10.0, 10.0)
    t = (c[0] + r / math.sqrt(2), c[1] + r / math.sqrt(2))
    u = (c[0] - r / math.sqrt(2), c[1] + r / math.sqrt(2))
    apex = (c[0], c[1] + r * math.sqrt(2))
    loop = [geom.Arc(c, r, math.radians(135), math.radians(405)),
            geom.Line(t, apex), geom.Line(apex, u)]
    region = geom.Region(loop)
    # The region lies in (u, v) = (y, z); extruded along w = x.
    hole = geom.extrude(region, 0.0, 20.0).remapped((2, 0, 1))
    cube = geom.box(0, 0, 0, 20, 20, 20)
    area = cube.area - 2 * region.area() + region.perimeter() * 20
    return cube.minus(hole, area=area).expected(faces=9)
