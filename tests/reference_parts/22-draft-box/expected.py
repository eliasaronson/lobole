# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A square frustum: each side leans in by h tan 5 degrees, so the top is
s1 = 40 - 2 h tan 5 for h = 30; V = h/3 (A0 + A1 + sqrt(A0 A1)), the
centroid h (A0 + 2 sqrt(A0 A1) + 3 A1) / (4 (A0 + sqrt(A0 A1) + A1)) up;
the sides are trapezoids (40 + s1)/2 by the slant h / cos 5; six faces."""
import math

import geom


def expected():
    h, s0 = 30.0, 40.0
    t = math.radians(5)
    s1 = s0 - 2 * h * math.tan(t)
    a0, a1 = s0 * s0, s1 * s1
    v = geom.frustum_volume(a0, a1, h)
    z = geom.frustum_centroid_height(a0, a1, h)
    area = a0 + a1 + 4 * (s0 + s1) / 2 * h / math.cos(t)
    s = geom.Solid(v, (0, 0, z), area, ((-20, -20, 0), (20, 20, h)))
    return s.expected(faces=6)
