# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The plate 100 x 40 x 10 less four through bores of ISO 273's normal
clearance for M6, 6.6 mm, at x = -40, -20, 0, 20: the centroid moves off the
plate's centre by the bores' first moment, so it proves the spacing. The
area loses eight discs and gains four bores; ten faces."""
import geom


def expected():
    plate = geom.box(-50, -20, 0, 50, 20, 10)
    r = 3.3
    s = plate
    for x in (-40, -20, 0, 20):
        s = s.minus(geom.cylinder_z(x, 0, 0, r, 10))
    s.with_area(plate.area + 4 * (2 * geom.PI * r * 10 - 2 * geom.PI * r * r))
    return s.expected(faces=10)
