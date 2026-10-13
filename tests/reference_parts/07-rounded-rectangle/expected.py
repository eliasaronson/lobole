# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A 50 x 30 rectangle with each corner rounded r = 5: A2 = wh - (4 - pi) r^2,
perimeter 2(w + h) - 8r + 2 pi r, extruded 8; centred at (25, 15); ten
faces (top, bottom, four flats, four rounds)."""
import geom


def expected():
    w, h, r = 50.0, 30.0, 5.0
    q = geom.PI / 2
    region = geom.Region([
        geom.Line((r, 0), (w - r, 0)), geom.Arc((w - r, r), r, -q, 0),
        geom.Line((w, r), (w, h - r)), geom.Arc((w - r, h - r), r, 0, q),
        geom.Line((w - r, h), (r, h)), geom.Arc((r, h - r), r, q, 2 * q),
        geom.Line((0, h - r), (0, r)), geom.Arc((r, r), r, 2 * q, 3 * q)])
    return geom.extrude(region, 0, 8).expected(faces=10)
