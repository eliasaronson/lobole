#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Random designs, the checks they must pass, and the minimiser
(docs/modeling/README.md#random-designs). tools/fuzz.py is the command line.

A DESIGN is a seeded sequence of steps, each one or two RPC calls, chosen by
random.Random(seed) against the state the previous steps left: a base (a
Box, a sketched profile extruded, or a revolve), then up to eight of an
extrude add/cut/new_body on one of the body's own faces, a Box, Cylinder or
Sphere on a face, a Fillet or Chamfer, a Shell, a Hole, a Pattern of the
last feature, a Draft, a Boolean. Placements snap to a 2.5 mm grid and
sizes are fractions of the body's own extents, so coincident faces,
tangencies and seams come up as often as they do in real parts.

Every feature's id is PINNED from its step's index (100 + 10*k + j): a
replay from nothing, a save and reopen and a minimised design all name the
same features, and the element names built from them still resolve. A step
refers to an earlier one as {"$step": k} and to a face or an edge by where
it is ({"$face": {near, normal}}, {"$edge": {near}}), resolved at run time
against list_elements -- so the minimiser can drop a step without
renumbering the others.

After every step the call's own answer (regen, digest) and one
check_geometry must hold (see `check_step`); at the end, the incremental
part must equal the same calls replayed from nothing, a save and reopen,
and undo-to-the-start then redo-to-the-end. Three Boolean identities per
seed tie a union, an intersect and a subtract together:
V(A u B) + V(A n B) = V(A) + V(B) and V(A - B) = V(A) - V(A n B).
"""
import copy
import json
import math
import random
import re

# Bumped whenever a change makes a seed build something else: a seed list
# (tests/fuzz_seeds_smoke.py) is only meaningful against one version.
GENERATOR_VERSION = 1

GRID = 2.5
# The failure classes, and which of them are findings (docs: the outcome
# table). `limit` and `clean` are not; `generator` fails the smoke but is
# the generator's bug, not the kernel's.
FINDINGS = {"boolean_failed", "invalid", "wrong_way", "identity", "rebuild",
            "reopen", "undo", "message", "internal", "nonfinite", "crash",
            "hang", "leak"}
LIMIT_TYPES = {"Fillet", "Chamfer", "Shell", "Draft", "Hole"}


class Failure(Exception):
    def __init__(self, cls, message, step=None):
        super().__init__(f"{cls}: {message}")
        self.cls = cls
        self.message = message
        self.step = step


def gist(cls, message):
    """A failure reduced for grouping: its class and the first 60
    characters of its message with the digits taken out."""
    text = re.sub(r"[0-9]+", "", str(message))
    text = re.sub(r"\s+", " ", text).strip()
    return f"{cls}: {text[:60]}"


def snap(x, grid=GRID):
    return round(x / grid) * grid


# --- the generator ------------------------------------------------------
def fid(k, j=0):
    """The pinned id of step k's j-th feature."""
    return 100 + 10 * k + j


TOP = {"origin": [0, 0, 0], "normal": [0, 0, 1], "uAxis": [1, 0, 0]}
FRONT = {"origin": [0, 0, 0], "normal": [0, 1, 0], "uAxis": [1, 0, 0]}


def _cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]]


def _u_axis(n):
    """An in-plane U axis for a plane of normal n: world X unless n is
    along X, then world Y."""
    return [0, 1, 0] if abs(n[0]) > 0.9 else [1, 0, 0]


def _project(u, n):
    d = sum(a * b for a, b in zip(u, n))
    v = [a - d * b for a, b in zip(u, n)]
    m = math.sqrt(sum(a * a for a in v)) or 1.0
    return [a / m for a in v]


class Generator:
    """Chooses the next step from the state the design is in. Every choice
    goes through self.rng, and the state is read sorted, so one seed and one
    build make one design."""

    def __init__(self, seed):
        self.seed = seed
        self.rng = random.Random(seed)

    def size(self, lo, hi):
        return max(GRID, snap(self.rng.uniform(lo, hi)))

    def profile(self, w, h):
        """A closed profile on a sketch plane, centred on the origin: a
        rectangle, a regular polygon, or a rectangle closed by an arc,
        sometimes notched."""
        r = self.rng
        kind = r.choice(["rect", "polygon", "arc", "notch"])
        if kind == "notch" and min(w, h) < 3 * GRID:
            kind = "rect"  # a notch as deep as the profile cuts it in two
        x0, y0, x1, y1 = -w / 2, -h / 2, w / 2, h / 2
        if kind == "rect":
            return [{"type": "rect", "c1": [x0, y0], "c2": [x1, y1]}]
        if kind == "polygon":
            n = r.randint(5, 8)
            rad = min(w, h) / 2
            pts = [[snap(rad * math.cos(2 * math.pi * i / n), 0.5),
                    snap(rad * math.sin(2 * math.pi * i / n), 0.5)]
                   for i in range(n)]
            return [{"type": "line", "a": pts[i], "b": pts[(i + 1) % n]}
                    for i in range(n)]
        if kind == "arc":
            # three sides and a half circle over the top
            return [
                {"type": "line", "a": [x0, y0], "b": [x1, y0]},
                {"type": "line", "a": [x1, y0], "b": [x1, 0]},
                {"type": "arc", "center": [0, 0], "radius": w / 2,
                 "start_angle_deg": 0, "end_angle_deg": 180},
                {"type": "line", "a": [x0, 0], "b": [x0, y0]}]
        # a rectangle with a notch out of its top edge, on the grid
        nw = max(GRID, snap(w / 3))
        nd = max(GRID, snap(h / 3))
        pts = [[x0, y0], [x1, y0], [x1, y1], [nw / 2, y1], [nw / 2, y1 - nd],
               [-nw / 2, y1 - nd], [-nw / 2, y1], [x0, y1]]
        return [{"type": "line", "a": pts[i], "b": pts[(i + 1) % len(pts)]}
                for i in range(len(pts))]

    def base(self):
        r = self.rng
        kind = r.choice(["box", "profile", "revolve"])
        w, d, h = self.size(10, 60), self.size(10, 60), self.size(5, 40)
        if kind == "box":
            return {"op": "base box", "expect": "new_body", "deps": [],
                    "calls": [{"method": "add_feature", "params": {
                        "feature": {"type": "Box", "name": "Base",
                                    "on": "Top", "width": w, "depth": d,
                                    "height": h, "ids": [fid(0), fid(0, 1)]}}}]}
        if kind == "profile":
            return {"op": "base profile", "expect": "new_body", "deps": [],
                    "calls": [
                        {"method": "add_feature", "params": {"feature": {
                            "type": "Sketch", "name": "Base sketch",
                            "id": fid(0, 1), "plane": TOP,
                            "entities": self.profile(w, d)}}},
                        {"method": "add_feature", "params": {"feature": {
                            "type": "Extrude", "name": "Base",
                            "id": fid(0), "sketch_id": fid(0, 1),
                            "distance": h, "mode": "new_body"}}}]}
        # A section on Front (XZ: u = +X, v = -Z), revolved about world Z.
        r0 = r.choice([0.0, self.size(2.5, 15)])  # 0: touching the axis
        r1 = r0 + self.size(5, 25)
        angle = r.choice([90.0, 180.0, 270.0, 360.0])
        return {"op": "base revolve", "expect": "new_body", "deps": [],
                "calls": [
                    {"method": "add_feature", "params": {"feature": {
                        "type": "Sketch", "name": "Base section",
                        "id": fid(0, 1), "plane": FRONT,
                        "entities": [{"type": "rect", "c1": [r0, 0],
                                      "c2": [r1, -h]}]}}},
                    {"method": "add_feature", "params": {"feature": {
                        "type": "Revolve", "name": "Base", "id": fid(0),
                        "sketch_id": fid(0, 1),
                        "axis": {"kind": "custom", "origin": [0, 0, 0],
                                 "direction": [0, 0, 1]},
                        "angle_deg": angle, "mode": "new_body"}}}]}

    def next(self, k, state, last):
        """Step k from `state` (sorted bodies with faces and edges) and the
        last step taken (for a pattern of it)."""
        r = self.rng
        bodies = state["bodies"]
        planar = [(b, f) for b in bodies for f in b["faces"]
                  if f["kind"] == "plane" and f.get("area_mm2", 0) > 1.0]
        edges = [(b, e) for b in bodies for e in b["edges"]
                 if e["kind"] in ("line", "circle") and e.get("length_mm", 0)
                 > 0.5]
        choices = ["extrude", "primitive", "fillet", "chamfer", "shell",
                   "hole", "draft"]
        if last and last.get("patternable"):
            choices += ["pattern", "pattern"]
        if len(bodies) >= 2:
            choices += ["boolean"]
        if not planar:
            choices = [c for c in choices if c not in ("extrude", "primitive",
                                                       "hole", "shell")]
        if not edges:
            choices = [c for c in choices if c not in ("fillet", "chamfer")]
        if not choices:
            return None
        op = r.choice(choices)
        return getattr(self, "step_" + op)(k, bodies, planar, edges, last)

    # Each step_* returns {"op", "expect", "deps", "calls", ...}.
    def _face_frame(self, face):
        n = face["normal"]
        u = _project(_u_axis(n), n)
        v = _cross(n, u)
        return face["centroid"], n, u, v

    def _face_ref(self, face):
        return {"$face": {"near": face["centroid"], "normal": face["normal"]}}

    def _extent(self, body):
        lo, hi = body["bbox_min"], body["bbox_max"]
        return [h - l for h, l in zip(hi, lo)]

    def step_extrude(self, k, bodies, planar, edges, last):
        r = self.rng
        body, face = r.choice(planar)
        c, n, u, v = self._face_frame(face)
        ext = max(self._extent(body))
        side = math.sqrt(face["area_mm2"])
        mode = r.choice(["add", "cut", "new_body"])
        w = max(GRID, snap(side * r.uniform(0.2, 0.8)))
        h = max(GRID, snap(side * r.uniform(0.2, 0.8)))
        # The sketch plane through the face, its origin on the grid near
        # the face's centroid.
        origin = [snap(x) if abs(nn) < 0.5 else x for x, nn in zip(c, n)]
        ou, ov = (snap(r.uniform(-side / 4, side / 4)) for _ in range(2))
        ents = (self.profile(w, h) if r.random() < 0.6 else
                [{"type": "circle", "center": [0, 0],
                  "radius": max(GRID / 2, snap(min(w, h) / 2, 0.5))}])
        for e in ents:  # shift the profile within the plane
            for key in ("a", "b", "c1", "c2", "center"):
                if key in e:
                    e[key] = [e[key][0] + ou, e[key][1] + ov]
        dist = max(GRID, snap(ext * r.uniform(0.1, 0.6)))
        sketch = {"type": "Sketch", "name": f"S{k}", "id": fid(k, 1),
                  "plane": {"origin": origin, "normal": n, "uAxis": u},
                  "entities": ents}
        ex = {"type": "Extrude", "name": f"E{k}", "id": fid(k),
              "sketch_id": fid(k, 1), "distance": dist, "mode": mode}
        if mode == "cut":
            ex["reversed"] = True  # into the body: the face looks out
        elif mode == "new_body" and r.random() < 0.5:
            ex["reversed"] = True
        return {"op": f"extrude {mode}", "expect": mode, "deps": [],
                "patternable": True,
                "calls": [{"method": "add_feature",
                           "params": {"feature": sketch}},
                          {"method": "add_feature",
                           "params": {"feature": ex}}]}

    def step_primitive(self, k, bodies, planar, edges, last):
        r = self.rng
        body, face = r.choice(planar)
        side = math.sqrt(face["area_mm2"])
        ext = max(self._extent(body))
        kind = r.choice(["Box", "Cylinder", "Sphere"])
        mode = r.choice(["add", "cut", "new_body"])
        f = {"type": kind, "name": f"P{k}", "on": self._face_ref(face),
             "offset": [snap(r.uniform(-side / 3, side / 3)),
                        snap(r.uniform(-side / 3, side / 3))],
             "mode": mode, "ids": [fid(k), fid(k, 1)]}
        if kind == "Box":
            f.update(width=max(GRID, snap(side * r.uniform(0.2, 0.7))),
                     depth=max(GRID, snap(side * r.uniform(0.2, 0.7))),
                     height=max(GRID, snap(ext * r.uniform(0.1, 0.5))))
        elif kind == "Cylinder":
            f.update(diameter=max(GRID, snap(side * r.uniform(0.2, 0.7))),
                     height=max(GRID, snap(ext * r.uniform(0.1, 0.5))))
        else:
            f.update(diameter=max(GRID, snap(side * r.uniform(0.2, 0.8))))
        if mode == "cut" and kind != "Sphere":
            f["reversed"] = True
        return {"op": f"{kind.lower()} {mode}", "expect": mode, "deps": [],
                "patternable": True,
                "calls": [{"method": "add_feature",
                           "params": {"feature": f}}]}

    def _edges(self, edges):
        r = self.rng
        n = min(len(edges), r.randint(1, 3))
        picked = r.sample(edges, n)
        shortest = min(e["length_mm"] for _, e in picked)
        refs = [{"$edge": {"near": e["midpoint"]}} for _, e in picked]
        return refs, shortest

    def step_fillet(self, k, bodies, planar, edges, last, kind="Fillet"):
        refs, shortest = self._edges(edges)
        radius = round(shortest * self.rng.uniform(0.05, 0.4), 2) or 0.1
        return {"op": kind.lower(), "expect": "any", "deps": [],
                "calls": [{"method": "add_feature", "params": {"feature": {
                    "type": kind, "name": f"{kind[0]}{k}", "id": fid(k),
                    "edges": refs, "radius": radius}}}]}

    def step_chamfer(self, k, bodies, planar, edges, last):
        return self.step_fillet(k, bodies, planar, edges, last, "Chamfer")

    def _body_ref(self, body):
        return {"$body": {"near": [(a + b) / 2 for a, b in
                                   zip(body["bbox_min"], body["bbox_max"])],
                          "volume": body["volume"]}}

    def step_shell(self, k, bodies, planar, edges, last):
        r = self.rng
        body, face = r.choice(planar)
        t = round(min(self._extent(body)) * r.uniform(0.05, 0.2), 2) or 0.2
        return {"op": "shell", "expect": "cut", "deps": [],
                "calls": [{"method": "add_feature", "params": {"feature": {
                    "type": "Shell", "name": f"Sh{k}", "id": fid(k),
                    "body_id": self._body_ref(body), "thickness": t,
                    "open_faces": [self._face_ref(face)]}}}]}

    def step_hole(self, k, bodies, planar, edges, last):
        r = self.rng
        body, face = r.choice(planar)
        c, n, u, v = self._face_frame(face)
        side = math.sqrt(face["area_mm2"])
        dia = max(1.0, snap(side * r.uniform(0.05, 0.3), 0.5))
        pts = [{"type": "point",
                "p": [snap(r.uniform(-side / 3, side / 3)),
                      snap(r.uniform(-side / 3, side / 3))]}
               for _ in range(r.randint(1, 3))]
        style = r.choice(["simple", "blind", "drilled"])
        hole = {"type": "Hole", "name": f"H{k}", "id": fid(k),
                "hole_kind": "simple", "sketch_id": fid(k, 1),
                "diameter": dia,
                "depth": max(GRID, snap(max(self._extent(body)) *
                                        r.uniform(0.1, 0.8)))}
        if style == "simple":
            hole["through"] = True
        elif style == "drilled":
            hole["drill_point_deg"] = 118.0
        return {"op": f"hole {style}", "expect": "cut", "deps": [],
                "patternable": True,
                "calls": [
                    {"method": "add_feature", "params": {"feature": {
                        "type": "Sketch", "name": f"HS{k}", "id": fid(k, 1),
                        "plane": {"origin": c, "normal": n, "uAxis": u},
                        "entities": pts}}},
                    {"method": "add_feature", "params": {"feature": hole}}]}

    def step_pattern(self, k, bodies, planar, edges, last):
        r = self.rng
        src = last["k"]
        kind = r.choice(["linear", "circular", "mirror"])
        f = {"type": "Pattern", "name": f"Pat{k}", "id": fid(k),
             "kind": kind, "source_id": {"$step": src}}
        allb = [b for b in bodies]
        lo = [min(b["bbox_min"][i] for b in allb) for i in range(3)]
        hi = [max(b["bbox_max"][i] for b in allb) for i in range(3)]
        if kind == "linear":
            axis = r.choice(["x", "y", "z"])
            span = hi["xyz".index(axis)] - lo["xyz".index(axis)]
            f.update(axis={"kind": axis}, count=r.randint(2, 4),
                     spacing=max(GRID, snap(span * r.uniform(0.1, 0.5))))
        elif kind == "circular":
            centre = [snap((a + b) / 2) for a, b in zip(lo, hi)]
            n = r.randint(2, 6)
            # angle_deg is BETWEEN neighbours: a full ring of n.
            f.update(axis={"kind": "custom", "origin": centre,
                           "direction": [0, 0, 1]},
                     count=n, angle_deg=round(360.0 / n, 6))
        else:
            f["base_plane"] = r.choice(["xy", "xz", "yz"])
        expect = last["expect"]
        if kind == "mirror" or expect == "new_body":
            expect = "any"  # a mirror or a copy of a body: new bodies
        return {"op": f"pattern {kind}", "expect": expect, "deps": [src],
                "calls": [{"method": "add_feature",
                           "params": {"feature": f}}]}

    def step_draft(self, k, bodies, planar, edges, last):
        r = self.rng
        sides = [(b, f) for b, f in planar if abs(f["normal"][2]) < 0.1]
        if not sides:
            return self.step_fillet(k, bodies, planar, edges, last)
        body, face = r.choice(sides)
        return {"op": "draft", "expect": "any", "deps": [],
                "calls": [{"method": "add_feature", "params": {"feature": {
                    "type": "Draft", "name": f"D{k}", "id": fid(k),
                    "angle_deg": float(r.choice([2, 5, 8, 10])),
                    "neutral_plane": "xy", "body_id": self._body_ref(body),
                    "faces": [self._face_ref(face)]}}}]}

    def step_boolean(self, k, bodies, planar, edges, last):
        r = self.rng
        # A Boolean names its operands by created_by, which the pieces of
        # one split pattern share: only bodies it can tell apart.
        by = {}
        for body in bodies:
            by.setdefault(body["created_by"], body)
        if len(by) < 2:
            return (self.step_fillet(k, bodies, planar, edges, last)
                    if edges else None)
        a, b = r.sample(sorted(by.values(), key=lambda x: x["created_by"]),
                        2)
        op = r.choice(["union", "subtract", "intersect"])
        if op == "intersect" and any(
                a["bbox_max"][i] < b["bbox_min"][i] or
                b["bbox_max"][i] < a["bbox_min"][i] for i in range(3)):
            op = "union"  # disjoint: an intersect could only be empty
        return {"op": f"boolean {op}",
                "expect": "union" if op == "union" else "cut", "deps": [],
                "calls": [{"method": "add_feature", "params": {"feature": {
                    "type": "Boolean", "name": f"B{k}", "id": fid(k),
                    "target_id": self._body_ref(a),
                    "tool_id": self._body_ref(b), "op": op}}}]}

    def identity_pair(self):
        """Two primitives as new bodies on the same grid: overlapping,
        touching or sharing a face."""
        r = self.rng
        out = []
        for i in range(2):
            kind = r.choice(["Box", "Cylinder", "Sphere"])
            f = {"type": kind, "name": "AB"[i], "on": "Top",
                 "offset": [snap(r.uniform(-10, 10)), snap(r.uniform(-10, 10))],
                 "mode": "new_body", "ids": [fid(90 + i), fid(90 + i, 1)]}
            if kind == "Box":
                f.update(width=self.size(5, 25), depth=self.size(5, 25),
                         height=self.size(5, 25))
            elif kind == "Cylinder":
                f.update(diameter=self.size(5, 25), height=self.size(5, 25))
            else:
                f.update(diameter=self.size(5, 25))
            out.append(f)
        return out


# --- resolving a step's selectors ---------------------------------------
def _dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def resolve(value, elements):
    """A step's params with every selector replaced by what it names now.
    Raises Failure("unresolved") when nothing matches."""
    if isinstance(value, list):
        return [resolve(v, elements) for v in value]
    if not isinstance(value, dict):
        return value
    if "$step" in value:
        return fid(value["$step"])
    bodies = elements["bodies"]
    if "$face" in value:
        sel = value["$face"]
        best = None
        for b in bodies:
            for f in b["faces"]:
                if f["kind"] != "plane":
                    continue
                if sum(x * y for x, y in zip(f["normal"],
                                             sel["normal"])) < 0.999:
                    continue
                d = _dist(f["centroid"], sel["near"])
                if best is None or d < best[0]:
                    best = (d, b, f)
        if best is None or best[0] > 1e-3 + 0.5 * math.sqrt(
                best[2].get("area_mm2", 1.0)):
            raise Failure("unresolved", f"no planar face near {sel}")
        return {"feature_id": best[1]["created_by"],
                "element": best[2]["name"]}
    if "$edge" in value:
        sel = value["$edge"]
        best = None
        for b in bodies:
            for e in b["edges"]:
                d = _dist(e["midpoint"], sel["near"])
                if best is None or d < best[0]:
                    best = (d, b, e)
        if best is None or best[0] > 1e-3:
            raise Failure("unresolved", f"no edge at {sel['near']}")
        return {"feature_id": best[1]["created_by"],
                "element": best[2]["name"]}
    if "$body" in value:
        sel = value["$body"]
        best = None
        for b in bodies:
            c = [(x + y) / 2 for x, y in zip(b["bbox_min"], b["bbox_max"])]
            # By its centre, and among bodies centred alike by its volume:
            # two bodies about one centre are not one body.
            d = _dist(c, sel["near"]) + abs(b.get("volume_mm3", 0.0) -
                                             sel.get("volume", 0.0)) * 1e-6
            if best is None or d < best[0]:
                best = (d, b)
        if best is None or best[0] > 1e-3:
            raise Failure("unresolved", f"no body centred at {sel['near']}")
        return best[1]["created_by"]
    return {k: resolve(v, elements) for k, v in value.items()}


def needs_elements(value):
    s = json.dumps(value)
    return '"$face"' in s or '"$edge"' in s or '"$body"' in s


def summarise(elements, digest_bodies=None):
    """The state a generator reads: bodies sorted by name, each with its
    faces and edges sorted by name, and the digest's bbox and volume."""
    by = {b["created_by"]: b for b in (digest_bodies or [])}
    bodies = []
    for b in sorted(elements["bodies"], key=lambda b: (b.get("name", ""),
                                                       b["created_by"])):
        d = by.get(b["created_by"], {})
        bodies.append({
            "created_by": b["created_by"], "name": b.get("name", ""),
            "volume": d.get("volume_mm3", 0.0),
            "bbox_min": d.get("bbox_min", [0, 0, 0]),
            "bbox_max": d.get("bbox_max", [0, 0, 0]),
            "faces": sorted(b["faces"], key=lambda f: f["name"]),
            "edges": sorted(b["edges"], key=lambda e: e["name"])})
    return {"bodies": bodies}


# --- the checks ---------------------------------------------------------
def measured(message):
    """A failure message that states a condition, not just a verdict: not
    empty, not "(see log)", not a prefix with nothing after its colon
    ("shell failed:")."""
    m = str(message or "").strip()
    return len(m) >= 12 and "(see log)" not in m and not m.endswith(":")


def check_answer(answer):
    """Section 1's two: `internal` and `nonfinite` (the runner raises on
    both before this; kept for the fake source)."""
    if isinstance(answer, dict) and "nonfinite" in answer:
        raise Failure("nonfinite", f"non-finite numbers at "
                      f"{answer['nonfinite'][:5]}")


def check_step(step, before, answer, geometry, part_volume, boxes=None):
    """The checks after one step, from its own answer and one
    check_geometry. `before` is the digest bodies before the step; `boxes`,
    when given, the measured (lo, hi) of every body before and after, which
    a cut is judged by instead of the digest's bounds."""
    check_answer(answer)
    regen = answer.get("regen", {})
    digest = answer.get("digest", {})
    for f in regen.get("failed", []):
        msg = f.get("error") or f.get("message") or ""
        if not measured(msg):
            raise Failure("message", f"{f.get('name')}: {msg!r}")
    after = digest.get("bodies", before)
    for b in after:
        v = b.get("volume_mm3")
        if v is None or not math.isfinite(v) or v <= 0:
            raise Failure("invalid", f"body {b.get('name')} has volume {v}")
        if b.get("solids", 1) < 1:
            raise Failure("invalid", f"body {b.get('name')} has no solid")
    if geometry is not None and not geometry.get("valid", True):
        bad = [(b.get("name"), b.get("reasons")) for b in
               geometry.get("bodies", []) if not b.get("valid", True)]
        raise Failure("invalid", f"check_geometry: {bad}")
    tol = 1e-6 * max(part_volume, 1.0)
    delta = digest.get("delta", {}).get("volume_mm3", 0.0)
    expect = step["expect"]
    if expect == "cut" and delta > tol:
        raise Failure("wrong_way", f"a {step['op']} added {delta:.6g} mm3")
    if expect == "add" and delta < -tol:
        raise Failure("wrong_way", f"an {step['op']} removed {-delta:.6g} "
                      f"mm3")
    if expect == "new_body":
        old = {b["created_by"]: b for b in before}
        new = {b["created_by"]: b for b in after}
        added = [c for c in new if c not in old]
        if len(added) != 1:
            raise Failure("wrong_way", f"a {step['op']} added "
                          f"{len(added)} bodies, not one")
        for c, b in old.items():
            if c not in new or abs(new[c]["volume_mm3"] - b["volume_mm3"]) > tol:
                raise Failure("wrong_way", f"a {step['op']} changed body "
                              f"{b.get('name')}")
    if expect == "cut" and before and after:
        if boxes and boxes[0] and boxes[1]:
            (lo0, hi0), (lo1, hi1) = boxes
        else:
            lo0 = [min(b["bbox_min"][i] for b in before) for i in range(3)]
            hi0 = [max(b["bbox_max"][i] for b in before) for i in range(3)]
            lo1 = [min(b["bbox_min"][i] for b in after) for i in range(3)]
            hi1 = [max(b["bbox_max"][i] for b in after) for i in range(3)]
        slack = 1e-4
        if any(lo1[i] < lo0[i] - slack or hi1[i] > hi0[i] + slack
               for i in range(3)):
            raise Failure("wrong_way", f"a {step['op']} grew the bbox from "
                          f"{lo0}..{hi0} to {lo1}..{hi1}")


def same_bodies(a, b, rel=1e-9, what="rebuild"):
    """Two digests' bodies agree: the same set, volume, bbox and solids."""
    ka = {x["created_by"]: x for x in a}
    kb = {x["created_by"]: x for x in b}
    if set(ka) != set(kb):
        raise Failure(what, f"bodies {sorted(ka)} vs {sorted(kb)}")
    for c in ka:
        x, y = ka[c], kb[c]
        scale = max(abs(x["volume_mm3"]), 1.0)
        if abs(x["volume_mm3"] - y["volume_mm3"]) > rel * scale:
            raise Failure(what, f"body {x.get('name')} volume "
                          f"{x['volume_mm3']!r} vs {y['volume_mm3']!r}")
        size = max(max(abs(v) for v in x["bbox_max"] + x["bbox_min"]), 1.0)
        for k in ("bbox_min", "bbox_max"):
            if any(abs(p - q) > max(rel * size, 1e-7) for p, q in
                   zip(x[k], y[k])):
                raise Failure(what, f"body {x.get('name')} {k} {x[k]} vs "
                              f"{y[k]}")
        if x.get("solids") != y.get("solids"):
            raise Failure(what, f"body {x.get('name')} solids "
                          f"{x.get('solids')} vs {y.get('solids')}")


def check_identity(va, vb, vu, vi, vd):
    """V(A u B) + V(A n B) = V(A) + V(B) and V(A - B) = V(A) - V(A n B),
    to 0.2 % of V(A) + V(B)."""
    tol = 0.002 * (va + vb)
    if abs(vu + vi - (va + vb)) > tol:
        raise Failure("identity", f"V(AuB) + V(AnB) = {vu + vi:.6g} but "
                      f"V(A) + V(B) = {va + vb:.6g}")
    if abs(vd - (va - vi)) > tol:
        raise Failure("identity", f"V(A-B) = {vd:.6g} but V(A) - V(AnB) = "
                      f"{va - vi:.6g}")


# --- the runner ---------------------------------------------------------
class Runner:
    """Runs a design against an answer source `d` (a lobole.client.Client, or
    the self-test's fake): `d.request(method, params)` returns the result
    or raises an exception whose `.diagnostic` says `internal`."""

    def __init__(self, d, log=None):
        self.d = d
        self.log = log if log is not None else []

    def call(self, method, params, step=None):
        try:
            answer = self.d.request(method, params)
        except Exception as e:  # noqa: BLE001 -- classified below
            diag = getattr(e, "diagnostic", None) or {}
            if "nonfinite" in type(e).__name__.lower() or hasattr(
                    e, "pointers"):
                raise Failure("nonfinite", str(e), step)
            if isinstance(e, (ConnectionError, OSError, TimeoutError)):
                raise Failure("crash" if not isinstance(e, TimeoutError)
                              else "hang", f"{method}: {e}", step)
            if diag.get("code") == "internal" or _deep_internal(diag):
                raise Failure("internal", f"{method}: {e}", step)
            raise Failure("generator", f"{method} refused: {e}", step)
        self.log.append({"method": method, "params": params})
        return answer

    def elements(self):
        """list_elements, each body with its digest fields (bbox, volume)
        from get_state beside its faces and edges."""
        els = self.call("list_elements", {})
        by = {b["created_by"]: b for b in self.bodies()}
        for b in els.get("bodies", []):
            for k, v in by.get(b["created_by"], {}).items():
                b.setdefault(k, v)
        return els

    def tight_box(self):
        """The union of every body's measured box (mass_properties)."""
        if not self.bodies():
            return None
        bodies = self.call("mass_properties", {}).get("bodies", [])
        boxes = [(b["bbox_min"], b["bbox_max"]) for b in bodies
                 if "bbox_min" in b]
        if not boxes:
            return None
        return ([min(b[0][i] for b in boxes) for i in range(3)],
                [max(b[1][i] for b in boxes) for i in range(3)])

    def bodies(self):
        st = self.call("get_state", {})
        return [{k: b.get(k) for k in ("created_by", "name", "volume_mm3",
                                       "bbox_min", "bbox_max", "solids")}
                for b in st.get("bodies", [])]

    def run_step(self, k, step, before):
        """Execute one step; returns (accepted calls as sent, the digest
        bodies after, a limit message or None)."""
        sent = []
        answer = {}
        elements = None
        # A cut is judged against MEASURED boxes: get_state's and the
        # digest's are a cheap bound that can stand off a curved face.
        tight_before = self.tight_box() if step["expect"] == "cut" else None
        for c in step["calls"]:
            params = c["params"]
            if needs_elements(params):
                elements = elements or self.elements()
                try:
                    params = resolve(params, elements)
                except Failure as f:
                    raise Failure("generator", f.message, k)
            else:
                params = resolve(params, {"bodies": []})
            try:
                answer = self.call(c["method"], params, k)
            except Failure as f:
                # A fillet, chamfer, shell, draft or hole the bodies refuse
                # before it is committed (Feature::WhyRefusedOn) is the
                # limit its failed build used to be, said earlier: undo what
                # this step sent and go on, as below.
                kind = (params.get("feature") or {}).get("type") \
                    if c["method"] == "add_feature" else None
                if (f.cls == "generator" and kind in LIMIT_TYPES
                        and measured(f.message)):
                    for _ in sent:
                        self.call("undo", {}, k)
                    return [], before, f.message
                raise
            sent.append({"method": c["method"], "params": params})
        regen = answer.get("regen", {}) if isinstance(answer, dict) else {}
        failed = regen.get("failed", [])
        part_volume = sum(b["volume_mm3"] for b in before) or 1.0
        mine = failed
        if mine:
            names = {c["params"].get("feature", {}).get("type")
                     for c in sent}
            msg = "; ".join(str(f.get("error") or f.get("message") or "")
                            for f in mine)
            for f in mine:
                if not measured(f.get("error") or f.get("message")):
                    raise Failure("message", f"{f.get('name')}: "
                                  f"{f.get('error')!r}", k)
            # An intersect or a subtract whose result is empty is a refusal
            # with a measured reason, not the kernel failing to cope.
            empty = (names == {"Boolean"} and "empty" in msg.lower()
                     and step["op"] != "boolean union")
            # A profile whose loops overlap is the sketch's fault, and the
            # extrude says so.
            bad_profile = "profile sweeps to an invalid solid" in msg
            if names & LIMIT_TYPES or empty or bad_profile:
                # A fillet, chamfer, shell, draft or hole that cannot build
                # says why; undo it and go on.
                for _ in sent:
                    self.call("undo", {}, k)
                return [], before, msg
            raise Failure("boolean_failed", f"{step['op']}: {msg}", k)
        geometry = self.call("check_geometry", {}, k)
        check = dict(answer)
        if "digest" not in check:
            check["digest"] = {"bodies": self.bodies(),
                               "delta": {"volume_mm3": 0.0}}
        try:
            check_step(step, before, check, geometry, part_volume,
                       boxes=(tight_before, self.tight_box())
                       if tight_before else None)
        except Failure as f:
            f.step = k
            raise
        return sent, check["digest"]["bodies"], None


def _deep_internal(diag):
    if isinstance(diag, dict):
        if diag.get("code") == "internal":
            return True
        return any(_deep_internal(v) for v in diag.values())
    if isinstance(diag, list):
        return any(_deep_internal(v) for v in diag)
    return False


def generate_and_run(d, seed, max_steps=8, log=None):
    """One seed: a design generated as it runs. Returns (outcome dict, the
    design as run). Outcome: {class, message, gist, step, limits[]}."""
    gen = Generator(seed)
    run = Runner(d, log)
    design = []
    limits = []
    sent_all = []
    d.request("new_project", {})
    before = []
    try:
        step = gen.base()
        n_steps = gen.rng.randint(1, max_steps)
        last = None
        for k in range(n_steps + 1):
            if k > 0:
                state = summarise(run.elements(), before)
                step = gen.next(k, state, last)
                if step is None:
                    break
            step["k"] = k
            design.append(step)
            sent, after, limit = run.run_step(k, step, before)
            if limit is not None:
                limits.append(gist("limit", limit))
                step["limit"] = limit
                continue
            sent_all.extend(sent)
            before = after
            last = step
        end_checks(run, d, sent_all, before)
        pairs = [gen.identity_pair() for _ in range(3)]
        for pair in pairs:
            identity(run, d, pair)
    except Failure as f:
        return ({"class": f.cls, "message": f.message,
                 "gist": gist(f.cls, f.message), "step": f.step,
                 "limits": limits}, design)
    return ({"class": "clean", "message": "", "gist": "", "step": None,
             "limits": limits}, design)


def run_design(d, design, log=None):
    """Replay a stored design (minimiser, corpus). Same checks; the limits
    are undone as they were."""
    run = Runner(d, log)
    sent_all = []
    before = []
    d.request("new_project", {})
    try:
        for step in design:
            k = step["k"]
            sent, after, limit = run.run_step(k, step, before)
            if limit is not None:
                continue
            sent_all.extend(sent)
            before = after
        end_checks(run, d, sent_all, before)
    except Failure as f:
        return {"class": f.cls, "message": f.message,
                "gist": gist(f.cls, f.message), "step": f.step}
    return {"class": "clean", "message": "", "gist": "", "step": None}


def end_checks(run, d, sent, final):
    """Incremental equals from scratch; undo to nothing and redo back; save
    and reopen."""
    if not sent:
        return
    incremental = run.bodies()
    # From nothing: the same calls, one batch, in a new part tab.
    tab = run.call("list_parts", {})
    here = tab["parts"][tab["active"]]["name"]
    run.call("add_part", {"name": "Rebuild"})
    run.call("set_active_part", {"part": "Rebuild"})
    try:
        answer = run.call("batch", {"calls": sent})
    except Failure as f:
        raise Failure("rebuild", f"the replay from nothing failed: "
                      f"{f.message}", f.step)
    failed = answer.get("regen", {}).get("failed", [])
    if failed:
        run.call("set_active_part", {"part": here})
        raise Failure("rebuild", "the replay from nothing failed where the "
                      "steps one at a time built: " + "; ".join(
                          f"{f.get('name')}: {f.get('error')}"
                          for f in failed))
    try:
        same_bodies(incremental, run.bodies(), 1e-9, "rebuild")
    finally:
        run.call("set_active_part", {"part": here})
    # Undo to the start, redo to the end.
    for _ in range(len(sent)):
        run.call("undo", {})
    left = run.bodies()
    if left:
        raise Failure("undo", f"undo to the start left {len(left)} bodies")
    for _ in range(len(sent)):
        run.call("redo", {})
    same_bodies(incremental, run.bodies(), 1e-9, "undo")
    # Save and reopen.
    path = getattr(d, "scratch_path", None)
    if path:
        run.call("save_project", {"path": path})
        run.call("open_project", {"path": path})
        run.call("set_active_part", {"part": here})
        same_bodies(incremental, run.bodies(), 1e-9, "reopen")


def identity(run, d, pair):
    d.request("new_project", {})
    for f in pair:
        run.call("add_feature", {"feature": f})
    bodies = {b["created_by"]: b["volume_mm3"] for b in run.bodies()}
    a, b = fid(90), fid(91)
    if a not in bodies or b not in bodies:
        raise Failure("identity", f"the pair did not make two bodies: "
                      f"{bodies}")
    va, vb = bodies[a], bodies[b]
    out = {}
    for op in ("union", "intersect", "subtract"):
        ans = run.call("add_feature", {"feature": {
            "type": "Boolean", "name": op, "id": fid(95), "op": op,
            "target_id": a, "tool_id": b}})
        failed = ans.get("regen", {}).get("failed", [])
        if failed:
            msg = failed[0].get("error", "")
            # Empty is an answer: A and B do not meet (intersect), or B
            # swallows A (subtract).
            if op in ("intersect", "subtract") and "empty" in msg.lower():
                out[op] = 0.0
            else:
                raise Failure("boolean_failed", f"{op}: {msg}")
        else:
            out[op] = sum(x["volume_mm3"] for x in run.bodies())
            if op != "union":
                # the tool body is consumed; what is left is A's result
                out[op] = sum(x["volume_mm3"] for x in run.bodies()
                              if x["created_by"] != b)
        run.call("undo", {})
    check_identity(va, vb, out["union"], out["intersect"], out["subtract"])


# --- the minimiser ------------------------------------------------------
def dependents(design, k):
    """Step k and every step that refers to it through $step."""
    out = {k}
    changed = True
    while changed:
        changed = False
        for s in design:
            if s["k"] not in out and any(dep in out for dep in s["deps"]):
                out.add(s["k"])
                changed = True
    return out


def minimise(design, failure, reproduces):
    """Cut `design` down to the steps `failure` needs. `reproduces(design)`
    replays a candidate and says whether it fails with the same class and
    gist. Drop steps (with their dependents) until none can go, then round
    the numbers to coarser grids."""
    current = list(design)
    progress = True
    while progress:
        progress = False
        for s in list(current):
            if s["k"] == 0 and len(current) > 1:
                pass  # the base can go too, if the failure does not need it
            drop = dependents(current, s["k"])
            cand = [x for x in current if x["k"] not in drop]
            if cand and len(cand) < len(current) and reproduces(cand):
                current = cand
                progress = True
                break
    for grid in (10.0, 5.0, 1.0):
        for i in range(len(current)):
            cand = copy.deepcopy(current)
            if _round_step(cand[i], grid) and reproduces(cand):
                current = cand
    return current


def _round_step(step, grid):
    """Round the plain numbers of a step's feature to `grid`; True when
    anything changed. Ids, counts and angles stay."""
    changed = [False]
    keep = {"id", "ids", "count", "sketch_id", "source_id", "angle_deg",
            "start_angle_deg", "end_angle_deg", "drill_point_deg"}

    def walk(v, key=None):
        if isinstance(v, dict):
            if any(k.startswith("$") for k in v):
                return v
            return {k: walk(x, k) for k, x in v.items()}
        if isinstance(v, list):
            return [walk(x, key) for x in v]
        if isinstance(v, float) and key not in keep and v != 0:
            r = round(v / grid) * grid
            if r == 0:
                r = grid if v > 0 else -grid
            if r != v:
                changed[0] = True
            return r
        return v

    for c in step["calls"]:
        c["params"] = walk(c["params"])
    return changed[0]


# --- the self-test ------------------------------------------------------
def self_test():
    """The checks and the minimiser against made-up answers: each planted
    failure must come out as its class."""
    problems = []

    def expect_class(cls, fn):
        try:
            fn()
        except Failure as f:
            if f.cls != cls:
                problems.append(f"expected {cls}, got {f.cls}: {f.message}")
            return
        problems.append(f"expected {cls}, nothing was raised")

    box = {"created_by": 100, "name": "Base", "volume_mm3": 1000.0,
           "bbox_min": [0, 0, 0], "bbox_max": [10, 10, 10], "solids": 1}
    grown = dict(box, volume_mm3=1010.0)
    cut = {"op": "extrude cut", "expect": "cut"}
    expect_class("wrong_way", lambda: check_step(
        cut, [box], {"regen": {"ok": True, "failed": []},
                     "digest": {"bodies": [grown],
                                "delta": {"volume_mm3": 10.0}}},
        {"valid": True}, 1000.0))
    expect_class("wrong_way", lambda: check_step(
        cut, [box], {"regen": {"ok": True, "failed": []},
                     "digest": {"bodies": [dict(box, bbox_max=[10, 10, 11],
                                                volume_mm3=990.0)],
                                "delta": {"volume_mm3": -10.0}}},
        {"valid": True}, 1000.0))
    expect_class("invalid", lambda: check_step(
        {"op": "fillet", "expect": "any"}, [box],
        {"regen": {"ok": True, "failed": []},
         "digest": {"bodies": [dict(box, volume_mm3=0.0)],
                    "delta": {"volume_mm3": -1000.0}}},
        {"valid": True}, 1000.0))
    expect_class("nonfinite", lambda: check_step(
        cut, [box], {"nonfinite": ["/digest/bodies/0/volume_mm3"]}, None, 1.0))
    expect_class("message", lambda: check_step(
        cut, [box], {"regen": {"failed": [{"name": "E1", "error":
                                           "failed"}]}}, None, 1.0))
    expect_class("identity", lambda: check_identity(1000, 500, 1400, 0, 1000))
    expect_class("identity", lambda: check_identity(1000, 500, 1400, 100,
                                                    800))
    check_identity(1000, 500, 1400, 100, 900)  # holds: must not raise
    expect_class("reopen", lambda: same_bodies(
        [box], [dict(box, volume_mm3=1000.001)], 1e-9, "reopen"))
    same_bodies([box], [dict(box)], 1e-9, "reopen")
    # The minimiser: a failure planted at step 5 of 8 that needs steps 2
    # and 5, and nothing else, comes out as exactly those two.
    design = [{"k": k, "deps": [], "op": f"s{k}", "calls": []}
              for k in range(8)]
    calls = [0]

    def reproduces(cand):
        calls[0] += 1
        ks = {s["k"] for s in cand}
        return {2, 5} <= ks
    small = minimise(design, None, reproduces)
    if [s["k"] for s in small] != [2, 5]:
        problems.append(f"minimise kept {[s['k'] for s in small]}, not "
                        f"[2, 5]")
    # ...and a step that needs another through $step goes with it.
    design[6]["deps"] = [2]
    if dependents(design, 2) != {2, 6}:
        problems.append(f"dependents(2) = {dependents(design, 2)}")
    if problems:
        for p in problems:
            print("  - " + p)
        return 1
    print(f"fuzz_design self-test: every planted failure came out as its "
          f"class; minimise kept [2, 5] of 8 in {calls[0]} replays")
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(self_test())
