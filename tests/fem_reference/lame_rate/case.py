# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The rate a tet10 converges at, on a problem whose 3-D elasticity
solution is known exactly: Lame's cylinder in plane strain (lame_common).
The bore's radial displacement is solved at four element sizes, each half
the last; its error against the closed form must fall every time, at a
rate above the floor. A rate is what a single-mesh tolerance cannot see: an
element that is consistently a little wrong (a Gauss weight, a shape
function) stops converging, while its answer on one mesh still looks
close."""

import math

import common as c
import lame_common as lame

SIZES = (4.0, 2.0, 1.0, 0.5)


def run(d, solver):
    part = lame.build(d, "steel")
    want = lame.closed_form(c.STEEL)
    errs, ur = [], None
    nodes = None
    for h in SIZES:
        ur, nodes = lame.solve(d, part, solver, h)
        errs.append(abs(ur - want) / want)
    rates = [math.log(errs[i] / errs[i + 1]) / math.log(SIZES[i] /
                                                         SIZES[i + 1])
             for i in range(len(errs) - 1)]
    monotone = all(errs[i + 1] < errs[i] for i in range(len(errs) - 1))
    # The first halving, from 2.5 elements through the wall, is before the
    # asymptotic range; the rate is the least of the later ones.
    return {"u_r_bore": ur, "rate": min(rates[1:]) if monotone else 0.0,
            "_errors": errs, "_nodes": nodes}
