# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""NAFEMS FV32, the cantilevered tapered membrane: the trapezoid (0, 0),
(10, 2), (10, 3), (0, 5) m, clamped along x = 0, its in-plane modes. As a
solid: half a 250 mm plate on a roller at its mid-plane, which holds out
every mode that bends it (each is antisymmetric about that plane), so the
six lowest are the membrane's."""

import common as c


def run(d, solver):
    pid = c.prism(d, [(0, 0), (10000, 2000), (10000, 3000), (0, 5000)],
                  125.0, "Membrane")
    part = c.Part(d, pid, "stainless_steel")
    sid = c.study(d, "FV32", analysis="modal", solver=solver,
                  mesh_size=250.0, num_modes=6)
    c.load(d, sid, "fixed", part.flat(0, 0.0))
    c.load(d, sid, "roller", part.flat(2, 0.0))
    r = c.solve(d, sid)
    freqs = r["bodies"][0]["frequencies_hz"]
    out = {"f%d" % (i + 1): f for i, f in enumerate(freqs[:6])}
    out["_nodes"] = r.get("nodes")
    return out
