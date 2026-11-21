# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""MacNeal and Harder's straight cantilever beam: 6.0 x 0.2 x 0.1, E 1.0e7,
nu 0.30, clamped at one end and loaded at the other by a unit force along
its length, across its width (in-plane), across its depth (out-of-plane)
and a unit twisting moment, each its own study. The benchmark's units are
consistent but not ours: lengths are read as mm and E as MPa, so the study
states its own material (docs/simulation/README.md
#a-study-states-its-own-material) and the answers are the paper's numbers.
Each tip motion is the free end's, from probe_study's rigid fit of the
face: its translation along the load, its rotation about the length."""

import common as c

L, W, T = 6.0, 0.2, 0.1
MATERIAL = {"young_gpa": 1.0e4, "poisson": 0.30}  # E 1.0e7 (MPa here)
SIZE = 0.05

LOADS = (
    ("extension", "force", {"magnitude": 1.0, "direction": [1, 0, 0]}),
    ("in_plane_shear", "force", {"magnitude": 1.0, "direction": [0, 1, 0]}),
    ("out_of_plane_shear", "force",
     {"magnitude": 1.0, "direction": [0, 0, 1]}),
    ("twist", "torque", {"torque_nmm": 1.0,
                         "axis_origin_mm": [L, W / 2, T / 2],
                         "axis_dir": [1, 0, 0]}),
)


def run(d, solver):
    part = c.Part(d, c.box(d, (0, 0, 0), (L, W, T), "Beam"), None)
    tip = part.flat(0, L)
    out = {}
    for name, kind, kw in LOADS:
        sid = c.study(d, name, solver=solver, mesh_size=SIZE,
                      material=MATERIAL)
        c.load(d, sid, "fixed", part.flat(0, 0.0))
        c.load(d, sid, kind, tip, **kw)
        r = c.solve(d, sid)
        p = d.call("probe_study", id=sid, face=tip)
        t, rot = p["translation_mm"], p["rotation_rad"]
        out[name] = {"extension": t[0], "in_plane_shear": t[1],
                     "out_of_plane_shear": t[2], "twist": rot[0]}[name]
        out["_nodes"] = r.get("nodes")
    return out
