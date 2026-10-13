# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The box 50 x 40 x 30 less the cavity 46 x 36 x 28 (walls and floor t = 2
inside it, open at the top): V = abc - (a - 2t)(b - 2t)(c - t); the area
is the outer five faces, the rim (ab less the opening), the cavity's floor
and its four walls; eleven faces."""
import geom


def expected():
    outer = geom.box(-25, -20, 0, 25, 20, 30)
    cavity = geom.box(-23, -18, 2, 23, 18, 30)
    area = (outer.area - 46 * 36) + 46 * 36 + 2 * (46 + 36) * 28
    return outer.minus(cavity, area=area).expected(faces=11)
