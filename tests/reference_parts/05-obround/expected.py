# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""An obround of radius r = 6 with centres L = 40 apart, (0, 0) and
(40, 0), extruded h = 10: A2 = 2rL + pi r^2, perimeter 2L + 2 pi r,
V = A2 h, A = 2 A2 + P h; the centroid at (L/2, 0, h/2); six faces."""
import geom


def expected():
    r, L = 6.0, 40.0
    region = geom.Region([
        geom.Line((0, -r), (L, -r)),
        geom.Arc((L, 0), r, -geom.PI / 2, geom.PI / 2),
        geom.Line((L, r), (0, r)),
        geom.Arc((0, 0), r, geom.PI / 2, 3 * geom.PI / 2)])
    return geom.extrude(region, 0, 10).expected(faces=6)
