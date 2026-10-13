# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The L loop (0,0)-(40,0)-(40,10)-(10,10)-(10,30)-(0,30) made a wall t = 2
thick, t/2 to each side: the outer boundary is the loop offset out by 1,
an arc of radius 1 about each of the five convex corners and sharp at the
reflex one; the inner is the loop offset in by 1, sharp at the convex
corners and an arc of radius 1 about the reflex corner (10, 10). The wall's
section is the outer region less the inner, each by Green's theorem over
its lines and arcs, extruded 15."""
import geom

Q = geom.PI / 2


def expected():
    outer = [
        geom.Line((0, -1), (40, -1)), geom.Arc((40, 0), 1, -Q, 0),
        geom.Line((41, 0), (41, 10)), geom.Arc((40, 10), 1, 0, Q),
        geom.Line((40, 11), (11, 11)), geom.Line((11, 11), (11, 30)),
        geom.Arc((10, 30), 1, 0, Q), geom.Line((10, 31), (0, 31)),
        geom.Arc((0, 30), 1, Q, 2 * Q), geom.Line((-1, 30), (-1, 0)),
        geom.Arc((0, 0), 1, 2 * Q, 3 * Q)]
    inner = [
        geom.Line((1, 1), (39, 1)), geom.Line((39, 1), (39, 9)),
        geom.Line((39, 9), (10, 9)), geom.Arc((10, 10), 1, -Q, -2 * Q),
        geom.Line((9, 10), (9, 29)), geom.Line((9, 29), (1, 29)),
        geom.Line((1, 29), (1, 1))]
    region = geom.Region(outer, [inner])
    return geom.extrude(region, 0, 15).expected()
