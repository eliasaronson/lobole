#!/usr/bin/env python3
"""Offline unit tests for the Onshape importer's pure mapping functions.

No network, no Designer, no Onshape quota — just the deterministic
translation layer (expressions, sketch entities, constraints) exercised
against a small checked-in fixture (tests/fixtures/onshape/shelltest.json,
a dump of a purpose-built shell+draft+thread part). The FS-derived extras
(face diffs, true planes) may be empty in the fixture — these tests cover
the feature-list mapping, which is what regresses most often (e.g. the
"(145.4+1) mm" unit bug that collapsed a hole to radius 0).

Usage: onshape_import_test.py   (the designer binary arg, if passed, is
ignored — this test needs neither the app nor a display).
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))

import onshape_import as oi  # noqa: E402

FIXTURE = os.path.join(HERE, "fixtures", "onshape", "shelltest.json")

failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)
        print(f"  FAIL: {msg}")
    else:
        print(f"  ok: {msg}")


def approx(a, b, tol=1e-6):
    return abs(a - b) < tol


# --- expression translation (pure) -----------------------------------------

def test_expressions():
    print("translate_expression:")
    cases = {
        "145.4 mm": 145.4,
        "700mm": 700.0,
        "(145.4+1) mm": 146.4,       # the regression: unit after ")"
        "#w170_height-#wall_thickness": None,  # variables -> can't eval here
        "2 * mm": 2.0,
        "1 cm": 10.0,
        "1 in": 25.4,
    }
    env = {}
    for expr, want in cases.items():
        got = oi.translate_expression(expr)
        check(got is not None, f"{expr!r} translated (not None)")
        if want is not None and got is not None:
            try:
                val = eval(got, {"__builtins__": {}}, env)  # noqa: S307
                check(approx(val, want),
                      f"{expr!r} -> {got!r} == {want} (got {val})")
            except Exception as e:  # noqa: BLE001
                check(False, f"{expr!r} -> {got!r} eval failed: {e}")
    # A units-only degree token collapses to a bare number (angle in degrees).
    check(oi.translate_expression("30 deg") == "30", "'30 deg' -> '30'")


# --- sketch entities from the fixture --------------------------------------

def load_partstudio():
    dump = json.load(open(FIXTURE))
    return next(iter(dump["partstudios"].values()))


def test_sketch_entities():
    print("map_sketch_entities:")
    ps = load_partstudio()
    sketches = [f for f in ps["features"]["features"]
                if f["featureType"] == "newSketch"]
    check(len(sketches) > 0, "fixture has sketches")
    total_circles = 0
    for sk in sketches:
        ents, id_map, pts2d, point_lookup, notes = \
            oi.map_sketch_entities(sk)
        # Every mapped entity has a positive-length representation, and no
        # circle should come out with radius 0 straight from the feature
        # list (that was the woofer-hole symptom).
        for e in ents:
            if e["type"] == "circle":
                total_circles += 1
                check(e["radius"] > 0.0,
                      f"{sk['name']} circle radius > 0 (got {e['radius']})")
            if e["type"] == "line":
                dx = e["b"][0] - e["a"][0]
                dy = e["b"][1] - e["a"][1]
                check((dx * dx + dy * dy) > 1e-9,
                      f"{sk['name']} line non-degenerate")
    print(f"  ({total_circles} circles checked)")


# --- constraints ------------------------------------------------------------

def test_constraints():
    print("map_constraint:")
    ps = load_partstudio()
    kinds = set()
    for sk in ps["features"]["features"]:
        if sk["featureType"] != "newSketch":
            continue
        ents, id_map, pts2d, point_lookup, notes = oi.map_sketch_entities(sk)
        for con in sk.get("constraints", []):
            payload, note = oi.map_constraint(con, id_map, ents)
            # Every constraint either maps to a known kind or is reported
            # (payload None + a reason) — never a silent crash.
            if payload is not None:
                check("kind" in payload and "refs" in payload,
                      f"{con['constraintType']} payload well-formed")
                kinds.add(payload["kind"])
            else:
                check(note is not None,
                      f"{con['constraintType']} skip has a reason")
    check(len(kinds) > 0, f"mapped constraint kinds: {sorted(kinds)}")


def main():
    print(f"fixture: {FIXTURE}")
    if not os.path.exists(FIXTURE):
        print("FAIL: fixture missing")
        return 1
    test_expressions()
    test_sketch_entities()
    test_constraints()
    if failures:
        print(f"\n{len(failures)} failure(s)")
        return 1
    print("\nall onshape_import unit checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
