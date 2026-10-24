# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The rectangle 60 x 40 less a disc r = 6 centred at (40, 25), extruded
20; seven faces."""
import geom


def expected():
    region = geom.Region(geom.rectangle(0, 0, 60, 40),
                         [geom.circle((40, 25), 6)])
    return geom.extrude(region, 0, 20).expected(faces=7)
