# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The panel 100 x 60, t = 1.5, its edge x = 100 folded back on itself:
the return lies a tenth of t (0.15) off the panel, so the 180 degree bend
has inside radius 0.075 and outside 1.575 about (100 - 1.575, 1.575) in
(x, z), the outline kept (the bend's outside on the edge), and the return
runs 10 back from the bend over the panel. The section by Green's theorem
over its lines and two half circles, extruded 60 along y."""
import geom

T = 1.5
Q = geom.PI / 2


def expected():
    ri = T / 20
    ro = ri + T
    cx, cz = 100 - ro, T + ri
    region = geom.Region([
        geom.Line((0, 0), (cx, 0)), geom.Arc((cx, cz), ro, -Q, Q),
        geom.Line((cx, cz + ro), (cx - 10, cz + ro)),
        geom.Line((cx - 10, cz + ro), (cx - 10, cz + ri)),
        geom.Line((cx - 10, cz + ri), (cx, cz + ri)),
        geom.Arc((cx, cz), ri, Q, -Q), geom.Line((cx, T), (0, T)),
        geom.Line((0, T), (0, 0))])
    return geom.extrude(region, 0, 60).remapped((0, 2, 1)).expected()
