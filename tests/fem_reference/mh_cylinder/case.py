# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""MacNeal and Harder's thick-walled cylinder: bore 3.0, outside 9.0, 1.0
long, E 1000, unit pressure in the bore, plane strain, at nu 0.49, 0.499
and 0.4999 -- the nearly incompressible check, past lame_tpu's 0.48. A
quarter on rollers on its two symmetry planes and both ends, as lame_common
builds Lame's; the material is the study's own, since no library row has
such a ratio (docs/simulation/README.md#a-study-states-its-own-material).
Lengths are read as mm and E as MPa, so the answers are the paper's
numbers. The bore's radial displacement is the mean over the face, from
probe_study's rigid fit, as lame_common reads it."""

import math

import common as c

A, B, H, P = 3.0, 9.0, 1.0, 1.0
SIZE = 0.5
RATIOS = (("u_r_nu_049", 0.49), ("u_r_nu_0499", 0.499),
          ("u_r_nu_04999", 0.4999))


def run(d, solver):
    ring = c.boolean(d, c.cylinder(d, B, H, "Outer"),
                     c.cylinder(d, A, H, "Bore"), "subtract", "Ring")
    quad = c.box(d, (0, 0, 0), (2 * B, 2 * B, H), "Quadrant")
    part = c.Part(d, c.boolean(d, ring, quad, "intersect", "Quarter"), None)
    bore = part.curved("cylinder", A)
    out = {}
    for name, nu in RATIOS:
        sid = c.study(d, "nu %g" % nu, solver=solver, mesh_size=SIZE,
                      material={"young_gpa": 1.0, "poisson": nu})
        for axis, at in ((0, 0.0), (1, 0.0), (2, 0.0), (2, H)):
            c.load(d, sid, "roller", part.flat(axis, at))
        c.load(d, sid, "pressure", bore, magnitude=P)
        r = c.solve(d, sid)
        t = d.call("probe_study", id=sid, face=bore)["translation_mm"]
        out[name] = 0.5 * (t[0] + t[1]) * math.pi / 2
        out["_nodes"] = r.get("nodes")
    return out
