# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Euler's column, fixed-free: a 10 x 10 x 200 mm steel column clamped at
its base, 1 kN down on its top; its first buckling factor."""

import common as c

W, L, P = 10.0, 200.0, 1000.0


def run(d, solver):
    part = c.Part(d, c.box(d, (0, 0, 0), (W, W, L), "Column"), "steel")
    sid = c.study(d, "Euler", analysis="buckling", solver=solver,
                  mesh_size=2.5, num_modes=2)
    c.load(d, sid, "fixed", part.flat(2, 0.0))
    c.load(d, sid, "force", part.flat(2, L), magnitude=P, direction=[0, 0, -1])
    r = c.solve(d, sid)
    return {"factor": r["bodies"][0]["buckling_factor"],
            "_nodes": r.get("nodes")}
