# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A free-free beam, 200 x 10 x 10 mm steel with no support at all: its six
rigid modes at zero and its first bending frequency against
Euler-Bernoulli's free-free beam (docs/simulation/README.md
#free-free-modes)."""

import common as c

L, W = 200.0, 10.0


def run(d, solver):
    c.Part(d, c.box(d, (0, 0, 0), (L, W, W), "Beam"), "steel")
    sid = c.study(d, "Hanging free", analysis="modal", solver=solver,
                  mesh_size=2.5, num_modes=2, free=True)
    r = c.solve(d, sid)
    body = r["bodies"][0]
    modes = body["modes"]
    if [m["rigid"] for m in modes] != [True] * 6 + [False] * 2:
        raise AssertionError("six rigid modes, then two elastic: %s" % modes)
    return {"f1": modes[6]["frequency_hz"],
            "_rigid_max_hz": max(m["frequency_hz"] for m in modes[:6]),
            "_nodes": r.get("nodes")}
