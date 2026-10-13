# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The plate 60 x 40 x 10 less the cavity of revolution whose (r, z)
section is (0,0)-(5,0)-(5,9)-(6,10)-(0,10): a bore r = 5 and a 45 degree
cone frustum from r = 5 at z = 9 to r = 6 at z = 10 (the chamfer, d = 1).
The plate's area loses the two openings (pi 6^2 on top, pi 5^2 below) and
gains the bore and the cone; eight faces."""
import geom


def expected():
    cavity = geom.revolve_z(geom.Region(geom.polygon(
        [(0, 0), (5, 0), (5, 9), (6, 10), (0, 10)])))
    plate = geom.box(-30, -20, 0, 30, 20, 10)
    openings = geom.PI * 36 + geom.PI * 25
    area = plate.area - 2 * openings + cavity.area
    return plate.minus(cavity, area=area).expected(faces=8)
