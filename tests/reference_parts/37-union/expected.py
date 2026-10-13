# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The box 40 x 30 x 20 (x +-20) and a cylinder r = 10, h = 30 on the axis
x = 20: the union is the box, the half cylinder outside it (x > 20, z 0..20,
centroid 4r / (3 pi) out) and the whole cylinder above it (z 20..30). The
area loses the box's covered half disc on top and the 20 x 20 strip of its
x = 20 side, and gains the outer half's side and bottom and the upper
cylinder's side and top."""
import geom


def expected():
    pi, r = geom.PI, 10.0
    box = geom.box(-20, -15, 0, 20, 15, 20)
    half = geom.Solid(pi * r * r / 2 * 20, (20 + 4 * r / (3 * pi), 0, 10), 0,
                      ((20, -r, 0), (20 + r, r, 20)))
    above = geom.cylinder_z(20, 0, 20, r, 10)
    area = box.area - 400 + 40 * pi * r + pi * r * r
    return box.plus(half).plus(above, area=area).expected()
