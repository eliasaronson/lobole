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
