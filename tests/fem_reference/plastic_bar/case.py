# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A bar pressed past its yield: 10 x 10 x 40 mm steel on three symmetry
rollers, 300 MPa on its top. Uniaxial and uniform, so its plastic strain is
the hardening curve's at 300 MPa. Solved on CalculiX."""

import common as c

W, H, S = 10.0, 40.0, 300.0


def run(d, solver):
    part = c.Part(d, c.box(d, (0, 0, 0), (W, W, H), "Bar"), "steel")
    sid = c.study(d, "Pressed", solver=solver, mesh_size=5.0,
                  nonlinear={"plasticity": True, "increments": 6})
    for axis in (0, 1, 2):
        c.load(d, sid, "roller", part.flat(axis, 0.0))
    c.load(d, sid, "pressure", part.flat(2, H), magnitude=S)
    r = c.solve(d, sid)
    mid = c.sample(d, sid, (W, W / 2, H / 2))
    return {"plastic_strain": mid["plastic_strain"], "_nodes": r.get("nodes")}
