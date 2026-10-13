# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The box 60 x 30 x 20 (x +-30) cut at x = 0 into two bodies, the top edge
of the far end (x = 30, along y, 30 long) rounded r = 2: the total less the
spandrel (4 - pi) r^2 / 4 times 30, whose centroid sits r (10 - 3 pi) /
(3 (4 - pi)) in from the edge on each side; the area trades 2r of flat for
pi r / 2 of round over the edge and loses a spandrel off each end face, and
the cut adds two 30 x 20 faces."""
import geom


def expected():
    pi, r = geom.PI, 2.0
    box = geom.box(-30, -15, 0, 30, 15, 20)
    spandrel = (4 - pi) * r * r / 4
    inset = r * (10 - 3 * pi) / (3 * (4 - pi))
    cut = geom.Solid(spandrel * 30, (30 - inset, 0, 20 - inset), 0, box.bbox)
    area = (box.area + 2 * 600 + (pi * r / 2 - 2 * r) * 30
            - 2 * spandrel)
    return box.minus(cut, area=area).expected(bodies=2)
