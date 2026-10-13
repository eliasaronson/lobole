# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Cavalieri: every section is the same 20 x 20 square turned about the z
axis, so V = A2 h = 400 x 30 and the centroid is on the axis at h/2. Over a
quarter turn each corner (at radius 10 sqrt 2) passes through an axis
direction, so the box is +-10 sqrt 2 in x and y, z 0..30."""
import math

import geom


def expected():
    e = 10 * math.sqrt(2)
    s = geom.Solid(400 * 30, (0, 0, 15), 0, ((-e, -e, 0), (e, e, 30)))
    return s.expected()
