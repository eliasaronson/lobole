# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Triangle meshes of a chosen size, for the smokes that time an import.

`torus(path, n)` writes a closed torus of about `n` triangles as binary STL
and returns the count it wrote. Closed, so it imports as one solid; a torus
rather than a sphere because its grid has no poles, so every cell is two
triangles and the count is what was asked for.
"""
import math
import struct


def torus(path, n, major=40.0, minor=12.0):
    cells = max(9, n // 2)
    u = max(3, int(math.sqrt(cells * 2)))
    v = max(3, cells // u)

    def p(i, j):
        a = 2 * math.pi * (i % u) / u
        b = 2 * math.pi * (j % v) / v
        r = major + minor * math.cos(b)
        return (r * math.cos(a), r * math.sin(a), minor * math.sin(b))

    count = 2 * u * v
    with open(path, "wb") as f:
        f.write(b"\0" * 80)
        f.write(struct.pack("<I", count))
        for i in range(u):
            for j in range(v):
                a, b, c, d = p(i, j), p(i + 1, j), p(i + 1, j + 1), p(i, j + 1)
                for tri in ((a, b, c), (a, c, d)):
                    f.write(struct.pack("<3f", 0.0, 0.0, 0.0))
                    for q in tri:
                        f.write(struct.pack("<3f", *q))
                    f.write(b"\0\0")
    return count
