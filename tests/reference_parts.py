#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Reference parts: whole parts, built over the RPC, against closed forms.

Each case in tests/reference_parts/NN-name/ is a recipe (`case.json`: the
RPC steps an agent would send, edges and faces picked by a point on them)
and a derivation (`expected.py`: the part's volume, area, centre of mass,
bounding box and body count from formulas, never from the app). The runner
builds each part in a fresh project, compares it, exports it to STEP, reads
it back with its own coordinates and compares again
(docs/modeling/reference-parts.md says what a case is and how tightly it is
held).

A case that is wrong today carries `known_failure`: a link to the entry that
describes the defect. It is reported and passes the run; the day it starts
passing it FAILS the run, so a fix cannot land without the mark coming off
and the entry going with it.

Usage:
  reference_parts.py <designer-binary> [-k NN ...] [--write-page]
  reference_parts.py --port PORT [-k NN ...]     # a running instance

Every case run renders the status page and compares it with the committed
docs/modeling/reference-parts.md; --write-page writes it instead. `-k`
runs the named cases only (by number or name) and prints expected against
measured, with the relative error.
"""
import argparse
import ast
import importlib.util
import json
import math
import os
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CASES = os.path.join(HERE, "reference_parts")
PAGE = os.path.join(ROOT, "docs", "modeling", "reference-parts.md")
sys.path.insert(0, HERE)
sys.path.insert(0, CASES)

import geom  # noqa: E402

QUANTITIES = ("bodies", "volume", "area", "center_of_mass", "bbox", "faces")
# Relative for volume and area; centre of mass relative to the bbox
# diagonal; bbox in mm per coordinate (OCC widens a box by edge tolerances);
# bodies and faces exact.
DEFAULT_TOL = {"volume": 1e-6, "area": 1e-6, "center_of_mass": 1e-6,
               "bbox": 1e-4}
CASE_KEYS = {"title", "exercises", "steps", "compare", "tolerance", "note",
             "known_failure"}
# The runner owns the project and the files: a recipe builds a part.
REFUSED_METHODS = ("new_project", "open_project", "save_project",
                   "close_project", "batch", "undo", "redo")
REFUSED_PREFIXES = ("export_", "import_", "git_", "checkpoint")
ALLOWED_IMPORTS = {"math", "geom"}


class CaseError(Exception):
    """The case is broken as written (a bad key, an ambiguous pick, an
    expected.py that imports what it may not): fails the run whatever the
    known_failure mark says."""


# ------------------------------------------------------------ loading cases

def check_imports(path):
    """expected.py may import only math and geom, so an expected value can
    never be measured off the app. Parsed, not run, before it is loaded."""
    tree = ast.parse(open(path).read(), path)
    for node in ast.walk(tree):
        names = []
        if isinstance(node, ast.Import):
            names = [a.name.split(".")[0] for a in node.names]
        elif isinstance(node, ast.ImportFrom):
            names = [(node.module or "").split(".")[0] if node.level == 0
                     else "." * node.level + (node.module or "")]
        elif isinstance(node, ast.Name) and node.id in (
                "__import__", "exec", "eval", "open", "compile"):
            raise CaseError(f"{path}: uses {node.id}(); an expected value "
                            "is computed from formulas only")
        for n in names:
            if n not in ALLOWED_IMPORTS:
                raise CaseError(
                    f"{path}: imports {n!r}; an expected.py may import only "
                    f"{sorted(ALLOWED_IMPORTS)}, so that its numbers cannot "
                    "come from the app")


def load_case(dirname):
    path = os.path.join(CASES, dirname)
    with open(os.path.join(path, "case.json")) as f:
        case = json.load(f)
    unknown = set(case) - CASE_KEYS
    if unknown:
        raise CaseError(f"{dirname}/case.json: unknown key(s) "
                        f"{sorted(unknown)}; accepted: {sorted(CASE_KEYS)}")
    for key in ("title", "exercises", "steps"):
        if key not in case:
            raise CaseError(f"{dirname}/case.json has no {key!r}")
    for q in case.get("compare", []):
        if q not in QUANTITIES:
            raise CaseError(f"{dirname}: compare names {q!r}; accepted: "
                            f"{list(QUANTITIES)}")
    for q in case.get("tolerance", {}):
        if q not in DEFAULT_TOL:
            raise CaseError(f"{dirname}: tolerance names {q!r}; accepted: "
                            f"{sorted(DEFAULT_TOL)}")
    if case.get("tolerance") and not case.get("note"):
        raise CaseError(f"{dirname}: a looser tolerance needs a `note` "
                        "saying why")
    for i, step in enumerate(case["steps"], 1):
        if set(step) - {"method", "params"} or "method" not in step:
            raise CaseError(f"{dirname}: step {i} must be "
                            "{method, params}")
        m = step["method"]
        if m in REFUSED_METHODS or m.startswith(REFUSED_PREFIXES):
            raise CaseError(f"{dirname}: step {i} calls {m}; project verbs, "
                            "exports and imports are the runner's")
    exp_path = os.path.join(path, "expected.py")
    check_imports(exp_path)
    spec = importlib.util.spec_from_file_location(
        f"reference_case_{dirname.replace('-', '_')}", exp_path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    method = (module.__doc__ or "").strip()
    if not method:
        raise CaseError(f"{dirname}/expected.py has no docstring; it is "
                        "the case's `method`")
    expected = module.expected()
    for key in ("bodies", "volume_mm3", "area_mm2", "center_of_mass",
                "bbox"):
        if key not in expected:
            raise CaseError(f"{dirname}: expected() returned no {key!r}")
    case["dir"] = dirname
    case["method"] = method
    case["expected"] = expected
    return case


def all_cases():
    return sorted(d for d in os.listdir(CASES)
                  if os.path.isfile(os.path.join(CASES, d, "case.json")))


# -------------------------------------------------------------- the recipe

def resolve(d, value, results):
    """Replace {"$ref": n[, "key"]} and {"$pick": kind, "at": p}."""
    if isinstance(value, list):
        return [resolve(d, v, results) for v in value]
    if not isinstance(value, dict):
        return value
    if "$ref" in value:
        n = value["$ref"]
        if not 1 <= n <= len(results):
            raise CaseError(f"$ref {n}: only {len(results)} step(s) ran "
                            "before this one")
        key = value.get("key", "id")
        if key not in results[n - 1]:
            raise CaseError(f"$ref {n}: step {n} returned no {key!r} "
                            f"(it returned {sorted(results[n - 1])})")
        return results[n - 1][key]
    if "$pick" in value:
        kind = value["$pick"]
        if kind not in ("edge", "face") or set(value) != {"$pick", "at"}:
            raise CaseError(f"bad pick {value}: {{\"$pick\": edge|face, "
                            "\"at\": [x, y, z]}")
        r = d.call("query_geometry", element_type=kind, on_point=value["at"],
                   expected_count=1)
        if not r["ok"]:
            raise CaseError(f"pick {kind} at {value['at']}: {r['count']} "
                            "elements pass through it, want exactly 1")
        m = r["matches"][0]
        if "element" not in m:
            raise CaseError(f"pick {kind} at {value['at']}: the {m['kind']} "
                            "found there has no stable name, so it cannot "
                            "be referenced")
        return {"feature_id": m["created_by"], "element": m["element"]}
    return {k: resolve(d, v, results) for k, v in value.items()}


class BuildFailure(Exception):
    """A step was refused or a feature did not build: the part is wrong."""


def build(d, case):
    results = []
    for i, step in enumerate(case["steps"], 1):
        params = resolve(d, step.get("params", {}), results)
        try:
            r = d.request(step["method"], params)
        except Exception as e:  # a refusal: the app said no
            raise BuildFailure(f"step {i} ({step['method']}) refused: {e}")
        r = r if isinstance(r, dict) else {"result": r}
        regen = r.get("regen")
        if isinstance(regen, dict) and not regen.get("ok", True):
            raise BuildFailure(f"step {i} ({step['method']}) left the part "
                               f"broken: {json.dumps(regen)}")
        # A part with a known answer builds without the app doubting it: a
        # warning (a cut that removed nothing) is a recipe or an app wrong.
        if isinstance(regen, dict) and regen.get("warnings"):
            raise BuildFailure(f"step {i} ({step['method']}) warned: "
                               f"{json.dumps(regen['warnings'])}")
        results.append(r)


# ------------------------------------------------------------- measurement

def measure(d):
    bodies = d.call("get_state")["bodies"]
    mp = d.call("mass_properties")
    out = {"bodies": len(bodies)}
    if not bodies:
        return out
    out["volume"] = mp["total"]["volume"]
    out["center_of_mass"] = mp["total"]["center_of_mass"]
    out["area"] = sum(b["surface_area"] for b in mp["bodies"])
    lo = [min(b["bbox_min"][i] for b in mp["bodies"]) for i in range(3)]
    hi = [max(b["bbox_max"][i] for b in mp["bodies"]) for i in range(3)]
    out["bbox"] = [lo, hi]
    out["faces"] = d.call("query_geometry", element_type="face",
                          max_results=0)["count"]
    return out


def compare(case, measured):
    """[(quantity, expected, measured, error, limit, ok)]"""
    exp = case["expected"]
    tol = dict(DEFAULT_TOL, **case.get("tolerance", {}))
    want = case.get("compare") or [q for q in QUANTITIES
                                   if q != "faces" or "faces" in exp]
    rows = []
    lo, hi = exp["bbox"]
    diag = math.dist(lo, hi)
    for q in want:
        if q == "bodies":
            e, m = exp["bodies"], measured.get("bodies")
            rows.append((q, e, m, None, 0, e == m))
            continue
        if q not in measured:
            rows.append((q, "", "(no body)", None, None, False))
            continue
        m = measured[q]
        if q == "faces":
            if "faces" not in exp:
                raise CaseError(f"{case['dir']}: compares faces but "
                                "expected() gives none")
            rows.append((q, exp["faces"], m, None, 0, exp["faces"] == m))
        elif q in ("volume", "area"):
            e = exp[q + ("_mm3" if q == "volume" else "_mm2")]
            err = abs(m - e) / abs(e)
            rows.append((q, e, m, err, tol[q], err <= tol[q]))
        elif q == "center_of_mass":
            e = exp["center_of_mass"]
            err = math.dist(e, m) / diag
            rows.append((q, e, m, err, tol[q], err <= tol[q]))
        elif q == "bbox":
            e = exp["bbox"]
            err = max(abs(e[k][i] - m[k][i]) for k in (0, 1)
                      for i in range(3))
            rows.append((q, e, m, err, tol[q], err <= tol[q]))
    return rows


def fmt(v):
    if isinstance(v, float):
        return f"{v:.10g}"
    if isinstance(v, list):
        return "[" + ", ".join(fmt(x) for x in v) + "]"
    return str(v)


def print_rows(label, rows):
    print(f"  {label}:")
    for q, e, m, err, limit, ok in rows:
        e_s = "" if err is None else f"  err {err:.3g} (limit {limit:g})"
        print(f"    {'ok  ' if ok else 'FAIL'} {q:15s} expected {fmt(e)}\n"
              f"         {'':15s} measured {fmt(m)}{e_s}")


# ---------------------------------------------------------------- one case

def run_case(d, case, scratch, verbose):
    """'pass' or a failure description (the part is wrong). A CaseError
    escapes: the case itself is broken."""
    d.call("new_project")
    try:
        build(d, case)
    except BuildFailure as e:
        return str(e)
    rows = compare(case, measure(d))
    failures = [r for r in rows if not r[5]]
    if verbose or failures:
        print_rows("as built", rows)
    if failures:
        return "as built: " + ", ".join(r[0] for r in failures) + " off"
    # The round trip a user's part takes: out to STEP and back, its own
    # coordinates kept (a recentred import would move every centroid).
    step = os.path.join(scratch, case["dir"] + ".step")
    d.call("export_step", path=step)
    d.call("new_project")
    d.call("import_step", path=step, keep_origin=True)
    rows = compare(case, measure(d))
    failures = [r for r in rows if not r[5]]
    if verbose or failures:
        print_rows("STEP round trip", rows)
    if failures:
        return ("STEP round trip: " + ", ".join(r[0] for r in failures)
                + " off")
    return "pass"


# ----------------------------------------------------------- status page

PAGE_INTRO = """\
# Reference parts

<!-- Generated by tests/reference_parts.py --write-page. Do not edit: the
     `reference_parts` test fails when this page differs from what the
     cases render. -->

Whole parts built the way an agent builds them, over the RPC, and checked
against values worked out by hand. A feature's own unit tests build one
feature on a body made in C++; these build a part sketch first, pick edges
off the geometry the previous feature made, regenerate it, export it to STEP
and read it back, which is where defects hide.

## What a case is

A directory under `tests/reference_parts/`:

- `case.json` holds the recipe: `steps`, a list of `{method, params}` sent
  in order to a fresh project, as `batch` takes them. `{"$ref": n}` is step
  *n*'s `id` (1-based; `{"$ref": n, "key": k}` another field), and
  `{"$pick": "edge"|"face", "at": [x, y, z]}` is the one element whose
  geometry passes through that world point (`query_geometry` `on_point`,
  `expected_count: 1`), sent as `{feature_id, element}`. Picks are by point
  because names and indices are what is under test. Project verbs, exports
  and imports are refused in a recipe; every step's `regen` must be ok.
- `expected.py` holds the derivation. `expected()` returns the body count,
  volume, surface area, centre of mass, bounding box and, where the
  geometry fixes it, the face count; the module docstring is the method.
  It may import only `math` and `tests/reference_parts/geom.py` (the closed
  forms, which check themselves before every run), and the runner refuses
  any other import before loading it, so no expected value can be measured
  off the app.

Every value is analytic: prisms, cylinders, cones, spheres, tori,
Pappus-Guldin, Green's theorem over lines and arcs, Cavalieri, Steiner's
formula, sheet-metal bends as annular sectors with BA = θ(R + K·t). FreeCAD
runs the same OpenCASCADE, so it would reproduce a kernel defect as the
expected value; a part with no closed form is not in the corpus. Inputs the
app takes from a table (a bend radius, a K-factor) are pinned in the recipe;
a published standard's table (ISO 273 clearance holes) is the reference.

## How tightly

| quantity | default |
|---|---|
| volume, area | relative 1e-6 |
| centre of mass | 1e-6 × the bbox diagonal |
| bbox (`mass_properties`, measured off the surfaces) | 1e-4 mm per coordinate |
| bodies, faces | exact |

A case built from approximated surfaces states a looser `tolerance` and a
`note` saying why; one whose geometry fixes only some quantities lists them
in `compare`. Each part is compared as built, then again after
`export_step` and `import_step` with `keep_origin`.

## Known failures are a ratchet

A case may carry `known_failure`, a link to the entry describing the
defect. A failing case without it fails the run; a marked case that fails is
reported; a marked case that PASSES fails the run, saying to remove the mark
and the entry it points at.

## Running it

`tools/ctest -L reference`, or `tests/reference_parts.py build/lobole -k 14`
for one case with expected and measured side by side. `--write-page`
rewrites this page.

## The cases

| case | part | exercises | method | result |
|---|---|---|---|---|
"""


def first_sentence(text):
    text = " ".join(text.split())
    for i, ch in enumerate(text):
        if ch == "." and (i + 1 == len(text) or text[i + 1] == " "):
            return text[:i + 1]
    return text


def cell(text):
    return text.replace("|", "\\|")


def render_page(cases):
    lines = [PAGE_INTRO]
    for c in cases:
        if c.get("known_failure"):
            link = os.path.relpath(os.path.join(ROOT, c["known_failure"]),
                                   os.path.dirname(PAGE))
            result = f"known failure ([entry]({link}))"
        else:
            result = "pass"
        lines.append(f"| {c['dir']} | {cell(c['title'])} | "
                     f"{cell(c['exercises'])} | "
                     f"{cell(first_sentence(c['method']))} | {result} |\n")
    return "".join(lines)


# ------------------------------------------------------------------- main

def run(d, cases, verbose):
    scratch = tempfile.mkdtemp(prefix="reference_parts_")
    bad = []
    try:
        for c in cases:
            t0 = time.time()
            try:
                outcome = run_case(d, c, scratch, verbose)
            except CaseError as e:
                print(f"{c['dir']}: BROKEN CASE: {e}")
                bad.append(c["dir"])
                continue
            marked = c.get("known_failure")
            dt = f"{time.time() - t0:.1f} s"
            if outcome == "pass" and not marked:
                print(f"{c['dir']}: pass ({dt})")
            elif outcome == "pass":
                print(f"{c['dir']}: PASSES but is marked a known failure: "
                      f"remove `known_failure` from its case.json and delete "
                      f"the entry it points at ({marked})")
                bad.append(c["dir"])
            elif marked:
                print(f"{c['dir']}: known failure ({marked}): {outcome}")
            else:
                print(f"{c['dir']}: FAIL: {outcome}")
                bad.append(c["dir"])
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("binary", nargs="?")
    ap.add_argument("--port", type=int,
                    help="drive a running instance instead of launching one")
    ap.add_argument("-k", action="append", default=[],
                    help="run only this case (number or name); repeatable")
    ap.add_argument("--write-page", action="store_true")
    args = ap.parse_args()
    if not args.binary and not args.port:
        ap.error("give the designer binary, or --port")

    geom.self_check()
    print("geom self-check: ok")

    names = all_cases()
    if args.k:
        want = [k.zfill(2) if k.isdigit() else k for k in args.k]
        names = [n for n in names
                 if any(n == k or n.startswith(k + "-") for k in want)]
        if not names:
            print(f"no case matches {args.k}")
            return 2
    try:
        cases = [load_case(n) for n in names]
    except CaseError as e:
        print(f"BROKEN CASE: {e}")
        return 1

    from lobole.client import Client
    if args.port:
        bad = run(Client.wait_for(args.port), cases, bool(args.k))
    else:
        from smoke_app import launch
        with launch(args.binary) as app:
            bad = run(app.designer, cases, bool(args.k))

    if bad:
        print(f"reference_parts: {len(bad)} case(s) failed: {bad}")
        return 1
    known = sum(1 for c in cases if c.get("known_failure"))
    print(f"reference_parts: {len(cases) - known} pass, {known} known "
          "failure(s)")
    if args.k:
        return 0
    page = render_page(cases)
    if args.write_page:
        with open(PAGE, "w") as f:
            f.write(page)
        print(f"wrote {os.path.relpath(PAGE, ROOT)}")
    else:
        try:
            committed = open(PAGE).read()
        except OSError:
            committed = ""
        if committed != page:
            print(f"{os.path.relpath(PAGE, ROOT)} differs from what the cases "
                  "render: run tests/reference_parts.py <binary> --write-page "
                  "and commit it with the case change")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
