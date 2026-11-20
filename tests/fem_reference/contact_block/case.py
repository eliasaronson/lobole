# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A block on a frictionless ground, pushed down off its axis: a 20 x 20 x
40 mm steel block standing on a contact support, 4 kN down at e = b/12 from
its axis. Inside the kern (b/6) the whole base stays closed and its
pressure is linear, the most P/A (1 + 6e/b). Solved on CalculiX."""

import common as c

B, H, P = 20.0, 40.0, 4000.0
E = B / 12.0


def run(d, solver):
    part = c.Part(d, c.box(d, (0, 0, 0), (B, B, H), "Block"), "steel")
    sid = c.study(d, "Block", solver=solver, mesh_size=4.0)
    c.load(d, sid, "contact", part.flat(2, 0.0))
    d.call("set_study", study=sid, nonlinear={"increments": 2})
    c.load(d, sid, "remote", part.flat(2, H), force_n=P, direction=[0, 0, -1],
           point_mm=[B / 2 + E, B / 2, H])
    r = c.solve(d, sid)
    return {"peak_pressure": r["bodies"][0]["contacts"][0]["max_pressure_mpa"],
            "_nodes": r.get("nodes")}
