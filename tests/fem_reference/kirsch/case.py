# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Kirsch's plate with a hole under uniaxial tension: a quarter of a 400 x
400 mm plate with a 10 mm hole, pulled at 100 MPa along x, on symmetry
rollers at x = 0 and y = 0 and in plane strain between rollers on both
faces (the in-plane stress of a traction problem is the same in plane stress
and plane strain). The hoop stress at the hole's side, (0, a), over the
applied stress is the concentration factor."""

import common as c


def run(d, solver):
    plate = c.box(d, (0, 0, 0), (200, 200, 5), "Plate")
    hole = c.cylinder(d, 5.0, 5.0, "Hole")
    part = c.Part(d, c.boolean(d, plate, hole, "subtract", "Holed plate"),
                  "steel")
    sid = c.study(d, "Kirsch", solver=solver, mesh_size=20.0)
    for axis, at in ((0, 0.0), (1, 0.0), (2, 0.0), (2, 5.0)):
        c.load(d, sid, "roller", part.flat(axis, at))
    bore = part.curved("cylinder", 5.0)
    c.refine(d, sid, bore, 0.4)
    c.load(d, sid, "pressure", part.flat(0, 200.0), magnitude=-100.0)
    r = c.solve(d, sid)
    at = c.sample(d, sid, (0.0, 5.0, 2.5))
    return {"kt": at["stress_mpa"][0] / 100.0, "_nodes": r.get("nodes")}
