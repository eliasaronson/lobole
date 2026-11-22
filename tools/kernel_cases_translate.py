#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A kernel case's recipe into RPC calls, at run time
(docs/modeling/README.md#kernel-cases; tools/kernel_cases.py runs it).

A recipe is SolidCraft's command script: `{command, params}` in order.
`Translator.command` turns each into steps of the dialect
tests/reference_parts.py runs -- `{method, params}` with `{"$ref": n}` and
`{"$pick": "edge"|"face", "at": [x, y, z]}` -- and sends them, since what a
later command becomes depends on what the earlier ones made (which body a
face is on, how many bodies the part has). Nothing translated is kept: the
recipe is the source, and `Translator.log` is what was sent.

A command or a parameter with no counterpart raises Untranslated, naming
it. Nothing is approximated: a round with a second radius is not built as a
plain one, a pick that finds no element or several is not given the
nearest. A call the app refuses raises Refusal; the runner grades it.
"""
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tests"))

import reference_parts as rp  # noqa: E402


class Untranslated(Exception):
    """The recipe uses what Loböle has no counterpart for. str(e) is the
    reason as status.json holds it: short, with no numbers of the case."""


class Refusal(Exception):
    """The app refused a call, or built the feature broken."""

    def __init__(self, message, internal=False, fits=None):
        super().__init__(message)
        self.message = message
        self.internal = internal
        # The size that fits, where a Fillet or Chamfer said one: the
        # refusal's diagnostic or the failed row's `fits`
        # (docs/modeling/README.md#the-size-that-fits).
        self.fits = fits


class Broken(Exception):
    """The translator sent a call the app could not read (an unknown or a
    missing argument): its own bug, never a result."""


# ------------------------------------------------------------------ vectors

def _sub(a, b):
    return [x - y for x, y in zip(a, b)]


def _add(a, b):
    return [x + y for x, y in zip(a, b)]


def _mul(a, s):
    return [x * s for x in a]


def _dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def _cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]]


def _unit(a):
    m = math.sqrt(_dot(a, a))
    if m < 1e-12:
        raise Untranslated("a direction of no length")
    return [x / m for x in a]


def _clean(v):
    """-0.0 as 0.0, so a frame prints the same whichever way it was
    computed."""
    return [0.0 if x == 0 else float(x) for x in v]


def perpendicular(n):
    """An in-plane U for a plane of normal n: the world axis least along n,
    made square to it."""
    n = _unit(n)
    k = min(range(3), key=lambda i: abs(n[i]))
    a = [1.0 if i == k else 0.0 for i in range(3)]
    return _clean(_unit(_sub(a, _mul(n, _dot(a, n)))))


AXES = {"X": [1.0, 0.0, 0.0], "Y": [0.0, 1.0, 0.0], "Z": [0.0, 0.0, 1.0]}
# SolidCraft's origin planes as frames (origin, x_dir, y_dir); the normal
# is x_dir x y_dir, so its XZ plane, drawn with y down, faces +Y. Never left
# to Loböle's Top/Front/Right.
PLANES = {"XY": ([0.0, 0.0, 0.0], AXES["X"], AXES["Y"]),
          "XZ": ([0.0, 0.0, 0.0], AXES["X"], [0.0, 0.0, -1.0]),
          "YZ": ([0.0, 0.0, 0.0], AXES["Y"], AXES["Z"])}


class Frame:
    def __init__(self, origin, x, y):
        self.origin = _clean(origin)
        self.x = _clean(_unit(x))
        self.y = _clean(_unit(y))
        self.normal = _clean(_unit(_cross(self.x, self.y)))

    def plane(self):
        # Loböle's in-plane V is normal x uAxis, which is this frame's y.
        return {"origin": self.origin, "normal": self.normal,
                "uAxis": self.x}

    def world(self, p):
        return _add(self.origin, _add(_mul(self.x, p[0]), _mul(self.y, p[1])))

    def shifted(self, d):
        return Frame(_add(self.origin, _mul(self.normal, d)), self.x, self.y)


# -------------------------------------------------------------- expressions

_TOKEN = re.compile(r"\s*(?:(\d+\.?\d*(?:[eE][-+]?\d+)?|\.\d+)|"
                    r"([A-Za-z_][A-Za-z0-9_]*)|(.))")


def evaluate(text, names):
    """A SolidCraft expression as a number: literals, parameters, + - * /
    and brackets, a trailing `mm` or `deg` (lengths are mm and angles
    degrees in a recipe already)."""
    tokens = []
    for num, name, op in _TOKEN.findall(text):
        if num:
            tokens.append(("n", float(num)))
        elif name in ("mm", "deg"):
            continue
        elif name:
            if name not in names:
                raise Untranslated("an expression naming what is not a "
                                   "parameter")
            tokens.append(("n", names[name]))
        elif op in "+-*/()":
            tokens.append((op, None))
        elif op.strip():
            raise Untranslated("an expression beyond + - * / and brackets")
    pos = [0]

    def peek():
        return tokens[pos[0]][0] if pos[0] < len(tokens) else None

    def take():
        pos[0] += 1
        return tokens[pos[0] - 1]

    def atom():
        k = peek()
        if k == "n":
            return take()[1]
        if k == "-":
            take()
            return -atom()
        if k == "+":
            take()
            return atom()
        if k == "(":
            take()
            v = total()
            if peek() != ")":
                raise Untranslated("an expression that does not parse")
            take()
            return v
        raise Untranslated("an expression that does not parse")

    def product():
        v = atom()
        while peek() in ("*", "/"):
            if take()[0] == "*":
                v *= atom()
            else:
                v /= atom()
        return v

    def total():
        v = product()
        while peek() in ("+", "-"):
            if take()[0] == "+":
                v += product()
            else:
                v -= product()
        return v

    v = total()
    if pos[0] != len(tokens):
        raise Untranslated("an expression that does not parse")
    return v


# ------------------------------------------------------------- the sketches

class Sketch:
    """A sketch as the recipe draws it: Loböle entities in sketch
    coordinates, and the recipe's ids for them."""

    def __init__(self, name, frame):
        self.name = name
        self.frame = frame
        self.entities = []
        self.ids = {}        # the recipe's id -> entity id
        self.kinds = {}      # "l" -> how many lines so far, for l1, l2...
        self.step = None     # the step that added it, once sent
        self.projected = False

    def add(self, entity, prefix, given=None, construction=False):
        entity = dict(entity, id=len(self.entities) + 1)
        if construction:
            entity["construction"] = True
        self.entities.append(entity)
        self.kinds[prefix] = self.kinds.get(prefix, 0) + 1
        # SolidCraft names an entity by its kind and order (l1, c1, a1)
        # unless the command gives an id.
        self.ids[given or f"{prefix}{self.kinds[prefix]}"] = entity["id"]
        return entity["id"]

    def profile(self):
        return [e for e in self.entities if not e.get("construction")
                and e["type"] != "point"]


def _ends(e):
    """An entity's two end points, or None for a closed one."""
    t = e["type"]
    if t == "line":
        return e["a"], e["b"]
    if t == "arc":
        c, r = e["center"], e["radius"]
        return tuple([c[0] + r * math.cos(math.radians(a)),
                      c[1] + r * math.sin(math.radians(a))]
                     for a in (e["start_angle_deg"], e["end_angle_deg"]))
    if t == "spline":
        p = e["poles"]
        return (p[0], p[-1]) if math.dist(p[0], p[-1]) > 1e-9 else None
    return None


def _sample(e, n=24):
    """Points along an entity, for containment tests only."""
    t = e["type"]
    if t == "line":
        return [e["a"], e["b"]]
    if t in ("circle", "arc"):
        a0, a1 = (0.0, 360.0) if t == "circle" else (
            e["start_angle_deg"], e["end_angle_deg"])
        c, r = e["center"], e["radius"]
        return [[c[0] + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
                 c[1] + r * math.sin(math.radians(a0 + (a1 - a0) * i / n))]
                for i in range(n + 1)]
    if t == "ellipse":
        c, rot = e["center"], math.radians(e.get("rotation_deg", 0.0))
        out = []
        for i in range(n + 1):
            u = 2 * math.pi * i / n
            x, y = e["r_major"] * math.cos(u), e["r_minor"] * math.sin(u)
            out.append([c[0] + x * math.cos(rot) - y * math.sin(rot),
                        c[1] + x * math.sin(rot) + y * math.cos(rot)])
        return out
    return list(e["poles"])  # a spline's hull bounds it: enough to nest by


def loops_of(entities):
    """The closed loops of a profile: [{ids, polygon}], or None when an
    entity is on no simple closed chain (an open end, a branch, curves that
    only cross), which is how SolidCraft's regions come from crossing
    curves and Loböle's do not."""
    closed, open_ = [], []
    for e in entities:
        (closed if _ends(e) is None else open_).append(e)
    loops = [{"ids": [e["id"]], "polygon": _sample(e)} for e in closed]
    tol = 1e-6
    # A branch: more or fewer than two entity ends at a joint.
    joints = [p for e in open_ for p in _ends(e)]
    for p in joints:
        if sum(1 for q in joints if math.dist(p, q) <= tol) != 2:
            return None
    left = list(open_)
    while left:
        first = left.pop(0)
        a, b = _ends(first)
        chain = [first]
        polygon = _sample(first)
        while math.dist(a, b) > tol:
            nxt = [e for e in left
                   if min(math.dist(b, p) for p in _ends(e)) <= tol]
            if len(nxt) != 1:
                return None
            e = nxt[0]
            left.remove(e)
            pts = _sample(e)
            if math.dist(b, _ends(e)[0]) > tol:
                pts = pts[::-1]
            polygon += pts[1:]
            b = pts[-1]
            chain.append(e)
        if len(chain) == 1:
            return None  # one open entity whose ends meet: not a profile
        loops.append({"ids": sorted(e["id"] for e in chain),
                      "polygon": polygon})
    return loops


def _inside(polygon, p):
    x, y = p
    inside = False
    for (x0, y0), (x1, y1) in zip(polygon, polygon[1:] + polygon[:1]):
        if (y0 > y) != (y1 > y) and \
                x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
            inside = not inside
    return inside


def nesting(loops):
    """Each loop's depth: how many other loops hold it."""
    return [sum(1 for j, other in enumerate(loops)
                if j != i and _inside(other["polygon"], loop["polygon"][0]))
            for i, loop in enumerate(loops)]


# ----------------------------------------------------------- the translator

SKIPPED = {
    # Read-only: they answer and build nothing.
    "inspect.measure", "document.inspect",
}


class Translator:
    def __init__(self, d, case_dir=None, out_dir=None):
        self.d = d
        self.case_dir = case_dir   # what `$CASE/` in a recipe path names
        self.out_dir = out_dir     # and `$OUT/`: the run's scratch
        self.reimport = None       # body names before a file.new
        self.log = []          # every step sent, as written
        self.results = []      # their answers, for $ref
        self.digest = None     # the last mutating call's
        self.names = {}        # parameters.add
        self.planes = {}       # construct.plane.offset
        self.open = None       # the sketch being drawn
        self.sketches = {}
        self.last_sketch = None
        self.bodies = {}       # the recipe's body name -> created_by
        self.features = {}     # the recipe's feature name -> its record
        self.body_serial = 0
        self.sketch_serial = 0

    # -- sending -----------------------------------------------------------

    def send(self, method, params):
        step = {"method": method, "params": params}
        self.log.append(step)
        try:
            resolved = rp.resolve(self.d, params, self.results)
        except rp.PickError as e:
            raise Untranslated(
                f"ambiguous pick: {'no' if e.count == 0 else 'several'} "
                f"{e.kind}s at the point")
        try:
            r = self.d.request(method, resolved)
        except (ConnectionError, OSError, TimeoutError):
            raise
        except Exception as e:  # noqa: BLE001 -- the app said no
            code = (getattr(e, "diagnostic", None) or {}).get("code")
            if code in ("unknown_argument", "missing_argument"):
                raise Broken(f"{method} {resolved}: {e}")
            raise Refusal(str(e), internal=code == "internal"
                          or type(e).__name__ == "NonFiniteError",
                          fits=(getattr(e, "diagnostic", None)
                                or {}).get("fits"))
        r = r if isinstance(r, dict) else {"result": r}
        self.results.append(r)
        if "digest" in r:
            self.digest = r["digest"]
        regen = r.get("regen")
        if isinstance(regen, dict) and not regen.get("ok", True):
            said = "; ".join(str(f.get("error", f))
                             for f in regen.get("failed", [])) or str(regen)
            fits = next((f["fits"] for f in regen.get("failed", [])
                         if isinstance(f, dict) and f.get("fits")), None)
            raise Refusal(said, fits=fits)
        return r

    def ref(self):
        """The step just sent, as later steps name it."""
        return {"$ref": len(self.results)}

    def feature(self, spec):
        r = self.send("add_feature", {"feature": spec})
        return r["id"]

    def live(self):
        return [b["created_by"] for b in (self.digest or {}).get("bodies",
                                                                 [])]

    # -- values ------------------------------------------------------------

    def num(self, v):
        if isinstance(v, bool):
            raise Untranslated("a number given as true or false")
        if isinstance(v, (int, float)):
            return float(v)
        if isinstance(v, str):
            return evaluate(v, self.names)
        raise Untranslated("a number given as a list or an object")

    def vec(self, v):
        if not isinstance(v, list):
            raise Untranslated("a point given as something other than "
                               "[x, y, z]")
        return [self.num(x) for x in v]

    def axis(self, v):
        if isinstance(v, str) and v in AXES:
            return list(AXES[v])
        if isinstance(v, list) and len(v) == 3:
            return _clean(_unit(self.vec(v)))
        raise Untranslated("an axis that is not X, Y, Z or a vector")

    def body(self, name):
        if name not in self.bodies:
            raise Untranslated("a body named by a later command that this "
                               "translation cannot name (a lump or a copy)")
        return self.bodies[name]

    @staticmethod
    def only(params, accepted, command):
        for k in params:
            if k not in accepted:
                raise Untranslated(f"`{command}` with `{k}`")

    # -- dispatch ----------------------------------------------------------

    def command(self, c):
        name, p = c["command"], c.get("params") or {}
        if name in SKIPPED:
            return
        if not name.startswith("sketch.") or name == "sketch.create":
            self.finish_sketch()
        fn = getattr(self, "c_" + name.replace(".", "_"), None)
        if fn is None:
            raise Untranslated(f"`{name}`")
        fn(p)

    # -- parameters and planes ---------------------------------------------

    def c_parameters_add(self, p):
        self.only(p, {"name", "expression", "unit"}, "parameters.add")
        if p.get("unit", "mm") not in ("mm", "deg", ""):
            raise Untranslated("a parameter in a unit other than mm or deg")
        self.names[p["name"]] = evaluate(str(p["expression"]), self.names)

    def c_construct_plane_offset(self, p):
        self.only(p, {"base", "offset", "name"}, "construct.plane.offset")
        self.planes[p["name"]] = self.frame(p["base"]).shifted(
            self.num(p["offset"]))

    def frame(self, plane):
        if isinstance(plane, str):
            if plane in PLANES:
                return Frame(*PLANES[plane])
            if plane in self.planes:
                return self.planes[plane]
            raise Untranslated("a sketch on a named plane that is not XY, "
                               "XZ, YZ or an offset of one")
        if isinstance(plane, dict) and set(plane) == {"origin", "x_dir",
                                                       "y_dir"}:
            x, y = self.vec(plane["x_dir"]), self.vec(plane["y_dir"])
            if abs(_dot(_unit(x), _unit(y))) > 1e-9:
                raise Untranslated("a sketch frame whose axes are not "
                                   "square")
            return Frame(self.vec(plane["origin"]), x, y)
        if isinstance(plane, dict) and set(plane) == {"face"}:
            # SolidCraft's frame on a face: the world origin dropped onto
            # its plane, the outward normal, and x along world X (on a
            # face square to Z) or along Z x normal.
            at = self.vec(plane["face"])
            r = self.d.call("query_geometry", element_type="face",
                            on_point=at)
            if r.get("count") != 1 or r["matches"][0]["kind"] != "plane":
                raise Untranslated("a sketch on a face that is not one "
                                   "planar face at the point")
            n = _unit(r["matches"][0]["normal"])
            x = AXES["X"] if abs(n[2]) > 0.9 else _unit(_cross(AXES["Z"], n))
            x = _unit(_sub(x, _mul(n, _dot(x, n))))
            return Frame(_mul(n, _dot(n, at)), x, _cross(n, x))
        raise Untranslated("a sketch plane of another form")

    # -- sketches ----------------------------------------------------------

    def c_sketch_create(self, p):
        self.only(p, {"plane", "name", "offset", "project_edges"},
                  "sketch.create")
        self.sketch_serial += 1
        frame = self.frame(p.get("plane", "XY"))
        on_face = isinstance(p.get("plane"), dict) and "face" in p["plane"]
        if p.get("project_edges") and not on_face:
            raise Untranslated("`sketch.create` with `project_edges`")
        if "offset" in p:
            frame = frame.shifted(self.num(p["offset"]))
        self.open = Sketch(p.get("name") or f"Sketch{self.sketch_serial}",
                           frame)
        # A face's own outline is projected into a sketch on it and
        # numbered with the drawn regions.
        self.open.projected = on_face and p.get("project_edges", True)

    def drawing(self, command):
        if self.open is None:
            raise Untranslated(f"`{command}` outside a sketch being drawn")
        return self.open

    def pt(self, v):
        if not isinstance(v, list) or len(v) != 2:
            raise Untranslated("a sketch point given as something other "
                               "than [x, y]")
        return [self.num(v[0]), self.num(v[1])]

    def c_sketch_finish(self, p):
        self.only(p, {"sketch"}, "sketch.finish")
        self.finish_sketch()

    def finish_sketch(self):
        s, self.open = self.open, None
        if s is None:
            return
        self.sketches[s.name] = s
        self.last_sketch = s

    def emit_sketch(self, s):
        """Send the sketch, once, when a feature first needs it."""
        if s.step is None:
            if not s.entities:
                raise Untranslated("a feature on an empty sketch")
            self.feature({"type": "Sketch", "name": s.name,
                          "plane": s.frame.plane(),
                          "entities": s.entities})
            s.step = self.ref()
        return s.step

    def c_sketch_line(self, p):
        self.only(p, {"points", "ids", "closed", "infer", "connect",
                      "construction"}, "sketch.line")
        s = self.drawing("sketch.line")
        pts = [self.pt(q) for q in p["points"]]
        if p.get("closed"):
            pts.append(pts[0])
        ids = p.get("ids") or []
        for i, (a, b) in enumerate(zip(pts, pts[1:])):
            s.add({"type": "line", "a": a, "b": b}, "l",
                  ids[i] if i < len(ids) else None,
                  bool(p.get("construction")))

    def _rectangle(self, s, lo, hi):
        (x0, y0), (x1, y1) = lo, hi
        x0, x1 = min(x0, x1), max(x0, x1)
        y0, y1 = min(y0, y1), max(y0, y1)
        c = [[x0, y0], [x1, y0], [x1, y1], [x0, y1]]
        for a, b in zip(c, c[1:] + c[:1]):
            s.add({"type": "line", "a": a, "b": b}, "l")

    def c_sketch_rectangle_two_point(self, p):
        self.only(p, {"p0", "p1"}, "sketch.rectangle.two_point")
        self._rectangle(self.drawing("sketch.rectangle.two_point"),
                        self.pt(p["p0"]), self.pt(p["p1"]))

    def c_sketch_rectangle_center(self, p):
        self.only(p, {"center", "corner"}, "sketch.rectangle.center")
        c, k = self.pt(p["center"]), self.pt(p["corner"])
        self._rectangle(self.drawing("sketch.rectangle.center"),
                        [2 * c[0] - k[0], 2 * c[1] - k[1]], k)

    def c_sketch_rectangle_rounded(self, p):
        self.only(p, {"p0", "p1", "radius"}, "sketch.rectangle.rounded")
        s = self.drawing("sketch.rectangle.rounded")
        (x0, y0), (x1, y1) = self.pt(p["p0"]), self.pt(p["p1"])
        x0, x1 = min(x0, x1), max(x0, x1)
        y0, y1 = min(y0, y1), max(y0, y1)
        r = self.num(p["radius"])
        if not 0 < 2 * r < min(x1 - x0, y1 - y0):
            raise Untranslated("a rounded rectangle whose radius leaves no "
                               "straight side")
        s.add({"type": "line", "a": [x0 + r, y0], "b": [x1 - r, y0]}, "l")
        s.add(self._arc([x1 - r, y0 + r], r, -90, 0), "a")
        s.add({"type": "line", "a": [x1, y0 + r], "b": [x1, y1 - r]}, "l")
        s.add(self._arc([x1 - r, y1 - r], r, 0, 90), "a")
        s.add({"type": "line", "a": [x1 - r, y1], "b": [x0 + r, y1]}, "l")
        s.add(self._arc([x0 + r, y1 - r], r, 90, 180), "a")
        s.add({"type": "line", "a": [x0, y1 - r], "b": [x0, y0 + r]}, "l")
        s.add(self._arc([x0 + r, y0 + r], r, 180, 270), "a")

    @staticmethod
    def _arc(center, radius, a0, a1):
        """Counter-clockwise from a0 to a1, degrees."""
        return {"type": "arc", "center": center, "radius": radius,
                "start_angle_deg": a0, "end_angle_deg": a1}

    def c_sketch_circle_center(self, p):
        self.only(p, {"center", "radius", "diameter"}, "sketch.circle.center")
        s = self.drawing("sketch.circle.center")
        r = self.num(p["radius"]) if "radius" in p else \
            self.num(p["diameter"]) / 2
        s.add({"type": "circle", "center": self.pt(p["center"]),
               "radius": r}, "c")

    def c_sketch_arc_center_point(self, p):
        self.only(p, {"center", "start", "end", "sweep", "id"},
                  "sketch.arc.center_point")
        s = self.drawing("sketch.arc.center_point")
        c, a = self.pt(p["center"]), self.pt(p["start"])
        r = math.dist(c, a)
        a0 = math.degrees(math.atan2(a[1] - c[1], a[0] - c[0]))
        if "sweep" in p:
            sweep = self.num(p["sweep"])
        else:
            b = self.pt(p["end"])
            if abs(math.dist(c, b) - r) > 1e-6:
                raise Untranslated("an arc whose end is not at its start's "
                                   "radius")
            a1 = math.degrees(math.atan2(b[1] - c[1], b[0] - c[0]))
            sweep = (a1 - a0) % 360.0 or 360.0
        if not 0 < abs(sweep) < 360.0:
            raise Untranslated("an arc of a whole turn or of none")
        lo, hi = (a0, a0 + sweep) if sweep > 0 else (a0 + sweep, a0)
        s.add(self._arc(c, r, lo, hi), "a", p.get("id"))

    def c_sketch_arc_three_point(self, p):
        self.only(p, {"start", "end", "through", "id"},
                  "sketch.arc.three_point")
        s = self.drawing("sketch.arc.three_point")
        a, b, m = (self.pt(p[k]) for k in ("start", "end", "through"))
        d = 2 * (a[0] * (b[1] - m[1]) + b[0] * (m[1] - a[1])
                 + m[0] * (a[1] - b[1]))
        if abs(d) < 1e-12:
            raise Untranslated("an arc through three points on a line")
        sq = [q[0] ** 2 + q[1] ** 2 for q in (a, b, m)]
        c = [(sq[0] * (b[1] - m[1]) + sq[1] * (m[1] - a[1])
              + sq[2] * (a[1] - b[1])) / d,
             (sq[0] * (m[0] - b[0]) + sq[1] * (a[0] - m[0])
              + sq[2] * (b[0] - a[0])) / d]
        r = math.dist(c, a)
        ang = [math.degrees(math.atan2(q[1] - c[1], q[0] - c[0]))
               for q in (a, b, m)]
        # Counter-clockwise from start to end unless the third point says
        # the arc goes the other way round.
        ccw = (ang[1] - ang[0]) % 360.0
        if (ang[2] - ang[0]) % 360.0 < ccw:
            s.add(self._arc(c, r, ang[0], ang[0] + ccw), "a", p.get("id"))
        else:
            s.add(self._arc(c, r, ang[1], ang[1] + 360.0 - ccw), "a",
                  p.get("id"))

    def _slot(self, s, c0, c1, width):
        r = width / 2
        length = math.dist(c0, c1)
        if length < 1e-9:
            raise Untranslated("a slot of no length")
        ux, uy = (c1[0] - c0[0]) / length, (c1[1] - c0[1]) / length
        nx, ny = -uy, ux
        a = math.degrees(math.atan2(uy, ux))
        s.add({"type": "line", "a": [c0[0] - nx * r, c0[1] - ny * r],
               "b": [c1[0] - nx * r, c1[1] - ny * r]}, "l")
        s.add(self._arc(c1, r, a - 90, a + 90), "a")
        s.add({"type": "line", "a": [c1[0] + nx * r, c1[1] + ny * r],
               "b": [c0[0] + nx * r, c0[1] + ny * r]}, "l")
        s.add(self._arc(c0, r, a + 90, a + 270), "a")

    def c_sketch_slot_center_to_center(self, p):
        self.only(p, {"p0", "p1", "width"}, "sketch.slot.center_to_center")
        self._slot(self.drawing("sketch.slot.center_to_center"),
                   self.pt(p["p0"]), self.pt(p["p1"]), self.num(p["width"]))

    def c_sketch_slot_overall(self, p):
        self.only(p, {"p0", "p1", "width"}, "sketch.slot.overall")
        a, b, w = self.pt(p["p0"]), self.pt(p["p1"]), self.num(p["width"])
        length = math.dist(a, b)
        if length <= w:
            raise Untranslated("a slot no longer than it is wide")
        u = [(b[0] - a[0]) / length, (b[1] - a[1]) / length]
        self._slot(self.drawing("sketch.slot.overall"),
                   [a[0] + u[0] * w / 2, a[1] + u[1] * w / 2],
                   [b[0] - u[0] * w / 2, b[1] - u[1] * w / 2], w)

    def _polygon(self, p, command, inscribed):
        self.only(p, {"center", "radius", "sides", "angle"}, command)
        s = self.drawing(command)
        c, n = self.pt(p["center"]), int(self.num(p["sides"]))
        r = self.num(p["radius"])
        # `angle` (90 unless given) points at the first vertex, or for a
        # circumscribed polygon at the middle of its first side.
        a0 = math.radians(self.num(p.get("angle", 90)))
        if not inscribed:
            r /= math.cos(math.pi / n)
            a0 += math.pi / n
        pts = [[c[0] + r * math.cos(a0 + 2 * math.pi * i / n),
                c[1] + r * math.sin(a0 + 2 * math.pi * i / n)]
               for i in range(n)]
        for a, b in zip(pts, pts[1:] + pts[:1]):
            s.add({"type": "line", "a": a, "b": b}, "l")

    def c_sketch_polygon_inscribed(self, p):
        self._polygon(p, "sketch.polygon.inscribed", True)

    def c_sketch_polygon_circumscribed(self, p):
        self._polygon(p, "sketch.polygon.circumscribed", False)

    def c_sketch_spline_control_point(self, p):
        self.only(p, {"points"}, "sketch.spline.control_point")
        s = self.drawing("sketch.spline.control_point")
        poles = [self.pt(q) for q in p["points"]]
        s.add({"type": "spline", "poles": poles,
               "degree": min(3, len(poles) - 1)}, "s")

    def c_sketch_ellipse(self, p):
        self.only(p, {"center", "major", "minor_radius"}, "sketch.ellipse")
        s = self.drawing("sketch.ellipse")
        c, m = self.pt(p["center"]), self.pt(p["major"])
        major, minor = math.dist(c, m), self.num(p["minor_radius"])
        if not major >= minor > 0:
            raise Untranslated("an ellipse whose minor radius is not the "
                               "smaller")
        s.add({"type": "ellipse", "center": c, "r_major": major,
               "r_minor": minor, "rotation_deg": math.degrees(
                   math.atan2(m[1] - c[1], m[0] - c[0]))}, "e")

    def c_sketch_point(self, p):
        self.only(p, {"point", "id"}, "sketch.point")
        self.drawing("sketch.point").add(
            {"type": "point", "p": self.pt(p["point"])}, "p", p.get("id"))

    # -- which sketch, which profile ---------------------------------------

    def sketch_of(self, p, key="sketch"):
        if key in p:
            if not isinstance(p[key], str) or p[key] not in self.sketches:
                raise Untranslated("a sketch named by something other than "
                                   "its name")
            return self.sketches[p[key]]
        if self.last_sketch is None:
            raise Untranslated("a feature with no sketch before it")
        return self.last_sketch

    def regions(self, s, profiles):
        """The Extrude/Revolve `regions` for a `profiles` pick, or None for
        the whole sketch. A region of Loböle's is a closed loop with what
        is nested in it as holes."""
        loops = loops_of(s.profile())
        if not loops:
            raise Untranslated("a profile whose regions come from crossing "
                               "or open curves")
        depth = nesting(loops)
        if profiles is None:
            return None
        nested = max(depth) > 0
        if profiles == "all":
            if nested:
                raise Untranslated("`profiles` that fill a loop's hole")
            return None
        if not isinstance(profiles, list) or not all(
                isinstance(q, dict) and set(q) == {"point"}
                for q in profiles):
            if profiles == [0] and len(loops) == 1 and not s.projected:
                return None
            raise Untranslated("`profiles` picked other than by a point")
        picked = []
        for q in profiles:
            pt = self.pt(q["point"])
            holding = [i for i, loop in enumerate(loops)
                       if _inside(loop["polygon"], pt)]
            if not holding:
                raise Untranslated("a `profiles` point in no region")
            inner = max(holding, key=lambda i: depth[i])
            if depth[inner] % 2:
                raise Untranslated("`profiles` that fill a loop's hole")
            # Its holes ride along; an island inside one of them would too,
            # and SolidCraft's region stops at the hole.
            if any(depth[j] >= depth[inner] + 2
                   and _inside(loops[inner]["polygon"],
                               loops[j]["polygon"][0])
                   for j in range(len(loops))):
                raise Untranslated("a region with an island in its hole")
            if loops[inner]["ids"] not in picked:
                picked.append(loops[inner]["ids"])
        outers = [loop["ids"] for i, loop in enumerate(loops)
                  if depth[i] % 2 == 0]
        if sorted(picked) == sorted(outers):
            return None
        return picked

    # -- bodies ------------------------------------------------------------

    def named(self, p):
        self.body_serial += 1
        return p.get("body_name") or f"Body{self.body_serial}"

    def solid(self, p, command, build):
        """A sketch-based or primitive solid in the recipe's `operation`:
        `build(mode)` sends the feature and returns its id. Returns the
        step of the feature a Pattern can repeat, or None."""
        op = p.get("operation", "new")
        if op == "new_body":
            op = "new"
        if op not in ("new", "join", "cut", "intersect"):
            raise Untranslated(f"`{command}` with an operation other than "
                               "new, join, cut or intersect")
        if "body_names" in p:
            raise Untranslated(f"`{command}` with `body_names`")
        live = self.live()
        if op == "new" or (op == "join" and not live
                           and not p.get("targets")):
            fid = build("new_body")
            self.bodies[self.named(p)] = fid
            return None
        untargeted = not p.get("targets")
        targets = list(live) if untargeted else \
            [self.body(t) for t in p["targets"]]
        if not targets:
            raise Untranslated(f"a {op} with no body to act on")
        every = sorted(targets) == sorted(live)
        if op == "cut":
            if every:
                build("cut")
                return len(self.results)
            if len(targets) > 1:
                raise Untranslated("a cut with several targets")
        elif op == "join" and (untargeted or (every and len(live) == 1)):
            # With no target named SolidCraft joins the bodies the tool
            # meets, which is what `add` does.
            build("add")
            return len(self.results)
        elif op == "intersect" and len(targets) > 1:
            raise Untranslated("an intersect with several targets")
        # Several bodies and a named target: a body of its own, and a
        # Boolean against each one named.
        tool = build("new_body")
        kind = {"join": "union", "cut": "subtract",
                "intersect": "intersect"}[op]
        for t in targets:
            tool = self.feature({"type": "Boolean", "target_id": t,
                                 "tool_id": tool, "op": kind})
        for name in [k for k, v in self.bodies.items() if v in targets]:
            self.bodies[name] = tool
        return 0

    def note(self, p, step):
        """Remember a named feature for a pattern or a mirror: `step` is
        the step of the cut or add, None for a feature that made a body, 0
        for one built as a body and a Boolean."""
        if p.get("name"):
            self.features[p["name"]] = step

    # -- primitives --------------------------------------------------------

    def _prism(self, p, command, name, frame, entities, height):
        def build(mode):
            self.feature({"type": "Sketch", "name": name,
                          "plane": frame.plane(), "entities": entities})
            return self.feature({"type": "Extrude", "name": name,
                                 "sketch_id": self.ref(),
                                 "distance": height, "mode": mode})
        self.note(p, self.solid(p, command, build))

    def c_solid_box(self, p):
        self.only(p, {"corner", "center", "length", "width", "height",
                      "body_name", "operation", "targets", "name"},
                  "solid.box")
        size = [self.num(p[k]) for k in ("length", "width", "height")]
        if "center" in p:
            if "corner" in p:
                raise Untranslated("`solid.box` with both `corner` and "
                                   "`center`")
            corner = _sub(self.vec(p["center"]), _mul(size, 0.5))
        else:
            corner = self.vec(p.get("corner", [0, 0, 0]))
        for i in range(3):
            if size[i] < 0:
                corner[i] += size[i]
                size[i] = -size[i]
        lw = [[0.0, 0.0], [size[0], 0.0], [size[0], size[1]],
              [0.0, size[1]]]
        entities = [{"type": "line", "id": i + 1, "a": a, "b": b}
                    for i, (a, b) in enumerate(zip(lw, lw[1:] + lw[:1]))]
        self._prism(p, "solid.box", p.get("name") or "Box",
                    Frame(corner, AXES["X"], AXES["Y"]), entities, size[2])

    def c_solid_cylinder(self, p):
        self.only(p, {"base", "radius", "diameter", "height", "axis",
                      "body_name", "operation", "targets", "name"},
                  "solid.cylinder")
        if "radius" in p and "diameter" in p:
            raise Untranslated("`solid.cylinder` with both `radius` and "
                               "`diameter`")
        r = self.num(p["radius"]) if "radius" in p else \
            self.num(p["diameter"]) / 2
        h = self.num(p["height"])
        n = self.axis(p.get("axis", "Z"))
        if h < 0:
            n, h = _mul(n, -1.0), -h
        u = perpendicular(n)
        frame = Frame(self.vec(p.get("base", [0, 0, 0])), u, _cross(n, u))
        self._prism(p, "solid.cylinder", p.get("name") or "Cylinder", frame,
                    [{"type": "circle", "id": 1, "center": [0.0, 0.0],
                      "radius": r}], h)

    def _turned(self, p, command, name, origin, axis, entities):
        """A section in a plane through `axis`, revolved about it."""
        frame = Frame(origin, perpendicular(axis), axis)

        def build(mode):
            self.feature({"type": "Sketch", "name": name,
                          "plane": frame.plane(), "entities": entities})
            return self.feature({
                "type": "Revolve", "name": name, "sketch_id": self.ref(),
                "axis": {"kind": "custom", "origin": frame.origin,
                         "direction": frame.y},
                "angle_deg": 360, "mode": mode})
        self.note(p, self.solid(p, command, build))

    def c_solid_sphere(self, p):
        self.only(p, {"radius", "diameter", "center", "body_name",
                      "operation", "targets", "name"}, "solid.sphere")
        r = self.num(p["radius"]) if "radius" in p else \
            self.num(p["diameter"]) / 2
        self._turned(p, "solid.sphere", p.get("name") or "Sphere",
                     self.vec(p.get("center", [0, 0, 0])), AXES["Z"],
                     [dict(self._arc([0.0, 0.0], r, -90, 90), id=1),
                      {"type": "line", "id": 2, "a": [0.0, r],
                       "b": [0.0, -r]}])

    def c_solid_cone(self, p):
        self.only(p, {"radius", "top_radius", "height", "base", "axis",
                      "body_name", "operation", "targets", "name"},
                  "solid.cone")
        r0, r1 = self.num(p["radius"]), self.num(p.get("top_radius", 0))
        h = self.num(p["height"])
        n = self.axis(p.get("axis", "Z"))
        if h < 0:
            n, h = _mul(n, -1.0), -h
        pts = [[0.0, 0.0], [r0, 0.0]] + ([[r1, h]] if r1 > 0 else []) \
            + [[0.0, h]]
        entities = [{"type": "line", "id": i + 1, "a": a, "b": b}
                    for i, (a, b) in enumerate(zip(pts, pts[1:] + pts[:1]))]
        self._turned(p, "solid.cone", p.get("name") or "Cone",
                     self.vec(p.get("base", [0, 0, 0])), n, entities)

    def c_solid_torus(self, p):
        self.only(p, {"major", "minor", "center", "body_name",
                      "operation", "targets", "name"}, "solid.torus")
        self._turned(p, "solid.torus", p.get("name") or "Torus",
                     self.vec(p.get("center", [0, 0, 0])), AXES["Z"],
                     [{"type": "circle", "id": 1,
                       "center": [self.num(p["major"]), 0.0],
                       "radius": self.num(p["minor"])}])

    # -- sketched solids ---------------------------------------------------

    def c_solid_extrude(self, p):
        self.only(p, {"sketch", "distance", "direction", "taper",
                      "through_all", "profiles", "exact_regions",
                      "operation", "targets", "body_name", "name"},
                  "solid.extrude")
        s = self.sketch_of(p)
        regions = self.regions(s, p.get("profiles"))
        spec = {"type": "Extrude", "name": p.get("name") or "Extrude"}
        direction = p.get("direction", "positive")
        if direction not in ("positive", "negative", "symmetric"):
            raise Untranslated("`solid.extrude` in a direction other than "
                               "positive, negative or symmetric")
        reverse = direction == "negative"
        if p.get("through_all"):
            if "distance" in p:
                raise Untranslated("`solid.extrude` with both `distance` "
                                   "and `through_all`")
            spec["end_mode"] = "through_all"
        else:
            distance = self.num(p["distance"])
            if distance < 0:
                distance, reverse = -distance, not reverse
            spec["distance"] = distance
        if direction == "symmetric":
            if p.get("through_all"):
                raise Untranslated("a symmetric extrude through all")
            spec["end_mode"] = "symmetric"
            reverse = False
        if reverse:
            spec["reversed"] = True
        if "taper" in p and self.num(p["taper"]) != 0:
            # SolidCraft's positive taper widens the far end; taper_deg
            # leans the walls in.
            spec["taper_deg"] = -self.num(p["taper"])
        if regions is not None:
            spec["regions"] = regions

        def build(mode):
            return self.feature(dict(spec, sketch_id=self.emit_sketch(s),
                                     mode=mode))
        self.note(p, self.solid(p, "solid.extrude", build))

    def revolve_axis(self, s, v):
        if isinstance(v, str) and v in AXES:
            return {"origin": [0.0, 0.0, 0.0], "direction": list(AXES[v])}
        if isinstance(v, str) and v in ("x", "y"):
            # The sketch's own axis, through its origin.
            return {"origin": s.frame.origin,
                    "direction": s.frame.x if v == "x" else s.frame.y}
        if isinstance(v, str) and v in s.ids:
            e = next(e for e in s.entities if e["id"] == s.ids[v])
            if e["type"] != "line":
                raise Untranslated("a revolve about a sketch curve that is "
                                   "not a line")
            a, b = s.frame.world(e["a"]), s.frame.world(e["b"])
            return {"origin": _clean(a),
                    "direction": _clean(_unit(_sub(b, a)))}
        if isinstance(v, dict) and set(v) == {"origin", "dir"}:
            return {"origin": self.vec(v["origin"]),
                    "direction": _clean(_unit(self.vec(v["dir"])))}
        raise Untranslated("a revolve about an axis of another form")

    def c_solid_revolve(self, p):
        self.only(p, {"sketch", "axis", "angle", "direction", "profiles",
                      "operation", "targets", "body_name", "name"},
                  "solid.revolve")
        s = self.sketch_of(p)
        regions = self.regions(s, p.get("profiles"))
        axis = self.revolve_axis(s, p["axis"])
        # SolidCraft turns the profile toward the side its sketch normal
        # faces, whichever way the axis points; ours turns right-handed
        # about the axis, so the axis is pointed to make those agree.
        for e in s.profile():
            w = _sub(s.frame.world(_sample(e)[1]), axis["origin"])
            radial = _sub(w, _mul(axis["direction"],
                                  _dot(w, axis["direction"])))
            if _dot(radial, radial) > 1e-12:
                if _dot(_cross(axis["direction"], radial),
                        s.frame.normal) < 0:
                    axis["direction"] = _clean(_mul(axis["direction"], -1.0))
                break
        spec = {"type": "Revolve", "name": p.get("name") or "Revolve",
                "axis": dict(axis, kind="custom"),
                "angle_deg": self.num(p.get("angle", 360))}
        direction = p.get("direction", "positive")
        if direction == "symmetric":
            spec["symmetric"] = True
        elif direction == "negative":
            spec["reversed"] = True
        elif direction != "positive":
            raise Untranslated("`solid.revolve` in a direction other than "
                               "positive, negative or symmetric")
        if regions is not None:
            spec["regions"] = regions

        def build(mode):
            return self.feature(dict(spec, sketch_id=self.emit_sketch(s),
                                     mode=mode))
        self.note(p, self.solid(p, "solid.revolve", build))

    def c_solid_loft(self, p):
        self.only(p, {"sections", "operation", "targets", "body_name",
                      "name"}, "solid.loft")
        sections = []
        for sec in p["sections"]:
            if not isinstance(sec, dict) or set(sec) != {"sketch"}:
                raise Untranslated("a loft section that is not a whole "
                                   "sketch")
            sections.append(self.sketch_of(sec))

        def build(mode):
            return self.feature({
                "type": "Loft", "name": p.get("name") or "Loft",
                "sketch_ids": [self.emit_sketch(s) for s in sections],
                "mode": mode})
        self.note(p, self.solid(p, "solid.loft", build))

    def c_solid_sweep(self, p):
        self.only(p, {"sketch", "path_sketch", "path", "orientation",
                      "operation", "targets", "body_name", "name"},
                  "solid.sweep")
        profile = self.sketch_of(p)
        path = self.sketch_of(p, "path_sketch")
        whole = sorted(e["id"] for e in path.profile())
        named = sorted(path.ids.get(i, -1) for i in p["path"])
        if named != whole:
            raise Untranslated("a sweep path that is part of its sketch")
        spec = {"type": "Sweep", "name": p.get("name") or "Sweep"}
        orientation = p.get("orientation", "perpendicular")
        if orientation == "parallel":
            spec["frame"] = "fixed"
        elif orientation != "perpendicular":
            raise Untranslated("a sweep orientation other than "
                               "perpendicular or parallel")

        def build(mode):
            return self.feature(dict(
                spec, profile_id=self.emit_sketch(profile),
                path_id=self.emit_sketch(path), mode=mode))
        self.note(p, self.solid(p, "solid.sweep", build))

    # -- files --------------------------------------------------------------

    def path(self, v, command):
        """A recipe path: `$CASE/` is the case's own directory, `$OUT/` a
        scratch directory of the run's. Any other path is not the case's."""
        if not isinstance(v, str):
            raise Untranslated(f"`{command}` with a path that is not a string")
        for key, base in (("$CASE/", self.case_dir), ("$OUT/", self.out_dir)):
            if v.startswith(key) and base:
                return os.path.join(base, v[len(key):])
        raise Untranslated(f"`{command}` with a path outside $CASE and $OUT")

    def c_file_insert_step(self, p):
        self.only(p, {"path"}, "file.insert_step")
        names = self.reimport or []
        self.reimport = None
        before = set(self.live())
        r = self.send("import_step", {"path": self.path(p["path"],
                                                        "file.insert_step"),
                                      "keep_origin": True})
        made = [b for b in (self.digest or {}).get("bodies", [])
                if b["created_by"] not in before]
        if not made:
            raise Refusal(r.get("warning") or "the import made no body")
        fid = made[0]["created_by"]
        # Every body of one import shares its created_by: a body a later
        # command names is the import's only when there is one.
        for name in names:
            if len(made) == 1:
                self.bodies[name] = fid
        self.bodies[f"Import{len(self.log)}"] = fid

    def c_file_new(self, p):
        self.only(p, {"name"}, "file.new")
        # What the recipe called its bodies, for a re-import of them.
        self.reimport = list(self.bodies)
        self.send("new_project", {})
        self.digest = {"bodies": []}
        self.bodies = {}
        self.features = {}
        self.sketches = {}
        self.last_sketch = None

    def _export(self, p, command):
        self.send("export_step", {"path": self.path(p["path"], command)})

    def c_file_export(self, p):
        self.only(p, {"path", "format"}, "file.export")
        if p.get("format", "step") not in ("step", "stp"):
            raise Untranslated(f"`file.export` as {p.get('format')}")
        self._export(p, "file.export")

    def c_file_export_step(self, p):
        self.only(p, {"path"}, "file.export_step")
        self._export(p, "file.export_step")

    # -- booleans ----------------------------------------------------------

    def c_solid_combine(self, p):
        self.only(p, {"target", "tools", "operation", "name"},
                  "solid.combine")
        op = p.get("operation", "join")
        if op not in ("join", "cut", "intersect"):
            raise Untranslated("`solid.combine` with an operation other "
                               "than join, cut or intersect")
        kind = {"join": "union", "cut": "subtract",
                "intersect": "intersect"}[op]
        target = self.body(p["target"])
        tools = [(t, self.body(t)) for t in p["tools"]]
        for name, tool in tools:
            target = self.feature({"type": "Boolean", "target_id": target,
                                   "tool_id": tool, "op": kind})
            del self.bodies[name]
            self.bodies[p["target"]] = target

    # -- blends, shells, faces ---------------------------------------------

    @staticmethod
    def picks(kind, points):
        return [{"$pick": kind, "at": [float(x) for x in pt]}
                for pt in points]

    def edge_picks(self, points):
        """Edges by a point on them. Where a seam or a tangent join passes
        through the point too, SolidCraft takes the real edge: so does
        this, and only then names the edge instead of leaving a pick."""
        out = []
        smooth = None
        for pt in points:
            pick = {"$pick": "edge", "at": [float(x) for x in pt]}
            r = self.d.call("query_geometry", element_type="edge",
                            on_point=pick["at"])
            if r.get("count", 0) > 1:
                if smooth is None:
                    smooth = {(b["created_by"], e["name"])
                              for b in self.d.call("list_elements")["bodies"]
                              for e in b["edges"] if e.get("smooth")}
                real = [m for m in r["matches"] if "element" in m
                        and (m["created_by"], m["element"]) not in smooth]
                if len(real) == 1 and len(real) < r["count"]:
                    pick = {"feature_id": real[0]["created_by"],
                            "element": real[0]["element"]}
            out.append(pick)
        return out

    def _points(self, v, command, key):
        if not isinstance(v, list) or not v or not all(
                isinstance(q, list) and len(q) == 3 for q in v):
            raise Untranslated(f"`{command}` whose `{key}` are not points")
        return [self.vec(q) for q in v]

    def c_solid_fillet(self, p):
        self.only(p, {"edges", "radius", "type", "tangent_chain", "name",
                      "body"}, "solid.fillet")
        if p.get("type", "constant") != "constant":
            raise Untranslated(f"`solid.fillet` of type `{p['type']}`")
        if p.get("tangent_chain") is False:
            raise Untranslated("`solid.fillet` with `tangent_chain` off")
        self.feature({"type": "Fillet", "name": p.get("name") or "Fillet",
                      "radius": self.num(p["radius"]),
                      "edges": self.edge_picks(self._points(
                          p["edges"], "solid.fillet", "edges"))})

    def c_solid_chamfer(self, p):
        self.only(p, {"edges", "distance", "tangent_chain", "name", "body"},
                  "solid.chamfer")
        if p.get("tangent_chain") is False:
            raise Untranslated("`solid.chamfer` with `tangent_chain` off")
        self.feature({"type": "Chamfer", "name": p.get("name") or "Chamfer",
                      "radius": self.num(p["distance"]),
                      "edges": self.edge_picks(self._points(
                          p["edges"], "solid.chamfer", "edges"))})

    @staticmethod
    def _on_body(faces):
        """The body the first picked face is on."""
        return dict(faces[0], key="feature_id")

    def c_solid_shell(self, p):
        self.only(p, {"faces", "thickness", "direction", "tangent_chain",
                      "body"}, "solid.shell")
        if p.get("direction", "inside") != "inside":
            raise Untranslated("`solid.shell` outward")
        if p.get("tangent_chain"):
            raise Untranslated("`solid.shell` with `tangent_chain`")
        spec = {"type": "Shell", "name": "Shell",
                "thickness": self.num(p["thickness"])}
        if p.get("faces"):
            faces = self.picks("face", self._points(
                p["faces"], "solid.shell", "faces"))
            spec["body_id"] = self._on_body(faces)
            spec["open_faces"] = faces
        elif "body" in p:
            spec["body_id"] = self.body(p["body"])
            spec["open_faces"] = []
        elif len(self.live()) == 1:
            spec["body_id"] = self.live()[0]
            spec["open_faces"] = []
        else:
            raise Untranslated("`solid.shell` naming neither a face nor a "
                               "body")
        self.feature(spec)

    def c_solid_offset_face(self, p):
        self.only(p, {"faces", "distance"}, "solid.offset_face")
        faces = self.picks("face", self._points(
            p["faces"], "solid.offset_face", "faces"))
        self.feature({"type": "OffsetFace", "name": "OffsetFace",
                      "body_id": self._on_body(faces), "faces": faces,
                      "distance_mm": self.num(p["distance"])})

    def c_face_delete(self, p):
        self.only(p, {"faces"}, "face.delete")
        faces = self.picks("face", self._points(
            p["faces"], "face.delete", "faces"))
        self.feature({"type": "DeleteFace", "name": "DeleteFace",
                      "body_id": self._on_body(faces), "faces": faces})

    def c_solid_draft(self, p):
        self.only(p, {"faces", "angle", "neutral"}, "solid.draft")
        if p.get("neutral") not in PLANES:
            raise Untranslated("`solid.draft` about a plane that is not "
                               "XY, XZ or YZ")
        faces = self.picks("face", self._points(
            p["faces"], "solid.draft", "faces"))
        self.feature({"type": "Draft", "name": "Draft",
                      "body_id": self._on_body(faces), "faces": faces,
                      "angle_deg": self.num(p["angle"]),
                      "neutral_plane": p["neutral"].lower()})

    # -- holes and threads -------------------------------------------------

    def c_solid_hole(self, p):
        self.only(p, {"position", "diameter", "depth", "type", "tip_angle",
                      "cb_diameter", "cb_depth", "cs_diameter", "cs_angle",
                      "thread", "name", "direction"}, "solid.hole")
        if "position" not in p:
            raise Untranslated("`solid.hole` placed by a sketch")
        at = self.vec(p["position"])
        if "direction" in p:
            normal = _mul(_unit(self.vec(p["direction"])), -1.0)
        else:
            r = self.d.call("query_geometry", element_type="face",
                            on_point=at)
            flat = [m for m in r.get("matches", []) if m["kind"] == "plane"]
            if r.get("count") != 1 or len(flat) != 1:
                raise Untranslated("a hole at a point on no one planar "
                                   "face")
            normal = flat[0]["normal"]
        kind = p.get("type", "simple")
        spec = {"type": "Hole", "name": p.get("name") or "Hole",
                "hole_kind": {"simple": "simple", "drilled": "simple",
                              "counterbore": "counterbore",
                              "countersink": "countersink"}.get(kind)}
        if spec["hole_kind"] is None:
            raise Untranslated(f"`solid.hole` of type `{kind}`")
        if "thread" in p:
            if "diameter" in p:
                raise Untranslated("`solid.hole` with both `diameter` and "
                                   "`thread`")
            spec["fit"] = {"thread": p["thread"], "kind": "tapped"}
        else:
            spec["diameter"] = self.num(p["diameter"])
        if "depth" in p:
            spec["depth"] = self.num(p["depth"])
            if kind == "drilled":
                spec["drill_point_deg"] = self.num(p.get("tip_angle", 118))
            elif "thread" in p:
                spec["drill_point_deg"] = 180  # flat, as the recipe's
        else:
            spec["through"] = True
        if kind == "counterbore":
            spec["cb_diameter"] = self.num(p["cb_diameter"])
            spec["cb_depth"] = self.num(p["cb_depth"])
        if kind == "countersink":
            d, cs = self.num(p["diameter"]), self.num(p["cs_diameter"])
            half = math.radians(self.num(p.get("cs_angle", 90)) / 2)
            spec["cs_diameter"] = cs
            spec["cs_depth"] = (cs - d) / 2 / math.tan(half)
        u = perpendicular(normal)
        frame = Frame(at, u, _cross(normal, u))
        self.feature({"type": "Sketch", "name": spec["name"],
                      "plane": frame.plane(),
                      "entities": [{"type": "point", "id": 1,
                                    "p": [0.0, 0.0]}]})
        self.feature(dict(spec, sketch_id=self.ref()))
        self.note(p, len(self.results))

    def c_solid_thread(self, p):
        self.only(p, {"face", "modeled", "designation", "length", "hand"},
                  "solid.thread")
        spec = {"type": "Thread", "name": "Thread",
                "designation": p["designation"],
                "modeled": bool(p.get("modeled", False)),
                "face": self.picks("face", [self.vec(p["face"])])[0]}
        if "length" in p:
            spec["length"] = self.num(p["length"])
        if "hand" in p:
            if p["hand"] not in ("right", "left"):
                raise Untranslated("a thread hand other than right or left")
            spec["hand"] = p["hand"]
        self.feature(spec)

    # -- patterns, mirrors, splits -----------------------------------------

    def source(self, p, command):
        """What a pattern repeats: a named feature that cut or added, or a
        body. (id, body name or None)."""
        if ("features" in p) == ("bodies" in p):
            raise Untranslated(f"`{command}` of both features and bodies, "
                               "or of neither")
        names = p.get("features") or p.get("bodies")
        if len(names) != 1:
            raise Untranslated(f"`{command}` of several features or bodies")
        if "bodies" in p:
            return self.body(names[0]), names[0]
        if names[0] not in self.features:
            raise Untranslated(f"`{command}` of a feature the recipe did "
                               "not name")
        step = self.features[names[0]]
        if step is None:
            raise Untranslated(f"`{command}` of a feature that made a body: "
                               "its copies are bodies of their own")
        if step == 0:
            raise Untranslated(f"`{command}` of a feature built as a body "
                               "and a Boolean")
        return self.results[step - 1]["id"], None

    def pattern_axis(self, v):
        if isinstance(v, str) and v in AXES:
            return {"kind": "custom", "origin": [0.0, 0.0, 0.0],
                    "direction": list(AXES[v])}
        if isinstance(v, dict) and set(v) == {"origin", "dir"}:
            return {"kind": "custom", "origin": self.vec(v["origin"]),
                    "direction": _clean(_unit(self.vec(v["dir"])))}
        raise Untranslated("a pattern about an axis of another form")

    def repeated(self, body):
        """A pattern of a body consumes it: the instances are one result,
        and its copies cannot be named apart."""
        if body:
            self.bodies[body] = self.results[-1]["id"]

    def c_solid_pattern_circular(self, p):
        self.only(p, {"features", "bodies", "axis", "count", "angle",
                      "name"}, "solid.pattern.circular")
        source, body = self.source(p, "solid.pattern.circular")
        n = int(self.num(p["count"]))
        total = self.num(p.get("angle", 360))
        # SolidCraft's angle is the whole sweep; ours the step between
        # copies. A whole turn is shared by n, an arc by its n - 1 gaps.
        step = total / n if abs(abs(total) - 360.0) < 1e-9 else \
            total / (n - 1)
        self.feature({"type": "Pattern", "name": p.get("name") or "Pattern",
                      "kind": "circular", "source_id": source,
                      "axis": self.pattern_axis(p["axis"]), "count": n,
                      "angle_deg": step})
        self.repeated(body)

    def c_solid_pattern_rectangular(self, p):
        self.only(p, {"features", "bodies", "dir1", "count1", "spacing1",
                      "name"}, "solid.pattern.rectangular")
        source, body = self.source(p, "solid.pattern.rectangular")
        self.feature({"type": "Pattern", "name": p.get("name") or "Pattern",
                      "kind": "linear", "source_id": source,
                      "axis": {"kind": "custom", "origin": [0.0, 0.0, 0.0],
                               "direction": _clean(_unit(
                                   self.vec(p["dir1"])))},
                      "count": int(self.num(p["count1"])),
                      "spacing": self.num(p["spacing1"])})
        self.repeated(body)

    def datum(self, plane, command):
        """A plane as a Split or a mirror takes it: a base plane and an
        offset, for a plane square to a world axis."""
        if isinstance(plane, str) and plane in PLANES:
            return plane.lower(), 0.0
        if isinstance(plane, dict) and set(plane) == {"origin", "normal"}:
            n = _unit(self.vec(plane["normal"]))
            o = self.vec(plane["origin"])
            for base, axis in (("yz", 0), ("xz", 1), ("xy", 2)):
                if abs(abs(n[axis]) - 1.0) < 1e-12:
                    return base, o[axis]
            raise Untranslated(f"`{command}` about a plane not square to a "
                               "world axis")
        raise Untranslated(f"`{command}` about a plane of another form")

    def c_solid_mirror(self, p):
        self.only(p, {"features", "bodies", "plane", "combine", "name"},
                  "solid.mirror")
        source, body = self.source(p, "solid.mirror")
        if body and not p.get("combine"):
            raise Untranslated("a mirror that keeps its copy a body of its "
                               "own")
        base, offset = self.datum(p["plane"], "solid.mirror")
        spec = {"type": "Pattern", "name": p.get("name") or "Mirror",
                "kind": "mirror", "source_id": source}
        if offset:
            self.feature({"type": "DatumPlane", "name": "Mirror plane",
                          "kind": "offset", "base_plane": base,
                          "offset": offset})
            spec["parent_id"] = self.ref()
        else:
            spec["base_plane"] = base
        self.feature(spec)
        self.repeated(body)

    def c_solid_split_body(self, p):
        self.only(p, {"body", "plane"}, "solid.split_body")
        base, offset = self.datum(p["plane"], "solid.split_body")
        self.feature({"type": "Split", "name": "Split",
                      "body_id": self.body(p["body"]), "knife": "plane",
                      "base_plane": base, "offset": offset})
        # Both pieces carry the Split's created_by: neither can be named.
        del self.bodies[p["body"]]
