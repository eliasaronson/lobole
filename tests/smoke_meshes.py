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


def stepped_shaft(path, wide=5.18, narrow=4.68, step_at=12.4, length=25.0,
                  top_step=0.3, top_step_r=2.4, noise=0.05, spacing=0.25):
    """A scanned stepped shaft about +Z, as binary STL: R`wide` from z = 0
    to `step_at`, R`narrow` above it to `length`, the step joined by the
    row of triangles across it as a scan joins what it cannot resolve; its
    top a flat with a disk of radius `top_step_r` standing `top_step` mm
    proud of it. Every vertex moved along its surface's normal by a
    deterministic noise of at most `noise`. Closed. What a fit that would
    average two surfaces is refused on
    (docs/modeling/README.md#two-surfaces)."""
    seed = [0x5eed]

    def jitter():
        seed[0] = (seed[0] * 6364136223846793005 + 1442695040888963407) % \
            (1 << 64)
        return noise * ((seed[0] >> 11) / float(1 << 53) * 2.0 - 1.0)

    # The profile, (r, z, normal) from the bottom's centre out, up the side
    # and in across the top to its centre; a rim point takes no noise.
    prof = []
    rings = int(round(wide / spacing))
    for k in range(rings):
        prof.append((wide * k / rings, 0.0, (0.0, -1.0)))
    prof.append((wide, 0.0, None))
    rows = int(round(length / spacing))
    for j in range(1, rows):
        z = length * j / rows
        prof.append((wide if z < step_at else narrow, z, (1.0, 0.0)))
    prof.append((narrow, length, None))
    rings = int(round(narrow / spacing))
    for k in range(rings - 1, -1, -1):
        r = narrow * k / rings
        prof.append((r, length + (top_step if r < top_step_r else 0.0),
                     (0.0, 1.0)))
    nu = int(round(2 * math.pi * wide / spacing))
    grid = []
    for (r, z, n) in prof:
        row = []
        for i in range(nu if r > 0 else 1):
            a = 2 * math.pi * i / nu
            dr, dz = (0.0, 0.0) if n is None else (n[0] * jitter(),
                                                    n[1] * jitter())
            row.append(((r + dr) * math.cos(a), (r + dr) * math.sin(a),
                        z + dz))
        grid.append(row)
    tris = []
    for j in range(len(grid) - 1):
        lo, hi = grid[j], grid[j + 1]
        for i in range(nu):
            i1 = (i + 1) % nu
            if len(lo) == 1:
                tris.append((lo[0], hi[i1], hi[i]))
            elif len(hi) == 1:
                tris.append((lo[i], lo[i1], hi[0]))
            else:
                tris.append((lo[i], lo[i1], hi[i1]))
                tris.append((lo[i], hi[i1], hi[i]))
    with open(path, "wb") as f:
        f.write(b"\0" * 80)
        f.write(struct.pack("<I", len(tris)))
        for tri in tris:
            f.write(struct.pack("<3f", 0.0, 0.0, 0.0))
            for q in tri:
                f.write(struct.pack("<3f", *q))
            f.write(b"\0\0")
    return len(tris)
