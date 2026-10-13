# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The blank of a 100 x 60 panel, t = 1.5, with 60 degree flanges of 15 on
its edges x = 100 and y = 60 (outer mold line on each edge, R = 3, K = 0.41
pinned): each side's flat length is L - (R + t) tan(theta/2)
+ theta (R + K t) + 15, and a blank is one gauge thick, its z span exactly t.
Only the body count and the box are fixed (the mitre and the relief cut
the corner by amounts with no closed form)."""
import math

import geom

R, T, K = 3.0, 1.5, 0.41


def expected():
    th = math.radians(60)
    grow = -(R + T) * math.tan(th / 2) + th * (R + K * T) + 15
    s = geom.box(0, 0, 0, 100 + grow, 60 + grow, T)
    return s.expected()
