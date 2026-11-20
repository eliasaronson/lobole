# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A bar pulled along its axis on three symmetry rollers (x = 0, y = 0,
z = 0): free to stretch and to thin, so its stress is F/A everywhere and
its end moves F L / (E A). A tet10 represents that field exactly, so this
is the solver's precision, not its mesh error."""

import common as c

L, W, F = 100.0, 10.0, 10000.0


def run(d, solver):
    part = c.Part(d, c.box(d, (0, 0, 0), (L, W, W), "Bar"), "steel")
    sid = c.study(d, "Bar", solver=solver, mesh_size=4.0)
    for axis in (0, 1, 2):
        c.load(d, sid, "roller", part.flat(axis, 0.0))
    c.load(d, sid, "force", part.flat(0, L), magnitude=F, direction=[1, 0, 0])
    r = c.solve(d, sid)
    end = c.sample(d, sid, (L, W, W))
    mid = c.sample(d, sid, (L / 2, W / 2, W))
    return {"u_end": end["displacement_mm"][0], "sigma": mid["stress_mpa"][0],
            "_nodes": r.get("nodes")}
