# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The panel 100 x 60, t = 1.5, its edge x = 100 bent up 90 degrees at
R = 3 (pinned) with the outer mold line on the edge: the panel stops
R + t short of it, the bend is a quarter annulus R..R + t about
(100 - R - t, 4.5) in (x, z), and the wall rises 15 past the bend. The
L section by Green's theorem over its lines and two arcs, extruded 60
along y."""
import geom

R, T = 3.0, 1.5
Q = geom.PI / 2


def expected():
    x0 = 100 - R - T
    c = (x0, T + R)
    region = geom.Region([
        geom.Line((0, 0), (x0, 0)), geom.Arc(c, R + T, -Q, 0),
        geom.Line((100, c[1]), (100, c[1] + 15)),
        geom.Line((100, c[1] + 15), (100 - T, c[1] + 15)),
        geom.Line((100 - T, c[1] + 15), (100 - T, c[1])),
        geom.Arc(c, R, 0, -Q), geom.Line((x0, T), (0, T)),
        geom.Line((0, T), (0, 0))])
    return geom.extrude(region, 0, 60).remapped((0, 2, 1)).expected()
