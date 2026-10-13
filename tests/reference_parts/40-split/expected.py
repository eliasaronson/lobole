# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The box 60 x 30 x 20 (x +-30) cut by the plane x = 10 into two bodies,
40 and 20 long: the volume and the centroid are the box's, the area gains
the two 30 x 20 cut faces; twelve faces."""
import geom


def expected():
    box = geom.box(-30, -15, 0, 30, 15, 20)
    return box.with_area(box.area + 2 * 600).expected(bodies=2, faces=12)
