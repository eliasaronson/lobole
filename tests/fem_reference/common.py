# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""What every reference case shares: building a part and a study over the
RPC, finding faces by where they are, and reading an answer back.

A case is `tests/fem_reference/<id>/case.py`, defining

    run(d, solver) -> {quantity: value}

where `d` is a connected client on a fresh project and `solver` is
"in_house" or "calculix", and `expected.json` beside it
(docs/simulation/reference-cases.md says what it holds). Every number a
case measures it reads through the RPC a user drives: the mesher, the
supports and the solver are the ones a study uses.
"""

import math

PLANE_Z0 = {"origin": [0, 0, 0], "normal": [0, 0, 1], "uAxis": [1, 0, 0]}

# The library's numbers the closed forms are evaluated with
# (src/cad/CADMaterials.cpp): a case whose target is a formula reads its
# material from here, and fails when the library changes under it.
STEEL = {"E": 210000.0, "nu": 0.30, "rho": 7850e-12}            # MPa, t/mm^3
STAINLESS = {"E": 200000.0, "nu": 0.30, "rho": 7900e-12}
TPU = {"E": 7.8, "nu": 0.48}


def sketch(d, entities, name="Section", plane=None):
    """A sketch of `entities` (a list of dicts, ids filled in) on `plane`."""
    ents = []
    for i, e in enumerate(entities, 1):
        e = dict(e)
        e["id"] = i
        ents.append(e)
    return d.call("add_feature", feature={
        "type": "Sketch", "name": name, "plane": plane or PLANE_Z0,
        "entities": ents, "constraints": []})["id"]


def extrude(d, sketch_id, distance, name="Solid"):
    return d.call("add_feature", feature={
        "type": "Extrude", "name": name, "sketch_id": sketch_id,
        "distance": float(distance), "mode": "new_body"})["id"]


def boolean(d, target, tool, op, name="Boolean"):
    return d.call("add_feature", feature={
        "type": "Boolean", "name": name, "target_id": target,
        "tool_id": tool, "op": op})["id"]


def polygon(points):
    """Closed polyline entities through `points` [(x, y), ...]."""
    out = []
    for i, a in enumerate(points):
        b = points[(i + 1) % len(points)]
        out.append({"type": "line", "a": list(a), "b": list(b)})
    return out


def prism(d, points, height, name="Prism"):
    """The polygon `points` (XY) extruded `height` along +Z."""
    return extrude(d, sketch(d, polygon(points), name + " section"), height,
                   name)


def box(d, lo, hi, name="Box"):
    (x0, y0, z0), (x1, y1, z1) = lo, hi
    if z0 != 0:
        raise ValueError("box() stands on z = 0")
    return prism(d, [(x0, y0), (x1, y0), (x1, y1), (x0, y1)], z1, name)


def cylinder(d, r, h, name="Cylinder", cx=0.0, cy=0.0):
    return extrude(d, sketch(d, [{"type": "circle", "center": [cx, cy],
                                  "radius": float(r)}], name + " section"),
                   h, name)


def ellipse_prism(d, a, b, h, name="Ellipse"):
    return extrude(d, sketch(d, [{"type": "ellipse", "center": [0, 0],
                                  "r_major": float(a), "r_minor": float(b),
                                  "rotation": 0.0}], name + " section"),
                   h, name)


class Part:
    """The project's only body, its faces found by where they are."""

    def __init__(self, d, solid, material):
        """`material` a library slug, or None for a part whose study states
        its own (docs/simulation/README.md#a-study-states-its-own-material)."""
        self.d = d
        if material is not None:
            d.call("set_material", feature_id=solid, material=material)
        body = d.call("list_elements")["bodies"]
        if len(body) != 1:
            raise AssertionError("expected one body, got %d" % len(body))
        self.created_by = body[0]["created_by"]
        self.faces = body[0]["faces"]

    def ref(self, f):
        return {"feature_id": self.created_by, "element": f["name"]}

    def face(self, pick, what):
        hits = [f for f in self.faces if pick(f)]
        if len(hits) != 1:
            raise AssertionError("%s: %d faces match, want one: %s" % (
                what, len(hits), [(f["name"], f.get("kind"),
                                   f.get("centroid")) for f in self.faces]))
        return self.ref(hits[0])

    def flat(self, axis, at, tol=1e-6):
        """The flat face square to world `axis` (0, 1, 2) at `at`."""
        def pick(f):
            return (f.get("kind") == "plane" and
                    abs(abs(f["normal"][axis]) - 1.0) < 1e-9 and
                    abs(f["centroid"][axis] - at) < tol)
        return self.face(pick, "the flat face at %s = %g" % ("xyz"[axis], at))

    def curved(self, kind, radius=None, tol=1e-6):
        def pick(f):
            return (f.get("kind") == kind and
                    (radius is None or abs(f.get("radius", -1) - radius) < tol))
        return self.face(pick, "the %s face%s" % (
            kind, "" if radius is None else " of radius %g" % radius))

    def not_flat(self, what):
        return self.face(lambda f: f.get("kind") != "plane", what)


def study(d, name, analysis="static", solver="in_house", mesh_size=None,
          **kw):
    sid = d.call("add_study", name=name)["id"]
    args = {"study": sid, "type": analysis, "solver": solver,
            "converge": "never"} if analysis != "modal" and \
        analysis != "topology" else {"study": sid, "type": analysis,
                                     "solver": solver}
    if mesh_size is not None:
        args["mesh_size"] = float(mesh_size)
    args.update(kw)
    d.call("set_study", **args)
    return sid


def load(d, sid, type_, face, **kw):
    return d.call("add_load", study=sid, type=type_, face=face, **kw)["id"]


def refine(d, sid, face, size):
    d.call("add_refinement", study=sid, face=face, size=float(size))


def solve(d, sid):
    r = d.call("run_study", id=sid, recompute=True)
    return r


def sample(d, sid, at):
    return d.call("sample_study", id=sid, at_mm=[float(c) for c in at])


def radial(u, x):
    rr = math.hypot(x[0], x[1])
    return (u[0] * x[0] + u[1] * x[1]) / rr
