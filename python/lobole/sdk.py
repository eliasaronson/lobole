# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The Loböle SDK — geometry references instead of transcribed numbers.

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
what the SO-101 gripper dogfood was missing (docs/scripting/sdk.md#the-hand-written-layer):

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

    from lobole.sdk import connect
    app = connect()                      # the announced instance, or --port
    part = app.part("piston")
    part.var("bore", 86)
    top = part.cylinder("Top", d="bore", height=40)  # a Body
    part.fillet(top.edges(circular=True), r=2)
    seat = part.frame("wrist_pin", top.faces(kind="cylinder")[0])
    part.check()                                 # raises on any broken feature
"""
import functools
import math
import os
import sys
import time
from typing import TYPE_CHECKING

from lobole.client import Client as _Remote  # noqa: E402
from lobole.contracts import contract_refusal  # noqa: E402
from lobole.contracts import validate as _validate_contract  # noqa: E402
from lobole.errors import SdkError, UnknownArgument  # noqa: E402
from lobole.inspection import (  # noqa: E402
    CONTRACT_HASH, CONTRACT_VERSION, InspectionClient)

from lobole.rpc import (API_VERSION, FEATURE_ENVELOPE_KEYS,  # noqa: E402
                        FEATURE_SCHEMAS, PRIMITIVE_RETIRED_KEYS)

if TYPE_CHECKING:
    from lobole.snapshots import MeasurementSnapshot
    # The contracted RPCs, typed for a checker only: at runtime every RPC is
    # the generated describe surface (docs/scripting/sdk.md#typed-stubs).
    from lobole.rpc import RpcSurface as _RpcSurface
else:
    _RpcSurface = object


def _renamed(**renames):
    """Refuse a keyword this SDK used to spell without its unit.

    Each value is (new name, what the new one takes). A bare angle meant
    degrees in `revolve` and radians in `bolt_circle`; the new keyword names
    its unit, and the old one is refused rather than guessed at, because
    `bolt_circle(start_angle=0.5)` read as degrees is a plausible wrong part
    (docs/scripting/sdk.md#angles-name-their-unit).
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
                # The refusal the app would send (UndeclaredParamsError in
                # RemoteDispatch.cpp), raised without the round trip.
                takes = ", ".join(sorted(self.params)) or "no parameters"
                accepted = sorted(self.params)
                diagnostic = {
                    "operation": self.name, "code": "unknown_argument",
                    "path": f"/params/{unknown[0]}", "param": unknown[0],
                    "expected": accepted, "observed": kw[unknown[0]],
                    "accepted": accepted}
                if len(unknown) > 1:
                    diagnostic["unknown"] = unknown
                raise UnknownArgument(
                    f"{self.name}() got {', '.join(unknown)}; it takes "
                    f"{takes}", diagnostic)
        return self._app.call(self.name, _timeout=_timeout, **kw)


class App(_RpcSurface):
    """A connection to one running Loböle, with its whole RPC surface.

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
        # The RPC surface's version (docs/scripting/rpc.md#the-api-version).
        # Another than this package was generated against still works for
        # every method both know, so it warns rather than refuses: a removed
        # method or key is refused by name when it is called.
        self.api_version = d.get("api_version")
        if self.api_version != API_VERSION:
            import warnings
            warnings.warn(
                f"the app's RPC surface is api_version {self.api_version}, "
                f"and this lobole package was generated against "
                f"{API_VERSION}; a method or key one of them lacks is "
                "refused when called", RuntimeWarning, stacklevel=2)
        # Which tab the app is on, as far as this client knows. Tracked so a
        # handle can make itself active without a round trip when it already
        # is — a script that builds a part, then places it, then builds
        # another part otherwise has to remember to switch back, and gets
        # "the active document is not an assembly" when it does not.
        self._active = None
        # The tab ids THIS client object created (add_part/add_assembly, by
        # `part(name)` or directly). `part(name)` opens one of these again
        # without being asked twice; any other tab of that name is somebody
        # else's, and writing into it is the PES-on-OM615 bug — a pump
        # generator built its camshaft INTO the engine's. Ids are per
        # project, so opening or starting one forgets them.
        self._created = set()

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
            raise contract_refusal(method, "unsupported_contract",
                                   "/capabilities/inspection_contract",
                                   expected, observed)
        return self.call(method, **params)

    def call(self, method, /, _timeout=None, **params):
        # `method` positional-only: describe(method=...) is a parameter, not
        # this argument (lobole.client.call says the same).
        if method in self._schemas:
            _validate_contract(self._schemas[method], params, method)
        if method in ("add_feature", "modify_feature"):
            _validate_feature(params.get("feature"), method)
        if method in self._MOVES_TAB:
            self._active = None
        if method in ("new_project", "open_project"):
            self._created.clear()
        if _timeout is None:
            out = self._remote.call(method, **params)
        else:
            out = self._remote.request(method, params, timeout=_timeout)
        if method in ("add_part", "add_assembly") and isinstance(out, dict) \
                and "id" in out:
            self._created.add(out["id"])
        return out

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

    def help(self, method=""):
        """What `method` does, takes and returns — the one lookup every
        client shares (lobole.client.method_help), so the SDK, the plain
        client and the MCP server's help() give the same answer. With no
        method, the index."""
        from lobole.client import method_help
        return method_help(self._remote.call, method)

    def close(self):
        self._remote.close()

    @property
    def catalogs(self):
        """Project engineering catalogs; handles pin immutable content."""
        return Catalogs(self)

    # --- tabs --------------------------------------------------------------
    def part(self, name=None, create=True, reuse=False):
        """Create a part tab and return a `Part` — or, when asked, open one.

        Which of the two a name means is the CALLER's choice, never a lookup
        that happens to hit:

        - `part(name)` creates the part. If a tab of that name already
          exists it must be one THIS client created (switching back to a
          part built earlier in the same script); anything else is refused,
          because writing into it silently is how a pump generator built its
          camshaft into the engine's.
        - `part(name, create=False)` opens an existing part, and refuses when
          there is none.
        - `part(name, reuse=True)` opens the part if it exists and creates it
          if not — for a generator that deliberately rebuilds its OWN part in
          place (an `--update-component` flow).

        `name` may be a slot ID, which is the only address that works when
        names repeat — as they do after any STEP import; an id always opens.
        `part()` with no name opens the first part.
        """
        return _open_tab(self, name, "part", create, reuse)

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

    def assembly(self, name=None, create=True, reuse=False):
        """An assembly tab, by the same rule as `part()`: `assembly(name)`
        creates, `create=False` opens an existing one, `reuse=True` either."""
        return _open_tab(self, name, "assembly", create, reuse)

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

    # ---- Requirements (docs/requirements/README.md) -----------------------

    def require(self, name, measure, bound=None, source="", **bound_kw):
        """State a requirement the design must meet, held by the project:

            app.require("lens at 3 g", {"kind": "study_probe", ...},
                        at_most=2.0, source="the shot must stay sharp")
            app.require("seats", {"kind": "check_seat", ...})   # passes

        The bound is `bound` ({kind, value|min|max}) or one keyword:
        `at_most=`, `at_least=` or `within=(min, max)`, each a number or an
        expression over the project's variables; a verdict measure takes
        none (its bound is `pass`). Returns the requirement's id. If a
        requirement of this name exists it is EDITED instead (its revision
        moves and its evidence goes), so a generator re-run states the same
        set rather than a second copy of it."""
        if bound is None:
            if len(bound_kw) > 1:
                raise SdkError(f"one bound, not {sorted(bound_kw)}")
            if not bound_kw:
                bound = {"kind": "pass"}
            else:
                (kind, v), = bound_kw.items()
                if kind == "within":
                    bound = {"kind": "within", "min": v[0], "max": v[1]}
                elif kind in ("at_most", "at_least"):
                    bound = {"kind": kind, "value": v}
                else:
                    raise SdkError(f"unknown bound {kind!r} (accepted: "
                                   "at_most, at_least, within)")
        elif bound_kw:
            raise SdkError("pass `bound` or one bound keyword, not both")
        have = {r["name"]: r for r in
                self.call("list_requirements")["requirements"]}
        if name in have:
            r = have[name]
            if (r["measure"] == measure and r["bound"] == bound
                    and r.get("source", "") == source):
                return r["id"]
            self.call("set_requirement", id=r["id"], measure=measure,
                      bound=bound, source=source)
            return r["id"]
        return self.call("add_requirement", name=name, measure=measure,
                         bound=bound, source=source)["id"]

    def verify_requirements(self, ids=None, recompute=False, poll=1.0,
                           timeout=3600.0, raise_on_failure=True):
        """Check the project's requirements and wait for the answer: as a
        job (check_requirements {async}) followed by id, since a check may
        run studies. Returns list_requirements' answer. Raises SdkError
        naming every requirement that is not `pass` -- a fail, and also a
        stale, unresolved or unchecked one, none of which is a pass -- unless
        `raise_on_failure` is False."""
        params = {"async": True, "recompute": recompute}
        if ids is not None:
            params["ids"] = list(ids)
        r = self.call("check_requirements", **params)
        if r.get("job"):
            deadline = time.monotonic() + timeout
            while True:
                st = self.call("study_status", job=r["job"])
                if not st.get("running"):
                    break
                if time.monotonic() > deadline:
                    self.call("cancel_study", job=r["job"])
                    raise SdkError(f"check_requirements: not done in "
                                   f"{timeout:g} s; cancelled")
                time.sleep(poll)
            if st.get("outcome") != "done":
                raise SdkError(f"check_requirements: the check {st.get('outcome')}"
                               f": {st.get('error', st.get('cancel_reason'))}")
        out = self.call("list_requirements")
        if ids is not None:
            want = set(ids)
            out["requirements"] = [x for x in out["requirements"]
                                   if x["id"] in want]
        bad = [x for x in out["requirements"] if x["state"] != "pass"]
        if bad and raise_on_failure:
            raise SdkError("requirements not met: " + "; ".join(
                f"{x['name']}: {x['state']}"
                + (f" ({x['value']:g} {x.get('unit', '')})"
                   if "value" in x else "")
                + (f" -- {x['note']}" if x.get("note") else "")
                for x in bad))
        return out


def _open_tab(app, name, kind, create, reuse=False):
    if reuse and not create:
        raise SdkError(f"{kind}({name!r}, create=False, reuse=True) asks for "
                       f"two things: create=False opens an existing {kind}, "
                       f"reuse=True opens or creates one")
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
    if (hit is not None and create and not reuse
            and hit["id"] not in app._created):
        raise SdkError(
            f"a {kind} named {name!r} already exists and this script did not "
            f"create it, so {kind}({name!r}) will not write into it. Say which "
            f"you mean: app.{kind}({name!r}, create=False) opens the existing "
            f"one; app.{kind}({name!r}, reuse=True) is for a generator that "
            f"rebuilds its own {kind} in place; or choose another name")
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
                "no running Loböle instance announced itself; start one "
                "(tools/headless-preview.sh <port>, or the app) or pass port=")
        else:
            # The registry says which ports; only the instance knows what it
            # has open NOW, so ask it (briefly: a stalled one is not waited on).
            def what(i):
                try:
                    r = _Remote(i["port"], timeout=2.0).call("ping")
                    return r.get("project") or "no project"
                except Exception:  # noqa: BLE001 - listing, not operating
                    return f"{i.get('mode', '?')}, not answering"
            listed = ", ".join(f"{i['port']} ({what(i)})" for i in found)
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

    # The three principal planes, and `flip=True` for the same plane facing
    # the other way — so a feature on the far side of a part extrudes a
    # POSITIVE distance instead of a negative one. V is always normal x U, so
    # flipping the normal must reverse U or V; each helper keeps the axis a
    # caller reads the sketch by (vertical planes keep V = world +Z):
    #
    #   plane        normal  U    V
    #   xy(z)          +Z    +X   +Y
    #   xy(z, flip)    -Z    +X   -Y
    #   xz(y)          -Y    +X   +Z
    #   xz(y, flip)    +Y    -X   +Z
    #   yz(x)          +X    +Y   +Z
    #   yz(x, flip)    -X    -Y   +Z
    #
    # A flipped vertical plane is the sketch seen from behind: up is still up
    # and left-right is mirrored. `world()` / `uv()` do the arithmetic.

    @staticmethod
    def xy(z=0.0, flip=False):
        """The plane at height z. Normal +Z, U +X, V +Y; flipped, normal -Z
        and V -Y (U stays +X)."""
        return Plane((0, 0, z), (0, 0, -1 if flip else 1), (1, 0, 0))

    @staticmethod
    def xz(y=0.0, flip=False):
        """The plane at y. Normal -Y so V = world +Z (not -Z) — the V-axis
        trap, once — with U +X. Flipped, normal +Y and U -X (V stays +Z)."""
        if flip:
            return Plane((0, y, 0), (0, 1, 0), (-1, 0, 0))
        return Plane((0, y, 0), (0, -1, 0), (1, 0, 0))

    @staticmethod
    def yz(x=0.0, flip=False):
        """The plane at x. Normal +X, U +Y, V +Z; flipped, normal -X and
        U -Y (V stays +Z)."""
        if flip:
            return Plane((x, 0, 0), (-1, 0, 0), (0, -1, 0))
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
    """Persistent thread feature (cosmetic or modelled), usable as a screw relation's source."""

    def __init__(self, part, id, face=None):
        self.part, self.id, self.face = part, id, face

    def mill(self, setup, job=None, tool=None, **kw):
        """A CAM thread mill for this thread (docs/cam/operations.md#thread-mill):
        ``add_cam_op`` of type ``thread_mill`` on the threaded face, so its
        size, pitch, hand, side and class come from the Thread rather than
        being typed again. ``tool`` defaults to the setup's first thread
        mill; ``job`` names the job (without it, the job tab in front).
        Other keywords (``passes``, ``climb``, ``lead_arcs``, ``part``) pass
        through. Returns the operation as ``add_cam_op`` answers it."""
        if self.face is None:
            raise SdkError("this Thread was not made by part.thread(), so it "
                           "does not know its face")
        params = dict(setup=setup, type="thread_mill", target=self.face.ref)
        if job is not None:
            params["job"] = job
        if tool is not None:
            params["tool"] = tool
        params.update(kw)
        return self.part.app.add_cam_op(**params)


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
        # What MADE it (a feature id; for a Derived copy's, one of the source
        # part's) and in what role: end_cap, side, generated, bore...
        # (docs/modeling/toponaming.md). `feature_id` is the body's, which a
        # ref needs; this is the answer to "which cut is this the floor of".
        self.made_by = raw.get("made_by")
        self.role = raw.get("role")
        self.entity = raw.get("entity")

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
        self.area = raw.get("area_mm2", 0.0)
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
        self.length = raw.get("length_mm", 0.0)
        self.midpoint = V(raw["midpoint"]) if "midpoint" in raw else None
        # the names of the faces it joins
        self.faces = list(raw.get("faces", []))
        self.radius = raw.get("radius")

    @property
    def center(self):
        """A circular edge's centre — asked of the app, not guessed from the
        midpoint (which is a point ON the circle)."""
        if self.kind not in ("circle", "arc"):
            raise SdkError(f"edge {self.name!r} is a {self.kind}; only a "
                           f"circle or arc has a centre")
        return self.frame().origin


def _face_names(faces):
    """Face names from a Face, a list of Faces, or names."""
    if isinstance(faces, (Face, str)):
        faces = [faces]
    return {f.name if isinstance(f, Face) else f for f in faces}


def _face_passes(f, kind=None, where=None, normal=None, radius=None,
                 tol=0.01, radius_tol=0.05):
    """Body.faces' geometric filters, for one face."""
    if kind and f.kind != kind:
        return False
    if normal is not None and not (
            f.normal and f.normal.dot(V(normal).unit()) > 1 - tol):
        return False
    if radius is not None and (f.radius is None
                               or abs(f.radius - radius) >= radius_tol):
        return False
    return not where or where(f)


def _made_filter(made_by, role, via, entity):
    kw = {}
    if made_by is not None:
        kw["made_by"] = made_by.id if isinstance(made_by, Feature) else made_by
    if role is not None:
        kw["role"] = role
    if via is not None:
        kw["via"] = via.id if isinstance(via, Feature) else via
    if entity is not None:
        kw["entity"] = int(entity)
    return kw


class Feature:
    """A feature of a part, and the faces and edges it made.

    What `hole`, `fillet`, `pattern`, a cut and every feature call that
    makes no new body return -- and what a new body's `.feature` is. It
    still reads like the response dict it replaces (`h["id"]`,
    `r["regen"]`), so older scripts keep working.

    Its faces are asked of the app by what MADE them, not guessed from
    normals and positions (docs/modeling/toponaming.md):

        pocket = part.cut(sk, 8, reversed=True)
        part.pocket_op(pocket.floor())      # the pocket's floor, by name
        hole.bores()                        # every bore the Hole drilled

    A question with no answer raises, saying why -- "made side x4, none
    end_cap" for the floor of a cut that went through -- rather than
    returning an empty list a script would carry on with.
    """

    def __init__(self, part, raw):
        self.part = part
        self._raw = dict(raw)
        self.id = raw["id"]
        self.name = raw.get("name")
        self.type = raw.get("type")

    # the response dict, as before
    def __getitem__(self, key):
        return self._raw[key]

    def get(self, key, default=None):
        return self._raw.get(key, default)

    def __contains__(self, key):
        return key in self._raw

    def keys(self):
        return self._raw.keys()

    def __repr__(self):
        return f"Feature({self.id}, {self.name!r})"

    def _ask(self, role, entity, edges, via=None, on=None,
             raw_made_by=False, configuration=None):
        on = on or self.part
        made_by = self.id if raw_made_by else self
        kw = _made_filter(made_by, role, via, entity)
        if configuration is not None:
            kw["configuration"] = configuration
        r = on.app.list_elements(part=on.id, **kw)
        out = []
        for row in r["bodies"]:
            body = Body(on, row["body"], {"name": row["name"],
                                         "created_by": row["created_by"]})
            for raw in row["edges" if edges else "faces"]:
                out.append((Edge if edges else Face)(on, body, raw))
        if not out:
            raise SdkError(r.get("why") or
                           f"feature {self.id} made no "
                           f"{'edges' if edges else 'faces'} here"
                           + (f" in role {role!r}" if role else ""))
        return out

    def faces(self, role=None, entity=None):
        """The faces this feature made, in `role` if given: start_cap,
        end_cap (floor), side (walls; `entity=` the sketch line), generated,
        bore, bottom, counterbore, counterbore_floor, countersink, point
        (a blind hole's drill point), outside, inner_side, outer_side, shield (a bearing's), positional,
        boundary."""
        return self._ask(role, entity, edges=False)

    def edges(self, role=None, entity=None):
        """The edges named from the faces this feature made."""
        return self._ask(role, entity, edges=True)

    def floors(self):
        """A cut's floors -- one per pattern copy when it has copies."""
        return self.faces(role="end_cap")

    def floor(self):
        """A cut's floor, when it has exactly one."""
        return _only(self.floors(), "floor", {"feature": self.id})

    def walls(self, entity=None):
        """A cut's or an extrude's side faces; `entity=` the sketch line
        one was swept from."""
        return self.faces(role="side", entity=entity)

    def end_cap(self):
        """An extrude's far end (a revolve's, when partial)."""
        return _only(self.faces(role="end_cap"), "end cap",
                     {"feature": self.id})

    def start_cap(self):
        """An extrude's first end, on its sketch plane."""
        return _only(self.faces(role="start_cap"), "start cap",
                     {"feature": self.id})

    def bores(self):
        """Every bore a Hole drilled."""
        return self.faces(role="bore")

    def bottoms(self):
        """Every flat-floored blind hole's bottom. A hole drilled with a
        point (a fit's, by default) ends in a cone instead: points()."""
        return self.faces(role="bottom")

    def points(self):
        """Every blind hole's drill point: the cone its floor is."""
        return self.faces(role="point")


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
        self.volume = raw.get("volume_mm3", 0.0)
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
        from lobole.snapshots import MeasurementSnapshot
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

    def _made(self, made_by, role, via, entity):
        """This body's elements a feature made, asked of the app."""
        kw = _made_filter(made_by, role, via, entity)
        r = self._part.app.list_elements(body=self.index, part=self._part.id,
                                         **kw)["bodies"][0]
        return ([Face(self._part, self, f) for f in r["faces"]],
                [Edge(self._part, self, e) for e in r["edges"]])

    def faces(self, kind=None, where=None, normal=None, radius=None,
              tol=0.01, radius_tol=0.05, made_by=None, role=None, via=None,
              entity=None):
        """Faces, filtered. `normal=(0,0,1)` keeps the ones pointing that
        way; `where` is any predicate over a Face.

        `made_by=` (a Feature, an id or a name) and `role=` ask for the
        faces a feature made, in a role -- `made_by=pocket, role="floor"` --
        instead of guessing them from normals and positions; `via=` a
        Derived feature when they came in through one (made_by then names a
        feature of the source part). See Feature.faces."""
        if made_by is not None or role is not None or via is not None \
                or entity is not None:
            out = self._made(made_by, role, via, entity)[0]
        else:
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
              tol=0.05, made_by=None, role=None, via=None, entity=None,
              between=None):
        """Edges, filtered.

        `circular=True` means "a round rim" — circle OR arc. Whether the
        kernel represents a hole's rim as one circle or as two half arcs is
        an implementation detail of the file it came from, and asking for
        `kind="circle"` silently misses the arc form: the Pi camera's four
        mounting holes are 24 arcs, and a filter written for circles found
        none of them and drilled nothing.

        `tol` is a real tolerance in millimetres (0.05 by default), not a
        float-comparison epsilon: an imported radius is rarely exact.

        `made_by=`, `role=`, `via=`, `entity=` as for faces(): an edge's
        origin is the face it was named from's.

        `between=(a, b)` keeps the edges joining a face of `a` to a face of
        `b` -- each a Face, a list of them, or names: the edges where a
        slab's top meets its walls are
        `edges(between=(slab.end_cap(), slab.walls()))`. An edge's own name
        comes from whichever face named it first, so it cannot say this.
        """
        if made_by is not None or role is not None or via is not None \
                or entity is not None:
            out = self._made(made_by, role, via, entity)[1]
        else:
            self._load()
            out = self._edges
        if between is not None:
            a, b = (_face_names(x) for x in between)
            out = [e for e in out
                   if any(x in a and y in b and x != y
                          for x in e.faces for y in e.faces)]
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
        self.plane_ref = None  # a Frame feature's id: sketch(on=frame)
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
        normal case (generators/README.md#single-cylinder-and-air-cooled-engines).

        `radius` may be an expression string or a `Param`, like a circle's:
        an arc whose centre follows a variable while its radius stayed the
        number it was drawn at no longer met the lines either side of it.
        """
        n = len(self.entities)
        if isinstance(radius, str):
            self._pending_exprs.append((n, "r", radius))
            radius_mm = 0.0
        else:
            if isinstance(radius, Param):
                self._pending_exprs.append((n, "r", radius.expr))
            radius_mm = float(radius)
        self.entities.append({"type": "arc",
                              "center": list(_uv(self, center, n, 0, "arc")),
                              "radius": radius_mm,
                              "start_angle": math.radians(start_deg),
                              "end_angle": math.radians(end_deg),
                              "construction": bool(construction)})
        # Its own index (it said 1, so an expression pending on an arc landed
        # on the second entity) — and `self`, so it chains like line().
        self._attach_exprs(n)
        return self

    def text(self, s, at, height, font="sans", angle_deg=0.0,
             halign="left", valign="baseline", spacing=0.0,
             construction=False):
        """A one-line text label (the app's `text` entity, v137) anchored at
        `at`. `height` is the CAP height in mm (a number, a Param or an
        expression string); `font` is "sans" (outline: extruding it cuts or
        adds every letter) or "hershey" (single stroke: open curves for
        engraving, no profile). `halign` left|center|right and `valign`
        baseline|bottom|middle|top place the label's box — the font's
        ascender and descender — on the anchor. The app refuses an unknown
        font or alignment, and a character the font has no glyph for, by
        name. The glyphs are regenerated from these fields, never sent."""
        n = len(self.entities)
        e = {"type": "text", "text": str(s),
             "anchor": list(_uv(self, at, n, 0, "text")),
             "font": font, "angle_deg": float(angle_deg),
             "halign": halign, "valign": valign}
        if isinstance(height, str) or isinstance(height, Param):
            expr = height if isinstance(height, str) else height.expr
            self._pending_exprs.append((n, "height", expr))
            e["height"] = float(height) if isinstance(height, Param) else 1.0
        else:
            e["height"] = float(height)
        if spacing:
            e["spacing"] = float(spacing)
        if construction:
            e["construction"] = True
        self.entities.append(e)
        self._attach_exprs(n)
        return self

    def spline(self, poles, degree=3, closed=False, construction=False):
        """A B-spline through CONTROL POLES [(u, v), ...] (the app's `spline`
        entity, v40): the curve follows the poles and touches only the first
        and last. `closed` repeats the first pole at the end, which makes a
        closed profile an extrude can use — dense poles sampled off a smooth
        outline (a cam lobe) give one smooth face where a polyline gives a
        facet per segment. A closed spline through a convex polygon lies
        just INSIDE it.

        A pole coordinate may be a `Param` or an expression string, as a line
        endpoint may: it rides as `p<k>x_expr` / `p<k>y_expr`, so the outline
        follows the variable that sizes the rest of the part."""
        poles = list(poles)
        if closed and poles and tuple(poles[0]) != tuple(poles[-1]):
            poles.append(poles[0])
        if len(poles) < degree + 1:
            raise SdkError(f"a degree-{degree} spline needs at least "
                           f"{degree + 1} poles, got {len(poles)}")
        n = len(self.entities)
        pts, exprs = [], {}
        for k, q in enumerate(poles):
            if isinstance(q, (tuple, list)) and len(q) == 2:
                for axis, c in zip("xy", q):
                    expr = c if isinstance(c, str) else (
                        c.expr if isinstance(c, Param) else None)
                    if expr is not None:
                        exprs[f"p{k}{axis}_expr"] = expr
                u, v = (0.0 if isinstance(c, str) else float(c) for c in q)
                pts.append((u, v))
            else:
                pts.append(tuple(_uv(self, q)))
        e = {"type": "spline", "degree": int(degree),
             "poles": [[float(u), float(v)] for u, v in pts]}
        e.update(exprs)
        if construction:
            e["construction"] = True
        self.entities.append(e)
        self._attach_exprs(n)
        return self

    def rect(self, c1, c2):
        """An axis-aligned rectangle between two opposite corners.

        A corner coordinate that is a `Param` or an expression string makes
        it four lines whose endpoints carry the expressions (the app's own
        rectangle has no expression fields). It used to go in as that
        rectangle at the numbers the Params held: on the pan/tilt head the
        yoke's base and foot flanges stayed the size a 1.5 kg camera made
        them while the joint-screw holes drawn as points followed a 1.8 kg
        one, out through the base's end (generators/pantilt/README.md)."""
        if _driven(c1) or _driven(c2):
            (u1, v1), (u2, v2) = (c if _driven(c) else _uv(self, c)
                                  for c in (c1, c2))
            return self.polyline([(u1, v1), (u2, v1), (u2, v2), (u1, v2)])
        self.entities.append({"type": "rect", "c1": list(_uv(self, c1)),
                              "c2": list(_uv(self, c2))})
        return self

    def centered_rect(self, center, w, h):
        """A rectangle `w` by `h` about `center`; Params stay variables (the
        corners are Param arithmetic, which rect() keeps)."""
        given = list(center) if _driven(center) else []
        if any(isinstance(c, str) for c in given + [w, h]):
            raise SdkError("centered_rect takes numbers or Params, not "
                           "expression strings: give rect() the corners")
        cu, cv = center if given else _uv(self, center)
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
        extruding (generators/README.md#single-cylinder-and-air-cooled-engines).

        Centres or a width given as `Param`s keep their expressions when the
        slot runs along u or v (its sides are then the centres' coordinates
        plus or minus half the width); an oblique one is refused, since its
        sides need trigonometry of the Params, rather than drawn at the
        numbers they hold and left there.
        """
        if _driven(a) or _driven(b) or isinstance(width, (Param, str)):
            return self._slot_driven(a, b, width)
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

    def _slot_driven(self, a, b, width):
        """slot() with Param centres or width: two lines and two arcs whose
        coordinates and radius carry the expressions."""
        given = [c for p in (a, b) if _driven(p) for c in p] + [width]
        if any(isinstance(c, str) for c in given):
            raise SdkError("slot takes numbers or Params, not expression "
                           "strings: a string has no value to draw it at")
        (au, av), (bu, bv) = (p if _driven(p) else _uv(self, p)
                              for p in (a, b))
        du, dv = float(bu) - float(au), float(bv) - float(av)
        if abs(du) < 1e-9 and abs(dv) < 1e-9:
            raise SdkError("a slot's two centres coincide")
        if abs(du) > 1e-9 and abs(dv) > 1e-9:
            raise SdkError(
                "a slot driven by Params must run along u or v: an oblique "
                "one's sides need the Params' trigonometry, which a sketch "
                "coordinate cannot carry — draw it with numbers, or as "
                "lines and arcs")
        rad = width / 2.0
        # The side offset, perpendicular to the slot (as slot() turns it).
        if abs(dv) < 1e-9:
            off = (0, rad) if du > 0 else (0, -rad)
        else:
            off = (-rad, 0) if dv > 0 else (rad, 0)

        def shift(p, sign):
            return tuple(c if (isinstance(o, int) and o == 0) else
                         (c + o if sign > 0 else c - o)
                         for c, o in zip(p, off))

        self.line(shift((au, av), 1), shift((bu, bv), 1))
        self.line(shift((au, av), -1), shift((bu, bv), -1))
        ang = math.degrees(math.atan2(dv, du))
        self.arc((bu, bv), rad, ang - 90.0, ang + 90.0)
        self.arc((au, av), rad, ang + 90.0, ang + 270.0)
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
        if self.plane_ref is not None:
            f["plane_ref"] = self.plane_ref
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
    "text": {0: ("x", "y")},
}


def _driven(at):
    """Whether a sketch position given as a (u, v) pair has a coordinate the
    document should evaluate: a `Param` or an expression string."""
    return (isinstance(at, (tuple, list)) and len(at) == 2 and
            any(isinstance(c, (Param, str)) for c in at))


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
            # app does not make expression-driven — it just will not follow
            # (rect() draws lines instead, which do). A bare string has no
            # value and cannot.
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

    def render(self, path, view="iso"):
        """Screenshot this part or assembly on its own, fitted. Needs a
        renderer, so an `--offscreen` instance (a `--headless` one refuses,
        saying so).

        Here because looking is what caught the defect none of the other
        checks could: four bolt bosses hanging off a deck flange, one solid
        body, clean study, bolts correctly sized, `check()` green
        (generators/README.md#single-cylinder-and-air-cooled-engines).
        """
        self.activate()
        self.app.screenshot(path=path, view=view, fit=True)
        return path


class Part(_Tab):
    def _add(self, feature):
        self.activate()
        return self.app.add_feature(feature=feature)

    def _feature(self, feature):
        """Add a feature and hand back its Feature (the response dict,
        with the faces it made to ask for)."""
        r = self._add(feature)
        return Feature(self, dict(r, name=feature.get("name"),
                                  type=feature.get("type")))

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
        with the body), `on=<Frame>` (one this part made) on the frame's XY
        plane and `on=<DatumPlane>` on the datum, each followed at every
        regeneration; `plane=` takes a Plane, a Frame or a Face, as numbers
        (a snapshot)."""
        if isinstance(on, Frame):
            if on.id is None:
                raise SdkError(
                    "that frame is a query result, not a Frame feature: "
                    "create one with part.frame(name, ...) to sketch on")
            b = SketchBuilder(self, on.plane(), name)
            b.plane_ref = on.id
            return b
        if isinstance(on, DatumPlane):
            return SketchBuilder(self, on, name)
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
             "sketch_id": ref_of(sketch), "mode": mode}
        _dim(f, "distance", distance)
        f.update(kw)
        return self._body_from(self._add(f), f)

    def cut(self, sketch, distance, **kw):
        return self.extrude(sketch, distance, mode="cut", **kw)

    @_renamed(angle=("angle_deg", "the same degrees"))
    def revolve(self, sketch, axis="z", angle_deg=360.0, mode="new_body",
                name=None, **kw):
        f = {"type": "Revolve", "name": name or f"Revolve {sketch.name}",
             "sketch_id": ref_of(sketch), "mode": mode,
             "axis": axis if isinstance(axis, dict) else {"kind": axis}}
        _dim(f, "angle_deg", angle_deg)
        f.update(kw)
        return self._body_from(self._add(f), f)

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

    # --- primitives ---------------------------------------------------------
    # A primitive is for a fixture, a stand-in or a first rough block; the
    # model is built on faces and planes from sketches. Each one is placed
    # `on` something -- "Top"/"Front"/"Right", a planar Face, a datum_plane(),
    # a frame() or a committed Sketch -- and stored as a dimensioned sketch on
    # it plus an extrude (a sphere: a revolve), so it follows what it is on
    # (docs/modeling/README.md#primitives). `at` is (u, v) in that thing's
    # frame: on a face its origin is the face's centroid and its u is
    # `face.frame().x`; on Top u is +X and v +Y. Every size, and `at`, takes
    # a number, a Param or an expression string. A size left out is derived
    # from what it is on.

    @_renamed(position=("on", "what the box stands on (a face, a plane, a "
                                "frame); `at=(u, v)` places it there"),
              w=("width", "the extent along the frame's u"),
              h=("height", "the extent along the NORMAL (it was the Y "
                           "extent)"),
              d=("depth", "the extent along the frame's v (it was the Z "
                          "extent)"))
    def box(self, on, width=None, depth=None, height=None, at=None,
            centered=True, mode="new_body", reversed=False, name="Box"):
        """A box on `on`: `width` along u, `depth` along v, `height` along
        the normal (`reversed=True` the other way). `at` is its centre, or
        with `centered=False` its low-u low-v corner."""
        f = {"type": "Box", "name": name, "on": _on(on), "mode": mode}
        _dim(f, "width", width)
        _dim(f, "depth", depth)
        _dim(f, "height", height)
        if not centered:
            f["centered"] = False
        if reversed:
            f["reversed"] = True
        _at(f, at)
        return self._body_from(self._add(f), f)

    @_renamed(center=("on", "what the cylinder stands on; `at=(u, v)` "
                            "places its axis there"),
              direction=("on", "a face, plane or frame: the axis is its "
                               "normal"),
              h=("height", "the same length, along the normal"))
    def cylinder(self, on, d=None, r=None, height=None, at=None,
                 mode="new_body", reversed=False, name="Cylinder"):
        """A cylinder standing on `on`, its axis along the normal through
        `at`."""
        f = {"type": "Cylinder", "name": name, "on": _on(on), "mode": mode}
        _diameter(f, d, r, "cylinder")
        _dim(f, "height", height)
        if reversed:
            f["reversed"] = True
        _at(f, at)
        return self._body_from(self._add(f), f)

    @_renamed(center=("on", "what the sphere sits on; `at=(u, v)` puts its "
                            "centre there, ON that plane"))
    def sphere(self, on, d=None, r=None, at=None, mode="new_body",
               name="Sphere"):
        """A sphere centred at `at` on `on` (half above it, half below)."""
        f = {"type": "Sphere", "name": name, "on": _on(on), "mode": mode}
        _diameter(f, d, r, "sphere")
        _at(f, at)
        return self._body_from(self._add(f), f)

    def hole(self, sketch, d, depth=None, through=False, kind="simple", **kw):
        """Holes at a sketch's points and at its circles' centres — so a
        sketch that PROJECTED another body's hole edges drills exactly
        there."""
        f = {"type": "Hole", "name": kw.pop("name", "Hole"),
             "sketch_id": ref_of(sketch), "hole_kind": kind}
        _dim(f, "diameter", d)
        if through:
            f["through"] = True
        if depth is not None:
            _dim(f, "depth", depth)
        f.update(kw)
        return self._feature(f)

    # --- templates (docs/modeling/templates.md) ---------------------------
    def save_template(self, name, features, literals=None, strings=None,
                      rename=None, comments=None, comment="", replace=False,
                      dry_run=False):
        """Save `features` of this part (Feature/Body objects or ids) as a
        template in the project's library. Every variable they read becomes
        a parameter of its name (or `rename`'s); `literals` makes literal
        fields parameters, as {feature, path, param} dicts (a check_literals
        path: "distance", "entities/0/r"); what a sketch stands on becomes
        the reference parameter `on`. `replace` saves the next revision of a
        template of that name. Returns the app's answer."""
        def fid(x):
            if isinstance(x, Body) and getattr(x, "feature", None) is not None:
                return x.feature.id
            return ref_of(x)
        kw = {"name": name, "feature_ids": [fid(f) for f in features]}
        if literals:
            kw["literals"] = [{"feature_id": fid(l["feature"]),
                               "path": l["path"], "param": l["param"]}
                              for l in literals]
        if strings:
            kw["strings"] = [dict(s_, feature_id=fid(s_.pop("feature")))
                             for s_ in (dict(x) for x in strings)]
        if rename:
            kw["rename"] = dict(rename)
        if comments:
            kw["comments"] = dict(comments)
        if comment:
            kw["comment"] = comment
        if replace:
            kw["replace"] = True
        if dry_run:
            kw["dry_run"] = True
        self.activate()
        return self.app.save_template(**kw)

    def template(self, template, on, params, name=None):
        """An instance of the library's `template` standing on `on` (a Face,
        a DatumPlane, a Frame, a Sketch or "Top"/"Front"/"Right"), with a
        value for EVERY parameter in `params` -- a number, a Param or an
        expression string for a number (a Param keeps its formula), a str
        for a string, a Face for a reference. Returns its Feature; its
        `.features` are the features it expanded into, each a Feature whose
        faces can be asked for like any (the instance's own faces are named
        under their ids)."""
        values = {}
        for k, v in params.items():
            if isinstance(v, Param):
                values[k] = v.expr
            elif isinstance(v, (Face, Edge)):
                values[k] = dict(v.ref)
            else:
                values[k] = v
        f = {"type": "Template", "template": template, "on": _on(on),
             "parameters": values}
        if name:
            f["name"] = name
        r = self._add(f)
        feat = Feature(self, dict(r, name=name or template, type="Template"))
        feat.features = [Feature(self, dict(x)) for x in r.get("features", [])]
        return feat

    def thread(self, face, designation, allow_engagement=False, minimum_engagement=0,
               modeled=False, hand="right", flank_angle_deg=None, pitch=None):
        """Annotate actual manufacturing geometry; engagement is explicit opt-in.

        Matching coaxial threads relieve only their common axial span during
        exact contact checks. Cosmetic by default: no geometry or STEP
        machining diameter is changed. ``modeled=True`` cuts the real helical
        groove (``hand`` "right" or "left"); a cut that fails is the
        feature's error and keeps the body.

        ``pitch`` (mm) is a CUSTOM thread's -- a designation that names no
        series -- and it needs one. A table thread takes its row's: leave it
        out, or give exactly that; another is REFUSED naming the row's pitch
        (and the fine row that has yours: "M8" with 1.0 is "M8x1").

        ``flank_angle_deg`` is half the thread angle (30, the ISO profile, when
        omitted); a CUSTOM thread may take 15-60, and 45 prints with no flank
        past 45 degrees of overhang
        (docs/modeling/holes-and-threads.md#flank-angle).
        """
        if hand not in ("right", "left"):
            raise SdkError(f"thread hand must be 'right' or 'left', not {hand!r}")
        if not isinstance(face, Face) or face.kind != "cylinder":
            raise SdkError("thread needs a cylindrical Face")
        if face._part.id != self.id or face._part.app is not self.app:
            raise SdkError("thread face belongs to a different part or connection")
        extra = {}
        if pitch is not None:
            extra["pitch"] = float(pitch)
        if flank_angle_deg is not None:
            extra["flank_angle_deg"] = float(flank_angle_deg)
        feature = self._add(
            dict(
                type="Thread",
                name=designation,
                face=face.ref,
                designation=designation,
                allow_engagement=allow_engagement,
                minimum_engagement=minimum_engagement,
                modeled=bool(modeled),
                hand=hand,
                **extra,
            )
        )
        return Thread(self, feature["id"], face)

    def _gear_blank(self, face, what):
        if not isinstance(face, Face) or face.kind != "cylinder":
            raise SdkError(f"{what} needs the blank's cylindrical Face")
        if face._part.id != self.id or face._part.app is not self.app:
            raise SdkError(f"{what} face belongs to a different part or connection")

    def worm(self, face, module=None, starts=1, hand="right", backlash=0.0,
             length=0.0, pressure_angle_deg=20.0, name="Worm"):
        """Cut a ZA worm into a cylindrical blank (docs/modeling/worm-gears.md).

        The blank's outside is the worm's tip: its pitch diameter is the tip
        less two modules, and ``module`` omitted is tip / 12 (a diameter
        quotient of 10). ``backlash`` (mm, axial) thins each flank by half of
        it. The response's ``report`` carries the lead angle and whether the
        pair self-locks dry and greased.
        """
        if hand not in ("right", "left"):
            raise SdkError(f"worm hand must be 'right' or 'left', not {hand!r}")
        self._gear_blank(face, "worm")
        f = dict(type="Worm", name=name, face=face.ref, starts=starts,
                 hand=hand, pressure_angle_deg=pressure_angle_deg)
        if module:
            f["module"] = module
        if backlash:
            f["backlash"] = backlash
        if length:
            f["length"] = length
        return self._feature(f)

    def worm_wheel(self, face, teeth=40, module=None, starts=1,
                   worm_diameter=None, hand="right", pressure_angle_deg=20.0,
                   name="Worm wheel"):
        """Hob a worm wheel from a cylindrical blank: its rim is the face,
        its length the face width. Give the mating worm's starts, pitch
        diameter (``worm_diameter``; omitted = 10 modules) and hand; the
        ``report`` carries the centre distance to mount the worm at, the
        ratio, the throat and self-locking."""
        if hand not in ("right", "left"):
            raise SdkError(f"worm wheel hand must be 'right' or 'left', not {hand!r}")
        self._gear_blank(face, "worm_wheel")
        f = dict(type="WormWheel", name=name, face=face.ref, teeth=teeth,
                 starts=starts, hand=hand, pressure_angle_deg=pressure_angle_deg)
        if module:
            f["module"] = module
        if worm_diameter:
            f["worm_diameter"] = worm_diameter
        return self._feature(f)

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
        return self._feature(f)

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
                 "parent_id": ref_of(parent) if parent is not None else 0,
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

        `count` may be a `Param` (its value a whole number): it goes as the
        app's `count_expr`, and the derived step as 360 over it, so the
        copies follow the variable. `int(count)` used to drop it: the
        pan/tilt knob's flutes stayed a 1.5 kg camera's count on a knob
        grown for a heavier one.
        """
        if isinstance(count, str):
            raise SdkError("pattern: count takes a number or a Param, not an "
                           "expression string (it has no value to check)")
        f = {"type": "Pattern", "name": name, "source_id": ref_of(source)}
        n = int(count)
        if isinstance(count, Param):
            if abs(float(count) - n) > 1e-9:
                raise SdkError(f"pattern: count {count!r} is not a whole "
                               f"number — floor() or ceil() its expression")
            f["count_expr"] = count.expr
        else:
            count = n
        if linear is not None:
            f.update({"kind": "linear",
                      "axis": linear if isinstance(linear, dict)
                      else {"kind": linear}, "count": n})
            _dim(f, "spacing", spacing)
        elif circular is not None:
            f.update({"kind": "circular",
                      "axis": circular if isinstance(circular, dict)
                      else {"kind": circular}, "count": n})
            if step_deg is None:
                if n < 1:
                    raise SdkError(f"pattern: count must be at least 1, "
                                   f"got {count}")
                step_deg = 360.0 / count
            _dim(f, "angle_deg", step_deg)
        else:
            raise SdkError("pattern takes linear= or circular= (an axis)")
        return self._feature(f)

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
    # (docs/scripting/rpc.md#reads-name-their-part).
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
        """Every body's faces, filtered as Body.faces. With `made_by=`,
        `role=`, `via=` or `entity=` it is one question of the whole part,
        and one with no answer raises with the app's why; `configuration=`
        asks it of the part as it is in that configuration (what a job part
        or a placement in it is made of)."""
        made = {k: kw.pop(k) for k in ("made_by", "role", "via", "entity",
                                       "configuration") if k in kw}
        if made:
            got = Feature(self, {"id": made.get("made_by")})._ask(
                made.get("role"), made.get("entity"), edges=False,
                via=made.get("via"), raw_made_by=True,
                configuration=made.get("configuration"))
            return [f for f in got if _face_passes(f, **kw)] if kw else got
        return [f for b in self.bodies() for f in b.faces(**kw)]

    def feature(self, name):
        """A feature of this part by name (or id), as a Feature: its faces
        are then `.faces(role=...)`, and it is what `made_by=` takes -- also
        for a Derived copy of this part, with `via=`."""
        rows = self.state()["features"]
        hit = [f for f in rows if f.get("name") == name or f.get("id") == name]
        return Feature(self, _only(hit, "feature", {"name": name}))

    def edges(self, **kw):
        return [e for b in self.bodies() for e in b.edges(**kw)]

    def islands(self, z=None, normal=(0, 0, 1), origin=None):
        """The connected regions of material a plane cuts this part into.

        `part.islands(z=200)` cuts a horizontal plane at z=200 and returns the
        areas, biggest first. Two islands where you expected one means a
        feature is not joined to the rest THERE, whatever it may be joined to
        somewhere else — which is how four bolt bosses came to hang off a deck
        flange with every other check green
        (generators/README.md#single-cylinder-and-air-cooled-engines).
        """
        if origin is None:
            origin = (0.0, 0.0, float(z or 0.0))
        r = self.app.section(origin=list(V(origin)), normal=list(V(normal)),
                             regions=True, part=self.id)
        return [x["area_mm2"] for x in r["regions"]]

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

        (generators/README.md#single-cylinder-and-air-cooled-engines)
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
        return self.app.mass_properties(part=self.id)["total"]["volume_mm3"]

    def bbox(self):
        """The part's MEASURED extent, from mass_properties.

        Not get_state's, which is a cheap conservative bound — it wraps a
        swept surface by its control points, so a helical spring reads about
        sixty per cent over. That box is right for a broad phase and wrong for
        a person (generators/README.md#single-cylinder-and-air-cooled-engines).
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

    def _body_from(self, r, feature=None):
        """A feature response, as the Body it created (looked up after the
        regeneration, so its faces and edges are the real ones), with the
        feature as its `.feature`. A feature that made no body of its own --
        a cut, an extrude joined to another body -- is the Feature (it was
        None, which left a cut's floor with nothing to ask)."""
        fid = r["id"]
        feat = Feature(self, dict(r, name=(feature or {}).get("name"),
                                  type=(feature or {}).get("type")))
        for b in self.bodies(reference=False):
            if b.created_by == fid:
                b.feature = feat
                return b
        return feat


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
        # The `frame` RPC resolves an occurrence against the ACTIVE assembly,
        # so make this one active first — as bbox() does — or the id is
        # looked up in whatever tab a previous call left open.
        self.asm.activate()
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

    def inner(self, occurrence, name=None):
        """An occurrence INSIDE this placed sub-assembly, by its id there (or
        an Occurrence handle from that assembly's own tab): a mate side
        `(occ.inner(k), "frame")` reaches into it with `inner_path`, and
        `.frame()` reads it where it stands. On a FLEXIBLE placement a
        moving part is mated as itself."""
        inner_id = occurrence.id if hasattr(occurrence, "id") else occurrence
        label = name or getattr(occurrence, "name", None) or str(inner_id)
        return InnerOccurrence(self, inner_id, label)

    def mate(self, mate):
        """A mate INSIDE this flexible sub-assembly placement, named
        {path, mate} (v129): its id in the sub-assembly, from that
        assembly's own tab. A relation or a motion study here can then drive
        it — an engine's chain turning its pump's camshaft."""
        return InnerMate(ref_of(mate), [self.id])

    def __repr__(self):
        return f"Occurrence({self.name!r}, id={self.id})"


class InnerOccurrence:
    """An occurrence inside a placed sub-assembly: the top-level placement
    (`id`, what a connector's `occurrence` is) and the path below it."""

    def __init__(self, root, inner_id, name):
        self.root, self.asm = root, root.asm
        self.id, self.inner_path = root.id, [inner_id]
        self.name = f"{root.name}/{name}"

    @property
    def path(self):
        return [self.root.id] + self.inner_path

    def frame(self, name=None):
        """WHERE a named frame of it is, in world space — as the placement
        has it standing (a flexible one's own pose of it)."""
        self.asm.activate()
        kw = {"occurrence": "/".join(str(i) for i in self.path)}
        if name is not None:
            kw["frame"] = name
        return Frame.from_query(self.asm, self.asm.app.frame(**kw), name=name)

    def __repr__(self):
        return f"InnerOccurrence({self.name!r}, path={self.path})"


class InnerMate(dict):
    """A mate named {path, mate} (v129): `id` in the sub-assembly the
    flexible placement `path` places."""

    def __init__(self, mate_id, path):
        super().__init__(id=mate_id, path=list(path))

    @property
    def id(self):
        return self["id"]

    @property
    def path(self):
        return self["path"]


def _mate_ref(key, mate):
    """`{key: id}`, plus `{key}_path` for a mate inside a flexible
    placement."""
    out = {key: ref_of(mate)}
    if isinstance(mate, InnerMate):
        out[f"{key}_path"] = mate.path
    return out


def _member(mate, **extra):
    """A member of a relation over several mates (v171): `{mate, path?}`
    plus whatever the member carries (a planet's teeth, a term's dof and
    coefficient)."""
    out = {"mate": ref_of(mate)}
    if isinstance(mate, InnerMate):
        out["path"] = mate.path
    out.update({k: v for k, v in extra.items() if v is not None})
    return out


class Asm(_Tab):
    def place(self, part, at=None, fixed=False, name=None, flexible=False,
              configuration=None, material=None):
        """Place a part OR ANOTHER ASSEMBLY. `at` is a world position; leave
        it out and mate it instead — which is the point.

        `add_occurrence` has always taken either ("a part OR another assembly
        in the active assembly"), and this took only a Part — so a subassembly
        had to be placed by passing its name as a bare string, which reads
        like a mistake rather than a feature.

        `configuration` (a name or id) is which of the placed document's
        configurations this placement is: a sub-assembly's, or a PART's
        (v135) -- one piece part placed as a pawn and as a rook.
        `material` (a slug) is what this placement of a part is made of,
        over the part's own: one part in maple and in walnut is one part.
        `flexible=True` places a sub-assembly whose own mechanism MOVES here
        (v129; its mates are then reached with `occ.mate(...)`).
        """
        self.activate()
        kw = {"part": part.name if isinstance(part, (Part, Asm)) else part,
              "fixed": fixed}
        if flexible:
            kw["flexible"] = True
        if name:
            kw["name"] = name
        if configuration is not None:
            kw["configuration"] = configuration
        if material is not None:
            kw["material"] = material
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
        """Every mate goes through here, which is why the `flipped` default
        is fixed here.

        `add_mate`'s own default is TRUE: the two connectors meet face to
        face with their normals opposed, which is what two touching FACES do
        and the opposite of what two FRAMES want. Its help says so. Left at
        the default, a bicycle's twenty-two occurrences all mated, nothing
        errored, and the result was a different bicycle — the fork turned end
        for end about its own steering axis and took the front wheel 700 mm
        into the air; the seatpost, saddle and carrier hung underneath; the
        chain and its guard were mirrored onto the wrong side.

        So: frame to frame defaults to FALSE here, face anchors keep the
        RPC's default, and anything that says `flipped` or `derive` for
        itself is left alone.
        """
        self.activate()
        ca, cb = _conn(a), _conn(b)
        if ("flipped" not in kw and "derive" not in kw
                and "frame" in ca and "frame" in cb):
            kw["flipped"] = False
        return self.app.add_mate(type=type_, a=ca, b=cb, **kw)

    def fasten(self, a, b, **kw):
        return self._mate("fastened", a, b, **kw)

    def join(self, a, b, **kw):
        """Fasten, with the app choosing which side and how far round.

        The one question a fastened mate between two frames cannot be asked
        in the call — `flipped` and `rotation` have eight combinations and
        none is predictable — is answered where both frames are visible:
        `derive="twist"` reads them off where the two occurrences already
        lie. The anchors still COINCIDE, so the joint is exact; only the way
        round comes from the placement.

        This is what to reach for when a part is placed roughly right and
        should end up exactly on its mate. It refuses, rather than picking a
        nearest turn, when the two anchors are not square to one axis — that
        is a joint that does not fit, and rounding it would hide that.

        `fasten_in_place` is the other question: keep the part exactly where
        it is, choosing among the eight EXACT joints and saying how far the
        nearest one was. Use that when the placement is the answer; use this
        when the mate is.
        """
        return self._mate("fastened", a, b, derive="twist", **kw)

    def fasten_in_place(self, a, b, tol=0.05, **kw):
        """Fasten, and pick the way round that leaves the moving part where
        it already is.

        A fastened mate between two frames has eight outcomes — which side,
        and which quarter turn about the joint's axis — and none is
        predictable from the call. Measured on one cabinet:
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
        # First ask the app, which can see both frames: `derive="twist"`
        # names the one exact joint whose way round matches the placement in
        # hand. When the placement is right — a generator computing its own
        # layout, which is this method's whole audience — that IS the answer,
        # in one solve instead of up to eight. It refuses when the two
        # anchors are not square to one axis, and then the search below still
        # has something to say: the nearest of the eight, and how far off.
        try:
            m = self._mate("fastened", a, b, derive="twist", **kw)
            self.solve()
            if max(abs(x - y) for x, y in
                   zip(_occ_centre(moving), was)) <= tol:
                return m
        except RuntimeError:
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

    def gear(self, mate_a, mate_b, teeth_a, teeth_b, internal=False, **kw):
        """A gear pair from its tooth counts: meshing outside they
        counter-rotate (ratio -teeth_a/teeth_b); `internal=True` is a gear
        meshing INSIDE a ring, which turns the same way (+teeth_a/teeth_b)."""
        self.activate()
        if internal:
            kw["internal"] = True
        return self.app.add_relation(type="gear", **_mate_ref("a", mate_a),
                                     **_mate_ref("b", mate_b), teeth_a=teeth_a,
                                     teeth_b=teeth_b, **kw)

    def epicyclic(self, sun, ring, carrier, teeth_sun, teeth_ring,
                  planets=(), **kw):
        """An epicyclic train by Willis (v171): sun, ring and carrier are
        their revolute mates, all turning about one axis in one frame.
        `planets` is [(planet_mate, teeth), ...], each planet's mate on the
        carrier; every one gets its own relation for its turn there.
        Driving one member holds the next in term order (the ring before the
        carrier) where it stands, and the relation's `held` says so
        (docs/assemblies/relations.md#relations-over-several-mates)."""
        self.activate()
        args = dict(type="epicyclic", sun=_member(sun), ring=_member(ring),
                    carrier=_member(carrier), teeth_sun=teeth_sun,
                    teeth_ring=teeth_ring)
        if planets:
            args["planets"] = [_member(m, teeth=z) for m, z in planets]
        return self.app.add_relation(**args, **kw)

    def linear_n(self, terms, **kw):
        """sum(coefficient * dof) = constant over several mates (v171):
        `terms` is [(mate, coefficient), ...] or [(mate, coefficient, dof),
        ...] — dof "rotation" or "offset", required of a cylindrical mate.
        `constant` defaults to where the members stand."""
        self.activate()
        out = []
        for t in terms:
            mate, coefficient, dof = (tuple(t) + (None,))[:3]
            out.append(_member(mate, coefficient=coefficient, dof=dof))
        return self.app.add_relation(type="linear_n", terms=out, **kw)

    def worm(self, worm_mate, wheel_mate, starts, teeth, hand, **kw):
        """Couple a worm's revolute to its wheel's on crossed axes. The ratio
        is starts/teeth; its sign is read from the hand and from where the
        worm sits (docs/assemblies/relations.md#worm) — a gear relation's
        fixed sign is wrong for half of the ways a pair goes together."""
        if hand not in ("right", "left"):
            raise SdkError(f"worm hand must be 'right' or 'left', not {hand!r}")
        self.activate()
        return self.app.add_relation(type="worm", **_mate_ref("a", worm_mate),
                                     **_mate_ref("b", wheel_mate), starts=starts,
                                     teeth=teeth, hand=hand, **kw)

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
            a=ref_of(mate),
            b=ref_of(mate),
            thread_part=thread.part.id,
            thread_feature=thread.id,
            **kw,
        )

    def chain(self, mate_a, mate_b, teeth_a, teeth_b, **kw):
        """A chain or belt: like a gear, but both sprockets turn the SAME
        way (v88)."""
        self.activate()
        return self.app.add_relation(type="chain", **_mate_ref("a", mate_a),
                                     **_mate_ref("b", mate_b), teeth_a=teeth_a,
                                     teeth_b=teeth_b, **kw)

    def rack(self, rotating, sliding, pitch_radius, **kw):
        """Rack and pinion: `rotating`'s turn and `sliding`'s slide, coupled
        by the pinion's pitch radius (mm) — offset = pitch_radius * angle.
        Either side can drive: a rack slider turns the pinion, a turned
        pinion slides the rack. `rotating` is a revolute or cylindrical mate
        (its turn), `sliding` a slider or cylindrical one (its slide)."""
        self.activate()
        return self.app.add_relation(type="rack", **_mate_ref("a", rotating),
                                     **_mate_ref("b", sliding),
                                     pitch_radius=pitch_radius, **kw)

    def cam(self, mate_a, mate_b, **kw):
        """A cam: rotation to the follower's slide or turn through a lift
        table (lift in mm for a slide, degrees for a turn), or {rise (mm) |
        rise_deg, duration_deg, start_deg} for a cosine lobe. A cylindrical
        follower takes dof_b="rotation" to turn (v88, turning v102)."""
        self.activate()
        return self.app.add_relation(type="cam", **_mate_ref("a", mate_a),
                                     **_mate_ref("b", mate_b), **kw)

    def intermittent(self, mate_a, mate_b, start_deg, slots=None, **kw):
        """A carry (v174): a's rotation steps b once per turn (or
        period_deg) during a window opening at start_deg, b standing still
        the rest of it. With `slots`, an external Geneva's exact law, step
        and window; otherwise a uniform step: step_deg (or step_mm for a
        slide) and duration_deg. Forward only: b driven alone leaves a
        where it is and the relation reports it — a lock holding
        (docs/assemblies/relations.md#intermittent)."""
        self.activate()
        if slots is not None:
            kw.update(motion="geneva", slots=slots)
        return self.app.add_relation(type="intermittent",
                                     **_mate_ref("a", mate_a),
                                     **_mate_ref("b", mate_b),
                                     start_deg=start_deg, **kw)

    def freewheel(self, mate_a, mate_b, drives, ratio=1.0, **kw):
        """A one-way clutch, rotation to rotation (v104): b follows a while
        a drives it in `drives` ("positive" or "negative") and slips when a
        goes back. Its history is the relation's `lead`."""
        self.activate()
        return self.app.add_relation(type="freewheel", **_mate_ref("a", mate_a),
                                     **_mate_ref("b", mate_b), drives=drives,
                                     ratio=ratio, **kw)
        # teeth=N and ride_mate/ride_dof/ride_mm|ride_deg (v105) make it a
        # toothed clutch that LIFTS what it names as it rides over them.

    def click(self, mate, drives, **kw):
        """A pawl (v105): one mate's rotation, free the way it `drives`
        ("positive" or "negative") and held at its mark the other way."""
        self.activate()
        return self.app.add_relation(type="click", a=ref_of(mate), b=ref_of(mate),
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
        ref["occurrence"] = ref_of(occurrence)
        if isinstance(occurrence, InnerOccurrence):
            ref["inner_path"] = occurrence.inner_path
        return self.app.fit_fastener(rim=ref, standard=standard, **kw)

    def motion(self, driver, **kw):
        """A motion study on this assembly (v87). `configuration=` names the
        assembly configuration it runs under (v128): an id, 0 for the base,
        omitted for whichever is active when it runs."""
        self.activate()
        ref = _mate_ref("driver", driver)
        return self.app.add_motion_study(**ref, **kw)

    def solve(self):
        """Run the mate solver. Always answers errored_mates, placed,
        occurrences, placed_by_mates, placed_by_transform and mates[]; the
        loop keys — dof, loop_mates, loops_closed, residual_mm,
        redundant_mates — only when the assembly HAS mate loops, so read
        those with .get()."""
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
            if c["volume_mm3"] > cap:
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
            "press_fits": "overlaps the size bands allow, relieved and so "
                          "not clashes (docs/tolerances/README.md"
                          "#press-fits); a fit past its band is in `pairs`",
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
            "press_fits": "overlaps the size bands allow, relieved and so "
                          "touching rather than clashing "
                          "(docs/tolerances/README.md#press-fits)",
            "clash": "the boolean behind `clashes`",
            "mode": "exact (the default) or mesh, which answer alike",
            "mesh_cleared": "mesh mode: pairs settled by distance alone",
            "deflection_mm": "mesh mode: the screen's coarsest mesh",
        },
        # Present only when they APPLY: the sweep says how far it got only
        # when it stopped early. They are still `consumed` — the whole point
        # of the gate — but their absence is not a rename.
        "optional": {"truncated", "truncation", "pairs_unchecked"},
    },
    # App.verify_requirements raises on every row whose state is not `pass`
    # (docs/requirements/README.md#states): stale, unresolved and unchecked
    # included, none of which is a pass.
    "App.verify_requirements": {
        "rpc": "list_requirements",
        "consumed": {"requirements"},
        "ignored": {
            "summary": "the count of each state in `requirements`",
            "all_pass": "the same as every row in `requirements` passing",
        },
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
        if isinstance(occ, InnerOccurrence):
            out = _conn((occ.root, what))
            out["inner_path"] = occ.inner_path
            return out
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
    if isinstance(spec, InnerOccurrence):
        return {"occurrence": spec.id, "origin": True,
                "inner_path": spec.inner_path}
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

def _validate_feature(spec, method):
    """Hold a feature spec to its type's contract before it is sent, as the
    server does (RemoteDispatch.cpp, ValidateRequestContract): the
    get_state envelope dropped, an old primitive key left to the parser's
    own refusal by name (docs/scripting/rpc.md#typed-feature-specs)."""
    if not isinstance(spec, dict) or not isinstance(spec.get("type"), str):
        return
    schema = FEATURE_SCHEMAS.get(spec["type"])
    if schema is None:
        return
    spec = {k: v for k, v in spec.items() if k not in FEATURE_ENVELOPE_KEYS}
    if spec["type"] in ("Box", "Sphere", "Cylinder") and any(
            k in spec for k in PRIMITIVE_RETIRED_KEYS):
        return
    _validate_contract(schema, spec, method, "/params/feature")


def ref_of(x):
    """The id an RPC takes for `x`: a handle (`Sketch`, `Frame`, `Feature`,
    `Occurrence`; a `Body` gives the feature that created it), a dict
    response (its `id`), a `$ref` inside a batch, or an id as it is. For
    building a raw feature dict around a handle:

        part.add_feature(feature={"type": "Revolve", "sketch_id": ref_of(sk),
                                  ...})
    """
    if x is None:
        return 0
    if isinstance(x, Ref):
        return x
    if isinstance(x, (Sketch, Frame, Occurrence, InnerOccurrence, Feature)):
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


def _on(on):
    """What a primitive stands on, as the app's `on`."""
    if isinstance(on, str):
        return on  # "Top" / "Front" / "Right"; the app refuses anything else
    if isinstance(on, Face):
        if on.kind != "plane":
            raise SdkError(
                f"face {on.name!r} is a {on.kind}, not planar: put the "
                f"primitive on part.frame(name, of=face) instead, whose Z is "
                f"the axis")
        return dict(on.ref)
    if isinstance(on, DatumPlane):
        return {"feature_id": on.id}
    if isinstance(on, (Frame, Sketch)):
        if on.id is None:
            raise SdkError(
                "that frame is a query result, not a Frame feature: create "
                "one with part.frame(name, ...) to stand a primitive on")
        return {"feature_id": on.id}
    if isinstance(on, Plane):
        raise SdkError(
            "a Plane is numbers, not a reference: a primitive stands on "
            "something it follows -- a Face, part.datum_plane(...), "
            "part.frame(...), a Sketch, or \"Top\"/\"Front\"/\"Right\"")
    raise SdkError(
        f"`on` must be a Face, a DatumPlane, a Frame, a Sketch or "
        f"\"Top\"/\"Front\"/\"Right\", not {on!r} (a primitive has no bare "
        f"world position: put it on a datum_plane or a frame placed there)")


def _at(feature, at):
    """`at=(u, v)`: where on its frame a primitive sits."""
    if at is None:
        return
    if len(at) != 2:
        raise SdkError("at is (u, v) in the frame of what it is on -- two "
                       "numbers, not a world point")
    nums, exprs = [], []
    for c in at:
        if isinstance(c, Param):
            nums.append(float(c))
            exprs.append(c.expr)
        elif isinstance(c, str):
            nums.append(0.0)
            exprs.append(c)
        else:
            nums.append(float(c))
            exprs.append("")
    feature["offset"] = nums
    if any(exprs):
        feature["offset_expr"] = exprs


def _diameter(feature, d, r, what):
    if d is not None and r is not None:
        raise SdkError(f"{what} takes d= or r=, not both")
    if d is not None:
        _dim(feature, "diameter", d)
    elif r is not None:
        _dim(feature, "radius", r)


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

    def check_seat(self, occurrence=None, **measurements_mm):
        """Explicit observations, never an automatic fit choice or geometry edit.

        Returns pass/fail/unverified with individual requirements. Missing
        limits stay unverified; reading a nominal diameter cannot supply them.
        With `occurrence` (a placed bearing) the observations are measured off
        the seats it is mated to instead (docs/parts/catalogs.md).
        """
        if occurrence is not None:
            if measurements_mm:
                raise SdkError("pass an occurrence OR typed measurements, "
                               "not both")
            return self.catalog.app.check_seat(
                occurrence=occurrence, **self.catalog._ref,
                designation=self.designation)
        return self.catalog.app.catalog_requirements(
            **self.catalog._ref, designation=self.designation,
            measurements=measurements_mm)
