# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The plate with no hole: the rectangle 60 x 40 extruded 20; six
faces."""
import geom


def expected():
    return geom.extrude(geom.Region(geom.rectangle(0, 0, 60, 40), []), 0,
                        20).expected(faces=6)
