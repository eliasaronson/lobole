"""The Designer SDK — geometry references instead of transcribed numbers.

Two layers, and the split is the whole design:

**The RPC surface is GENERATED, never hand-written.** `describe` already returns
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
- `part.batch()` / `app.transaction()` — a context manager over the `batch`
  RPC, so a run of edits is one transaction, one regeneration and one undo
  step. Handles returned inside it are `$ref`s, so a sketch and the extrude
  that names it go together. On `App` the verb is `transaction`, because
  `app.batch` resolves to the generated RPC and fails with "missing required
  parameter 'calls'".

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
import functools
import math
import os
import sys
from typing import TYPE_CHECKING

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from designer_remote import Designer as _Remote  # noqa: E402
from designer_contracts import ContractError, validate as _validate_contract  # noqa: E402
from designer_inspection import (  # noqa: E402
    CONTRACT_HASH, CONTRACT_VERSION, InspectionClient)

if TYPE_CHECKING:
    from designer_snapshots import MeasurementSnapshot


class SdkError(RuntimeError):
    """A refusal from the SDK itself (never from the app)."""


def _renamed(**renames):
    """Refuse a keyword this SDK used to spell without its unit.

    Each value is (new name, what the new one takes). A bare angle meant
    degrees in `revolve` and radians in `bolt_circle`; the new keyword names
    its unit, and the old one is refused rather than guessed at, because
    `bolt_circle(start_angle=0.5)` read as degrees is a plausible wrong part
    (docs/sdk-units-plan.md).
    """
    def wrap(fn):
        @functools.wraps(fn)
        def refusing(*args, **kw):
            for old, (new, takes) in renames.items():
                if old in kw:
                    raise SdkError(f"{fn.__name__}: `{old}=` is now "
                                   f"`{new}=`, which takes {takes}")
            return fn(*args, **kw)
        return refusing
    return wrap


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

class Param(float):
    """A number that remembers the expression that produced it.

    The reason 13 of 16 generators declared no document variables at all: the
    parametric path costs five lines where Python arithmetic costs one. A
    module constant is evaluated in the runner and reaches the document as a
    literal, so the drone — 91 occurrences, 42 derived fasteners — opens in
    the GUI with nothing to turn. vcad names the same disease in its loon
    bridge: "the emitted document keeps only the arithmetic result, so design
    intent is unrecoverable."

    A Param IS a float, so every existing path keeps working unchanged —
    filtering faces by diameter, `list(V(...))`, comparisons, `math.*`. It
    also carries `.expr`, and arithmetic between Params composes it:

        L = part.param("arm_len", 110)      # Param(110.0, expr="arm_len")
        x = L - 18                          # Param(92.0,  expr="(arm_len - 18)")
        part.extrude(s, distance=x)         # distance 92.0, distance_expr "(arm_len - 18)"

    So the script does its arithmetic in Python, as it always did, and the
    document receives the formula rather than the answer. `_dim`/`_dim3` are
    where that lands; anywhere the document cannot hold an expression a Param
    simply behaves as its number, which is exactly today's behaviour.

    Arithmetic with a plain float yields a Param whose expression carries the
    literal — that is intended: `L / 2` is still driven by `arm_len`.
    """

    __slots__ = ("expr",)

    def __new__(cls, value, expr):
        self = float.__new__(cls, value)
        self.expr = expr
        return self

    @staticmethod
    def _e(other):
        """The expression text for the other operand of a binary op."""
        if isinstance(other, Param):
            return other.expr
        if isinstance(other, str):
            return other
        return repr(float(other))

    def _op(self, other, symbol, value, reflected=False):
        if value is NotImplemented:
            return NotImplemented
        a, b = self.expr, Param._e(other)
        if reflected:
            a, b = b, a
        return Param(value, f"({a} {symbol} {b})")

    def __add__(self, o):
        return self._op(o, "+", float.__add__(self, o))

    def __radd__(self, o):
        return self._op(o, "+", float.__radd__(self, o), True)

    def __sub__(self, o):
        return self._op(o, "-", float.__sub__(self, o))

    def __rsub__(self, o):
        return self._op(o, "-", float.__rsub__(self, o), True)

    def __mul__(self, o):
        return self._op(o, "*", float.__mul__(self, o))

    def __rmul__(self, o):
        return self._op(o, "*", float.__rmul__(self, o), True)

    def __truediv__(self, o):
        return self._op(o, "/", float.__truediv__(self, o))

    def __rtruediv__(self, o):
        return self._op(o, "/", float.__rtruediv__(self, o), True)

    def __neg__(self):
        return Param(float.__neg__(self), f"(-{self.expr})")

    def __pos__(self):
        return self

    def __abs__(self):
        return Param(float.__abs__(self), f"abs({self.expr})")

    def __repr__(self):
        return f"Param({float(self)!r}, {self.expr!r})"

    # A Param prints as its NUMBER. The repr above is for debugging; a
    # generator that interpolates a dimension into a name or a log line wants
    # the value, and got "Param(6.0, 'cells')S 1300mAh" until this existed.
    def __str__(self):
        return float.__repr__(self)

    def __format__(self, spec):
        return float.__format__(self, spec)


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
        # `from` and `class` are python keywords, so a caller cannot pass
        # them as keyword arguments at all: add_motion_study(from=0) is a
        # syntax error, and the only way through was app.call() with a dict.
        # A trailing underscore is python's own convention for exactly this.
        for name in [k for k in kw if k.endswith("_") and k[:-1] in self.params]:
            kw[name[:-1]] = kw.pop(name)
        if not self.free_form and self.name not in self._app._schemas:
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
        self._schemas = {m["name"]: m["request_schema"] for m in d["methods"]
                         if "request_schema" in m}
        self._inspection_contract = d.get("inspection_contract", {})
        self.inspection: InspectionClient = InspectionClient(self._inspection_call)
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

    # RPCs that make a DIFFERENT tab active as a side effect, so the cached
    # `_active` below is a lie afterwards. Creating any tab activates it, and
    # add_drawing_view switches to the drawing it adds to.
    #
    # Found by the drawing gate: create_drawing left `_active` pointing at a
    # part, so the next part.box() short-circuited its own activate() and the
    # app refused the add_feature with "the active document is a drawing".
    # A stale cache is worse than no cache, so anything that can move the tab
    # drops it rather than trying to predict where it landed.
    _MOVES_TAB = frozenset((
        "add_part", "add_assembly", "create_drawing", "add_drawing_view",
        "add_job", "new_project", "open_project", "delete_part",
        "edit_in_context", "set_active_part", "claim_component", "update_component",
    ))

    def _inspection_call(self, method, params):
        """Do not cast a different server contract into generated local types."""
        expected = dict(version=CONTRACT_VERSION, hash=CONTRACT_HASH)
        observed = {k: self._inspection_contract.get(k) for k in expected}
        if observed != expected:
            raise ContractError(method, "unsupported_contract",
                                "/capabilities/inspection_contract", expected,
                                observed)
        return self.call(method, **params)

    def call(self, method, _timeout=None, **params):
        if method in self._schemas:
            _validate_contract(self._schemas[method], params, method)
        if method in self._MOVES_TAB:
            self._active = None
        if _timeout is None:
            return self._remote.call(method, **params)
        return self._remote.request(method, params, timeout=_timeout)

    def active(self):
        """The id of the tab that is in front, asked rather than remembered.

        `_MOVES_TAB` above is an optimization, not a source of truth: it saves
        a round trip on a burst of writes, and a stale entry costs a LOUD
        refusal ("the active document is not a part") rather than a silent
        wrong write. This is how you check it, and how a caller that did
        something the SDK does not know about can resync:

            app._active = app.active()
        """
        return self.list_parts()["active_id"]

    def param(self, name, value, comment=""):
        """A PROJECT variable, and a Param that knows it.

        The project-scope twin of `Part.param`. A dimension several parts must
        agree about — a bolt circle two plates share, a plate thickness the
        standoff length is measured from — is one variable here, not one per
        part that happens to hold the same number.
        """
        self.set_variable(name=name, expression=str(value), comment=comment,
                          scope="project")
        for v in self.list_variables()["variables"]:
            if v["name"] == name:
                return Param(float(v["value"]), name)
        raise SdkError(f"the app accepted variable '{name}' and then did not "
                       f"list it")

    def params(self, table, comment=""):
        """Declare a whole dimension table at project scope; {name: Param}.

        Insertion order matters: a derived entry may name any entry declared
        before it.
        """
        return {k: self.param(k, v, comment=comment) for k, v in table.items()}

    def methods(self, contains=""):
        """The documented method names, optionally filtered."""
        return sorted(n for n in self._methods if contains in n)

    def help(self, method):
        return self._methods[method].__doc__

    def close(self):
        self._remote.close()

    @property
    def catalogs(self):
        """Project engineering catalogs; handles pin immutable content."""
        return Catalogs(self)

    # --- tabs --------------------------------------------------------------
    def parts(self):
        return self.call("list_parts")["parts"]

    def part(self, name=None, create=True):
        """Switch to (or create) a part tab and return a `Part`.

        `name` may be a slot ID, which is the only address that works when
        names repeat — as they do after any STEP import.
        """
        return _open_tab(self, name, "part", create)

    def parts(self):
        """The PART tabs only. `list_parts` lists every tab — assemblies and
        drawings included, whatever its name says — and taking it at its
        word filed an engine and a carburettor under Fasteners."""
        return [x for x in self.list_parts()["parts"]
                if x.get("type") == "part"]

    def assemblies(self):
        """The ASSEMBLY tabs only. See `parts()`."""
        return [x for x in self.list_parts()["parts"]
                if x.get("type") == "assembly"]

    def assembly(self, name=None, create=True):
        return _open_tab(self, name, "assembly", create)

    def transaction(self, stop_on_error=True):
        """A `Batch` over several calls: one transaction, one regeneration,
        one undo step, and a rollback if any of them fails.

        Named `transaction` and not `batch` because `batch` is a generated
        RPC and `App.__getattr__` would hand you that instead — which fails
        with "missing required parameter 'calls'" and reads like a bug in the
        caller. `Part.batch()` is the same object for one part.
        """
        return Batch(self, stop_on_error)

    def new(self):
        self.call("new_project")
        return self


def _open_tab(app, name, kind, create):
    slots = app.call("list_parts")["parts"]
    # By id as well as by name. An imported STEP names thirty tabs `Part 1`,
    # so the name is not an address and the slot id is the only way to say
    # which one — reaching for `Part 1` returns whichever came first.
    if isinstance(name, int):
        hit = next((s for s in slots if s["id"] == name), None)
        if hit is None:
            raise SdkError(f"no tab with id {name}; the project has "
                           f"{len(slots)} of them")
        if hit["type"] != kind:
            raise SdkError(f"tab {name} is a {hit['type']}, not a {kind}")
        app.call("set_active_part", part=hit["id"])
        app._active = hit["id"]
        return (Part if kind == "part" else Asm)(app, hit)
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
    app = App(_Remote.wait_for(int(port)))
    # A process keeps running the binary it started with. If that file has
    # since been rebuilt, this instance answers for code nobody can look at
    # any more — and the answers are indistinguishable from a missing feature
    # or a regression. Said once, loudly, at the point of connection, because
    # the alternative is finding out from a measurement that makes no sense.
    try:
        if app.ping().get("binary_replaced"):
            print(f"WARNING: the instance on port {port} is running a binary "
                  f"that has been rebuilt since it started. Restart it, or "
                  f"you are testing the old code.", file=sys.stderr)
    except Exception:
        pass  # an old build with no such field: not worth failing a connect
    return app


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


class DatumPlane(Plane):
    """Persistent sketch plane; offset is measured along its normal.

    Use sketch UV coordinates or native projection. Unlike a numeric Plane,
    this handle never pretends its fallback origin is an evaluated placement.
    """

    def __init__(self, part, id, basis):
        super().__init__(basis.origin, basis.normal, basis.u)
        self.part, self.id = part, id

    def uv(self, point):
        raise SdkError(
            "a live datum needs native projection; use sketch UV coordinates"
        )

    def world(self, u, v=None):
        raise SdkError("a live datum is a reference, not an evaluated world plane")

    def offset(self, distance):
        raise SdkError(
            "create a persistent datum with part.datum_plane(parent=datum, offset=...)"
        )


class Thread:
    """Persistent cosmetic thread feature, usable as a screw relation's source."""

    def __init__(self, part, id):
        self.part, self.id = part, id


class ProjectedCurve:
    """A projected edge/silhouette with validated native dimension picks."""

    def __init__(self, row):
        if row.get("kind") not in ("edge", "silhouette") or "ref" not in row:
            raise SdkError("expected a list_drawing_geometry curve")
        self.row = row

    def pick(self, endpoint="middle"):
        ends = {"middle": 0, "start": 1, "end": 2}
        if endpoint not in ends:
            raise SdkError("endpoint must be middle, start or end")
        field = "edge" if self.row["kind"] == "edge" else "sil_face"
        return {field: dict(self.row["ref"]), "end": ends[endpoint]}


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
                                             element=self.name,
                                             part=self._part.id))

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

    def snapshot(self, *, circular=False, fields=("center", "radius"),
                 max_elements=2000) -> "MeasurementSnapshot":
        """Explicit frozen edge measurements; reading them makes no RPCs.

        Coordinates are part-local. validate() checks freshness; refresh()
        returns a new snapshot. Existing Edge.center remains a live query.
        """
        from designer_snapshots import MeasurementSnapshot
        return MeasurementSnapshot.capture(self._part.app, self._part.id,
                                           self.index, circular, fields,
                                           max_elements)

    def _load(self):
        if self._els is None:
            r = self._part.app.list_elements(
                body=self.index, part=self._part.id)["bodies"][0]
            self._els = r
            self._faces = [Face(self._part, self, f) for f in r["faces"]]
            self._edges = [Edge(self._part, self, e) for e in r["edges"]]
        return self._els

    @property
    def created_by(self):
        # From the CHEAP body list when it is there. This one integer used to
        # come from `list_elements`, which enumerates every face and edge of
        # the body to produce it — and `_body_from` reads it after every
        # extrude, so building a cylinder head spent 55 s on surface analysis
        # nobody asked for.
        if "created_by" in self._raw:
            return self._raw["created_by"]
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
        string OR a `Param`, which keeps the dimension a variable."""
        if (d is None) == (r is None):
            raise SdkError("circle takes exactly one of d= (diameter) or "
                           "r= (radius)")
        n = len(self.entities)
        e = {"type": "circle",
             "center": list(_uv(self, center, n, 0, "circle"))}
        # A Param counts as an expression, the same way `_dim` treats one.
        # It did not, and a Param is a float subclass, so `circle(d=P["bore"])`
        # took the literal branch below and wrote the NUMBER with no `r_expr`
        # — a bore that looked parametric, was not, and said nothing. Every
        # hole a generator drills goes through here.
        given = d if d is not None else r
        expr = (given if isinstance(given, str)
                else (given.expr if isinstance(given, Param) else None))
        if expr is not None:
            # The app's own field is `r_expr` (a RADIUS), so a diameter given
            # as an expression is halved in the expression itself rather than
            # in python — the document stays the one thing that evaluates it.
            e["radius"] = float(given) if isinstance(given, Param) else 0.0
            if d is not None and isinstance(given, Param):
                e["radius"] = float(given) / 2.0
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

    def arc(self, center, radius, start_deg, end_deg, construction=False):
        """An arc, counter-clockwise from `start_deg` to `end_deg`.

        The entity type always existed — `slot` emits two of them — but there
        was no way to draw one, so every rounded corner in the air-cooled
        engine is a slot or a polygon. A radius on a drawn profile is the
        normal case (docs/dogfood-friction-plan.md, F2).
        """
        n = len(self.entities)
        self.entities.append({"type": "arc",
                              "center": list(_uv(self, center, n, 0, "arc")),
                              "radius": float(radius),
                              "start_angle": math.radians(start_deg),
                              "end_angle": math.radians(end_deg),
                              "construction": bool(construction)})
        return self._attach_exprs(1)

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

    def extent(self):
        """The (u, v) bounding box of everything drawn so far, as
        ((umin, vmin), (umax, vmax)).

        Local arithmetic over the entities, so it costs nothing and answers
        the question a profile most often gets wrong: how far does this
        actually reach? Expression-valued coordinates are skipped — they are
        not numbers yet.
        """
        lo = [float("inf"), float("inf")]
        hi = [float("-inf"), float("-inf")]
        def seen(u, v, pad=0.0):
            if isinstance(u, str) or isinstance(v, str):
                return
            lo[0] = min(lo[0], u - pad); hi[0] = max(hi[0], u + pad)
            lo[1] = min(lo[1], v - pad); hi[1] = max(hi[1], v + pad)
        for e in self.entities:
            t = e.get("type")
            if t in ("circle", "arc"):
                c = e["center"]
                seen(c[0], c[1], float(e.get("radius", 0.0)))
            elif t == "line":
                for k in ("a", "b", "p1", "p2", "start", "end"):
                    if k in e:
                        seen(e[k][0], e[k][1])
            elif "position" in e:
                seen(e["position"][0], e["position"][1])
        if lo[0] == float("inf"):
            raise SdkError("nothing drawn yet, or every coordinate is an "
                           "expression — extent() reads numbers")
        return (V((lo[0], lo[1], 0)), V((hi[0], hi[1], 0)))

    @_renamed(rotate=("rotate_deg", "DEGREES (the old name took radians)"))
    def polygon(self, center, n, r=None, across_flats=None, rotate_deg=0.0):
        """A regular n-gon. `r` is the circumradius; `across_flats` is the
        spanner size, converted rather than approximated. `rotate_deg` turns
        the first vertex off the +U axis."""
        if (r is None) == (across_flats is None):
            raise SdkError("polygon takes one of r= or across_flats=")
        if across_flats is not None:
            r = (across_flats / 2.0) / math.cos(math.pi / n)
        cu, cv = _uv(self, center)
        rot = math.radians(rotate_deg)
        pts = [(cu + r * math.cos(rot + 2 * math.pi * i / n),
                cv + r * math.sin(rot + 2 * math.pi * i / n))
               for i in range(n)]
        return self.polyline(pts, close=True)

    def slot(self, a, b, width):
        """A rounded slot between two centres — two lines and two arcs, so it
        is a real closed profile rather than a stadium approximation.

        IT REACHES HALF ITS WIDTH PAST EACH CENTRE. A slot from (0,0) to
        (0,43) that is 90 wide spans -45..88, not 0..43 — which is how a crank
        web drawn round a pin 43 up came to reach z=85 and hit a barrel whose
        underside is at 82. `extent()` below is the cheap way to check before
        extruding (docs/dogfood-friction-plan.md, F2).
        """
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
        if isinstance(self.plane, DatumPlane):
            if self.plane.part.id != self.part.id or self.plane.part.app is not self.part.app:
                raise SdkError("datum plane belongs to a different part")
            f["plane_ref"] = self.plane.id
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

    def constrain(self, kind, refs, value=None, expr=None, value_deg=None):
        """One sketch constraint.

        `value` is MILLIMETRES for the length kinds and RADIANS for `angle` —
        pass `value_deg` for degrees, never both, and a bare `value` over a
        full turn is refused as a degrees number in the radians field.
        `expr` is different: it is an expression in the document's base units,
        where angles are DEGREES ("45", "45deg" and "0.785rad" all agree).

        A `Param` sends its expression too, so the constraint follows the
        variable. That needs the number and the expression to share a unit,
        which they do as `value_deg` or as a length `value` — and do not as
        an angle's `value`, radians against a degree expression, so that one
        is refused.
        """
        if kind == "angle" and isinstance(value, Param):
            raise SdkError(
                "constrain: an angle's `value` is RADIANS but a Param's "
                f"expression ({value.expr!r}) evaluates in DEGREES — pass "
                "the Param as value_deg=")
        for p in (value, value_deg):
            if isinstance(p, Param) and expr is None:
                expr = p.expr
        kw = {"sketch_id": self.id, "kind": kind, "refs": refs}
        if value is not None:
            kw["value"] = value
        if value_deg is not None:
            kw["value_deg"] = value_deg
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

    A coordinate may be an EXPRESSION string, or a `Param` — the same thing
    with a number attached. It is evaluated against the document to place the
    entity, and — when `entity` says which entity and point it belongs to —
    recorded so `.add()` can pin it with a real dimensional constraint.

    A Param counts here for the reason it counts in `_dim`: it is a float
    subclass, so testing only for `str` sends `polyline(pts)` built out of
    Params down the literal path and the profile silently stops following its
    table. A generator whose whole outline is Param arithmetic then rebuilds
    to exactly the same shape at any parameter value, and the document says
    `cached, executed 0`.
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
        expr = c if isinstance(c, str) else (
            c.expr if isinstance(c, Param) else None)
        if expr is not None and (entity is None or field is None):
            # A Param still has a NUMBER, so it can go in a coordinate the
            # app does not make expression-driven (a rect's corners) — it
            # just will not follow. A bare string has no value and cannot.
            if isinstance(c, Param):
                expr = None
            else:
                raise SdkError(
                    f"{c!r}: an expression can only drive a coordinate the "
                    f"app makes expression-driven (a point, a line endpoint, "
                    f"a circle or arc centre) — pass a number here")
        if expr is not None:
            builder._pending_exprs.append((entity, field, expr))
            # The Param's own value keeps the entity in the right place until
            # the document evaluates the expression over it.
            out.append(float(c) if isinstance(c, Param) else 0.0)
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
    def var(self, name, expression, comment="", scope="part"):
        """A named parameter. Every dimension the spec names should be one:
        that is what makes the document parametric rather than a script's
        output.

        `scope="project"` for a dimension several parts share — a bore that
        two parts must agree about is one variable, not two that happen to
        hold the same number.
        """
        self.activate()
        self.app.set_variable(name=name, expression=str(expression),
                              comment=comment, scope=scope)
        return self

    def param(self, name, value, comment="", scope="part"):
        """Declare a document variable and hand back a number that knows it.

        The half of `var()` that was missing. `var` declares; using the result
        meant passing the NAME as a string everywhere, and the moment a script
        needed `arm_len - 18` a literal was the only short path. A Param is a
        float that carries its expression, so the arithmetic stays Python and
        the document still receives the formula:

            L = part.param("arm_len", 110)
            part.extrude(s, distance=L / 2)   # distance_expr "(arm_len / 2)"

        `value` may itself be an expression over parameters already declared,
        in which case the document owns the formula and the Param refers to it
        by NAME — one definition, not two:

            part.param("femur_len", "arm_len - 40")   # expr is "femur_len"
        """
        self.var(name, value, comment=comment, scope=scope)
        return Param(self.value_of(name), name)

    def params(self, table, comment="", scope="part"):
        """Declare a whole dimension table at once; returns {name: Param}.

        Generators already keep their dimensions in exactly this shape — the
        clamp's `D`, `CHOICES` and `SPACER` dicts are a parameter table that
        never reached the document. Binding the dict through here makes every
        existing `D["jaw_l"]` yield a Param instead of an int, so the call
        sites do not change at all and the arithmetic between them starts
        composing expressions.

        Insertion order matters: a derived entry may name any entry declared
        before it.
        """
        return {k: self.param(k, v, comment=comment, scope=scope)
                for k, v in table.items()}

    def param_of(self, name):
        """An already-declared variable, as a Param."""
        return Param(self.value_of(name), name)

    def vars(self):
        self.activate()
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
        self.activate()
        for v in self.app.list_variables()["variables"]:
            if v["name"] == name:
                return float(v["value"])
        raise SdkError(
            f"'{name}' is not a variable of this document. A number is "
            f"needed here, and the document — not this SDK — evaluates "
            f"expressions; pass a number or a variable name")

    def datum_plane(self, base="xy", offset=0, parent=None, name="Datum plane"):
        """Live offset plane. XZ uses SDK axes (+X,+Z; normal -Y).

        Numeric Plane/Frame arguments to sketch remain snapshots; this handle
        retains the native dependency through plane_ref.
        """
        if base not in ("xy", "xz", "yz"):
            raise SdkError("base must be xy, xz or yz")
        basis = {"xy": Plane.xy, "xz": Plane.xz, "yz": Plane.yz}[base]()
        fields = {"base_plane": base}
        if parent is not None:
            if (
                not isinstance(parent, DatumPlane)
                or parent.part.id != self.id
                or parent.part.app is not self.app
            ):
                raise SdkError("parent must be a datum plane in this part")
            fields = {"parent_id": parent.id}
            basis = Plane(parent.origin, parent.normal, parent.u)
        elif base == "xz":
            fields = {
                "parent_id": self._add(
                    dict(
                        type="DatumPlane",
                        name=name + " basis",
                        kind="angled",
                        base_plane="xy",
                        axis={"kind": "x"},
                        angle_deg=90,
                    )
                )["id"]
            }
        feature = dict(type="DatumPlane", name=name, kind="offset", **fields)
        _dim(feature, "offset", offset)
        return DatumPlane(self, self._add(feature)["id"], basis)

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

    @_renamed(angle=("angle_deg", "the same degrees"))
    def revolve(self, sketch, axis="z", angle_deg=360.0, mode="new_body",
                name=None, **kw):
        f = {"type": "Revolve", "name": name or f"Revolve {sketch.name}",
             "sketch_id": _id(sketch), "mode": mode,
             "axis": axis if isinstance(axis, dict) else {"kind": axis}}
        _dim(f, "angle_deg", angle_deg)
        f.update(kw)
        return self._body_from(self._add(f))

    @_renamed(angle=("angle_deg", "the same degrees"))
    def revolve_profile(self, points, plane=None, axis="z", angle_deg=360.0,
                        name="Revolved", **kw):
        """A closed profile and the revolve of it, in one call — the shape a
        turned part (a piston, a bushing, a pulley) is actually described
        in."""
        sk = self.sketch(plane or Plane.xz(), name=f"{name} profile")
        sk.polyline(points, close=True)
        return self.revolve(sk.add(), axis=axis, angle_deg=angle_deg,
                            name=name, **kw)

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

    def thread(self, face, designation, allow_engagement=False, minimum_engagement=0):
        """Annotate actual manufacturing geometry; engagement is explicit opt-in.

        Matching coaxial threads relieve only their common axial span during
        exact contact checks. No geometry or STEP machining diameter is changed.
        """
        if not isinstance(face, Face) or face.kind != "cylinder":
            raise SdkError("thread needs a cylindrical Face")
        if face._part.id != self.id or face._part.app is not self.app:
            raise SdkError("thread face belongs to a different part or connection")
        feature = self._add(
            dict(
                type="Thread",
                name=designation,
                face=face.ref,
                designation=designation,
                allow_engagement=allow_engagement,
                minimum_engagement=minimum_engagement,
            )
        )
        return Thread(self, feature["id"])

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

    @_renamed(rotate=("rotate_deg", "the same degrees per axis"))
    def frame(self, name, of=None, origin=None, z=(0, 0, 1), x=(1, 0, 0),
              parent=None, offset=(0, 0, 0), rotate_deg=(0, 0, 0),
              flip=False):
        """A named datum frame (v86). `of=` one element, or several circular
        ones for a bolt circle; `parent=` another frame; otherwise explicit.

        This is the thing a mate anchors to BY NAME, so the assembly follows
        the geometry instead of a number someone read off it.

        CREATES a frame. To ask where one ENDED UP once the part is placed,
        the same verb one class away does the opposite: `occ.frame("pin")`
        returns its origin and axes in world space. Worth saying here
        because that is not guessable — a whole round of this project
        answered "which way does this arm point, where does that pin land"
        by transforming bounding-box corners in Python, having written this
        form twenty times without meeting the other one.
        """
        if of is not None:
            els = of if isinstance(of, (list, tuple)) else [of]
            f = {"type": "Frame", "name": name, "kind": "from_element",
                 "elements": [e.ref for e in els]}
            if flip:
                f["flip"] = True
        elif parent is not None or any(offset) or any(rotate_deg):
            f = {"type": "Frame", "name": name, "kind": "offset",
                 "parent_id": _id(parent) if parent is not None else 0,
                 "rotation_deg": list(rotate_deg)}
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

    @_renamed(angle=("step_deg", "the same degrees PER STEP; leave it out "
                               "to space the copies evenly round a full "
                               "turn"))
    def pattern(self, source, count, linear=None, circular=None, spacing=None,
                step_deg=None, name="Pattern"):
        """A linear or circular pattern of a feature. `linear=` an axis and
        `spacing=` (mm); `circular=` an axis and `step_deg=`.

        `step_deg` is the angle between one instance and the next, not the
        span they cover. Left out, it is derived from `count`: 360/count, the
        copies evenly round a full turn. (A fixed 360 default rotated every
        copy a full turn back onto the original.)
        """
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
            if step_deg is None:
                if int(count) < 1:
                    raise SdkError(f"pattern: count must be at least 1, "
                                   f"got {count}")
                step_deg = 360.0 / int(count)
            _dim(f, "angle_deg", step_deg)
        else:
            raise SdkError("pattern takes linear= or circular= (an axis)")
        return self._add(f)

    @_renamed(start_angle=("start_deg",
                           "DEGREES (the old name took radians)"))
    def bolt_circle(self, plane, n, pcd, d, start_deg=0.0, name="Bolt holes",
                    through=True, depth=None):
        """N holes on a pitch circle, and the frame that names the pattern.
        The first hole sits `start_deg` off the plane's +U axis.

        Returns (hole_feature, frame): the frame is centred on the pattern
        with X toward the first hole, which is what an assembly mates to —
        so the plate can move and the mate follows.
        """
        sk = self.sketch(plane, name=f"{name} centres")
        for i in range(n):
            a = math.radians(start_deg) + 2 * math.pi * i / n
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
    #
    # None of these activate. A read does not need the tab — the app can reach
    # any slot's document whether it is active or stashed — and switching to
    # one is not free: it clears the selection, the hidden bodies, the measure
    # session, simulation mode and the explode state. Read a part on somebody's
    # running instance and that was their work
    # (docs/agent-scale-followup-plan.md, W2).
    def state(self):
        return self.app.get_state(part=self.id)

    def bodies(self, reference=None):
        """This part's bodies. Cheap: the LIST comes from get_state, and each
        body's faces and edges are read only when asked for. `reference=`
        filters ghosts in or out."""
        out = [Body(self, i, b)
               for i, b in enumerate(self.state()["bodies"])]
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

    def islands(self, z=None, normal=(0, 0, 1), origin=None):
        """The connected regions of material a plane cuts this part into.

        `part.islands(z=200)` cuts a horizontal plane at z=200 and returns the
        areas, biggest first. Two islands where you expected one means a
        feature is not joined to the rest THERE, whatever it may be joined to
        somewhere else — which is how four bolt bosses came to hang off a deck
        flange with every other check green
        (docs/dogfood-friction-plan.md, F1).
        """
        if origin is None:
            origin = (0.0, 0.0, float(z or 0.0))
        r = self.app.section(origin=list(V(origin)), normal=list(V(normal)),
                             regions=True, part=self.id)
        return [x["area"] for x in r["regions"]]

    def bored_through(self, normal=(0, 1, 0), origin=(0, 0, 0)):
        """Is this duct actually open end to end?

        The dual of `islands()`, and the same one call: a plane CONTAINING a
        passage's axis cuts an open duct's metal into TWO lumps, one wall
        either side of the bore. A blocked one is still a single lump,
        because the plug joins the walls across the middle.

        Worth having because nothing else notices. A revolve profile that
        starts at r=0 is a legal shape and builds without complaint — it is
        simply solid — so there is no cut to report "removed no material",
        no clash, and no island. An exhaust shipped that way twice: a flange
        with a pipe standing on it, and nothing through either.

            if not exhaust.bored_through():
                raise SdkError("the exhaust is a plug")
        """
        return len(self.islands(origin=origin, normal=normal)) >= 2

    def extrude_shapes(self, plane, draws, distance, name="Shape",
                       mode=None, **kw):
        """Extrude several OVERLAPPING shapes as one lump.

        A sketch refuses loops that overlap ("profile loops overlap or nest
        inconsistently") rather than guessing which one was meant to be a
        hole — right, but it makes a silhouette drawn from overlapping shapes
        into one extrude each. `draws` is a list of functions that each draw
        on a fresh sketch; the first makes the body and the rest add to it.

            p.extrude_shapes(Plane.xz(14), [
                lambda s: s.polyline(shank_outline),
                lambda s: s.circle((0, 0), d=70),
                lambda s: s.circle((0, rod_l), d=40),
            ], 28.0, name="Rod")

        (docs/dogfood-friction-plan.md, F2)
        """
        body = None
        for i, draw in enumerate(draws):
            sk = self.sketch(plane, name=f"{name} {i + 1}" if len(draws) > 1
                             else name)
            draw(sk)
            body = self.extrude(sk.add(), distance,
                                mode=(mode or "new_body") if i == 0 else "add",
                                name=f"{name} {i + 1}" if len(draws) > 1
                                else name, **kw)
        return body

    def render(self, path, view="iso"):
        """Screenshot this part on its own, fitted. Needs a renderer, so an
        `--offscreen` instance (a `--headless` one refuses, saying so).

        Here because looking is what caught the defect none of the other
        checks could: four bolt bosses hanging off a deck flange, one solid
        body, clean study, bolts correctly sized, `check()` green
        (docs/dogfood-friction-plan.md, F1).
        """
        self.activate()
        self.app.screenshot(path=path, view=view, fit=True)
        return path

    def material(self, slug, body=None):
        """Give this part's bodies a material, so mass properties and the BOM
        have a density to work with (and the viewport something to shade).

        `body` picks one by index; the default gives every body of the part
        the same material, which is what a part made of one thing wants.
        Unlike the reads above this MUST activate — a material is written to
        the feature that created the body, and writes stay on the active tab.
        """
        self.activate()
        indices = [body] if body is not None else range(len(self.bodies()))
        for i in indices:
            self.app.set_material(body=i, material=slug)
        return self

    def volume(self):
        return self.app.mass_properties(part=self.id)["total"]["volume"]

    def bbox(self):
        """The part's MEASURED extent, from mass_properties.

        Not get_state's, which is a cheap conservative bound — it wraps a
        swept surface by its control points, so a helical spring reads about
        sixty per cent over. That box is right for a broad phase and wrong for
        a person (docs/dogfood-friction-plan.md).
        """
        rows = self.app.mass_properties(part=self.id).get("bodies") or []
        boxes = [(V(r["bbox_min"]), V(r["bbox_max"])) for r in rows
                 if "bbox_min" in r]
        if boxes:
            lo = V([min(b[0][i] for b in boxes) for i in range(3)])
            hi = V([max(b[1][i] for b in boxes) for i in range(3)])
            return lo, hi
        bs = [b for b in self.state()["bodies"]
              if not b.get("reference")]
        lo = V([min(b["bbox_min"][i] for b in bs) for i in range(3)])
        hi = V([max(b["bbox_max"][i] for b in bs) for i in range(3)])
        return lo, hi

    # --- the standing check ------------------------------------------------
    def check(self, warn=True, *, solids=None):
        """Raise if any feature is broken; print advisories. The scripted
        equivalent of looking at the tree — every generator used to hand-roll
        this and most scratch scripts forgot to. ``solids=1`` additionally
        requires one solid across the part's non-reference bodies. A valid
        body may contain disconnected solids; body count is not this check.
        This is a topology requirement, not a strength/manufacturing check.
        """
        if solids is not None and (type(solids) is not int or solids < 0):
            raise SdkError("solids must be a non-negative integer")
        st = self.state()
        bad = [f for f in st["features"] if f.get("error")]
        if bad:
            raise SdkError("; ".join(f"{f['name']}: {f['error']}"
                                     for f in bad))
        if solids is not None:
            bodies = [b for b in st["bodies"] if not b.get("reference")]
            measured = [dict(name=b["name"], feature_id=b["created_by"],
                             solids=self.app.measure(a={"part": self.id,
                                 "feature_id": b["created_by"]})["solids"])
                        for b in bodies]
            actual = sum(b["solids"] for b in measured)
            if actual != solids:
                raise SdkError(f"{self.name}: expected {solids} solid(s), got "
                               f"{actual}; bodies: {measured}")
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
        """WHERE a named frame of this occurrence is, in world space.

        The read half of `Part.frame(...)`, which creates them. Origin and
        axes as the assembly has them placed, so "which way does this grow"
        and "did that pin land on the coupler's plane" are one call each.
        """
        kw = {"occurrence": self.id}
        if name is not None:
            kw["frame"] = name
        return Frame.from_query(self.asm, self.asm.app.frame(**kw), name=name)

    def bbox(self):
        """This occurrence's WORLD bounding box, as (min, max).

        Not the part's own box transformed: that is a box around a rotated
        box, and reading one as if it were the part put the throttle lever
        at z 8..40 when it came near neither end.
        """
        self.asm.activate()
        o = _only([x for x in self.asm.app.list_occurrences(bounds=True)
                   ["occurrences"] if x["id"] == self.id],
                  "occurrence", {"id": self.id})
        if "bbox_min" not in o:
            raise SdkError(
                f"'{self.name}' has no geometry to bound — an empty part, "
                "or one whose bodies all failed to build")
        return tuple(o["bbox_min"]), tuple(o["bbox_max"])

    def __repr__(self):
        return f"Occurrence({self.name!r}, id={self.id})"


class Asm(_Tab):
    def place(self, part, at=None, fixed=False, name=None):
        """Place a part OR ANOTHER ASSEMBLY. `at` is a world position; leave
        it out and mate it instead — which is the point.

        `add_occurrence` has always taken either ("a part OR another assembly
        in the active assembly"), and this took only a Part — so a subassembly
        had to be placed by passing its name as a bare string, which reads
        like a mistake rather than a feature.
        """
        self.activate()
        kw = {"part": part.name if isinstance(part, (Part, Asm)) else part,
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

    def fasten_in_place(self, a, b, tol=0.05, **kw):
        """Fasten, and pick the way round that leaves the moving part where
        it already is.

        A fastened mate between two frames has four outcomes — which side,
        and whether it is turned half a turn about the joint's axis — and
        none is predictable from the call. Measured on one cabinet:
        `flipped=True` seated a woofer correctly and turned another driver
        magnet-first at the room, two mates written identically; and two
        dowels at +/-110 are symmetric, so the half-turn is a real ambiguity
        that brought a side wall back upside down.

        When a caller has already placed the part correctly — a generator
        computing its own layout — the placement is the answer to that one
        question, and this asks it: try each way round, keep the one that
        does not move the part. Raises if none does, which means the two
        halves of the joint disagree and no mate can fix that.

        `a` is the MOVING side.
        """
        moving = a[0] if isinstance(a, tuple) else a
        was = _occ_centre(moving)
        m = self._mate("fastened", a, b, **kw)
        nearest, axial = None, None
        for flipped in (False, True):
            for twist in (0.0, 180.0):
                self.app.set_mate(id=m["id"], flipped=flipped,
                                  rotation_deg=twist)
                self.solve()
                now = _occ_centre(moving)
                gaps = [abs(x - y) for x, y in zip(now, was)]
                off = max(gaps)
                if off <= tol:
                    return m
                nearest = off if nearest is None else min(nearest, off)
                # A residual along ONE axis is the frames' own depths, not a
                # disagreement: a frame built from bores sits at their
                # MIDPOINT, and two mating holes are rarely drilled the same
                # depth — 8 mm into a panel's face, 22 into an edge. That is
                # closable with the offset, and only that case is.
                if sorted(gaps)[1] <= tol and (axial is None
                                               or off < axial[2]):
                    axial = (flipped, twist, off)
        if axial is not None:
            flipped, twist, _ = axial
            self.app.set_mate(id=m["id"], flipped=flipped,
                              rotation_deg=twist)
            self.solve()
            now = _occ_centre(moving)
            k = max(range(3), key=lambda i: abs(now[i] - was[i]))
            self.seat(m, lambda: _occ_centre(moving)[k], was[k], tol=tol)
            return m
        raise SdkError(
            f"no way round this mate leaves {moving.name!r} where it was "
            f"(nearest {nearest:.2f} mm out, and not along one axis) — the "
            f"two halves of the joint disagree, which a mate cannot fix")

    def seat(self, mate, measure, want, tol=0.01):
        """Solve a fastened mate's offset so something LANDS where it should.

        `measure` returns the quantity (a face's height, a bbox edge) and
        `want` is what it must equal. The offset's sensitivity is PROBED —
        nudge by 1 mm, see which way the part went — and then solved in one
        step, because a fastened mate is linear in its offset.

        Probing rather than deriving is the point. The sign depends on which
        way round the solver took the two frames, and it is not the same for
        two mates written alike: correcting by `-err` seated a port flare
        perfectly and sent the tube it carries to z = -2250.
        """
        mate_id = mate["id"] if isinstance(mate, dict) else int(mate)
        base = self._mate_offset(mate_id)
        self.solve()
        first = measure()
        if abs(want - first) <= tol:
            return first
        self.app.set_mate(id=mate_id, offset=base + 1.0)
        self.solve()
        slope = measure() - first
        if abs(slope) < 1e-6:
            raise SdkError(f"mate {mate_id}: its offset does not move what "
                           f"is being measured")
        self.app.set_mate(id=mate_id, offset=base + (want - first) / slope)
        self.solve()
        got = measure()
        if abs(want - got) > tol:
            raise SdkError(f"mate {mate_id}: wanted {want:.3f}, seated at "
                           f"{got:.3f} — not linear in the offset")
        return got

    def _mate_offset(self, mate_id):
        for m in self.app.list_mates()["mates"]:
            if m["id"] == mate_id:
                return float(m.get("offset", 0.0))
        return 0.0

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

    def thread_engagement(self, a, b):
        """Check that a declared thread joint exists, not merely that it is clash-free.

        A separated pair fails this gate even though ordinary interference
        correctly reports no collision. Uses each feature's minimum engagement.
        """
        if not all(isinstance(o, Occurrence) and o.asm is self for o in (a, b)):
            raise SdkError("thread_engagement needs two occurrences in this assembly")
        self.activate()
        result = self.app.interference(a=a.id, b=b.id, mode="exact")
        if result["clash"] or not result.get("engaged_thread_pairs", 0):
            raise SdkError(f"thread engagement failed for {a.name} / {b.name}: {result}")
        return result

    def screw(self, mate, thread, **kw):
        """Couple a cylindrical mate to a persistent thread feature's pitch."""
        if not isinstance(thread, Thread) or thread.part.app is not self.app:
            raise SdkError("screw requires a Thread handle from this connection")
        self.activate()
        return self.app.add_relation(
            type="screw",
            a=_id(mate),
            b=_id(mate),
            thread_part=thread.part.id,
            thread_feature=thread.id,
            **kw,
        )

    def chain(self, mate_a, mate_b, teeth_a, teeth_b, **kw):
        """A chain or belt: like a gear, but both sprockets turn the SAME
        way (v88)."""
        self.activate()
        return self.app.add_relation(type="chain", a=_id(mate_a),
                                     b=_id(mate_b), teeth_a=teeth_a,
                                     teeth_b=teeth_b, **kw)

    def cam(self, mate_a, mate_b, **kw):
        """A cam: rotation to the follower's slide or turn through a lift
        table (lift in mm for a slide, degrees for a turn), or {rise (mm) |
        rise_deg, duration_deg, start_deg} for a cosine lobe. A cylindrical
        follower takes dof_b="rotation" to turn (v88, turning v102)."""
        self.activate()
        return self.app.add_relation(type="cam", a=_id(mate_a),
                                     b=_id(mate_b), **kw)

    def freewheel(self, mate_a, mate_b, drives, ratio=1.0, **kw):
        """A one-way clutch, rotation to rotation (v104): b follows a while
        a drives it in `drives` ("positive" or "negative") and slips when a
        goes back. Its history is the relation's `lead`."""
        self.activate()
        return self.app.add_relation(type="freewheel", a=_id(mate_a),
                                     b=_id(mate_b), drives=drives,
                                     ratio=ratio, **kw)
        # teeth=N and ride_mate/ride_dof/ride_mm|ride_deg (v105) make it a
        # toothed clutch that LIFTS what it names as it rides over them.

    def click(self, mate, drives, **kw):
        """A pawl (v105): one mate's rotation, free the way it `drives`
        ("positive" or "negative") and held at its mark the other way."""
        self.activate()
        return self.app.add_relation(type="click", a=_id(mate), b=_id(mate),
                                     drives=drives, **kw)

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

    def check_clashes(self, **kw):
        """Exact standing interference gate, without contact distances.

        Use `check` when connectivity, near misses or realized mates matter.
        This checks rigid pairs too, unlike a motion study. A partial sweep
        is an error; increase max_pairs/deadline_s to finish it.
        """
        if kw.get("mode", "exact") != "exact" or "a" in kw or "b" in kw:
            raise SdkError("check_clashes requires a whole-assembly exact sweep")
        self.activate()
        r = self.app.interference(**dict(kw, mode="exact"))
        if r.get("truncated"):
            raise SdkError("the sweep was TRUNCATED: " +
                           str(r.get("truncation", "")))
        if r["clash"]:
            raise SdkError(f"{len(r['pairs'])} clash(es): {r['pairs'][:4]}")
        # The receipt is the general form of the two checks above: anything
        # that is not `verified` is not a pass, including a reason this
        # wrapper has not been taught to name yet.
        standing = r.get("receipt", {}).get("standing", "verified")
        if standing != "verified":
            raise SdkError(r["receipt"].get("summary", standing))
        return r

    def check(self, connected=True, clearances=True, realized=False,
              allow=None, **kw):
        """The assembly's standing check: nothing clashing, nothing floating,
        and no near miss under a millimetre — which is almost always an
        arithmetic slip, and is exactly what a transcribed coordinate
        produces.

        `allow` is for contacts that are MEANT to happen — a screw entering
        the mouth of its tapped hole, a crank running on its pivot boss —
        which are two solids touching and have no other way to be declared.
        It maps an exact pair of occurrence names to the most cubic
        millimetres of overlap that is still fine:

            asm.check(allow={("carb body", "idle screw"): 2.0})

        Three rules, and each one is there because the version without it
        failed: the pair is matched EXACTLY, not by substring; going over
        the ceiling is still a failure, so a screw touching by 1 mm3 passes
        while a coupler through a crank at 130 does not; and an allowance
        that matched no clash at all is itself an error, so the list cannot
        quietly rot as the model changes underneath it.

        The version this replaces asked whether NEITHER name in a pair
        appeared in the error message. Every pair named the body, so nothing
        ever raised, and it hid six real clashes including a coupler running
        through both cranks it joins. An escape hatch written to admit two
        known cases admitted everything.
        """
        self.activate()
        r = self.app.contact_report(**kw)
        allow = {tuple(sorted(k)): v for k, v in (allow or {}).items()}
        problems, used = [], set()
        over = []
        for c in r.get("clashes", []):
            key = tuple(sorted((c["a"], c["b"])))
            cap = allow.get(key)
            if cap is None:
                over.append(c)
                continue
            used.add(key)
            if c["volume"] > cap:
                over.append(dict(c, allowed=cap))
        if over:
            problems.append(f"{len(over)} clash(es): {over[:4]}")
        stale = sorted(set(allow) - used)
        if stale:
            problems.append(
                f"{len(stale)} allowance(s) matched nothing and should go: "
                f"{stale}")
        if connected and len(r.get("components", [])) > 1:
            problems.append(
                f"{len(r['components'])} disconnected group(s) — something is "
                f"floating: {[c[:3] for c in r['components']]}")
        if clearances and r.get("near_misses"):
            problems.append(f"{len(r['near_misses'])} near miss(es) under "
                            f"1 mm: {r['near_misses'][:4]}")
        # The sweep is BOUNDED and says so; "no clashes among the pairs I got
        # to" is not "no clashes". Dropping that made a 241-occurrence engine
        # whose sweep reached 145 of 790 pairs read as a clean bill of health.
        # Raise the budget (max_pairs, deadline_s) rather than ignoring it.
        if r.get("truncated"):
            problems.append("the sweep was TRUNCATED, so this is not a clean "
                            "bill of health: " + str(r.get("truncation", "")))
        # ...but only when asked. A mate between two parts on a RUNNING FIT
        # is a mate that deliberately does not touch — a crank in its
        # bearings, a piston in its bore, a valve in its guide — and an
        # engine is mostly those: 113 of this one's 227 mates, all correct.
        # Raised by default it was a gate that no assembly with a clearance
        # in it could ever pass. The count comes back either way, so a
        # caller that wants it still hears it.
        if realized and r.get("unrealized_mates"):
            problems.append(
                f"{len(r['unrealized_mates'])} mate(s) did not achieve what "
                f"they say: {r['unrealized_mates'][:4]}")
        if problems:
            raise SdkError("; ".join(problems))
        return r


# A wrapper that answers a VERDICT — "is this clean?" — must account for
# every field its RPC returns, because a key it does not read is a caveat the
# caller never hears. `Asm.check` read `clashes` and dropped `truncated`, and
# a 241-occurrence engine whose bounded sweep reached 145 of 790 pairs came
# back as an unqualified pass. `tests/sdk_smoke.py` compares this table
# against the live response, so the server growing a key fails the gate until
# somebody decides what it means here.
VERDICT_CONTRACTS = {
    "Asm.check_clashes": {
        "rpc": "interference",
        "consumed": {"clash", "pairs", "truncated", "truncation", "receipt"},
        "ignored": {
            "clashes": "the same list as `pairs`, under the name "
                       "contact_report uses for it",
            "mode": "check_clashes explicitly selects exact mode",
            "pairs_checked": "coverage detail; truncated is the verdict",
            "pairs_unchecked": "quoted by truncation",
            "pairs_box_cleared": "coverage detail",
            "elapsed_s": "cost, not correctness",
            "occurrences": "how many were swept",
        },
        "optional": {"truncated", "truncation", "pairs_unchecked"},
    },
    "Asm.check": {
        "rpc": "contact_report",
        "consumed": {
            "clashes",            # the verdict itself
            "components",         # >1 means something is floating
            "near_misses",        # a gap under near_tol
            "truncated",          # ...and whether any of the above is final
            "truncation",
            "unrealized_mates",   # a mate whose parts do not touch — read
                                  # under `realized=`, because a running fit
                                  # is a mate that must NOT touch
            "receipt",            # the standing: anything but `verified` is
                                  # not a pass, including a reason this
                                  # wrapper cannot name yet
        },
        "ignored": {
            "pairs": "the per-pair detail behind clashes/near_misses",
            "pairs_checked": "coverage detail; `truncated` is the verdict",
            "pairs_unchecked": "quoted by `truncation` when it matters",
            "pairs_box_cleared": "coverage detail",
            "elapsed_s": "cost, not correctness",
            "occurrences": "how many were swept",
            "connected": "the boolean behind `components`",
            "near_tol": "the tolerance `near_misses` was found at",
            "touch_tol": "the tolerance `clashes` was split from touching at",
            "unmated_contacts": "parts that touch without a mate — designed "
                                "contact is normal in an engine, so this is "
                                "advisory and printed, never raised",
        },
        # Present only when they APPLY: the sweep says how far it got only
        # when it stopped early. They are still `consumed` — the whole point
        # of the gate — but their absence is not a rename.
        "optional": {"truncated", "truncation", "pairs_unchecked"},
    },
}


def _occ_centre(occurrence):
    lo, hi = occurrence.bbox()
    return [(lo[i] + hi[i]) / 2.0 for i in range(3)]


def _conn(spec):
    """A mate connector. A Frame is named BY NAME (the mate as designed); a
    Face or Edge falls back to the element anchor; an Occurrence alone means
    its origin."""
    if isinstance(spec, tuple) and len(spec) == 2:
        occ, what = spec
        # A PATH names a part inside a sub-assembly ("carburettor/carb body"),
        # which is how the app says to reach one: a sub-assembly occurrence
        # "has no frames or faces of its own".
        who = occ if isinstance(occ, str) else occ.id
        if isinstance(what, str):
            return {"occurrence": who, "frame": what}
        if isinstance(what, Frame):
            return {"occurrence": who, "frame": what.name}
        if isinstance(what, Edge):
            return {"occurrence": who, "face": what.ref}
        if isinstance(what, Face):
            return {"occurrence": who, "face": what.ref}
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
    ekey = (key.replace("_deg", "") if key.endswith("_deg") else key) + "_expr"
    if isinstance(value, Param):
        # Both halves: the number geometry reads, and the formula that keeps
        # the field parametric. A Param is a float, so without this branch it
        # would silently take the literal path below.
        feature[key] = float(value)
        feature[ekey] = value.expr
    elif isinstance(value, str):
        feature[key] = 1.0
        feature[ekey] = value
    elif value is not None:
        feature[key] = float(value)


def _dim3(feature, key, value):
    nums, exprs, any_expr = [], [], False
    for c in value:
        if isinstance(c, Param):
            nums.append(float(c))
            exprs.append(c.expr)
            any_expr = True
        elif isinstance(c, str):
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


class Catalogs:
    """Import plans compose preview and install; ordinary RPCs stay generated."""

    def __init__(self, app):
        self.app = app

    def preview_file(self, path, *, id, revision, source, license, units,
                     storage="reference", mapping=None):
        options = dict(id=id, revision=revision, source=source, license=license,
                       units=units, storage=storage)
        if mapping is not None:
            options["mapping"] = mapping
        result = self.app.preview_catalog(path=os.fspath(path), options=options)
        return CatalogImport(self.app, os.fspath(path), options, result)

    def import_file(self, path, **options):
        return self.preview_file(path, **options).install()

    def import_package(self, path, *, storage="reference"):
        options = dict(storage=storage)
        result = self.app.preview_catalog(path=os.fspath(path), options=options)
        return CatalogImport(self.app, os.fspath(path), options, result).install()

    def open(self, id, revision):
        for entry in self.app.list_catalogs()["catalogs"]:
            if entry["id"] == id and entry["revision"] == revision:
                return CatalogVersion(self.app, entry)
        raise SdkError(f"catalog not found: {id} @ {revision}")


class CatalogImport:
    def __init__(self, app, path, options, summary):
        import copy
        self.app, self.path = app, path
        self._options = copy.deepcopy(options)
        self.summary = summary
        self._hash = summary["hash"]

    def install(self):
        result = self.app.import_catalog(path=self.path, options=self._options,
                                         expected_hash=self._hash)
        return CatalogVersion(self.app, result["catalog"])


class CatalogVersion:
    def __init__(self, app, metadata):
        self.app = app
        self.metadata = dict(metadata)
        self._ref = dict(catalog=metadata["id"], revision=metadata["revision"],
                         hash=metadata["hash"])

    def find(self, designation):
        result = self.app.query_catalog(**self._ref, designation=designation)
        return CatalogRecord(self, result["record"])

    def records(self, page_size=50):
        offset = 0
        while offset is not None:
            result = self.app.query_catalog(**self._ref, offset=offset,
                                            limit=page_size)
            for record in result["records"]:
                yield CatalogRecord(self, record)
            offset = result["next_offset"]

    def export(self, path, *, mode):
        return self.app.export_catalog(**self._ref, path=os.fspath(path), mode=mode)


class CatalogRecord:
    """A pinned record; dimensional fields and check observations are in mm."""

    def __init__(self, version, fields):
        self.catalog = version
        self.fields = dict(fields)
        self.designation = fields["designation"]

    def check_seat(self, **measurements_mm):
        """Explicit observations, never an automatic fit choice or geometry edit.

        Returns pass/fail/unverified with individual requirements. Missing
        limits stay unverified; reading a nominal diameter cannot supply them.
        """
        return self.catalog.app.catalog_requirements(
            **self.catalog._ref, designation=self.designation,
            measurements=measurements_mm)
