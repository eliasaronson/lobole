# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A tube r = 3 along (0,0,0)-(0,0,30), a quarter circle R = 20 about
(20, 0, 30) to (20, 0, 50), then to (50, 0, 50): two cylinders (30 long)
and a quarter torus, V = pi r^2 (60 + pi R / 2), A = 2 pi r (60 + pi R / 2)
+ 2 pi r^2. The quarter torus's centroid by Pappus in moments (its disc
swept a quarter turn about the bend's axis), the cylinders' at their
middles; the box runs from the first end face to the last."""
import geom


def expected():
    r, R = 3.0, 20.0
    up = geom.cylinder_z(0, 0, 0, r, 30)
    bend = geom.revolve_z(geom.Region(geom.circle((R, 0), r)), geom.PI / 2)
    cx, cy, _ = bend.centroid
    # Local +x is world -x about the bend's centre, local +y is world +z.
    bend = geom.Solid(bend.volume, (20 - cx, 0, 30 + cy), bend.area,
                      ((0, 0, 0), (0, 0, 0)))
    across = geom.Solid(geom.PI * r * r * 30, (35, 0, 50), 0,
                        ((0, 0, 0), (0, 0, 0)))
    length = 60 + geom.PI * R / 2
    area = 2 * geom.PI * r * length + 2 * geom.PI * r * r
    s = up.plus(bend).plus(across, area=area)
    return s.with_bbox((-3, -3, 0), (50, 3, 53)).expected()
