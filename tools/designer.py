#!/usr/bin/env python3
"""A pythonic client for Designer — the same interface the LLM drives, for
humans in a REPL or notebook (scripting S3).

Start the app with `lobole --remote 8437` (or `--headless --remote 8437`),
then:

    from designer import Designer
    d = Designer(8437)
    d.new()
    d.var("w", 40).var("h", "w / 2")
    sk = (d.sketch(plane="xy")
            .line((0, 0), (40, 0)).line((40, 0), (40, 20))
            .line((40, 20), (0, 20)).line((0, 20), (0, 0))
            .add())
    d.extrude(sk, depth="w / 8", name="Pad")
    print(d.volume())                # 40*20*5
    d.screenshot("bracket.png", view="iso")

Thin fluent sugar over the JSON-RPC remote (tools/designer_remote.py); every
call maps to a documented RPC, so the semantics match the MCP tools exactly.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from designer_remote import Designer as _Remote

# Datum references usable in constraints (mirror designer_mcp).
ORIGIN = 0xFFFFFF00
AXIS_X = 0xFFFFFF01
AXIS_Y = 0xFFFFFF02

_PLANES = {
    "xy": {"origin": [0, 0, 0], "normal": [0, 0, 1], "uAxis": [1, 0, 0]},
    "xz": {"origin": [0, 0, 0], "normal": [0, 1, 0], "uAxis": [1, 0, 0]},
    "yz": {"origin": [0, 0, 0], "normal": [1, 0, 0], "uAxis": [0, 1, 0]},
}


class Sketch:
    """A sketch being composed. Collect entities fluently, then .add() to
    commit it and get back a handle (its feature id + assigned entity ids)."""

    def __init__(self, doc, plane, name):
        self._doc = doc
        self._plane = _PLANES[plane] if isinstance(plane, str) else plane
        self._name = name
        self._entities = []
        self.id = None
        self.entity_ids = []

    def line(self, a, b, construction=False):
        e = {"type": "line", "a": list(a), "b": list(b)}
        if construction:
            e["construction"] = True
        self._entities.append(e)
        return self

    def circle(self, center, radius, construction=False):
        e = {"type": "circle", "center": list(center), "radius": radius}
        if construction:
            e["construction"] = True
        self._entities.append(e)
        return self

    def rect(self, c1, c2):
        (x0, y0), (x1, y1) = c1, c2
        return (self.line((x0, y0), (x1, y0)).line((x1, y0), (x1, y1))
                .line((x1, y1), (x0, y1)).line((x0, y1), (x0, y0)))

    def arc(self, center, radius, start, end, construction=False):
        e = {"type": "arc", "center": list(center), "radius": radius,
             "start_angle": start, "end_angle": end}
        if construction:
            e["construction"] = True
        self._entities.append(e)
        return self

    def add(self):
        """Commit the sketch; returns self with .id and .entity_ids filled."""
        r = self._doc._call("add_feature", feature={
            "type": "Sketch", "name": self._name, "plane": self._plane,
            "entities": self._entities})
        self.id = r["id"]
        self.entity_ids = r.get("entity_ids", [])
        return self

    def constrain(self, kind, refs, value=None, expr=None, value_deg=None):
        """Add a constraint. refs is [[entity_id, which], ...].

        `value` is MILLIMETRES for the length kinds and RADIANS for `angle` —
        pass `value_deg` for degrees, never both, and a bare `value` over a
        full turn is refused as a degrees number in the radians field.
        `expr` is different: an expression in the document's base units,
        where angles are DEGREES ("45", "45deg" and "0.785rad" all agree).
        """
        assert self.id is not None, "call .add() before constraining"
        self._doc._call("add_constraint", sketch_id=self.id, kind=kind,
                        refs=refs, **({"value": value} if value is not None
                                     else {}),
                        **({"value_deg": value_deg} if value_deg is not None
                           else {}),
                        **({"expr": expr} if expr is not None else {}))
        return self


class Designer:
    """Fluent document handle. Every method returns something useful for
    chaining or inspection; raises RuntimeError on a remote error."""

    def __init__(self, port=8437, host="127.0.0.1", wait=True):
        # wait=True retries while the app starts up (the common case when a
        # script launches it); wait=False connects once, failing fast.
        self._remote = _Remote.wait_for(port) if wait else _Remote(port, host)

    def _call(self, method, **params):
        return self._remote.call(method, **params)

    def close(self):
        self._remote.close()

    # --- document lifecycle -------------------------------------------------
    def new(self):
        self._call("new_project")
        return self

    def open(self, path):
        return self._call("open_project", path=path)

    def save(self, path, message="save"):
        self._call("save_project", path=path, message=message)
        return self

    def undo(self):
        self._call("undo")
        return self

    def redo(self):
        self._call("redo")
        return self

    # --- parameters ---------------------------------------------------------
    def var(self, name, expression, comment=""):
        self._call("set_variable", name=name, expression=str(expression),
                   comment=comment)
        return self

    def vars(self):
        return {v["name"]: v for v in self._call("list_variables")["variables"]}

    # --- modeling -----------------------------------------------------------
    def sketch(self, plane="xy", name="Sketch"):
        return Sketch(self, plane, name)

    def feature(self, **feature):
        """Add any feature by its raw JSON (see the MCP FORMATS); returns the
        assigned id."""
        return self._call("add_feature", feature=feature)["id"]

    def extrude(self, sketch, depth=1.0, name="Extrude", mode="new_body",
                **kw):
        sid = sketch.id if isinstance(sketch, Sketch) else sketch
        feature = {"type": "Extrude", "name": name, "sketch_id": sid,
                   "mode": mode}
        if isinstance(depth, str):
            feature["distance"], feature["distance_expr"] = 1.0, depth
        else:
            feature["distance"] = depth
        feature.update(kw)
        return self._call("add_feature", feature=feature)["id"]

    def box(self, on, width, depth, height, at=(0, 0), centered=True,
            name="Box"):
        """A box standing on `on` ("Top"/"Front"/"Right", a face
        {feature_id, element}, or {feature_id} of a datum plane, sketch or
        frame): width along u, depth along v, height along the normal, `at`
        its centre (or low corner, centered=False) in that frame. Stored as
        a sketch and an extrude (docs/modeling/README.md#primitives)."""
        return self.feature(type="Box", name=name, on=on, width=width,
                            depth=depth, height=height, offset=list(at),
                            centered=centered)

    # --- inspection ---------------------------------------------------------
    def state(self):
        return self._call("get_state")

    def bodies(self):
        return self._call("get_state")["bodies"]

    def volume(self, index=0):
        return self._call("get_state")["bodies"][index]["volume"]

    def mass_properties(self, body=None):
        return self._call("mass_properties",
                          **({"body": body} if body is not None else {}))

    def measure(self, a, b=None):
        return self._call("measure", a=a, **({"b": b} if b else {}))

    def elements(self, body=None):
        return self._call("list_elements",
                         **({"body": body} if body is not None else {}))

    # --- verification -------------------------------------------------------
    def screenshot(self, path, view=None):
        if view:
            self._call("view", name=view)
        self._call("screenshot", path=path)
        return path

    def view(self, name, fit=True):
        return self._call("view", name=name, fit=fit)
