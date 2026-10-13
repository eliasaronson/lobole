# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A cylinder: V = pi r^2 h, A = 2 pi r^2 + 2 pi r h for r = 15, h = 50,
its axis the z axis from z = 0; three faces (two discs and the side)."""
import geom


def expected():
    return geom.cylinder_z(0, 0, 0, 15, 50).expected(faces=3)
