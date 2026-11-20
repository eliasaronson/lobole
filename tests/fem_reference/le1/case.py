# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""NAFEMS LE1, the elliptic membrane: a quarter of the plate between the
ellipses (x/2)^2 + y^2 = 1 and (x/3.25)^2 + (y/2.75)^2 = 1 (m), under 10 MPa
outward on its outer edge, on symmetry rollers at x = 0 and y = 0. Plane
stress, as a solid: half the 0.1 m thickness, on a roller at its mid-plane,
its faces free. The target is the hoop stress sigma_yy at D (2, 0)."""

import math

import common as c


def run(d, solver):
    pts = c.sketch(d, [
        {"type": "line", "a": [2000, 0], "b": [3250, 0]},
        {"type": "ellipse_arc", "center": [0, 0], "r_major": 3250.0,
         "r_minor": 2750.0, "rotation": 0.0, "start_param": 0.0,
         "end_param": math.pi / 2},
        {"type": "line", "a": [0, 2750], "b": [0, 1000]},
        {"type": "ellipse_arc", "center": [0, 0], "r_major": 2000.0,
         "r_minor": 1000.0, "rotation": 0.0, "start_param": 0.0,
         "end_param": math.pi / 2},
    ], "Membrane section")
    part = c.Part(d, c.extrude(d, pts, 50.0, "Membrane"), "steel")
    sid = c.study(d, "LE1", solver=solver, mesh_size=100.0)
    for axis in (0, 1, 2):
        c.load(d, sid, "roller", part.flat(axis, 0.0))
    inner = part.face(lambda f: f.get("kind") != "plane" and
                      f["centroid"][0] ** 2 + f["centroid"][1] ** 2 < 2500 ** 2,
                      "the inner ellipse")
    outer = part.face(lambda f: f.get("kind") != "plane" and
                      f["centroid"][0] ** 2 + f["centroid"][1] ** 2 > 2500 ** 2,
                      "the outer ellipse")
    c.refine(d, sid, inner, 25.0)
    c.load(d, sid, "pressure", outer, magnitude=-10.0)
    r = c.solve(d, sid)
    at = c.sample(d, sid, (2000.0, 0.0, 0.0))
    return {"sigma_yy_D": at["stress_mpa"][1], "_nodes": r.get("nodes")}
