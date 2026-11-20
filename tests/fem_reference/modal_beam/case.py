# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A cantilever's first bending frequency, 100 x 10 x 10 mm steel clamped
at x = 0, against Euler-Bernoulli."""

import common as c

L, W = 100.0, 10.0


def run(d, solver):
    part = c.Part(d, c.box(d, (0, 0, 0), (L, W, W), "Beam"), "steel")
    sid = c.study(d, "Modes", analysis="modal", solver=solver,
                  mesh_size=2.5, num_modes=2)
    c.load(d, sid, "fixed", part.flat(0, 0.0))
    r = c.solve(d, sid)
    return {"f1": r["bodies"][0]["frequencies_hz"][0],
            "_nodes": r.get("nodes")}
