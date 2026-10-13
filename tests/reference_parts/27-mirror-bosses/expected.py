# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A block 80 x 40 x 10 and two bosses r = 6, h = 15 standing on it at
(+-25, 5): V = abc + 2 pi r^2 h, the centroid pulled to y > 0 and up by the
bosses and kept at x = 0 by the mirror; the area gains the two boss sides
(each top replaces the footprint it stands on); ten faces."""
import geom


def expected():
    block = geom.box(-40, -20, 0, 40, 20, 10)
    s = block.plus(geom.cylinder_z(25, 5, 10, 6, 15)).plus(
        geom.cylinder_z(-25, 5, 10, 6, 15))
    s.with_area(block.area + 2 * 2 * geom.PI * 6 * 15)
    return s.expected(faces=10)
