#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Partition the GUI suite into balanced shards for gui-tests-xvfb.sh.

Reads the suite's OWN inventory (`lobole --gui-test-list`, one
"category/name" per line) and the recorded per-test times, and prints one
`--gui-test-filter` string per shard.

What the ImGui test engine's filter does drives the design
(ImGuiTestEngine_PassFilter, deps/imgui_test_engine/imgui_test_engine/
imgui_te_engine.cpp), and `passes` below is that function, term for term:

* A term is matched against a test's NAME *or* its CATEGORY, never
  "category/name", and without regard to case. So a test's term is
  `^name$` (anchored, or "select" would also pull in "select_delete").
* `^` and `$` are not a regex: `^aaa$` is "starts with aaa" AND "ends with
  aaa", which `aaa_takes_aaa` also is. A name that is also a category name
  (`defaults/pattern` vs the `pattern` category), two tests of one name in
  two categories (`defaults/box`, `defaults_scale/box`) and a name that
  starts and ends with another's are the same problem: a term cannot say
  "this test and no other".
* `all`, `tests` and `perfs` are keywords, so a test of one of those names
  cannot be addressed at all: refused.

So every test's term is matched, here, against the whole inventory, and
tests whose terms reach each other are MERGED into one unit that goes to one
shard: the over-match lands inside a shard instead of running a test (the
`pattern` category's 58-second stress test, once) in two of them. Before
printing, every shard's filter is run through `passes` over the inventory:
a test that two shards would run, or none, stops the run naming it, so a
term the engine reads more widely than this script does fails here rather
than as the runner's MISMATCH.

Times only balance the shards; they never decide who runs. A test missing
from the times files gets the median of the recorded ones and still runs —
a stale file costs balance, not coverage, which is the failure mode we can
live with. `--times` may be given more than once: the committed seed
(tools/gui-test-times.txt), then the times every run records
(tools/gui_shard_record.py), which win.
"""

import argparse
import collections
import sys

# Weight for a test with no recorded time (a new one) when nothing is
# recorded at all; otherwise the median of what is. Never 0: an unknown test
# should not look free.
DEFAULT_WEIGHT = 1.7


def load_times(path):
    times = {}
    if not path:
        return times
    try:
        with open(path) as f:
            for line in f:
                line = line.rstrip("\n")
                if not line or line.startswith("#"):
                    continue
                secs, _, name = line.partition("\t")
                try:
                    times[name] = float(secs)
                except ValueError:
                    continue
    except FileNotFoundError:
        pass
    return times


KEYWORDS = {"all", "tests", "perfs"}


def term_matches(term, test):
    """Whether one filter term (no leading `-`) matches `test`
    ("category/name") as the engine matches it: the name or the category,
    case-insensitive, `^` anchoring the start and a trailing `$` the end --
    each on its own, so `^a$` is "starts with a and ends with a"."""
    start = term.startswith("^")
    body = term[1:] if start else term
    end = body.endswith("$")
    if end:
        body = body[:-1]
    body = body.lower()
    category, _, name = test.partition("/")
    for s in (name.lower(), category.lower()):
        if not start and not end:
            ok = body in s
        else:
            ok = (not start or s.startswith(body)) and \
                 (not end or s.endswith(body))
        if ok:
            return True
    return False


def passes(spec, test):
    """ImGuiTestEngine_PassFilter for the terms this script writes: comma-
    separated, `-` excludes, the LAST term that matches decides, and a spec
    that starts with an exclusion starts from "everything"."""
    include = spec.lstrip("^").startswith("-")
    for raw in spec.split(","):
        exclude = False
        while raw[:1] in ("-", "^"):
            if raw[0] == "-":
                exclude = True
                raw = raw[1:]
            else:
                break
        if raw.strip("^$").lower() in KEYWORDS:
            include = not exclude  # every test is in the Tests group
        elif term_matches(raw, test):
            include = not exclude
    return include


def term_of(test):
    return "^%s$" % test.partition("/")[2]


def build_units(tests):
    """Group tests into indivisible units: {filter terms: [test, ...]}.

    One test per unit, addressed by `^name$` -- except where a test's term
    also matches another test (see the module's docstring): then the two
    are one unit, and so is everything a member's term reaches, until the
    unit's terms match nothing outside it. A unit is printed with the fewest
    of its members' terms that select all of it (`^pattern$` alone selects
    `defaults/pattern` and the whole `pattern` category). Only the units
    that must merge do, or a 25-test category like `defaults` would be one
    indivisible 42-second lump.
    """
    for t in tests:
        if t.partition("/")[2].lower() in KEYWORDS:
            sys.exit("gui_shard: a GUI test named %r cannot be selected: the "
                     "engine's filter reads that name as a keyword (all, "
                     "tests, perfs). Rename the test." % t)
    reach = {t: [u for u in tests if term_matches(term_of(t), u)]
             for t in tests}
    parent = {t: t for t in tests}

    def find(t):
        while parent[t] != t:
            parent[t] = parent[parent[t]]
            t = parent[t]
        return t

    for t, hits in reach.items():
        for u in hits:
            parent[find(u)] = find(t)
    groups = collections.defaultdict(list)
    for t in tests:
        groups[find(t)].append(t)
    units = {}
    for members in groups.values():
        need = set(members)
        terms = []
        # Widest term first; keep one only when it selects something new.
        for t in sorted(members, key=lambda m: (-len(reach[m]), m)):
            new = need & set(reach[t])
            if new:
                need -= new
                terms.append(term_of(t))
        units[",".join(sorted(set(terms)))] = sorted(members)
    return units


def check_partition(tests, filters, skipped):
    """Each test must be run by exactly one shard's filter, and a skipped
    test by none, under the engine's own reading (`passes`). Returns the
    problems, each naming the test and the shards that would run it."""
    problems = []
    for t in tests:
        runs = [i + 1 for i, f in enumerate(filters) if passes(f, t)]
        want = 0 if t in skipped else 1
        if len(runs) != want:
            problems.append("%s would run in %s (wanted %s)" % (
                t, ("shards " + ", ".join(map(str, runs))) if runs else
                "no shard", "none: it is skipped" if want == 0 else "one"))
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", default="-", help="test inventory ('-' = stdin)")
    ap.add_argument("--times", action="append", default=[],
                    help="recorded per-test times (repeatable; later wins)")
    ap.add_argument("--shards", type=int, default=4)
    ap.add_argument("--slower-than", type=float, default=None,
                    help="skip tests recorded at or above this many seconds")
    ap.add_argument("--self-test", action="store_true",
                    help="check the matcher and the partition on planted "
                         "inventories (ctest gui_shard_selftest)")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    src = sys.stdin if args.list == "-" else open(args.list)
    tests = [ln.strip() for ln in src if ln.strip()]
    if not tests:
        sys.exit("gui_shard: empty test list")
    inventory = list(tests)
    times = {}
    for path in args.times:
        times.update(load_times(path))
    known = sorted(times[t] for t in tests if t in times)
    global DEFAULT_WEIGHT
    if known:
        DEFAULT_WEIGHT = max(0.1, known[len(known) // 2])

    skipped = []
    if args.slower_than is not None:
        # An UNKNOWN test is never skipped: a newly added test must run even
        # in fast mode, or the mode quietly stops testing new work.
        slow = [t for t in tests
                if times.get(t, DEFAULT_WEIGHT) >= args.slower_than]
        for t in slow:
            # Skipping means an exclusion term `-^name$`, and that excludes
            # every test its term matches (module docstring). If that is more
            # than this test, the exclusion would take the others down with
            # it -- so this one cannot be expressed, and it runs. Better slow
            # than silently dropping its neighbours.
            others = [u for u in tests if u != t and term_matches(term_of(t), u)]
            if others:
                print("# cannot skip %s (its term also selects %s)"
                      % (t, ", ".join(others)), file=sys.stderr)
                continue
            skipped.append(t)
        tests = [t for t in tests if t not in set(skipped)]
        if not tests:
            sys.exit("gui_shard: everything was filtered out")

    units = build_units(tests)
    weighted = sorted(
        ((sum(times.get(t, DEFAULT_WEIGHT) for t in members), token)
         for token, members in units.items()),
        reverse=True)

    n = max(1, min(args.shards, len(weighted)))
    load = [0.0] * n
    shards = [[] for _ in range(n)]
    for weight, token in weighted:  # longest-processing-time first
        i = load.index(min(load))
        load[i] += weight
        shards[i].append(token)

    # Exclusions go on EVERY shard, after the includes (the matcher applies
    # tokens in order, last match wins). A unit's include token can be a whole
    # category — `^pattern$`, because `defaults/pattern` shares the name — so
    # dropping one test from that category is only expressible as an
    # exclusion. Without this, --fast still ran the 58-second stress test and
    # came out SLOWER than the full suite, because the shards were balanced
    # as though it would not.
    excludes = "".join("," + "-" + term_of(t) for t in sorted(skipped))
    filters = [",".join(sorted(tokens)) + excludes for tokens in shards]
    problems = check_partition(inventory, filters, set(skipped))
    if problems:
        sys.exit("gui_shard: the partition is not exact under the engine's "
                 "filter:\n  " + "\n  ".join(problems))
    for f in filters:
        print(f)
    # The runner asserts the shards ran exactly this many tests: a filter
    # that silently selects nothing is the one failure this design must not
    # have.
    print("# tests: %d" % len(tests), file=sys.stderr)
    est = ", ".join("%.0fs" % l for l in load)
    print("# estimated per shard: %s (%d of %d tests timed)"
          % (est, len(known), len(tests)), file=sys.stderr)


def self_test():
    """The engine's matching, and partitions of inventories planted with
    every way a term reaches another test, checked by `passes`. Exit 0."""
    failures = []

    def check(what, ok):
        if not ok:
            failures.append(what)

    # The engine's reading of a term (its own FIXME: `^aaa$` matches
    # aaabbbaaa), its case-blindness, and the category.
    check("^aaa$ matches aaabbbaaa", term_matches("^aaa$", "c/aaabbbaaa"))
    check("^aaa$ not aaab", not term_matches("^aaa$", "c/aaab"))
    check("case", term_matches("^Box$", "c/box"))
    check("category", term_matches("^pattern$", "pattern/stress"))
    check("exclusion last", not passes("^a$,-^a$", "c/a"))
    check("leading exclusion starts from all", passes("-^b$", "c/a"))
    planted = [
        "modeling/fillet_takes_the_scans_round",
        "modeling/fillet",
        "x/round", "y/round_and_round",  # ^round$ is round_and_round too
        "defaults/pattern", "pattern/stress", "pattern/small",
        "defaults/box", "defaults_scale/box",
        "ui/Select", "ui/select_delete",
    ] + ["filler/t%02d" % i for i in range(20)]
    for shards in (1, 3, 8):
        for slower in (None, 0.5):
            times = {"pattern/stress": 58.0, "y/round_and_round": 1.0}
            units = build_units(planted)
            for members in units.values():
                for t in members:
                    check("%s in one unit" % t,
                          sum(t in m for m in units.values()) == 1)
            check("round reaches round_and_round",
                  any("x/round" in m and "y/round_and_round" in m
                      for m in units.values()))
            check("pattern merges", any(
                "defaults/pattern" in m and "pattern/stress" in m
                for m in units.values()))
            check("box merges", any(
                "defaults/box" in m and "defaults_scale/box" in m
                for m in units.values()))
            check("select stays apart", not any(
                "ui/Select" in m and "ui/select_delete" in m
                for m in units.values()))
            # A partition of them, as main() deals it.
            skipped = []
            if slower is not None:
                skipped = [t for t in planted if times.get(t, 0.1) >= slower
                           and not [u for u in planted if u != t and
                                    term_matches(term_of(t), u)]]
            keep = [t for t in planted if t not in skipped]
            units = build_units(keep)
            dealt = [[] for _ in range(min(shards, len(units)))]
            for i, terms in enumerate(sorted(units)):
                dealt[i % len(dealt)].append(terms)
            excl = "".join(",-" + term_of(t) for t in sorted(skipped))
            filters = [",".join(d) + excl for d in dealt]
            problems = check_partition(planted, filters, set(skipped))
            check("exact partition, %d shards, slower %s: %s"
                  % (shards, slower, problems), not problems)
    # And a wrong partition is caught, naming the test: ^round$ on its own
    # in one shard and round_and_round's term in another runs it twice.
    problems = check_partition(["x/round", "y/round_and_round"],
                               ["^round$", "^round_and_round$"], set())
    check("a double run is named", len(problems) == 1 and
          problems[0].startswith("y/round_and_round would run in shards 1, 2"))
    for f in failures:
        print("FAILED:", f)
    print("gui_shard self-test: %s" % ("FAILED" if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
