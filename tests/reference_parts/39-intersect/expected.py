# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A ball R = 25 less six disjoint caps of height h = 5 (the cube's faces at
+-20): V = 4/3 pi R^3 - 6 pi h^2 (3R - h)/3, the centroid at the origin by
symmetry; the area is the ball's less six caps' 2 pi R h plus six flat discs
of radius sqrt(h (2R - h)) = 15 (which stay inside the 40 x 40 faces); seven
faces."""
import geom


def expected():
    R, h = 25.0, 5.0
    v, _, curved, flat = geom.spherical_cap(R, h)
    ball = geom.sphere(0, 0, 0, R)
    area = ball.area - 6 * curved + 6 * flat
    s = geom.Solid(ball.volume - 6 * v, (0, 0, 0), area,
                   ((-20, -20, -20), (20, 20, 20)))
    return s.expected(faces=7)
