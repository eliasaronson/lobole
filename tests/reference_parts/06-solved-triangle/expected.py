# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A triangle with A = (0, 0), B = (50, 0), the angle at A 60 degrees and
BC = 45: by the law of cosines AC = b solves b^2 - 50 b + (50^2 - 45^2) = 0,
b = 25 + sqrt(150) (the root nearest the drawn apex, b ~ 37.2), so
C = b (cos 60, sin 60). Extruded 10: V = A2 h, the centroid the vertex
mean at h/2; five faces."""
import math

import geom


def expected():
    b = 25 + math.sqrt(150)
    c = (b * math.cos(math.radians(60)), b * math.sin(math.radians(60)))
    region = geom.Region(geom.polygon([(0, 0), (50, 0), c]))
    return geom.extrude(region, 0, 10).expected(faces=5)
