# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Lame's thick cylinder, a quarter of it, the case lame_rate and lame_tpu
share: bore a, outside b, 2 mm long, on rollers on its two symmetry planes
and both ends (plane strain), `p` MPa in the bore."""

import math

import common as c

A, B, H, P = 10.0, 20.0, 2.0, 10.0


def closed_form(mat):
    """u_r at the bore, plane strain (Timoshenko & Goodier, ch. 4)."""
    E, nu = mat["E"], mat["nu"]
    return P * A * A * (1 + nu) * (B * B / A + (1 - 2 * nu) * A) / (
        E * (B * B - A * A))


def build(d, material):
    ring = c.boolean(d, c.cylinder(d, B, H, "Outer"),
                     c.cylinder(d, A, H, "Bore"), "subtract", "Ring")
    quad = c.box(d, (0, 0, 0), (2 * B, 2 * B, H), "Quadrant")
    return c.Part(d, c.boolean(d, ring, quad, "intersect", "Quarter"),
                  material)


def solve(d, part, solver, size):
    sid = c.study(d, "Lame %g" % size, solver=solver, mesh_size=size)
    for axis, at in ((0, 0.0), (1, 0.0), (2, 0.0), (2, H)):
        c.load(d, sid, "roller", part.flat(axis, at))
    c.load(d, sid, "pressure", part.curved("cylinder", A), magnitude=P)
    bore = part.curved("cylinder", A)
    r = c.solve(d, sid)
    # The bore's radial displacement, over the whole face: its rigid fit's
    # translation is the mean of u_r times the quarter arc's mean radial
    # direction, (2/pi, 2/pi). A point value is noisier than the error it
    # is read for (which node a mesh puts there, and its midside or corner
    # place), and a rate read off it is not monotone.
    t = d.call("probe_study", id=sid, face=bore)["translation_mm"]
    return 0.5 * (t[0] + t[1]) * math.pi / 2, r.get("nodes")
