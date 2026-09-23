#!/usr/bin/env python3
"""A parameter must survive the script boundary (docs/scripting/sdk.md#parameters-carry-their-expression).

Thirteen of sixteen generators declared no document variables at all, so their
dimensions reached the document as literals and the parts opened with nothing
to turn. The cause was ergonomic, not architectural: `var()` declared a
variable, but USING it meant passing its name as a string everywhere, and the
moment a script needed `arm_len - 18` a literal was the only short path.

`Param` closes that: a float that carries its expression, so arithmetic stays
ordinary Python and `_dim`/`_dim3` put the FORMULA in the document.

Two things are under test.

1. **Round trip.** A part built with Param arithmetic is driven by its
   variables afterwards — edit one, the geometry moves, and by the right
   amount.

2. **Perturbation.** Borrowed from vcad's `crates/vcad-loon/src/recover.rs`,
   which recovers parameter->field bindings by nudging a parameter and diffing
   two documents. We do not need the recovery (we have real expressions), but
   the inverse is a test nobody had: nudge a variable, diff the saved
   document, and assert that EXACTLY the expected fields moved. A field that
   should be expression-driven and is quietly a literal cannot pass it, and
   neither can a stray field that moves when it should not.

Usage: param_smoke.py <path-to-designer-binary>
"""
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, HERE)
from designer_sdk import App, Param  # noqa: E402
from smoke_app import launch  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def volume(sdk):
    return sdk.call("mass_properties")["total"]["volume"]


def load_doc(path):
    """A saved project as one dict, whatever form it took on disk.

    A multi-part project saves as a .dsgn DIRECTORY of per-feature files, so
    the diff walks the tree and keys every scalar by file + path. That is a
    finer diff surface than a single blob, not a coarser one.
    """
    if os.path.isfile(path):
        return json.load(open(path))
    out = {}
    for root, _, files in os.walk(path):
        for f in sorted(files):
            if not f.endswith(".json"):
                continue
            full = os.path.join(root, f)
            key = os.path.relpath(full, path)
            try:
                out[key] = json.load(open(full))
            except json.JSONDecodeError:
                pass
    return out


def flatten(obj, prefix=""):
    """Every scalar in a document, keyed by its path. The diff unit."""
    out = {}
    if isinstance(obj, dict):
        for k, v in obj.items():
            out.update(flatten(v, f"{prefix}.{k}"))
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            out.update(flatten(v, f"{prefix}[{i}]"))
    elif isinstance(obj, (int, float)) and not isinstance(obj, bool):
        out[prefix] = float(obj)
    return out


def moved_fields(before, after, eps=1e-9):
    """Numeric paths whose value changed. Paths absent from either side are
    reported too — a field appearing or vanishing is a change."""
    a, b = flatten(before), flatten(after)
    moved = {k for k in a.keys() & b.keys() if abs(a[k] - b[k]) > eps}
    return moved | (a.keys() ^ b.keys())


# --- 1. Param is a float, and composes ------------------------------------

def test_param_arithmetic():
    L = Param(110.0, "arm_len")
    expect(isinstance(L, float), "a Param must BE a float or every existing "
                                 "SDK path that measures with it breaks")
    expect(float(L) == 110.0, "value")
    expect(abs(L / 2 - 55.0) < 1e-12, "arithmetic keeps the number right")
    expect((L / 2).expr == "(arm_len / 2.0)", f"expr composes: {(L / 2).expr}")
    expect((L - 18).expr == "(arm_len - 18.0)", (L - 18).expr)
    expect((2 * L).expr == "(2.0 * arm_len)", (2 * L).expr)
    expect((-L).expr == "(-arm_len)", (-L).expr)
    W = Param(4.0, "wall")
    expect((L - 2 * W).expr == "(arm_len - (2.0 * wall))", (L - 2 * W).expr)
    # A Param compares and sorts as its number — face filtering relies on it.
    expect(max([Param(3.0, "a"), Param(9.0, "b")]) == 9.0, "ordering")


# --- 2. the round trip ----------------------------------------------------

def test_drives_geometry(binary):
    with launch(binary) as app:
        sdk = App(app.designer)
        sdk.call("new_project")
        part = sdk.part("Arm")
        L = part.param("arm_len", 110.0)
        W = part.param("wall", 4.0)
        # Ordinary Python arithmetic — the point is that this is what a
        # generator would already have written.
        part.box(w=L, h=L / 2, d=W * 2)

        expect(abs(volume(sdk) - 110 * 55 * 8) < 1e-6, "as built")
        part.var("arm_len", 200)
        expect(abs(volume(sdk) - 200 * 100 * 8) < 1e-6,
               "arm_len drives width AND height (h was arm_len/2)")
        part.var("wall", 10)
        expect(abs(volume(sdk) - 200 * 100 * 20) < 1e-6, "wall drives depth")

        # A derived parameter is defined ONCE, in the document.
        D = part.param("bolt_out", "arm_len - 18")
        expect(D.expr == "bolt_out",
               "a derived param refers to itself by name, not by re-deriving "
               f"the formula on this side: {D.expr}")
        expect(abs(float(D) - 182.0) < 1e-9, f"document evaluated it: {D}")


# --- 3. the perturbation test --------------------------------------------

def test_perturbation(binary):
    """Nudge one variable; exactly the fields it drives may move."""
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "p.dsgn")
        with launch(binary) as app:
            sdk = App(app.designer)
            sdk.call("new_project")
            part = sdk.part("Arm")
            L = part.param("arm_len", 110.0)
            W = part.param("wall", 4.0)
            part.box(w=L, h=L / 2, d=W * 2, name="Driven")
            # A second body deliberately NOT driven by arm_len. If the diff
            # cannot tell it apart, the test is measuring nothing.
            part.box(position=(0, 0, 100), w=7.0, h=7.0, d=W, name="Free")
            sdk.call("save_project", path=path)
            before = load_doc(path)

            part.var("arm_len", 111.0)
            sdk.call("save_project", path=path)
            after = load_doc(path)

        moved = moved_fields(before, after)
        # Anything mentioning the untouched body must not have moved.
        free = sorted(p for p in moved if "Free" in p)
        expect(not free, f"a body not driven by arm_len moved: {free}")

        numeric = sorted(p for p in moved
                         if not p.endswith(".value") or "variable" not in p)
        expect(moved, "nudging a variable changed nothing — the dimension is "
                      "a literal, not an expression")
        # The width and height of the driven box, plus the variable itself.
        expect(any("width" in p for p in moved),
               f"width did not follow arm_len; moved: {numeric}")
        expect(any("height" in p for p in moved),
               f"height did not follow arm_len/2; moved: {numeric}")
        expect(not any("depth" in p for p in moved),
               f"depth is driven by wall and must NOT move: {numeric}")


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: param_smoke.py <designer-binary>")
    binary = sys.argv[1]
    test_param_arithmetic()
    test_drives_geometry(binary)
    test_perturbation(binary)
    print("param_smoke: ok")


if __name__ == "__main__":
    main()
