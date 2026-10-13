# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A tube r = 1 along a helix R = 10, pitch p = 5, three turns: its length
L = 3 sqrt((2 pi R)^2 + p^2), V = pi r^2 L and A = 2 pi r L + 2 pi r^2 (a
disc swept normal to a curve whose curvature radius exceeds r; the inside
of each bend is compressed exactly as the outside is stretched). Over
whole turns the centroid is on the axis at half height. The box: +-(R + r)
in x and y; in z the end discs reach r times the binormal's z component,
2 pi R / sqrt((2 pi R)^2 + p^2), below the start and above the end."""
import math

import geom


def expected():
    r, R, p = 1.0, 10.0, 5.0
    turn = math.sqrt((2 * geom.PI * R) ** 2 + p * p)
    L = 3 * turn
    bz = r * 2 * geom.PI * R / turn
    s = geom.Solid(geom.PI * r * r * L, (0, 0, 7.5),
                   2 * geom.PI * r * L + 2 * geom.PI * r * r,
                   ((-11, -11, -bz), (11, 11, 15 + bz)))
    return s.expected()
