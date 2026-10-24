# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A prism on the XZ trapezoid (0, 0), (60, 0), (60, 20 + 30 t),
(0, 20 - 30 t), t = tan 10 degrees, 40 deep in Y. Its height
h(x) = 20 + (x - 30) t averages 20, so V = 60 * 20 * 40; the centroid is
x = 30 + 15 t (from the integral of x h(x)), y = 20,
z = 10 + 7.5 t^2 (half the integral of h(x)^2, over the integral of h);
the top is 60 / cos 10 by 40, the ends 40 by 20 -+ 30 t, the two
trapezoids 60 x 20 each; six faces."""
import math

import geom


def expected():
    t = math.tan(math.radians(10))
    v = 60 * 20 * 40
    com = (30 + 15 * t, 20, 10 + 7.5 * t * t)
    area = (60 * 40 + 60 / math.cos(math.radians(10)) * 40 +
            40 * (20 - 30 * t) + 40 * (20 + 30 * t) + 2 * 60 * 20)
    s = geom.Solid(v, com, area, ((0, 0, 0), (60, 40, 20 + 30 * t)))
    return s.expected(faces=6)
