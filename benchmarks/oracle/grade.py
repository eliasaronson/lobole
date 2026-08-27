#!/usr/bin/env python3
"""Grade a STEP file against a task's checks. Facts come from bench_oracle.

Every check is BINARY and name-independent: it asks the geometry a question a
feature name cannot answer. A part whose tree reads Extrude / "4x M6 clearance"
and whose solid has no holes in it fails here, which is the entire point.

    grade.py TASK.json PART.step [--oracle build/bench_oracle]

Prints {"passed": n, "total": n, "checks": [...]} and exits 0 when every check
passed, 1 otherwise. Import `grade_file` to use it from a runner.
"""
import argparse
import json
import math
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_ORACLE = os.path.join(ROOT, "build", "bench_oracle")


def read_facts(step_path, oracle=DEFAULT_ORACLE):
    """Run the oracle. A missing/unreadable file is a fact too, not a crash."""
    if not os.path.exists(step_path):
        return {"ok": False, "error": "no STEP file was exported"}
    proc = subprocess.run([oracle, step_path], capture_output=True, text=True)
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError:
        return {"ok": False,
                "error": (proc.stderr or proc.stdout or "oracle said nothing"
                          ).strip()[:400]}


# --- check kinds -----------------------------------------------------------
# Each returns (passed, got) and is handed the oracle's facts plus the check.

def _within(value, want, tol):
    return abs(value - want) <= tol


def _c_solids(f, c):
    got = f.get("solids", 0)
    if "eq" in c:
        return got == c["eq"], got
    lo, hi = c.get("min", 0), c.get("max", 1 << 30)
    return lo <= got <= hi, got


def _c_bbox(f, c):
    got = f.get("bbox", {}).get("size")
    if not got:
        return False, None
    want, tol = c["size"], c.get("tol", 0.2)
    a, b = (sorted(got), sorted(want)) if c.get("sorted") else (got, want)
    return all(_within(x, y, tol) for x, y in zip(a, b)), got


def _c_volume(f, c):
    got = f.get("volume")
    if got is None:
        return False, None
    if "min" in c or "max" in c:
        return c.get("min", -math.inf) <= got <= c.get("max", math.inf), got
    tol = c["value"] * c.get("tol_pct", 1.0) / 100.0
    return _within(got, c["value"], tol), got


def _c_cyl_group(f, c):
    """N cylindrical faces of a given diameter on a given side of the material.

    `internal` is what separates a Ø7 hole from a Ø7 boss, and the oracle
    derives it from the face's outward normal, not from any feature name.
    """
    tol, want_d = c.get("tol", 0.05), c["diameter"]
    internal = c.get("internal", True)
    got = sum(g["count"] for g in f.get("cylinder_groups", [])
              if g["internal"] == internal and _within(g["diameter"], want_d, tol))
    # Report what IS there, so a near miss reads as a wrong diameter rather
    # than as "no holes at all".
    near = [g for g in f.get("cylinder_groups", []) if g["internal"] == internal]
    return got == c["count"], {"count": got, "present": near}


def _c_inclined(f, c):
    got = f.get("inclined_planes", 0)
    if "eq" in c:
        return got == c["eq"], got
    return c.get("min", 0) <= got <= c.get("max", 1 << 30), got


def _c_com(f, c):
    got = f.get("com")
    if not got:
        return False, None
    tol = c.get("tol", 0.5)
    return all(_within(x, y, tol) for x, y in zip(got, c["value"])), got


CHECKS = {
    "solids": _c_solids,
    "bbox": _c_bbox,
    "volume": _c_volume,
    "cyl_group": _c_cyl_group,
    "inclined_planes": _c_inclined,
    "com": _c_com,
}


def grade(facts, checks):
    """Apply a task's checks to oracle facts. Every check runs; none short-circuit
    — a run that fails four checks and a run that fails one are different runs,
    and a pass rate that stops at the first failure cannot tell them apart."""
    results = []
    for c in checks:
        kind = c["kind"]
        if not facts.get("ok"):
            results.append({**c, "pass": False, "got": None,
                            "note": facts.get("error", "no facts")})
            continue
        fn = CHECKS.get(kind)
        if fn is None:
            results.append({**c, "pass": False, "got": None,
                            "note": f"unknown check kind {kind!r}"})
            continue
        ok, got = fn(facts, c)
        results.append({**c, "pass": bool(ok), "got": got})
    return {"passed": sum(1 for r in results if r["pass"]),
            "total": len(results), "checks": results}


def grade_file(task, step_path, oracle=DEFAULT_ORACLE):
    facts = read_facts(step_path, oracle)
    out = grade(facts, task["checks"])
    out["facts"] = facts
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("task")
    ap.add_argument("step")
    ap.add_argument("--oracle", default=DEFAULT_ORACLE)
    a = ap.parse_args()
    with open(a.task) as fh:
        task = json.load(fh)
    out = grade_file(task, a.step, a.oracle)
    print(json.dumps({k: v for k, v in out.items() if k != "facts"}, indent=2))
    return 0 if out["passed"] == out["total"] else 1


if __name__ == "__main__":
    sys.exit(main())
