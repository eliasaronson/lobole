# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Lame's cylinder (lame_common) in a nearly incompressible material: the
check an element that locks fails."""

import lame_common as lame


def run(d, solver):
    part = lame.build(d, "tpu")
    ur, nodes = lame.solve(d, part, solver, 1.0)
    return {"u_r_bore": ur, "_nodes": nodes}
