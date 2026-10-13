# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A box a x b x c = 40 x 30 x 20 whose top edges are bevelled d = 2 at 45
degrees: the box below z = c - d, ab(c - d), plus a slab whose section at
height u above it is (a - 2u)(b - 2u), so its volume is
int_0^d (a - 2u)(b - 2u) du = abd - (a + b)d^2 + 4d^3/3 and its moment
int (a - 2u)(b - 2u)(c - d + u) du. Area: the bottom, the shrunken top
(a - 2d)(b - 2d), the sides 2(a + b)(c - d), and four trapezoid bevels of
slant d sqrt 2; ten faces."""
import math

import geom


def expected():
    a, b, c, d = 40.0, 30.0, 20.0, 2.0
    z0 = c - d
    i0 = a * b * d - (a + b) * d * d + 4 * d ** 3 / 3
    i1 = a * b * d * d / 2 - 2 * (a + b) * d ** 3 / 3 + d ** 4
    lower = geom.box(-a / 2, -b / 2, 0, a / 2, b / 2, z0)
    slab = geom.Solid(i0, (0, 0, z0 + i1 / i0), 0, lower.bbox)
    area = (a * b + (a - 2 * d) * (b - 2 * d) + 2 * (a + b) * z0
            + 2 * (2 * a - 2 * d) / 2 * d * math.sqrt(2)
            + 2 * (2 * b - 2 * d) / 2 * d * math.sqrt(2))
    s = lower.plus(slab, area=area).with_bbox((-a / 2, -b / 2, 0),
                                              (a / 2, b / 2, c))
    return s.expected(faces=10)
