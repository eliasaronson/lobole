# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The box 40 x 30 x 20 less a hemisphere r = 8 centred on its top face at
(0, 0, 20): V = abc - 2/3 pi r^3, the hemisphere's centroid 3r/8 below the
face; the area loses the disc pi r^2 and gains the bowl 2 pi r^2; seven
faces."""
import geom


def expected():
    pi, r = geom.PI, 8.0
    box = geom.box(-20, -15, 0, 20, 15, 20)
    bowl = geom.Solid(2 / 3 * pi * r ** 3, (0, 0, 20 - 3 * r / 8), 0,
                      box.bbox)
    return box.minus(bowl, area=box.area + pi * r * r).expected(faces=7)
