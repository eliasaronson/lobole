# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A ball: V = 4/3 pi r^3, A = 4 pi r^2 for r = 12, centred on the origin
(the primitive's centre lies on the plane it is placed on)."""
import geom


def expected():
    return geom.sphere(0, 0, 0, 12).expected()
