# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""An annular sector, r 4..12, h = 8, swept theta = 3 pi / 2 from +x
towards +y: V = theta/2 (R^2 - r^2) h, the centroid from the sector's first
moments (R^3 - r^3)/3 h (sin theta, 1 - cos theta) over V, area theta
(R + r) h + theta (R^2 - r^2) + 2 (R - r) h. The sweep passes +y, -x and
-y, so the box is +-12 in x and y; six faces."""
import geom


def expected():
    s = geom.revolve_z(geom.Region(geom.rectangle(4, 0, 12, 8)),
                       3 * geom.PI / 2)
    return s.with_bbox((-12, -12, 0), (12, 12, 8)).expected(faces=6)
