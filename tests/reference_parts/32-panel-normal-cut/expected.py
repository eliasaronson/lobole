# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A 100 x 60 panel t = 1.5 thick (aluminium 1.5 mm) with a hole r = 3 at
(50, 30) cut square through it: V = (6000 - 9 pi) t exactly, the area two
faces less the hole, the outline's edges and the bore; seven faces."""
import geom


def expected():
    t = 1.5
    region = geom.Region(geom.rectangle(0, 0, 100, 60),
                         [geom.circle((50, 30), 3)])
    return geom.extrude(region, 0, t).expected(faces=7)
