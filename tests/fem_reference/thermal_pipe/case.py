# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Steady conduction through a pipe wall: a steel pipe, bore 10 mm held at
100 C, its outside cooled at 25 W/(m^2 K) by air at 20 C, its ends
insulated. The heat through it is the series of the wall's conduction and
the film's."""

import common as c

RI, RO, L = 10.0, 20.0, 40.0


def run(d, solver):
    pipe = c.boolean(d, c.cylinder(d, RO, L, "Pipe"),
                     c.cylinder(d, RI, L, "Bore"), "subtract", "Tube")
    part = c.Part(d, pipe, "steel")
    sid = c.study(d, "Pipe", analysis="thermal", solver=solver,
                  mesh_size=2.5)
    c.load(d, sid, "temperature", part.curved("cylinder", RI),
           temperature_c=100.0)
    outside = c.load(d, sid, "convection", part.curved("cylinder", RO),
                     film_w_m2k=25.0, ambient_c=20.0)
    r = c.solve(d, sid)
    flows = r["bodies"][0]["flows"]
    out = [f["watts_out"] for f in flows if f["load"] == outside]
    return {"heat_w": out[0], "_nodes": r.get("nodes")}
