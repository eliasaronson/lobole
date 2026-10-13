# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The plate 80 x 40 x 20 (top at z = 20) less three cavities of
revolution: at x = -25 a counterbore, cylinders r 5.5 for 6.5 and r 3.3
through the rest; at x = 0 a countersink, a cone frustum r 6.5 at the top
to r 3.3 at 3.2 deep (90 degrees included) on a bore r 3.3 through; at
x = 25 a blind bore r 4 to the full-diameter depth 12 with a cone of height
r / tan 59 degrees under it (a 118 degree drill point, depth as
docs/modeling/holes-and-threads.md defines it). Each by Pappus on its
(r, z) section; the plate's area loses each mouth and through exit and gains
each cavity's walls; 6 + 3 + 2 + 2 faces."""
import math

import geom


def cavity(points, x):
    s = geom.revolve_z(geom.Region(geom.polygon(points)))
    return s.moved(x, 0, 0)


def expected():
    plate = geom.box(-40, -20, 0, 40, 20, 20)
    tip = 4 / math.tan(math.radians(59))
    cb = cavity([(0, 0), (3.3, 0), (3.3, 13.5), (5.5, 13.5), (5.5, 20),
                 (0, 20)], -25)
    cs = cavity([(0, 0), (3.3, 0), (3.3, 16.8), (6.5, 20), (0, 20)], 0)
    bl = cavity([(0, 8 - tip), (4, 8), (4, 20), (0, 20)], 25)
    pi = geom.PI
    # Each cavity's own area counts its flat ends; the plate loses those.
    area = (plate.area
            + cb.area - 2 * pi * (3.3 ** 2 + 5.5 ** 2)
            + cs.area - 2 * pi * (3.3 ** 2 + 6.5 ** 2)
            + bl.area - 2 * pi * 4 ** 2)
    s = plate.minus(cb).minus(cs).minus(bl, area=area)
    return s.expected(faces=13)
