# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A lug under a bearing load (docs/simulation/README.md#bearing-loads): a
60 x 20 x 4 mm steel plate with an 8 mm bore at x = 12, clamped at x = 60
and pulled 400 N towards its end by a bearing load on the bore -- the load a
pin in the hole puts there. The radial stress on the bore where it is
pushed straight against the cosine pressure's peak, and the clamp's
reaction against the force."""

import common as c

L, W, T, D, X0, P = 60.0, 20.0, 4.0, 8.0, 12.0, 400.0


def run(d, solver):
    plate = c.box(d, (0, 0, 0), (L, W, T), "Plate")
    hole = c.cylinder(d, D / 2, T, "Hole", cx=X0, cy=W / 2)
    solid = c.boolean(d, plate, hole, "subtract", "Lug")
    part = c.Part(d, solid, "steel")
    sid = c.study(d, "Lug", solver=solver, mesh_size=1.2)
    c.load(d, sid, "fixed", part.flat(0, L))
    c.load(d, sid, "bearing", part.curved("cylinder", D / 2),
           force_n=P, direction=[-1, 0, 0])
    r = c.solve(d, sid)
    # On the bore where the bearing load pushes straight, mid-thickness.
    at = c.sample(d, sid, (X0 - D / 2, W / 2, T / 2))
    sxx = at["stress_mpa"][0]  # the radial direction there is -x
    return {"bearing_peak": -sxx,
            "reaction": r["reaction_total"]["force_n"][0],
            "_nodes": r.get("nodes")}
