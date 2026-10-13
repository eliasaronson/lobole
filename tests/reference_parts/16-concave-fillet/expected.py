# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The L section (0,0)-(60,0)-(60,8)-(8,8)-(8,40)-(0,40) in (x, z) with its
reflex corner at (8, 8) rounded r = 4 (an arc about (12, 12)), extruded 30
along -y: the fillet adds the spandrel r^2 (1 - pi/4) times the width and
trades 2r of flat for pi r / 2 of round; section by Green's theorem; nine
faces."""
import geom

Q = geom.PI / 2


def expected():
    region = geom.Region([
        geom.Line((0, 0), (60, 0)), geom.Line((60, 0), (60, 8)),
        geom.Line((60, 8), (12, 8)), geom.Arc((12, 12), 4, -Q, -2 * Q),
        geom.Line((8, 12), (8, 40)), geom.Line((8, 40), (0, 40)),
        geom.Line((0, 40), (0, 0))])
    s = geom.extrude(region, 0, 30).remapped((0, 2, 1), (1, -1, 1))
    return s.expected(faces=9)
