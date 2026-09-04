"""The Designer SDK — geometry references instead of transcribed numbers.

Two layers, and the split is the whole design:

**The RPC surface is GENERATED, never typed.** `describe` already returns
every method, the parameter keys each one accepts, and every enum, and
`tests/rpc_params_gate.py` keeps that honest in both directions. So this
module writes no wrapper per RPC: `app.list_parts()`, `app.add_feature(...)`,
`app.frame(...)` all resolve through `describe` at connect time, with the
keyword arguments checked against the documented list BEFORE the round trip.
A new RPC is callable the day it lands, a renamed one fails by name, and
nothing here can drift from the server. `tests/sdk_smoke.py` fails if a
hand-written method ever shadows a generated one.

**Only what `describe` cannot express is hand-written**, which is exactly
what the SO-101 gripper dogfood was missing (docs/agent-scale-plan.md, G0):

- `Face`, `Edge`, `Frame`, `Plane`, `Body` — REFERENCES. A position stays an
  object with `.center`, `.axis`, `.normal`, `.plane()`; it never becomes a
  float the caller typed back in. The gripper's script has twelve such
  coordinates, and every offset bug in it came from one of them.
- `plane.uv(world)` / `plane.world(u, v)` — the arithmetic that put a flange
  7.57 mm off its axis, done once and correctly. Sketch UV is measured FROM
  the plane origin: `world = origin + u*U + v*V`.
- compositions that are several RPCs — `bolt_circle`, `revolve_profile`,
  `fillet(edges=predicate)`, a sketch builder that CONSTRAINS rather than
  positioning by number.
- `batch()` — a context manager over the `batch` RPC, so a run of edits is
  one transaction, one regeneration and one undo step. Handles returned
  inside it are `$ref`s, so a sketch and the extrude that names it go
  together.

Usage:

    from designer_sdk import connect
    app = connect()                      # the announced instance, or --port
    part = app.part("piston")
    part.var("bore", 86)
    top = part.cylinder(d="bore", h=40)          # a Body
    part.fillet(top.edges(kind="circle"), r=2)
    seat = part.frame("wrist_pin", top.faces(kind="cylinder")[0])
    part.check()                                 # raises on any broken feature
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from designer_remote import Designer as _Remote  # noqa: E402


class SdkError(RuntimeError):
    """A refusal from the SDK itself (never from the app)."""


# Sketch datum references, usable wherever an entity id is (the app's own
# constants — see the constraint documentation in `describe`).
DATUM_ORIGIN = 0xFFFFFF00
DATUM_AXIS_X = 0xFFFFFF01
DATUM_AXIS_Y = 0xFFFFFF02


# --------------------------------------------------------------------------
# Vectors — small, so a caller never reaches for numpy to add two points.
# --------------------------------------------------------------------------

class V(tuple):
    """A 3D vector. Immutable, prints readably, does the four things a
    position needs so a caller never has to unpack one into floats."""

    def __new__(cls, x, y=None, z=None):
        if y is None:
            x, y, z = x  # from a sequence
        return super().__new__(cls, (float(x), float(y), float(z)))

    x = property(lambda self: self[0])
    y = property(lambda self: self[1])
    z = property(lambda self: self[2])

    def __add__(self, o):
        return V(self[0] + o[0], self[1] + o[1], self[2] + o[2])

    def __sub__(self, o):
        return V(self[0] - o[0], self[1] - o[1], self[2] - o[2])

    def __mul__(self, k):
        return V(self[0] * k, self[1] * k, self[2] * k)

    __rmul__ = __mul__

    def dot(self, o):
        return sum(a * b for a, b in zip(self, o))

    def cross(self, o):
        return V(self[1] * o[2] - self[2] * o[1],
                 self[2] * o[0] - self[0] * o[2],
                 self[0] * o[1] - self[1] * o[0])

    def length(self):
        return math.sqrt(self.dot(self))

    def unit(self):
        n = self.length()
        if n < 1e-12:
            raise SdkError("cannot normalize a zero-length vector")
        return self * (1.0 / n)

    def __repr__(self):
        return f"V({self[0]:.4g}, {self[1]:.4g}, {self[2]:.4g})"


# --------------------------------------------------------------------------
# The generated RPC surface
# --------------------------------------------------------------------------

class _Rpc:
    """One documented RPC, bound to a connection.

    Keyword arguments are checked against the parameter list `describe`
    publishes, so a typo is caught here — naming the keys that method DOES
    take — instead of costing a round trip. A method documented as free-form
    (`add_constraint`) accepts anything.
    """

    def __init__(self, app, name, params, doc, free_form):
        self._app, self.name = app, name
        self.params, self.free_form = set(params), free_form
        self.__doc__ = doc
        self.__name__ = name

    def __call__(self, _timeout=None, **kw):
        """`_timeout` (seconds) for the calls that need one — a big STEP
        import runs for minutes. Underscored so it can never collide with a
        documented parameter."""
        if not self.free_form:
            unknown = sorted(k for k in kw if k not in self.params)
            if unknown:
                takes = ", ".join(sorted(self.params)) or "no parameters"
                raise SdkError(
                    f"{self.name}() got {', '.join(unknown)}; it takes "
                    f"{takes}")
        return self._app.call(self.name, _timeout=_timeout, **kw)


class App:
    """A connection to one running Designer, with its whole RPC surface.

    Every method `describe` lists is an attribute. The enums it publishes
    (constraint kinds, extrude modes, material slugs, feature types) are on
    `.enums`, read from the app rather than copied into this file.
    """

    def __init__(self, remote):
        self._remote = remote
        d = remote.call("describe")
        self._methods = {}
        for m in d["methods"]:
            self._methods[m["name"]] = _Rpc(
                self, m["name"], m.get("params", []), m.get("doc", ""),
                m.get("params_free_form", False))
        self.enums = {k: v for k, v in d.items() if k != "methods"}
        self.format_version = d.get("document_format_version")
        # Which tab the app is on, as far as this client knows. Tracked so a
        # handle can make itself active without a round trip when it already
        # is — a script that builds a part, then places it, then builds
        # another part otherwise has to remember to switch back, and gets
        # "the active document is not an assembly" when it does not.
        self._active = None

    # The generated surface. Deliberately __getattr__ rather than setattr in
    # a loop: an SDK method defined on the class always wins, and shadowing
    # is then a thing a test can detect (sdk_smoke) instead of a silent
    # override at connect time.
    def __getattr__(self, name):
        try:
            return self.__dict__["_methods"][name]
        except KeyError:
            pass
        raise AttributeError(
            f"no RPC '{name}' — the app documents "
            f"{len(self.__dict__.get('_methods', ()))} methods; "
            f"app.methods() lists them")

    def call(self, method, _timeout=None, **params):
        if _timeout is None:
            return self._remote.call(method, **params)
        return self._remote.request(method, params, timeout=_timeout)

    def methods(self, contains=""):
        """The documented method names, optionally filtered."""
        return sorted(n for n in self._methods if contains in n)

    def help(self, method):
        return self._methods[method].__doc__

    def close(self):
        self._remote.close()

    # --- tabs --------------------------------------------------------------
    def parts(self):
        return self.call("list_parts")["parts"]

    def part(self, name=None, create=True):
        """Switch to (or create) a part tab and return a `Part`."""
        return _open_tab(self, name, "part", create)

    def assembly(self, name=None, create=True):
        return _open_tab(self, name, "assembly", create)

    def new(self):
        self.call("new_project")
        return self


def _open_tab(app, name, kind, create):
    slots = app.call("list_parts")["parts"]
    hit = next((s for s in slots if s["name"] == name and s["type"] == kind),
               None) if name else None
    if hit is None and name is not None and create:
        app.call("add_part" if kind == "part" else "add_assembly", name=name)
        hit = next(s for s in app.call("list_parts")["parts"]
                   if s["name"] == name and s["type"] == kind)
    if hit is None:
        if name is None:
            hit = next((s for s in slots if s["type"] == kind), None)
        if hit is None:
            have = ", ".join(f"{s['name']} ({s['type']})" for s in slots)
            raise SdkError(f"no {kind} named {name!r}; the project has: "
                           f"{have or 'nothing'}")
    app.call("set_active_part", part=hit["id"])
    app._active = hit["id"]
    return (Part if kind == "part" else Asm)(app, hit)


def connect(port=None, host="127.0.0.1"):
    """The running app, by port or by discovery — never a guess.

    With no port: the ONE announced instance, or a refusal naming the ones it
    found. This is `generators/_lib.connect` (CLAUDE.md, "Don't guess
    ports"), which is why that helper now calls through to here.
    """
    if port is None:
        found = _Remote.instances()
        if len(found) == 1:
            port = found[0]["port"]
        elif not found:
            raise SdkError(
                "no running Designer instance announced itself; start one "
                "(tools/headless-preview.sh <port>, or the app) or pass port=")
        else:
            listed = ", ".join(f"{i['port']} ({i.get('project') or 'no project'})"
                               for i in found)
            raise SdkError(f"several instances are running, pass port=: "
                           f"{listed}")
    return App(_Remote.wait_for(int(port)))


# --------------------------------------------------------------------------
# References — the point of the whole SDK
# --------------------------------------------------------------------------

class Plane:
    """A sketch plane, carrying its own frame.

    UV is measured FROM the origin: `world = origin + u*U + v*V`, and V is
    `normal x U` — the two facts that between them account for the gripper
    dogfood's worst modelling bug (a flange 7.57 mm off its axis, because the
    plane origin was added twice). `uv()` and `world()` do that arithmetic so
    a caller never does.
    """

    def __init__(self, origin, normal, u):
        self.origin = V(origin)
        self.normal = V(normal).unit()
        u = V(u)
        u = (u - self.normal * u.dot(self.normal))
        if u.length() < 1e-9:
            u = (V(0, 1, 0) if abs(self.normal.y) < 0.9 else V(1, 0, 0))
            u = u - self.normal * u.dot(self.normal)
        self.u = u.unit()
        self.v = self.normal.cross(self.u)

    @staticmethod
    def xy(z=0.0, flip=False):
        return Plane((0, 0, z), (0, 0, -1 if flip else 1), (1, 0, 0))

    @staticmethod
    def xz(y=0.0):
        """Normal -Y so v = world +Z (not -Z) — the V-axis trap, once."""
        return Plane((0, y, 0), (0, -1, 0), (1, 0, 0))

    @staticmethod
    def yz(x=0.0):
        return Plane((x, 0, 0), (1, 0, 0), (0, 1, 0))

    def world(self, u, v=None):
        """The world point at sketch coordinates (u, v)."""
        if v is None:
            u, v = u
        return self.origin + self.u * float(u) + self.v * float(v)

    def uv(self, point):
        """The sketch coordinates of a world point (projected onto the
        plane). The inverse of `world`."""
        d = V(point) - self.origin
        return (d.dot(self.u), d.dot(self.v))

    def offset(self, distance):
        """A parallel plane `distance` along the normal."""
        return Plane(self.origin + self.normal * distance, self.normal,
                     self.u)

    def json(self):
        return {"origin": list(self.origin), "normal": list(self.normal),
                "uAxis": list(self.u)}

    def __repr__(self):
        return f"Plane(origin={self.origin}, normal={self.normal})"


class Element:
    """A face or edge, by its STABLE name — never by index."""

    def __init__(self, part, body, raw):
        self._part, self.body, self._raw = part, body, raw
        self.name = raw["name"]
        self.kind = raw.get("kind", "other")
        self.feature_id = body.created_by

    @property
    def ref(self):
        """The {feature_id, element} an RPC takes."""
        return {"feature_id": self.feature_id, "element": self.name}

    def frame(self):
        """This element's frame, from the app — the same one a mate anchored
        here would use."""
        return Frame.from_query(
            self._part, self._part.app.frame(feature_id=self.feature_id,
                                             element=self.name))

    def __repr__(self):
        return f"{type(self).__name__}({self.name!r}, {self.kind})"


class Face(Element):
    def __init__(self, part, body, raw):
        super().__init__(part, body, raw)
        self.area = raw.get("area", 0.0)
        self.centroid = V(raw["centroid"]) if "centroid" in raw else None
        self.normal = V(raw["normal"]) if "normal" in raw else None
        self.axis = V(raw["axis"]) if "axis" in raw else None
        self.radius = raw.get("radius")

    @property
    def diameter(self):
        return None if self.radius is None else 2.0 * self.radius

    def plane(self, offset=0.0, u=None):
        """This face as a sketch plane: origin at its centroid, normal its
        outward normal. `offset` lifts it along that normal."""
        if self.kind != "plane":
            raise SdkError(
                f"face {self.name!r} is a {self.kind}, not planar — "
                f"`.frame()` gives a cylinder its axis frame")
        p = Plane(self.centroid, self.normal, u or _any_perp(self.normal))
        return p.offset(offset) if offset else p


class Edge(Element):
    def __init__(self, part, body, raw):
        super().__init__(part, body, raw)
        self.length = raw.get("length", 0.0)
        self.midpoint = V(raw["midpoint"]) if "midpoint" in raw else None
        self.radius = raw.get("radius")

    @property
    def center(self):
        """A circular edge's centre — asked of the app, not guessed from the
        midpoint (which is a point ON the circle)."""
        if self.kind not in ("circle", "arc"):
            raise SdkError(f"edge {self.name!r} is a {self.kind}; only a "
                           f"circle or arc has a centre")
        return self.frame().origin


class Frame:
    """A named datum frame (v86) or the frame of an element/occurrence."""

    def __init__(self, part, id, name, origin, x, y, z, space="part"):
        self.part, self.id, self.name = part, id, name
        self.origin, self.x, self.y, self.z = (V(origin), V(x), V(y), V(z))
        self.space = space

    @staticmethod
    def from_query(part, r, id=None, name=None):
        return Frame(part, id, name or r.get("source", ""), r["origin"],
                     r["x"], r["y"], r["z"], r.get("space", "part"))

    def plane(self, offset=0.0):
        """This frame as a sketch plane (its Z is the normal, its X the U)."""
        p = Plane(self.origin, self.z, self.x)
        return p.offset(offset) if offset else p

    def __repr__(self):
        return f"Frame({self.name!r}, origin={self.origin})"


class Body:
    """One body of a part. Its faces and edges are fetched the first time
    they are asked for, and only for THIS body.

    Deliberately lazy and per-body: describing a face is an OpenCASCADE
    surface analysis, so a part carrying a derived 197-body import has ~18k
    of them and asking for all of them at once is refused by the app (and
    took 172 s before it was bounded). The body LIST is cheap; the geometry
    is not.
    """

    def __init__(self, part, index, raw):
        self._part, self._raw, self.index = part, raw, index
        self.name = raw.get("name", "")
        self.volume = raw.get("volume", 0.0)
        self.reference = raw.get("reference", False)
        self.bbox = (V(raw["bbox_min"]), V(raw["bbox_max"])) \
            if "bbox_min" in raw else None
        self._els = None

    def _load(self):
        if self._els is None:
            r = self._part.app.list_elements(body=self.index)["bodies"][0]
            self._els = r
            self._faces = [Face(self._part, self, f) for f in r["faces"]]
            self._edges = [Edge(self._part, self, e) for e in r["edges"]]
        return self._els

    @property
    def created_by(self):
        return self._load()["created_by"]

    def faces(self, kind=None, where=None, normal=None, radius=None,
              tol=0.01, radius_tol=0.05):
        """Faces, filtered. `normal=(0,0,1)` keeps the ones pointing that
        way; `where` is any predicate over a Face."""
        self._load()
        out = self._faces
        if kind:
            out = [f for f in out if f.kind == kind]
        if normal is not None:
            n = V(normal).unit()
            out = [f for f in out if f.normal and f.normal.dot(n) > 1 - tol]
        if radius is not None:
            out = [f for f in out if f.radius is not None
                   and abs(f.radius - radius) < radius_tol]
        if where:
            out = [f for f in out if where(f)]
        return out

    def edges(self, kind=None, where=None, radius=None, circular=None,
              tol=0.05):
        """Edges, filtered.

        `circular=True` means "a round rim" — circle OR arc. Whether the
        kernel represents a hole's rim as one circle or as two half arcs is
        an implementation detail of the file it came from, and asking for
        `kind="circle"` silently misses the arc form: the Pi camera's four
        mounting holes are 24 arcs, and a filter written for circles found
        none of them and drilled nothing.

        `tol` is a real tolerance in millimetres (0.05 by default), not a
        float-comparison epsilon: an imported radius is rarely exact.
        """
        self._load()
        out = self._edges
        if circular:
            out = [e for e in out if e.kind in ("circle", "arc")]
        if kind:
            out = [e for e in out if e.kind == kind]
        if radius is not None:
            out = [e for e in out
                   if e.radius is not None and abs(e.radius - radius) < tol]
        if where:
            out = [e for e in out if where(e)]
        return out

    def face(self, **kw):
        """Exactly one face matching, or a refusal that says how many there
        were — never the first of several."""
        return _only(self.faces(**kw), "face", kw)

    def edge(self, **kw):
        return _only(self.edges(**kw), "edge", kw)

    def __repr__(self):
        n = "unread" if self._els is None else f"{len(self._faces)} faces"
        return f"Body({self.name!r}, {n})"


def _only(items, what, kw):
    if len(items) == 1:
        return items[0]
    filt = ", ".join(f"{k}={v!r}" for k, v in kw.items()) or "no filter"
    if not items:
        raise SdkError(f"no {what} matched ({filt})")
    raise SdkError(
        f"{len(items)} {what}s matched ({filt}) — narrow it, or use the "
        f"plural form and say which: {[i.name for i in items[:6]]}")


def _any_perp(n):
    n = V(n)
    a = V(0, 1, 0) if abs(n.y) < 0.9 else V(1, 0, 0)
    return (a - n * a.dot(n)).unit()


# --------------------------------------------------------------------------
# Sketches
# --------------------------------------------------------------------------

class SketchBuilder:
    """Entities collected on a plane, then committed as one feature.

    Positions are sketch UV. Anything taken from geometry goes through
    `plane.uv(...)` or a projection, so a world coordinate is never typed in
    twice. `radius=` accepts an expression string, which is how a dimension
    the spec names stays a variable instead of a literal.
    """

    def __init__(self, part, plane, name, face_ref=None):
        self.part, self.plane, self.name = part, plane, name
        self.entities, self.projections = [], []
        self.face_ref = face_ref
        self.id = None
        # Nothing here re-implements the expression language: a coordinate
        # or radius given as a string rides on the entity as the app's own
        # `<field>_expr` (v38 — `ax`/`ay`/`bx`/`by` on a line, `cx`/`cy`/`r`
        # on a circle or arc, `x`/`y` on a point), and the DOCUMENT evaluates
        # it on every regeneration. A field name the entity does not have is
        # refused by the app rather than dropped.
        self._pending_exprs = []  # (entity index, field, expression)

    # --- primitives -------------------------------------------------------
    def point(self, at):
        n = len(self.entities)
        self.entities.append(
            {"type": "point", "p": list(_uv(self, at, n, 0, "point"))})
        self._attach_exprs(n)
        return self

    def line(self, a, b, construction=False):
        n = len(self.entities)
        e = {"type": "line", "a": list(_uv(self, a, n, 0, "line")),
             "b": list(_uv(self, b, n, 1, "line"))}
        if construction:
            e["construction"] = True
        self.entities.append(e)
        self._attach_exprs(n)
        return self

    def circle(self, center, d=None, r=None, construction=False):
        """A circle. Give `d` (diameter) or `r`; either may be an EXPRESSION
        string, which keeps the dimension a variable."""
        if (d is None) == (r is None):
            raise SdkError("circle takes exactly one of d= (diameter) or "
                           "r= (radius)")
        n = len(self.entities)
        e = {"type": "circle",
             "center": list(_uv(self, center, n, 0, "circle"))}
        expr = d if isinstance(d, str) else (r if isinstance(r, str) else None)
        if expr is not None:
            # The app's own field is `r_expr` (a RADIUS), so a diameter given
            # as an expression is halved in the expression itself rather than
            # in python — the document stays the one thing that evaluates it.
            e["radius"] = 0.0
            self._pending_exprs.append(
                (n, "r", f"({expr}) / 2" if d is not None else expr))
        else:
            e["radius"] = float(d) / 2.0 if d is not None else float(r)
        if construction:
            e["construction"] = True
        self.entities.append(e)
        self._attach_exprs(n)
        return self

    def _attach_exprs(self, n):
        """Move this entity's pending expressions onto its JSON."""
        rest = []
        for ent, field, expr in self._pending_exprs:
            if ent == n:
                self.entities[n][field + "_expr"] = expr
            else:
                rest.append((ent, field, expr))
        self._pending_exprs = rest

    def rect(self, c1, c2):
        self.entities.append({"type": "rect", "c1": list(_uv(self, c1)),
                              "c2": list(_uv(self, c2))})
        return self

    def centered_rect(self, center, w, h):
        cu, cv = _uv(self, center)
        return self.rect((cu - w / 2.0, cv - h / 2.0),
                         (cu + w / 2.0, cv + h / 2.0))

    def polyline(self, points, close=True):
        """A chain of lines. `close` joins the last point to the first, which
        is what makes it a profile an extrude can use."""
        pts = list(points)
        if len(pts) < 2:
            raise SdkError("a polyline needs at least two points")
        # The RAW points go to line(), not evaluated ones: an expression
        # coordinate has to reach line() to be recorded as a constraint.
        # Pre-converting here is what silently froze a parametric profile
        # into the numbers its variables happened to hold.
        for a, b in zip(pts, pts[1:]):
            self.line(a, b)
        # Compared as GIVEN — two coordinates written the same way are the
        # same point, and an expression cannot be evaluated here (the
        # document evaluates it, not this SDK).
        if close and tuple(pts[0]) != tuple(pts[-1]):
            self.line(pts[-1], pts[0])
        return self

    def polygon(self, center, n, r=None, across_flats=None, rotate=0.0):
        """A regular n-gon. `r` is the circumradius; `across_flats` is the
        spanner size, converted rather than approximated."""
        if (r is None) == (across_flats is None):
            raise SdkError("polygon takes one of r= or across_flats=")
        if across_flats is not None:
            r = (across_flats / 2.0) / math.cos(math.pi / n)
        cu, cv = _uv(self, center)
        pts = [(cu + r * math.cos(rotate + 2 * math.pi * i / n),
                cv + r * math.sin(rotate + 2 * math.pi * i / n))
               for i in range(n)]
        return self.polyline(pts, close=True)

    def slot(self, a, b, width):
        """A rounded slot between two centres — two lines and two arcs, so it
        is a real closed profile rather than a stadium approximation."""
        (au, av), (bu, bv) = _uv(self, a), _uv(self, b)
        dx, dy = bu - au, bv - av
        n = math.hypot(dx, dy)
        if n < 1e-9:
            raise SdkError("a slot's two centres coincide")
        rad = width / 2.0
        px, py = -dy / n * rad, dx / n * rad
        self.line((au + px, av + py), (bu + px, bv + py))
        self.line((au - px, av - py), (bu - px, bv - py))
        ang = math.atan2(dy, dx)
        self.entities.append({"type": "arc", "center": [bu, bv],
                              "radius": rad, "start_angle": ang - math.pi / 2,
                              "end_angle": ang + math.pi / 2})
        self.entities.append({"type": "arc", "center": [au, av],
                              "radius": rad, "start_angle": ang + math.pi / 2,
                              "end_angle": ang + 3 * math.pi / 2})
        return self

    def arc_through(self, start, mid, end):
        """An arc through three points, in the direction that passes through
        `mid` — the check every scratch script used to get wrong."""
        (ax, ay), (bx, by), (cx, cy) = (_uv(self, start), _uv(self, mid),
                                        _uv(self, end))
        d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by))
        if abs(d) < 1e-12:
            raise SdkError("arc_through: the three points are collinear")
        ux = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) * (cy - ay)
              + (cx * cx + cy * cy) * (ay - by)) / d
        uy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx)
              + (cx * cx + cy * cy) * (bx - ax)) / d
        r = math.hypot(ax - ux, ay - uy)
        angs = [math.atan2(p[1] - uy, p[0] - ux)
                for p in ((ax, ay), (bx, by), (cx, cy))]
        tau = 2 * math.pi
        if (angs[1] - angs[0]) % tau < (angs[2] - angs[0]) % tau:
            s, e = angs[0], angs[2]
        else:
            s, e = angs[2], angs[0]
        self.entities.append({"type": "arc", "center": [ux, uy], "radius": r,
                              "start_angle": s, "end_angle": e})
        return self

    # --- geometry that comes from the model -------------------------------
    def project(self, *elements):
        """Project faces/edges of existing bodies into this sketch as live
        construction geometry. A projected hole edge is a circle, and a Hole
        takes its centre — so a hole lands on another body's hole with no
        coordinate copied anywhere."""
        for e in elements:
            for one in (e if isinstance(e, (list, tuple)) else [e]):
                self.projections.append(
                    dict(one.ref, kind="edge" if isinstance(one, Edge)
                         else "face"))
        return self

    # --- commit -----------------------------------------------------------
    def json(self):
        f = {"type": "Sketch", "name": self.name, "plane": self.plane.json(),
             "entities": self.entities}
        if self.projections:
            f["external"] = self.projections
        if self.face_ref:
            f["face_ref"] = self.face_ref
        return f

    def add(self):
        """Commit; returns a `Sketch` handle."""
        r = self.part._add(self.json())
        return Sketch(self.part, r["id"], self)


class Sketch:
    """A committed sketch: its feature id, plus the plane it lives on."""

    def __init__(self, part, id, builder):
        self.part, self.id, self.plane = part, id, builder.plane
        self.name = builder.name

    def constrain(self, kind, refs, value=None, expr=None):
        kw = {"sketch_id": self.id, "kind": kind, "refs": refs}
        if value is not None:
            kw["value"] = value
        if expr is not None:
            kw["expr"] = expr
        self.part.app.add_constraint(**kw)
        return self

    def entities(self):
        """This sketch's entities as the document holds them — including the
        construction geometry a projection produced."""
        for f in self.part.state()["features"]:
            if f.get("id") == self.id:
                return f.get("entities", [])
        return []

    def projected_centers(self):
        """The distinct centres of the circles and arcs this sketch's
        projections produced, in sketch UV. This is how a hole pattern is
        taken from another body without a number being read off a screen."""
        seen, out = set(), []
        for e in self.entities():
            if e.get("type") in ("circle", "arc") and "center" in e:
                key = (round(e["center"][0], 4), round(e["center"][1], 4))
                if key not in seen:
                    seen.add(key)
                    out.append(key)
        return out

    def __repr__(self):
        return f"Sketch({self.name!r}, id={self.id})"


# Which entity field each coordinate is, per entity kind and point index.
_COORD_FIELDS = {
    "point": {0: ("x", "y")},
    "line": {0: ("ax", "ay"), 1: ("bx", "by")},
    "circle": {0: ("cx", "cy")},
    "arc": {0: ("cx", "cy")},
}


def _uv(builder, at, entity=None, point=0, kind=None):
    """A sketch position: a (u, v) pair, or anything with a world position —
    a Face, an Edge, a Frame, a V — projected onto the builder's plane. This
    is the one place a world coordinate becomes a sketch coordinate.

    A coordinate may be an EXPRESSION string. It is evaluated against the
    document to place the entity, and — when `entity` says which entity and
    point it belongs to — recorded so `.add()` can pin it with a real
    dimensional constraint.
    """
    if isinstance(at, (Face,)):
        return builder.plane.uv(at.centroid)
    if isinstance(at, Edge):
        return builder.plane.uv(at.center if at.kind in ("circle", "arc")
                                else at.midpoint)
    if isinstance(at, Frame):
        return builder.plane.uv(at.origin)
    if isinstance(at, V) or (isinstance(at, (tuple, list)) and len(at) == 3):
        return builder.plane.uv(V(at))
    u, v = at
    out = []
    fields = _COORD_FIELDS.get(kind or "", {}).get(point, (None, None))
    for field, c in zip(fields, (u, v)):
        if isinstance(c, str):
            if entity is None or field is None:
                raise SdkError(
                    f"{c!r}: an expression can only drive a coordinate the "
                    f"app makes expression-driven (a point, a line endpoint, "
                    f"a circle or arc centre) — pass a number here")
            builder._pending_exprs.append((entity, field, c))
            out.append(0.0)  # the document overwrites this from the expression
        else:
            out.append(float(c))
    return (out[0], out[1])


# --------------------------------------------------------------------------
# A part tab
# --------------------------------------------------------------------------

class _Tab:
    def __init__(self, app, slot):
        self.app, self.slot = app, slot
        self.name, self.id = slot["name"], slot["id"]

    def activate(self):
        """Make this tab the active one. Free when it already is: the client
        knows which tab it last switched to."""
        if self.app._active != self.id:
            self.app.set_active_part(part=self.id)
            self.app._active = self.id
        return self

    def __repr__(self):
        return f"{type(self).__name__}({self.name!r})"


class Part(_Tab):
    def _add(self, feature):
        self.activate()
        return self.app.add_feature(feature=feature)

    # --- parameters --------------------------------------------------------
    def var(self, name, expression, comment=""):
        """A named parameter. Every dimension the spec names should be one:
        that is what makes the document parametric rather than a script's
        output."""
        self.app.set_variable(name=name, expression=str(expression),
                              comment=comment, scope="part")
        return self

    def vars(self):
        return {v["name"]: v for v in self.app.list_variables()["variables"]}

    def value_of(self, name):
        """A variable's current value, as the DOCUMENT holds it.

        Deliberately not an expression evaluator: the document owns the
        expression language, and a second one here would be exactly the
        duplication this SDK is built to avoid. Anywhere a NUMBER is needed
        (filtering faces by diameter, say), pass a number or a variable
        name.
        """
        if not isinstance(name, str):
            return float(name)
        for v in self.app.list_variables()["variables"]:
            if v["name"] == name:
                return float(v["value"])
        raise SdkError(
            f"'{name}' is not a variable of this document. A number is "
            f"needed here, and the document — not this SDK — evaluates "
            f"expressions; pass a number or a variable name")

    # --- sketches ----------------------------------------------------------
    def sketch(self, plane=None, name="Sketch", on=None):
        """A sketch builder. `on=<Face>` puts it ON that face (so it moves
        with the body); `plane=` takes a Plane, a Frame or a Face."""
        if on is not None:
            return SketchBuilder(self, on.plane(), name, face_ref=on.ref)
        if isinstance(plane, Frame):
            plane = plane.plane()
        elif isinstance(plane, Face):
            plane = plane.plane()
        elif plane is None:
            plane = Plane.xy()
        return SketchBuilder(self, plane, name)

    # --- features ----------------------------------------------------------
    def extrude(self, sketch, distance, mode="new_body", name=None, **kw):
        f = {"type": "Extrude", "name": name or f"Extrude {sketch.name}",
             "sketch_id": _id(sketch), "mode": mode}
        _dim(f, "distance", distance)
        f.update(kw)
        return self._body_from(self._add(f))

    def cut(self, sketch, distance, **kw):
        return self.extrude(sketch, distance, mode="cut", **kw)

    def revolve(self, sketch, axis="z", angle=360.0, mode="new_body",
                name=None, **kw):
        f = {"type": "Revolve", "name": name or f"Revolve {sketch.name}",
             "sketch_id": _id(sketch), "mode": mode,
             "axis": axis if isinstance(axis, dict) else {"kind": axis}}
        _dim(f, "angle_deg", angle)
        f.update(kw)
        return self._body_from(self._add(f))

    def revolve_profile(self, points, plane=None, axis="z", angle=360.0,
                        name="Revolved", **kw):
        """A closed profile and the revolve of it, in one call — the shape a
        turned part (a piston, a bushing, a pulley) is actually described
        in."""
        sk = self.sketch(plane or Plane.xz(), name=f"{name} profile")
        sk.polyline(points, close=True)
        return self.revolve(sk.add(), axis=axis, angle=angle, name=name, **kw)

    def box(self, position=(0, 0, 0), w=1.0, h=1.0, d=1.0, name="Box"):
        f = {"type": "Box", "name": name, "position": list(V(position))}
        _dim(f, "width", w)
        _dim(f, "height", h)
        _dim(f, "depth", d)
        return self._body_from(self._add(f))

    def cylinder(self, center=(0, 0, 0), direction=(0, 0, 1), d=None, r=None,
                 h=1.0, name="Cylinder"):
        if (d is None) == (r is None):
            raise SdkError("cylinder takes one of d= or r=")
        f = {"type": "Cylinder", "name": name, "center": list(V(center)),
             "direction": list(V(direction))}
        _dim(f, "radius", (f"({d}) / 2" if isinstance(d, str) else
                           (d / 2.0 if d is not None else r)))
        _dim(f, "height", h)
        return self._body_from(self._add(f))

    def sphere(self, center=(0, 0, 0), d=None, r=None, name="Sphere"):
        if (d is None) == (r is None):
            raise SdkError("sphere takes one of d= or r=")
        f = {"type": "Sphere", "name": name, "center": list(V(center))}
        _dim(f, "radius", (f"({d}) / 2" if isinstance(d, str) else
                           (d / 2.0 if d is not None else r)))
        return self._body_from(self._add(f))

    def hole(self, sketch, d, depth=None, through=False, kind="simple", **kw):
        """Holes at a sketch's points and at its circles' centres — so a
        sketch that PROJECTED another body's hole edges drills exactly
        there."""
        f = {"type": "Hole", "name": kw.pop("name", "Hole"),
             "sketch_id": _id(sketch), "hole_kind": kind}
        _dim(f, "diameter", d)
        if through:
            f["through"] = True
        if depth is not None:
            _dim(f, "depth", depth)
        f.update(kw)
        return self._add(f)

    def fillet(self, edges, r, name="Fillet"):
        return self._blend("Fillet", edges, r, name)

    def chamfer(self, edges, r, name="Chamfer"):
        return self._blend("Chamfer", edges, r, name)

    def _blend(self, type_, edges, r, name):
        edges = list(edges) if isinstance(edges, (list, tuple)) else [edges]
        if not edges:
            raise SdkError(f"{type_.lower()} got no edges — a filter that "
                           f"matched nothing is a mistake, not a no-op")
        f = {"type": type_, "name": name,
             "edges": [e.ref for e in edges]}
        _dim(f, "radius", r)
        return self._add(f)

    def frame(self, name, of=None, origin=None, z=(0, 0, 1), x=(1, 0, 0),
              parent=None, offset=(0, 0, 0), rotate=(0, 0, 0), flip=False):
        """A named datum frame (v86). `of=` one element, or several circular
        ones for a bolt circle; `parent=` another frame; otherwise explicit.

        This is the thing a mate anchors to BY NAME, so the assembly follows
        the geometry instead of a number someone read off it.
        """
        if of is not None:
            els = of if isinstance(of, (list, tuple)) else [of]
            f = {"type": "Frame", "name": name, "kind": "from_element",
                 "elements": [e.ref for e in els]}
            if flip:
                f["flip"] = True
        elif parent is not None or any(offset) or any(rotate):
            f = {"type": "Frame", "name": name, "kind": "offset",
                 "parent_id": _id(parent) if parent is not None else 0,
                 "rotation_deg": list(rotate)}
            _dim3(f, "offset", offset)
        else:
            f = {"type": "Frame", "name": name, "kind": "explicit",
                 "z_axis": list(V(z)), "x_axis": list(V(x))}
            _dim3(f, "origin", origin or (0, 0, 0))
        fid = self._add(f)["id"]
        return Frame.from_query(self, self.app.frame(frame=fid), id=fid,
                                name=name)

    def frames(self):
        return [f for f in self.state()["features"] if f["type"] == "Frame"]

    def get_frame(self, name):
        return Frame.from_query(self, self.app.frame(frame=name), name=name)

    def pattern(self, source, count, linear=None, circular=None, spacing=None,
                angle=360.0, name="Pattern"):
        """A linear or circular pattern of a feature. `linear=` an axis and
        `spacing=`; `circular=` an axis and `angle=`."""
        f = {"type": "Pattern", "name": name, "source_id": _id(source)}
        if linear is not None:
            f.update({"kind": "linear",
                      "axis": linear if isinstance(linear, dict)
                      else {"kind": linear}, "count": int(count)})
            _dim(f, "spacing", spacing)
        elif circular is not None:
            f.update({"kind": "circular",
                      "axis": circular if isinstance(circular, dict)
                      else {"kind": circular}, "count": int(count)})
            _dim(f, "angle_deg", angle)
        else:
            raise SdkError("pattern takes linear= or circular= (an axis)")
        return self._add(f)

    def bolt_circle(self, plane, n, pcd, d, start_angle=0.0, name="Bolt holes",
                    through=True, depth=None):
        """N holes on a pitch circle, and the frame that names the pattern.

        Returns (hole_feature, frame): the frame is centred on the pattern
        with X toward the first hole, which is what an assembly mates to —
        so the plate can move and the mate follows.
        """
        sk = self.sketch(plane, name=f"{name} centres")
        for i in range(n):
            a = start_angle + 2 * math.pi * i / n
            sk.point((pcd / 2.0 * math.cos(a), pcd / 2.0 * math.sin(a)))
        s = sk.add()
        hole = self.hole(s, d=d, through=through, depth=depth, name=name)
        bores = self.body().faces(kind="cylinder",
                                  where=lambda f: abs((f.radius or 0) * 2
                                                      - _num(d, self)) < 0.05)
        fr = None
        if len(bores) >= 2:
            fr = self.frame(f"{name} centre", of=bores[:n])
        return hole, fr

    # --- reading the model -------------------------------------------------
    def state(self):
        self.activate()
        return self.app.get_state()

    def bodies(self, reference=None):
        self.activate()
        """This part's bodies. Cheap: the LIST comes from get_state, and each
        body's faces and edges are read only when asked for. `reference=`
        filters ghosts in or out."""
        out = [Body(self, i, b)
               for i, b in enumerate(self.app.get_state()["bodies"])]
        if reference is not None:
            out = [b for b in out if b.reference == reference]
        return out

    def body(self, name=None, index=None):
        bs = self.bodies()
        if index is not None:
            return bs[index]
        if name is not None:
            hit = [b for b in bs if b.name == name]
            return _only(hit, "body", {"name": name})
        if len(bs) != 1:
            raise SdkError(
                f"this part has {len(bs)} bodies — say which: "
                f"{[b.name for b in bs]}")
        return bs[0]

    def faces(self, **kw):
        return [f for b in self.bodies() for f in b.faces(**kw)]

    def edges(self, **kw):
        return [e for b in self.bodies() for e in b.edges(**kw)]

    def volume(self):
        self.activate()
        return self.app.mass_properties()["total"]["volume"]

    def bbox(self):
        bs = [b for b in self.app.get_state()["bodies"]
              if not b.get("reference")]
        lo = V([min(b["bbox_min"][i] for b in bs) for i in range(3)])
        hi = V([max(b["bbox_max"][i] for b in bs) for i in range(3)])
        return lo, hi

    # --- the standing check ------------------------------------------------
    def check(self, warn=True):
        self.activate()
        """Raise if any feature is broken; print advisories. The scripted
        equivalent of looking at the tree — every generator used to hand-roll
        this and most scratch scripts forgot to."""
        st = self.app.get_state()
        bad = [f for f in st["features"] if f.get("error")]
        if bad:
            raise SdkError("; ".join(f"{f['name']}: {f['error']}"
                                     for f in bad))
        if warn:
            for f in st["features"]:
                if f.get("warning"):
                    print(f"  warning — {f['name']}: {f['warning']}")
        return self

    # --- transactions ------------------------------------------------------
    def batch(self):
        """One transaction: `with part.batch() as b:` collects calls and
        sends them as one `batch` RPC — one regeneration, one undo step, and
        a rollback if any of them fails."""
        return Batch(self.app)

    def _body_from(self, r):
        """A feature response, as the Body it created (looked up after the
        regeneration, so its faces and edges are the real ones)."""
        fid = r["id"]
        for b in self.bodies(reference=False):
            if b.created_by == fid:
                return b
        return None


# --------------------------------------------------------------------------
# An assembly tab
# --------------------------------------------------------------------------

class Occurrence:
    def __init__(self, asm, raw):
        self.asm, self.id, self.name = asm, raw["id"], raw["name"]
        self.part_id = raw.get("part_id")

    def frame(self, name=None):
        kw = {"occurrence": self.id}
        if name is not None:
            kw["frame"] = name
        return Frame.from_query(self.asm, self.asm.app.frame(**kw), name=name)

    def __repr__(self):
        return f"Occurrence({self.name!r}, id={self.id})"


class Asm(_Tab):
    def place(self, part, at=None, fixed=False, name=None):
        """Place a part. `at` is a world position; leave it out and mate it
        instead — which is the point."""
        self.activate()
        kw = {"part": part.name if isinstance(part, Part) else part,
              "fixed": fixed}
        if name:
            kw["name"] = name
        if at is not None:
            kw["transform"] = {"position": list(V(at)),
                               "orientation": [0, 0, 0, 1]}
        r = self.app.add_occurrence(**kw)
        return Occurrence(self, dict(r, name=r.get("name", name or "")))

    def occurrences(self):
        self.activate()
        return [Occurrence(self, o)
                for o in self.app.list_occurrences()["occurrences"]]

    def occurrence(self, name):
        return _only([o for o in self.occurrences() if o.name == name],
                     "occurrence", {"name": name})

    # --- mates: a connector is a FRAME NAME, not a coordinate -------------
    def _mate(self, type_, a, b, **kw):
        self.activate()
        return self.app.add_mate(type=type_, a=_conn(a), b=_conn(b), **kw)

    def fasten(self, a, b, **kw):
        return self._mate("fastened", a, b, **kw)

    def revolute(self, a, b, **kw):
        return self._mate("revolute", a, b, **kw)

    def slider(self, a, b, **kw):
        return self._mate("slider", a, b, **kw)

    def cylindrical(self, a, b, **kw):
        return self._mate("cylindrical", a, b, **kw)

    def planar(self, a, b, **kw):
        return self._mate("planar", a, b, **kw)

    def ball(self, a, b, **kw):
        return self._mate("ball", a, b, **kw)

    def gear(self, mate_a, mate_b, teeth_a, teeth_b, **kw):
        self.activate()
        return self.app.add_relation(type="gear", a=_id(mate_a),
                                     b=_id(mate_b), teeth_a=teeth_a,
                                     teeth_b=teeth_b, **kw)

    def chain(self, mate_a, mate_b, teeth_a, teeth_b, **kw):
        """A chain or belt: like a gear, but both sprockets turn the SAME
        way (v88)."""
        self.activate()
        return self.app.add_relation(type="chain", a=_id(mate_a),
                                     b=_id(mate_b), teeth_a=teeth_a,
                                     teeth_b=teeth_b, **kw)

    def cam(self, mate_a, mate_b, **kw):
        """A cam: rotation to offset through a lift table, or {rise,
        duration_deg, start_deg} for a cosine lobe (v88)."""
        self.activate()
        return self.app.add_relation(type="cam", a=_id(mate_a),
                                     b=_id(mate_b), **kw)

    def bolt(self, rim, occurrence, standard="iso4762", **kw):
        """Fit a fastener to a hole rim, deriving its size and length.

        Self-activating like every other Asm verb, and that is the point:
        finding the rim means querying a PART, which switches the tab, so a
        bare `app.fit_fastener` right after one is refused with "a fastener
        goes in an ASSEMBLY" even though the caller never left it.
        """
        self.activate()
        ref = rim.ref if hasattr(rim, "ref") else dict(rim)
        ref["occurrence"] = _id(occurrence)
        return self.app.fit_fastener(rim=ref, standard=standard, **kw)

    def motion(self, driver, **kw):
        """A motion study on this assembly (v87)."""
        self.activate()
        return self.app.add_motion_study(driver=_id(driver), **kw)

    def solve(self):
        self.activate()
        return self.app.solve_assembly()

    def check(self, connected=True, clearances=True):
        self.activate()
        """The assembly's standing check: nothing clashing, nothing floating,
        and no near miss under a millimetre — which is almost always an
        arithmetic slip, and is exactly what a transcribed coordinate
        produces."""
        r = self.app.contact_report()
        problems = []
        if r.get("clashes"):
            problems.append(f"{len(r['clashes'])} clash(es): "
                            f"{r['clashes'][:4]}")
        if connected and len(r.get("components", [])) > 1:
            problems.append(
                f"{len(r['components'])} disconnected group(s) — something is "
                f"floating: {[c[:3] for c in r['components']]}")
        if clearances and r.get("near_misses"):
            problems.append(f"{len(r['near_misses'])} near miss(es) under "
                            f"1 mm: {r['near_misses'][:4]}")
        if problems:
            raise SdkError("; ".join(problems))
        return r


def _conn(spec):
    """A mate connector. A Frame is named BY NAME (the mate as designed); a
    Face or Edge falls back to the element anchor; an Occurrence alone means
    its origin."""
    if isinstance(spec, tuple) and len(spec) == 2:
        occ, what = spec
        if isinstance(what, str):
            return {"occurrence": occ.id, "frame": what}
        if isinstance(what, Frame):
            return {"occurrence": occ.id, "frame": what.name}
        if isinstance(what, Edge):
            return {"occurrence": occ.id, "face": what.ref}
        if isinstance(what, Face):
            return {"occurrence": occ.id, "face": what.ref}
        raise SdkError(
            f"a connector is (occurrence, frame name | Frame | Face | Edge), "
            f"not {what!r} — for an occurrence's ORIGIN pass the occurrence "
            f"on its own")
    if isinstance(spec, Occurrence):
        return {"occurrence": spec.id, "origin": True}
    if isinstance(spec, dict):
        return spec
    raise SdkError("a mate side is an Occurrence (its origin) or "
                   "(occurrence, frame name)")


# --------------------------------------------------------------------------
# Batching
# --------------------------------------------------------------------------

class Ref(dict):
    """A handle to a call that has not run yet — resolves to `{"$ref": n}`
    when the batch is sent."""

    def __init__(self, n, key=None):
        super().__init__({"$ref": n} if key is None
                         else {"$ref": n, "key": key})
        self.n = n

    @property
    def id(self):
        return self


class Batch:
    """Collects calls, sends them as one `batch`.

        with part.batch() as b:
            sk = b.add_feature(feature={...})     # a Ref
            b.add_feature(feature={..., "sketch_id": sk})
        b.results            # each call's response

    An id returned inside the block is a `$ref`, so a sketch and the extrude
    that names it go in the same transaction.
    """

    def __init__(self, app, stop_on_error=True):
        self.app, self.calls, self.stop_on_error = app, [], stop_on_error
        self.results, self.response = [], None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        if exc_type is None and self.calls:
            self.send()
        return False

    def __getattr__(self, name):
        def queue(**kw):
            self.calls.append({"method": name, "params": kw})
            return Ref(len(self.calls))
        return queue

    def send(self):
        self.response = self.app.batch(calls=self.calls,
                                       stop_on_error=self.stop_on_error)
        self.results = [r.get("result") for r in self.response["results"]]
        return self.response


# --------------------------------------------------------------------------
# Small shared helpers
# --------------------------------------------------------------------------

def _id(x):
    """A feature id from a handle, a dict response, or an int."""
    if x is None:
        return 0
    if isinstance(x, Ref):
        return x
    if isinstance(x, (Sketch, Frame, Occurrence)):
        return x.id
    if isinstance(x, Body):
        return x.created_by
    if isinstance(x, dict):
        return x.get("id", x)
    return x


def _dim(feature, key, value):
    """A dimension: a number, or an EXPRESSION string that stays a variable.

    This is the half of "parameters first" a script usually drops. A literal
    where the spec named a dimension is how a document stops being
    parametric.
    """
    if isinstance(value, str):
        feature[key] = 1.0
        feature[key.replace("_deg", "") + "_expr"
                if key.endswith("_deg") else key + "_expr"] = value
    elif value is not None:
        feature[key] = float(value)


def _dim3(feature, key, value):
    nums, exprs, any_expr = [], [], False
    for c in value:
        if isinstance(c, str):
            nums.append(0.0)
            exprs.append(c)
            any_expr = True
        else:
            nums.append(float(c))
            exprs.append("")
    feature[key] = nums
    if any_expr:
        feature[key + "_expr"] = exprs


def _num(value, part):
    """A dimension as a number: a number, or a variable the document holds."""
    return part.value_of(value)
