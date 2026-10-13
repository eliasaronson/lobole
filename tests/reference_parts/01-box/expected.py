# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A box: V = abc and A = 2(ab + bc + ca) for a = 40, b = 30, c = 20,
centred on the origin in x and y and standing on z = 0, six faces."""
import geom


def expected():
    return geom.box(-20, -15, 0, 20, 15, 20).expected(faces=6)
