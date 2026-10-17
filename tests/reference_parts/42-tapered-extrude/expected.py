# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A square frustum: the 20 mm square at z = 0, every wall leaning in 15
degrees over 10 mm, so the top square's side is b = 20 - 2 * 10 tan 15.
V and the centroid's height are the similar-section frustum's; the area is
the two squares and four trapezoids (20 + b)/2 wide, each as tall as its
slant, sqrt(10^2 + (10 tan 15)^2). The box is the base's, z 0..10; six
faces."""
import math

import geom


def expected():
    h, a = 10.0, 20.0
    k = math.tan(math.radians(15.0))
    b = a - 2 * h * k
    v = geom.frustum_volume(a * a, b * b, h)
    zc = geom.frustum_centroid_height(a * a, b * b, h)
    slant = math.hypot(h, h * k)
    area = a * a + b * b + 4 * (a + b) / 2 * slant
    s = geom.Solid(v, (0, 0, zc), area, ((-10, -10, 0), (10, 10, h)))
    return s.expected(faces=6)
