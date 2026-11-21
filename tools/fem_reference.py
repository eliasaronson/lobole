#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The FEM reference cases (docs/simulation/reference-cases.md).

  fem_reference.py check
      no app: every tests/fem_reference/<case>/expected.json is well
      formed (a source, a value, a unit, a tolerance or a minimum and a
      written reason for each quantity), the procedure table in
      src/core/fem/FemReferenceCases.h names exactly the cases that say
      they verify it, and the committed page names every case. Seconds;
      the headless suite runs it (fem_reference_table).

  fem_reference.py run [--binary B] [--case ID ...] [--no-ccx]
                       [--write-page] [--check-page] [--results OUT]
      runs every case (or those named) on one headless instance: in-house,
      and on CalculiX where the case asks for it and the app finds ccx.
      Exits non-zero when a quantity misses its tolerance, when a
      known_failure passes (remove it), or with --check-page when the
      committed page differs from this run's. --write-page writes it.

  fem_reference.py run --planted ...
      the same with LOBOLE_TEST_PLANT_GAUSS_WEIGHT=1 in the app (one of the
      tet10's four Gauss weights wrong): passes only when the cases that
      say `"planted": "fails"` in their expected.json all FAIL -- the
      check that the rate and the near-incompressible case can see an
      element bug a single answer hides.

A reference is a published value or a closed form, never another solver's
number: CalculiX is reported beside the reference and held to the same
tolerance, a cross-check, not the target.
"""

import argparse
import difflib
import importlib.util
import json
import math
import os
import re
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CASES = os.path.join(ROOT, "tests", "fem_reference")
PAGE = os.path.join(ROOT, "docs", "simulation", "reference-cases.md")
TABLE = os.path.join(ROOT, "src", "core", "fem", "FemReferenceCases.h")

KINDS = ("theory", "benchmark")


def case_ids():
    return sorted(d for d in os.listdir(CASES)
                  if os.path.isfile(os.path.join(CASES, d, "expected.json")))


def expected(cid):
    with open(os.path.join(CASES, cid, "expected.json")) as f:
        return json.load(f)


def table():
    """procedure -> [case ids], read from FemReferenceCases.h."""
    src = open(TABLE).read()
    out = {}
    for proc, cases in re.findall(r'\{"([^"]+)",\s*"([^"]*)"\}', src):
        out[proc] = [c.strip() for c in cases.split(",") if c.strip()]
    return out


def check_static():
    errors = []
    tab = table()
    named = {c: p for p, cs in tab.items() for c in cs}
    ids = case_ids()
    for cid in ids:
        e = expected(cid)
        where = "tests/fem_reference/%s/expected.json" % cid
        for key in ("title", "procedure", "model", "quantities"):
            if not e.get(key):
                errors.append("%s: no `%s`" % (where, key))
        if e.get("calculix") not in (True, False, "only"):
            errors.append("%s: `calculix` is true, false or \"only\"" % where)
        if not os.path.isfile(os.path.join(CASES, cid, "case.py")):
            errors.append("%s: no case.py beside it" % where)
        if named.get(cid) != e.get("procedure"):
            errors.append("%s: says it verifies `%s`, and %s lists it under "
                          "`%s`" % (where, e.get("procedure"),
                                    os.path.relpath(TABLE, ROOT),
                                    named.get(cid)))
        for q, spec in e.get("quantities", {}).items():
            w = "%s: %s" % (where, q)
            if spec.get("kind") not in KINDS:
                errors.append("%s: kind is theory or benchmark" % w)
            for key in ("source", "reason", "unit"):
                if key not in spec or (key != "unit" and not spec[key]):
                    errors.append("%s: no `%s`" % (w, key))
            if not isinstance(spec.get("value"), (int, float)):
                errors.append("%s: no numeric `value`" % w)
            if ("tolerance" in spec) == ("minimum" in spec):
                errors.append("%s: exactly one of `tolerance` (relative) "
                              "and `minimum`" % w)
            if "scale" in spec and not spec.get("scale_reason"):
                errors.append("%s: a `scale` needs its `scale_reason`" % w)
            if "known_failure" in spec and not spec["known_failure"]:
                errors.append("%s: a known_failure names its cause" % w)
    for cid, proc in named.items():
        if cid not in ids:
            errors.append("%s lists `%s` under `%s`, and tests/fem_reference "
                          "has no such case" % (os.path.relpath(TABLE, ROOT),
                                                cid, proc))
    page = open(PAGE).read() if os.path.exists(PAGE) else ""
    for cid in ids:
        if "(`%s`)" % cid not in page:
            errors.append("%s does not show case `%s`: run "
                          "tools/fem_reference.py run --write-page" % (
                              os.path.relpath(PAGE, ROOT), cid))
    return errors


# ---- running ---------------------------------------------------------------

def load_case(cid):
    sys.path.insert(0, CASES)
    spec = importlib.util.spec_from_file_location(
        "fem_case_" + cid, os.path.join(CASES, cid, "case.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def judge(spec, got):
    """(passes, error) of one measured value against its spec."""
    if got is None or not isinstance(got, (int, float)) or not math.isfinite(
            got):
        return False, None
    want = spec["value"] * spec.get("scale", 1.0)
    if "minimum" in spec:
        return got >= spec["minimum"], None
    err = (got - want) / abs(want) if want != 0 else got
    return abs(err) <= spec["tolerance"], err


def run_case(d, cid, solver):
    mod = load_case(cid)
    d.call("new_project")
    t0 = time.monotonic()
    try:
        got = mod.run(d, solver)
        why = None
    except Exception as ex:  # a case that cannot run is a failure, named
        got, why = {}, "%s: %s" % (type(ex).__name__, ex)
    return got, why, time.monotonic() - t0


def run(args):
    sys.path.insert(0, os.path.join(ROOT, "tests"))
    sys.path.insert(0, os.path.join(ROOT, "python"))
    from smoke_app import launch
    binary = args.binary or os.path.join(ROOT, "build", "lobole")
    ids = args.case or case_ids()
    env = dict(os.environ)
    if args.planted:
        env["LOBOLE_TEST_PLANT_GAUSS_WEIGHT"] = "1"
        ids = [c for c in ids if expected(c).get("planted") == "fails"]
    results = {"cases": {}, "ccx": None}
    t_all = time.monotonic()
    with launch(binary, env=env, timeout=60.0) as app:
        d = app.client
        ccx = d.call("ping").get("features", {}).get("calculix")
        results["ccx"] = ccx.get("version") if isinstance(ccx, dict) else None
        for cid in ids:
            e = expected(cid)
            entry = {"runs": {}}
            solvers = []
            if e["calculix"] != "only":
                solvers.append("in_house")
            if e["calculix"] and not args.no_ccx and results["ccx"]:
                solvers.append("calculix")
            for solver in solvers:
                got, why, secs = run_case(d, cid, solver)
                entry["runs"][solver] = {"values": got, "error": why,
                                         "seconds": round(secs, 1)}
                print("fem-reference: %-14s %-9s %6.1f s %s" % (
                    cid, solver, secs, why or ""), flush=True)
            results["cases"][cid] = entry
    results["seconds"] = round(time.monotonic() - t_all, 1)
    failures = verdicts(results)
    if args.results:
        with open(args.results, "w") as f:
            json.dump(results, f, indent=2)
    if args.planted:
        caught = [f for f in failures if f.startswith("FAIL")]
        missed = [c for c in ids
                  if not any(f.split()[1] == c for f in caught)]
        for c in missed:
            print("fem-reference: planted bug NOT caught by %s" % c)
        print("fem-reference: planted bug caught by %d of %d case(s)" % (
            len(ids) - len(missed), len(ids)))
        return 1 if missed or not ids else 0
    page = render(results, partial=bool(args.case))
    rc = 0
    for f in failures:
        print("fem-reference: " + f)
        rc = 1
    if args.write_page:
        with open(PAGE, "w") as f:
            f.write(page)
        print("fem-reference: wrote " + os.path.relpath(PAGE, ROOT))
    if args.check_page:
        committed = open(PAGE).read() if os.path.exists(PAGE) else ""
        if strip_ccx(committed, results) != strip_ccx(page, results):
            sys.stdout.writelines(difflib.unified_diff(
                committed.splitlines(True), page.splitlines(True),
                "committed", "this run"))
            print("fem-reference: the committed page is not this run's: "
                  "tools/fem_reference.py run --write-page")
            rc = 1
    print("fem-reference: %d case(s), %.0f s" % (len(ids),
                                                  results["seconds"]))
    return rc


def strip_ccx(page, results):
    """Without ccx here, the page's CalculiX column cannot be compared."""
    if results["ccx"]:
        return page
    return re.sub(r"<!-- ccx -->.*?<!-- /ccx -->", "", page, flags=re.S)


def verdicts(results):
    out = []
    for cid, entry in results["cases"].items():
        e = expected(cid)
        for solver, r in entry["runs"].items():
            if r["error"]:
                out.append("FAIL %s %s: %s" % (cid, solver, r["error"]))
                continue
            for q, spec in e["quantities"].items():
                ok, _ = judge(spec, r["values"].get(q))
                known = spec.get("known_failure")
                if known and ok and solver == "in_house":
                    out.append("PASSING-KNOWN-FAILURE %s %s: %s now passes; "
                               "remove its known_failure" % (cid, solver, q))
                elif not ok and not (known and solver == "in_house"):
                    out.append("FAIL %s %s: %s = %r against %s" % (
                        cid, solver, q, r["values"].get(q),
                        describe_target(spec)))
    return out


def describe_target(spec):
    want = spec["value"] * spec.get("scale", 1.0)
    if "minimum" in spec:
        return "at least %g" % spec["minimum"]
    return "%s %s within %g %%" % (fmt(want), spec["unit"],
                                   100 * spec["tolerance"])


def fmt(v):
    if v is None:
        return "-"
    if isinstance(v, (int, float)):
        if v == 0:
            return "0"
        return "%.5g" % v
    return str(v)


def pct(err):
    return "-" if err is None else "%+.2f %%" % (100 * err)


def render(results, partial=False):
    lines = [
        "# FEM reference cases",
        "",
        "<!-- Generated by tools/fem_reference.py run --write-page; do not "
        "edit by hand. -->",
        "",
        "Published benchmarks and closed forms the solvers are run against, "
        "each through the RPC a user drives (the mesher, the supports and "
        "the solver a study uses), with its source and a tolerance whose "
        "reason is written beside it. A reference is a published value, "
        "never another solver's number: CalculiX "
        "([CalculiX](README.md#calculix)) runs the same study as a "
        "cross-check, held to the same tolerance. A miss with a known cause "
        "stays on this page as a known failure; one that starts passing "
        "fails the run until its entry is removed. How the suite runs and "
        "what it is for: [the reference cases](README.md#reference-cases).",
        "",
        "<!-- ccx -->CalculiX: %s.<!-- /ccx -->" % (
            ("ccx " + results["ccx"]) if results["ccx"] else "not found"),
        "",
        "| case | quantity | source | target | in-house | error | "
        "<!-- ccx -->CalculiX | error<!-- /ccx --> | tolerance |",
        "|---|---|---|---|---|---|<!-- ccx -->---|---<!-- /ccx -->|---|",
    ]
    details = []
    for cid in case_ids():
        if cid not in results["cases"]:
            continue
        e = expected(cid)
        entry = results["cases"][cid]
        for q, spec in e["quantities"].items():
            want = spec["value"] * spec.get("scale", 1.0)
            cells = []
            for solver in ("in_house", "calculix"):
                r = entry["runs"].get(solver)
                if not r:
                    cells += ["n/a", ""]
                    continue
                if r["error"]:
                    cells += ["refused", ""]
                    continue
                got = r["values"].get(q)
                ok, err = judge(spec, got)
                mark = "" if ok else (" (known failure)" if spec.get(
                    "known_failure") and solver == "in_house" else " (FAIL)")
                cells += [fmt(got) + mark, pct(err)]
            tol = ("at least %g" % spec["minimum"] if "minimum" in spec
                   else "%g %%" % (100 * spec["tolerance"]))
            target = ("%s %s" % (fmt(want), spec["unit"])).strip()
            lines.append(
                "| %s (`%s`) | %s | %s | %s | %s | %s | <!-- ccx -->%s | "
                "%s<!-- /ccx --> | %s |" % (
                    e["title"], cid, q, spec["source"], target, cells[0],
                    cells[1], cells[2], cells[3], tol))
        details.append("### %s (`%s`)" % (e["title"], cid))
        details.append("")
        details.append("Verifies %s. %s." % (e["procedure"], e["model"]))
        details.append("")
        for q, spec in e["quantities"].items():
            text = "- **%s**, %s: %s" % (q, spec["kind"], spec["reason"])
            if "scale" in spec:
                text += ". Scaled by %.6g: %s" % (spec["scale"],
                                                  spec["scale_reason"])
            if spec.get("known_failure"):
                text += ". **Known failure:** " + spec["known_failure"]
            details.append(text + ".")
        details.append("")
    lines += ["", "## The cases", ""] + details
    lines += ["## Left out", "",
              "Cases the plan asked for that the suite does not run, and why:",
              ""] + ["- " + s for s in LEFT_OUT] + [""]
    return "\n".join(lines)


# What the suite does not run, and why: printed on the page.
LEFT_OUT = [
    "NAFEMS LE10 (thick plate under pressure, sigma_yy at D, -5.38 MPa): "
    "its out-of-plane support is a LINE, the outer edge's mid-plane, and a "
    "support holds a face; holding the whole outer face, or a band of it, "
    "is another problem",
    "NAFEMS LE11 (solid cylinder, taper and sphere under a temperature "
    "field T = (x^2 + y^2)^(1/2) + z): a thermal load is a thermal study's "
    "result or one uniform temperature, and no study states a field as a "
    "function of position",
    "MacNeal and Harder's curved beam (Finite Elements in Analysis and "
    "Design 1 (1985) 3-20, Table 3): a tet10 mesh converges to 0.08851 "
    "in-plane (1.3 % over the published 0.08734, at 0.05 and 0.025 "
    "elements alike, and on the bending-only closed form pi P R^3 / 4EI) "
    "and towards 0.4978 out-of-plane (0.9 % under 0.5022); the paper does "
    "not say how its targets were derived, so no tolerance with a reason "
    "holds them. Its twisted beam needs a part twisted 90 degrees along "
    "its length, and its plates and shells are one tet10 layer through a "
    "thickness of 1/100 to 1/1000 of the part. Its straight beam and "
    "thick-walled cylinder run (`mh_beam`, `mh_cylinder`)",
]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check")
    r = sub.add_parser("run")
    r.add_argument("--binary")
    r.add_argument("--case", action="append")
    r.add_argument("--no-ccx", action="store_true")
    r.add_argument("--write-page", action="store_true")
    r.add_argument("--check-page", action="store_true")
    r.add_argument("--planted", action="store_true")
    r.add_argument("--results")
    a = ap.parse_args()
    if a.cmd == "check":
        errors = check_static()
        for e in errors:
            print("fem-reference: " + e)
        print("fem-reference: %d case(s), %d problem(s)" % (
            len(case_ids()), len(errors)))
        return 1 if errors else 0
    if a.write_page and a.case:
        sys.exit("fem-reference: --write-page writes the whole page; run "
                 "every case")
    return run(a)


if __name__ == "__main__":
    sys.exit(main())
