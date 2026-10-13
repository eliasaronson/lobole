# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""An annulus R = 50, r = 15, h = 10 less six bores r = 5 on a 40 radius:
V = pi (R^2 - r^2) h - 6 pi 5^2 h; six-fold symmetry keeps the centroid on
the axis at h/2 (each bore placed at 60 degree steps from +x); the area
loses twelve discs and gains six bores; ten faces."""
import math

import geom


def expected():
    pi = geom.PI
    flange = geom.cylinder_z(0, 0, 0, 50, 10).minus(
        geom.cylinder_z(0, 0, 0, 15, 10))
    s = flange
    for k in range(6):
        a = k * pi / 3
        s = s.minus(geom.cylinder_z(40 * math.cos(a), 40 * math.sin(a), 0,
                                    5, 10))
    area = (2 * pi * (50 ** 2 - 15 ** 2) + 2 * pi * (50 + 15) * 10
            + 6 * (2 * pi * 5 * 10 - 2 * pi * 25))
    return s.with_area(area).expected(faces=10)
