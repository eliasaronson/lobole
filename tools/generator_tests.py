#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The big generators' test declarations, and the README table made of them.

Each big generator declares itself as a test in ONE file in its own
directory, generators/<name>/test.json
(generators/README.md#the-big-generators-as-tests). Three lists used to be
written by hand -- the RUNS table in tests/generator_runs.py, the budgets in
tests/CMakeLists.txt and the table in generators/README.md -- and three of
five branch merges conflicted on the same rows of them. Now
tests/generator_runs.py reads the files, tests/CMakeLists.txt registers one
test per file, and this writes the README's table between its markers.

    tools/generator_tests.py            rewrite the README table
    tools/generator_tests.py --check    fail when the table is stale (ctest
                                        generator_table_gate)

A test.json holds:

    order       where its row goes in the README table, ascending
    why         what the run checks and what it leaves out, and why
    steps       the commands, each an argv list, run in order with `&&`;
                `{python}` is the interpreter running the test, `{generators}`
                the generators/ directory, `{port}` the instance's port and
                `{out}` the test's private output directory
    markers     regexes that must all be in the output: exit 0 without them
                is a run that stopped early
    own_log     (optional) a log the generator writes itself, read for the
                markers too
    instance    headless | offscreen (it takes screenshots or renders)
    env         (optional) extra environment for the generator
    budget_s    killed past it; ctest's TIMEOUT is a minute more
    readme      {runs, measured}: the table's "runs" cell (the instance kind
                is added) and "measured alone"

A key it does not know, a missing one or an unknown instance kind is
refused, naming what is accepted: a typo'd budget must not run unbounded.
"""
import argparse
import json
import os
import shlex
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "generators")
README = os.path.join(GEN, "README.md")
BEGIN = "<!-- generator-tests: made by tools/generator_tests.py -->"
END = "<!-- generator-tests: end -->"

REQUIRED = ("order", "why", "steps", "markers", "instance", "budget_s",
            "readme")
OPTIONAL = ("own_log", "env")
INSTANCES = ("headless", "offscreen")


def _refuse(path, why):
    raise SystemExit(f"{os.path.relpath(path, ROOT)}: {why}")


def _check(path, t):
    if not isinstance(t, dict):
        _refuse(path, "is not an object")
    unknown = sorted(set(t) - set(REQUIRED) - set(OPTIONAL))
    if unknown:
        _refuse(path, f"unknown key {unknown} (accepted: "
                f"{', '.join(REQUIRED + OPTIONAL)})")
    missing = [k for k in REQUIRED if k not in t]
    if missing:
        _refuse(path, f"missing {missing}")
    if t["instance"] not in INSTANCES:
        _refuse(path, f"instance '{t['instance']}' (accepted: "
                f"{', '.join(INSTANCES)})")
    if not (isinstance(t["budget_s"], int) and t["budget_s"] > 0):
        _refuse(path, f"budget_s {t['budget_s']!r} is not a whole number "
                "of seconds over 0")
    if not isinstance(t["order"], int):
        _refuse(path, f"order {t['order']!r} is not a whole number")
    steps = t["steps"]
    if not (isinstance(steps, list) and steps and all(
            isinstance(s, list) and s and all(isinstance(a, str) for a in s)
            for s in steps)):
        _refuse(path, "steps is not a list of argv lists of strings")
    if not (isinstance(t["markers"], list) and t["markers"] and all(
            isinstance(m, str) for m in t["markers"])):
        _refuse(path, "markers is not a list of regexes: a run with none "
                "passes on exit 0 alone")
    readme = t["readme"]
    if not (isinstance(readme, dict) and set(readme) == {"runs", "measured"}):
        _refuse(path, "readme is not {runs, measured}")
    env = t.get("env", {})
    if not (isinstance(env, dict) and all(
            isinstance(v, str) for v in env.values())):
        _refuse(path, "env is not an object of strings")


def load():
    """{name: declaration} for every generators/<name>/test.json, in the
    README's order (order, then name)."""
    found = {}
    for name in sorted(os.listdir(GEN)):
        path = os.path.join(GEN, name, "test.json")
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8") as f:
            try:
                t = json.load(f)
            except json.JSONDecodeError as e:
                _refuse(path, f"is not JSON: {e}")
        _check(path, t)
        found[name] = t
    return dict(sorted(found.items(), key=lambda kv: (kv[1]["order"], kv[0])))


def command(t, port, out, python=sys.executable):
    """(argv, markers, own_log) for one run: one step as itself, several as
    one `bash -c` of them joined by `&&`."""
    subs = {"{python}": python, "{generators}": GEN, "{port}": str(port),
            "{out}": out}

    def fill(s):
        for k, v in subs.items():
            s = s.replace(k, v)
        return s

    steps = [[fill(a) for a in step] for step in t["steps"]]
    argv = steps[0] if len(steps) == 1 else [
        "bash", "-c", " && ".join(shlex.join(s) for s in steps)]
    own = t.get("own_log")
    return argv, list(t["markers"]), fill(own) if own else None


def table(tests):
    rows = ["| test | runs | measured alone | budget |", "|---|---|---|---|"]
    for name, t in tests.items():
        rows.append(f"| `generator_{name}` | {t['readme']['runs']} "
                    f"({t['instance']}) | {t['readme']['measured']} | "
                    f"{t['budget_s']} s |")
    return "\n".join(rows) + "\n"


def readme_with(text, tests):
    if text.count(BEGIN) != 1 or text.count(END) != 1:
        raise SystemExit(f"generators/README.md: needs one '{BEGIN}' and one "
                         f"'{END}' around the table")
    head, rest = text.split(BEGIN)
    _, tail = rest.split(END)
    return head + BEGIN + "\n" + table(tests) + END + tail


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="fail when the README table is not what the "
                         "test.json files make")
    args = ap.parse_args()
    tests = load()
    with open(README, encoding="utf-8") as f:
        text = f.read()
    want = readme_with(text, tests)
    if args.check:
        if want != text:
            print("generators/README.md's table of the big generators is "
                  "stale: run tools/generator_tests.py", file=sys.stderr)
            return 1
        print(f"generator_table_gate: {len(tests)} rows current")
        return 0
    if want != text:
        with open(README, "w", encoding="utf-8") as f:
            f.write(want)
        print(f"generators/README.md: {len(tests)} rows written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
