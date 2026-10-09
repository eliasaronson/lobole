#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

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


def read_facts(step_path, oracle=DEFAULT_ORACLE, host=None, contact_eps=None):
    """Run the oracle. A missing/unreadable file is a fact too, not a crash.

    `host` is a Fit task's other half — the part the candidate has to mate
    with, which the GRADER sees and the agent never does. Passing it makes the
    oracle emit `fit`: how close the two come, how much solid is in solid, and
    how much of the candidate is actually touching.
    """
    if not os.path.exists(step_path):
        return {"ok": False, "error": "no STEP file was exported"}
    cmd = [oracle, step_path]
    if host:
        if not os.path.exists(host):
            # A missing host is a BROKEN TASK, and the one thing it must not
            # do is grade the accessory alone and report a clean fit.
            return {"ok": False,
                    "error": f"the task's host geometry is missing: {host}"}
        cmd += ["--host", host]
        if contact_eps is not None:
            cmd += ["--contact-eps", str(contact_eps)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
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


def _c_solid_com(f, c):
    """Where the i-th solid is, ranked by volume descending.

    The aggregate `com` cannot see an assembly whose parts moved in opposite
    directions, which is exactly what a mechanism does — so a task with more
    than one body grades each one's own position.
    """
    solids = f.get("solid_details") or []
    i = c.get("solid", 0)
    if i >= len(solids):
        return False, {"solids": len(solids)}
    got = solids[i].get("com")
    if not got:
        return False, None
    tol = c.get("tol", 0.5)
    return all(_within(x, y, tol) for x, y in zip(got, c["value"])), got


def _c_solid_volume(f, c):
    """The i-th solid's own volume, ranked by volume descending."""
    solids = f.get("solid_details") or []
    i = c.get("solid", 0)
    if i >= len(solids):
        return False, {"solids": len(solids)}
    got = solids[i].get("volume")
    if "value" in c:
        pct = c.get("tol_pct", 1.0)
        return abs(got - c["value"]) <= abs(c["value"]) * pct / 100.0, got
    lo, hi = c.get("min", 0.0), c.get("max", float("inf"))
    return lo <= got <= hi, got


def _c_com(f, c):
    got = f.get("com")
    if not got:
        return False, None
    tol = c.get("tol", 0.5)
    return all(_within(x, y, tol) for x, y in zip(got, c["value"])), got


# Every name `oracle.cpp::SurfaceKind` can emit. A check naming anything else
# is a typo, and a typo must not read as a measurement — see _c_faces.
SURFACE_KINDS = ("plane", "cylinder", "cone", "sphere", "torus", "bezier",
                 "bspline", "revolution", "extrusion", "other")


def _c_faces(f, c):
    """How many faces, optionally of one surface kind.

    The cheapest check that separates a part from the block of its own bbox:
    a block has six planes whatever its size, an L-bracket has fourteen. bbox
    and volume together cannot see the difference between a shape and any
    other shape of the same size and displacement; this can.

    With `of`, it is also the check that asks HOW the part was built. A revolve
    and a stack of extruded discs approximating it have the same bbox, the same
    volume and the same centre of mass; only one has a cone in it. `of` takes
    one kind or a LIST summed together, because an operation is usually
    reachable more than one way — a bend by sweeping a sketch arc (`torus`) or
    by piping along a spline (`bspline`) — and naming a single kind would fail
    correct work. BenchCAD calls these OR-tuples and needs them for the same
    reason; its own version regexes the model's SOURCE, so a program that
    writes `.sweep(` and produces a prism passes. A census of the exported
    B-rep cannot be written past.
    """
    census = f.get("faces_by_kind") or {}
    if "of" in c:
        want = c["of"]
        names = [want] if isinstance(want, str) else list(want)
        unknown = [n for n in names if n not in SURFACE_KINDS]
        if unknown:
            # A misspelt kind counts zero faces, which silently PASSES a `max`
            # and silently FAILS a `min`. Both look like a result.
            return False, {"unknown": unknown, "accepted": list(SURFACE_KINDS)}
        got = sum(census.get(n, 0) for n in names)
    else:
        got = f.get("faces", 0)
    ok = (got == c["eq"] if "eq" in c
          else c.get("min", 0) <= got <= c.get("max", 1 << 30))
    # Report the whole census when a kind was named, so a miss reads "it is 46
    # planes and 12 cylinders" rather than "no cones" — the first names the
    # wrong construction, the second only says the right one is absent.
    return (ok, {"count": got, "census": census}) if "of" in c else (ok, got)


def _c_area(f, c):
    """Total surface area. Sensitive to every hole, step and pocket that
    volume can be made to compensate for."""
    got = f.get("area")
    if got is None:
        return False, None
    if "min" in c or "max" in c:
        return c.get("min", -math.inf) <= got <= c.get("max", math.inf), got
    tol = c["value"] * c.get("tol_pct", 1.0) / 100.0
    return _within(got, c["value"], tol), got


# --- Fit checks ------------------------------------------------------------
# An accessory is graded against the HOST it mates with, which the agent never
# saw. Each of these answers something the others cannot: how close it comes,
# how much solid is in solid, how much of it is doing the holding, and whether
# it stayed inside the envelope it was given. A single "does it clash" answers
# none of them — a cap hovering a millimetre off the tube clears it perfectly.

def _fit(f, c):
    """The fit facts, or a reason there are none. Never silently absent: a Fit
    check on a run with no host must FAIL, not pass by default."""
    fit = f.get("fit")
    if not fit:
        return None, "this task declared no host geometry, so nothing was mated"
    if not fit.get("ok"):
        return None, fit.get("error", "the fit did not compute")
    return fit, None


def _c_mate_clearance(f, c):
    fit, why = _fit(f, c)
    if fit is None:
        return False, why
    got = fit["distance_mm"]
    lo, hi = c.get("min_mm", 0.0), c.get("max_mm", math.inf)
    return lo <= got <= hi, got


def _c_interference_volume(f, c):
    fit, why = _fit(f, c)
    if fit is None:
        return False, why
    got = fit["interference_mm3"]
    return got <= c.get("max_mm3", 0.0), got


def _c_contact_area(f, c):
    fit, why = _fit(f, c)
    if fit is None:
        return False, why
    got = fit["contact_area_mm2"]
    return got >= c.get("min_mm2", 0.0), got


def _c_envelope(f, c):
    """A bbox CEILING, not a target. Caps the "make it enormous and it will
    certainly fit" move that every clearance check on its own rewards."""
    got = f.get("bbox", {}).get("size")
    if not got:
        return False, None
    want = c["max_mm"]
    return all(g <= w + c.get("tol", 0.01) for g, w in zip(got, want)), got


CHECKS = {
    "solids": _c_solids,
    "bbox": _c_bbox,
    "volume": _c_volume,
    "cyl_group": _c_cyl_group,
    "inclined_planes": _c_inclined,
    "com": _c_com,
    "solid_com": _c_solid_com,
    "solid_volume": _c_solid_volume,
    "faces": _c_faces,
    "area": _c_area,
    "mate_clearance": _c_mate_clearance,
    "interference_volume": _c_interference_volume,
    "contact_area": _c_contact_area,
    "envelope": _c_envelope,
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


def host_of(task, root=None):
    """A Fit task's host STEP, from its `inputs`. Only entries the agent must
    NOT see are hosts; anything agent_visible is a reference the prompt hands
    over, not a grading input."""
    base = root or os.path.join(ROOT, "benchmarks")
    for i in task.get("inputs", []):
        if isinstance(i, dict) and i.get("kind") == "host_geometry":
            if i.get("agent_visible"):
                raise ValueError(
                    f"{task['id']}: host_geometry must be agent_visible: false "
                    f"— it is the answer to the task")
            return os.path.join(base, i["path"])
    return None


def grade_file(task, step_path, oracle=DEFAULT_ORACLE):
    facts = read_facts(step_path, oracle, host=host_of(task),
                       contact_eps=task.get("contact_epsilon_mm"))
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
