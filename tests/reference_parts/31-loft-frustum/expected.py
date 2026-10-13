# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A square frustum, 40 at z = 0 to 20 at z = 30: V = h/3 (A0 + A1
+ sqrt(A0 A1)), the centroid h (A0 + 2 sqrt(A0 A1) + 3 A1) /
(4 (A0 + sqrt(A0 A1) + A1)) up, four trapezoid sides (40 + 20)/2 by the
slant sqrt(30^2 + 10^2); six faces."""
import math

import geom


def expected():
    h, a0, a1 = 30.0, 1600.0, 400.0
    area = a0 + a1 + 4 * 30 * math.hypot(30, 10)
    s = geom.Solid(geom.frustum_volume(a0, a1, h),
                   (0, 0, geom.frustum_centroid_height(a0, a1, h)), area,
                   ((-20, -20, 0), (20, 20, h)))
    return s.expected(faces=6)
