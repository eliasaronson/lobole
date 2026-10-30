# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""World-aligned test blocks, built the way the app takes primitives now.

A Box, Cylinder or Sphere is placed `on` something and stored as a Sketch
plus an Extrude (a Sphere: a Revolve) -- docs/modeling/README.md#primitives.
Most smokes only need "a block here, this big", and used to say so with a
world position. These helpers keep that one-liner for TESTS: they put the
primitive on the world plane through the point when there is one, and on a
DatumPlane offset from it otherwise (one more feature in the tree, which a
test counting features must allow for). Each returns the add_feature
response, whose `id` is the solid's -- the bodies' `created_by`.

The world-plane frames (SketchPlane): Top = XY, u +X, v +Y, normal +Z;
Front = XZ, u +X, v -Z, normal +Y; Right = YZ, u +Y, v +Z, normal +X.
"""

# axis letter -> (base plane, its name, which world axes are u and v, and the
# sign of v against that world axis)
_PLANES = {
    "z": ("xy", "Top", (0, 1), (1, 1)),
    "y": ("xz", "Front", (0, 2), (1, -1)),
    "x": ("yz", "Right", (1, 2), (1, 1)),
}


def _on(d, axis, level, name):
    """The plane normal to world `axis` at `level`: a world plane when it
    passes through the origin, else a DatumPlane offset to it."""
    base, word, _, _ = _PLANES[axis]
    if level == 0:
        return word
    r = d.call("add_feature", feature={
        "type": "DatumPlane", "name": f"{name} plane", "kind": "offset",
        "base_plane": base, "offset": float(level)})
    return {"feature_id": r["id"]}


def _uv(axis, p):
    _, _, (iu, iv), (su, sv) = _PLANES[axis]
    return [su * p[iu], sv * p[iv]]


def box(d, lo, size, name="Box", **kw):
    """A box with its low corner at world `lo` and extents `size` (X, Y, Z),
    standing on the XY plane through lo.z."""
    lo = [float(c) for c in lo]
    on = _on(d, "z", lo[2], name)
    f = {"type": "Box", "name": name, "on": on, "centered": False,
         "offset": [lo[0], lo[1]], "width": float(size[0]),
         "depth": float(size[1]), "height": float(size[2])}
    f.update(kw)
    return d.call("add_feature", feature=f)


def cylinder(d, base, axis, r, h, name="Cylinder", **kw):
    """A cylinder whose base circle is centred at world `base`, rising `h`
    along world `axis` ("x", "y", "z", or "-x" etc. to run the other way)."""
    neg = axis.startswith("-")
    a = axis.lstrip("-")
    base = [float(c) for c in base]
    idx = "xyz".index(a)
    on = _on(d, a, base[idx], name)
    f = {"type": "Cylinder", "name": name, "on": on,
         "offset": _uv(a, base), "diameter": 2.0 * float(r),
         "height": float(h)}
    if neg:
        f["reversed"] = True
    f.update(kw)
    return d.call("add_feature", feature=f)


def sphere(d, center, r, name="Sphere", **kw):
    """A sphere centred at world `center`."""
    center = [float(c) for c in center]
    on = _on(d, "z", center[2], name)
    f = {"type": "Sphere", "name": name, "on": on,
         "offset": [center[0], center[1]], "diameter": 2.0 * float(r)}
    f.update(kw)
    return d.call("add_feature", feature=f)


def mesh_sphere(path, center=(0.0, 0.0, 0.0), r=20.0, rings=48, cut_z=None):
    """Writes a UV sphere of `r` about `center` as a binary STL, an OBJ, a
    binary PLY or a GLB in metres (by `path`'s extension;
    docs/interchange/README.md#ply-and-gltf) and returns its triangle
    count: a closed,
    smooth mesh a scan stands in for
    (docs/interchange/README.md#mesh-bodies). `cut_z` drops every triangle with a corner below that height,
    leaving it open, as a table-top scan arrives. Pure Python, no geometry
    kernel."""
    import math
    import struct
    cx, cy, cz = center
    segs = 2 * rings
    pts = []
    for i in range(rings + 1):
        th = math.pi * i / rings
        for j in range(segs):
            ph = 2 * math.pi * j / segs
            pts.append((cx + r * math.sin(th) * math.cos(ph),
                        cy + r * math.sin(th) * math.sin(ph),
                        cz + r * math.cos(th)))
    tris = []
    for i in range(rings):
        for j in range(segs):
            a = i * segs + j
            b = i * segs + (j + 1) % segs
            c = (i + 1) * segs + j
            e = (i + 1) * segs + (j + 1) % segs
            if i > 0:
                tris.append((a, c, b))
            if i < rings - 1:
                tris.append((b, c, e))
    if cut_z is not None:
        tris = [t for t in tris if min(pts[k][2] for k in t) >= cut_z]
    if path.endswith(".ply"):
        with open(path, "wb") as f:
            f.write(("ply\nformat binary_little_endian 1.0\n"
                     "element vertex %d\nproperty float x\nproperty float y\n"
                     "property float z\nelement face %d\n"
                     "property list uchar int vertex_indices\nend_header\n"
                     % (len(pts), len(tris))).encode())
            for p in pts:
                f.write(struct.pack("<3f", *p))
            for t in tris:
                f.write(struct.pack("<B3i", 3, *t))
    elif path.endswith(".glb"):
        import json
        pos = b"".join(struct.pack("<3f", *(c / 1000.0 for c in p))
                       for p in pts)
        idx = b"".join(struct.pack("<3I", *t) for t in tris)
        lo = [min(p[k] for p in pts) / 1000.0 for k in range(3)]
        hi = [max(p[k] for p in pts) / 1000.0 for k in range(3)]
        doc = {"asset": {"version": "2.0", "generator": "smoke_shapes"},
               "scene": 0, "scenes": [{"nodes": [0]}],
               "nodes": [{"mesh": 0}],
               "meshes": [{"primitives": [{"attributes": {"POSITION": 0},
                                           "indices": 1}]}],
               "accessors": [
                   {"bufferView": 0, "componentType": 5126,
                    "count": len(pts), "type": "VEC3", "min": lo, "max": hi},
                   {"bufferView": 1, "componentType": 5125,
                    "count": 3 * len(tris), "type": "SCALAR"}],
               "bufferViews": [
                   {"buffer": 0, "byteOffset": 0, "byteLength": len(pos)},
                   {"buffer": 0, "byteOffset": len(pos),
                    "byteLength": len(idx)}],
               "buffers": [{"byteLength": len(pos) + len(idx)}]}
        text = json.dumps(doc).encode()
        text += b" " * (-len(text) % 4)
        binary = pos + idx
        binary += b"\0" * (-len(binary) % 4)
        with open(path, "wb") as f:
            f.write(struct.pack("<3I", 0x46546C67, 2,
                                12 + 8 + len(text) + 8 + len(binary)))
            f.write(struct.pack("<2I", len(text), 0x4E4F534A) + text)
            f.write(struct.pack("<2I", len(binary), 0x004E4942) + binary)
    elif path.endswith(".obj"):
        with open(path, "w") as f:
            for p in pts:
                f.write("v %.6f %.6f %.6f\n" % p)
            for t in tris:
                f.write("f %d %d %d\n" % (t[0] + 1, t[1] + 1, t[2] + 1))
    else:
        with open(path, "wb") as f:
            f.write(b"\0" * 80 + struct.pack("<I", len(tris)))
            for t in tris:
                f.write(struct.pack("<3f", 0, 0, 0))
                for k in t:
                    f.write(struct.pack("<3f", *pts[k]))
                f.write(b"\0\0")
    return len(tris)
