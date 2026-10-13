# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A cylinder R = 4, L = 20 less the ISO 68-1 basic groove for M8 (P = 1.25,
H = sqrt 3 / 2 P): a trapezoid 7P/8 wide at the major diameter and P/4 at
the minor, 5H/8 deep, of area A = 45 P H / 128 and centroid radius
R - 11/27 (5H/8). A screw motion carries it round: the groove removes
2 pi A rbar per pitch, so L 2 pi A rbar / P over the length (the set is
invariant under the screw, so a slab's volume is linear in its length)."""
import math

import geom


def expected():
    R, L, P = 4.0, 20.0, 1.25
    H = math.sqrt(3) / 2 * P
    h = 5 * H / 8
    a = (7 * P / 8 + P / 4) / 2 * h
    rbar = R - 11 * h / 27
    groove = L * 2 * geom.PI * a * rbar / P
    rod = geom.cylinder_z(0, 0, 0, R, L)
    cut = geom.Solid(groove, (0, 0, L / 2), 0, rod.bbox)
    return rod.minus(cut).expected()
