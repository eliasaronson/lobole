# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The rectangle 60 x 40 less a disc r = 6 centred at (20, 15), extruded
20: V = (ab - pi r^2) c, A = 2(ab - pi r^2) + (2a + 2b + 2 pi r) c, the
centroid the region's (Green's theorem) at half height; seven faces."""
import geom


def expected():
    region = geom.Region(geom.rectangle(0, 0, 60, 40),
                         [geom.circle((20, 15), 6)])
    return geom.extrude(region, 0, 20).expected(faces=7)
