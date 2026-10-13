# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A block 100 x 30 x 10 and three bosses r = 5, h = 10 on it at x = -30, 0,
30, the last with its top rim rounded r = 2: that boss by Pappus on its
(r, z) section (as 17-cylinder-rim-fillet), the other two cylinders. Each
boss's footprint replaces block top; thirteen faces."""
import geom

Q = geom.PI / 2


def expected():
    block = geom.box(-50, -15, 0, 50, 15, 10)
    plain = [geom.cylinder_z(x, 0, 10, 5, 10) for x in (-30, 0)]
    rounded = geom.revolve_z(geom.Region([
        geom.Line((0, 10), (5, 10)), geom.Line((5, 10), (5, 18)),
        geom.Arc((3, 18), 2, 0, Q), geom.Line((3, 20), (0, 20)),
        geom.Line((0, 20), (0, 10))])).moved(30, 0, 0)
    pi = geom.PI
    area = (block.area + sum(c.area - 2 * pi * 25 for c in plain)
            + rounded.area - 2 * pi * 25)
    s = block.plus(plain[0]).plus(plain[1]).plus(rounded, area=area)
    return s.with_bbox((-50, -15, 0), (50, 15, 20)).expected(faces=13)
