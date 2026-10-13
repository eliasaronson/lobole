# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A 30 x 20 rectangle on z = 0 extruded 10 along the plane normal (+z) and
25 against it: the box 0..30, 0..20, -25..10, whose centroid at z = -7.5
catches a swapped side; six faces."""
import geom


def expected():
    return geom.box(0, 0, -25, 30, 20, 10).expected(faces=6)
