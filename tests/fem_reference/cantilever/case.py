# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A cantilever, 100 x 10 x 10 mm steel, clamped at x = 0 and loaded 1 kN
across its free end: its tip deflection against Timoshenko's beam (bending
plus shear)."""

import common as c

L, B, H, P = 100.0, 10.0, 10.0, 1000.0


def run(d, solver):
    part = c.Part(d, c.box(d, (0, 0, 0), (L, B, H), "Beam"), "steel")
    sid = c.study(d, "Cantilever", solver=solver, mesh_size=2.5)
    c.load(d, sid, "fixed", part.flat(0, 0.0))
    c.load(d, sid, "force", part.flat(0, L), magnitude=P, direction=[0, 0, -1])
    r = c.solve(d, sid)
    tip = c.sample(d, sid, (L, B / 2, H / 2))
    return {"tip": -tip["displacement_mm"][2], "_nodes": r.get("nodes")}
