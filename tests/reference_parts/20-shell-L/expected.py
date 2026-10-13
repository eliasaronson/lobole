# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The L prism (section (0,0)-(60,0)-(60,8)-(8,8)-(8,40)-(0,40), 20 high)
less its cavity: the section eroded by t = 2, sharp at the convex corners
and an arc of radius t about the reflex corner (8, 8), so its area is
352 + 4 - pi, from z = 2 to the open top. Area: the outer faces less the top,
the rim (the L less the opening), the cavity floor and its walls (the
eroded perimeter times 18)."""
import geom

Q = geom.PI / 2


def expected():
    l_region = geom.Region(geom.polygon(
        [(0, 0), (60, 0), (60, 8), (8, 8), (8, 40), (0, 40)]))
    hollow = geom.Region([
        geom.Line((2, 2), (58, 2)), geom.Line((58, 2), (58, 6)),
        geom.Line((58, 6), (8, 6)), geom.Arc((8, 8), 2, -Q, -2 * Q),
        geom.Line((6, 8), (6, 38)), geom.Line((6, 38), (2, 38)),
        geom.Line((2, 38), (2, 2))])
    outer = geom.extrude(l_region, 0, 20)
    cavity = geom.extrude(hollow, 2, 18)
    area = outer.area + hollow.perimeter() * 18
    return outer.minus(cavity, area=area).expected()
