# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Pappus on the (r, z) section r 0..20, z 0..30 with the corner (20, 30)
rounded r = 3 about (17, 27): the fillet removes the spandrel
r^2 (1 - pi/4), whose centroid sits r (10 - 3 pi) / (3 (4 - pi)) in from
the corner, swept round; the round's area is 2 pi times its arc's length
times its arc centroid's radius R - r + 2r/pi. Computed by Green's theorem
over the section's edges; four faces."""
import geom

Q = geom.PI / 2


def expected():
    region = geom.Region([
        geom.Line((0, 0), (20, 0)), geom.Line((20, 0), (20, 27)),
        geom.Arc((17, 27), 3, 0, Q), geom.Line((17, 30), (0, 30)),
        geom.Line((0, 30), (0, 0))])
    s = geom.revolve_z(region)
    return s.with_bbox((-20, -20, 0), (20, 20, 30)).expected(faces=4)
