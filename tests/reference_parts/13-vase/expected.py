# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Pappus-Guldin on the (r, z) profile (0,0)-(20,0)-(20,30), an arc about
(20, 40) of radius 10 bulging out to r = 30, (20,50)-(12,70)-(0,70) and
back down the axis: V = 2 pi int r dA and A = 2 pi loop(r ds), both by
Green's theorem over the edges; the centroid on the axis at
int r z dA / int r dA. The box is +-30 in x and y, z 0..70; five faces."""
import geom


def expected():
    region = geom.Region([
        geom.Line((0, 0), (20, 0)), geom.Line((20, 0), (20, 30)),
        geom.Arc((20, 40), 10, -geom.PI / 2, geom.PI / 2),
        geom.Line((20, 50), (12, 70)), geom.Line((12, 70), (0, 70)),
        geom.Line((0, 70), (0, 0))])
    s = geom.revolve_z(region)
    return s.with_bbox((-30, -30, 0), (30, 30, 70)).expected(faces=5)
