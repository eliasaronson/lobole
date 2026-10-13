# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A ring torus R = 20, r = 4 about the z axis: V = 2 pi^2 R r^2,
A = 4 pi^2 R r, centred on the origin; one face."""
import geom


def expected():
    return geom.torus_z(20, 4).expected(faces=1)
