#!/usr/bin/env python3
"""Partition the GUI suite into balanced shards for gui-tests-xvfb.sh.

Reads the suite's OWN inventory (`lobole --gui-test-list`, one
"category/name" per line) and the recorded per-test times, and prints one
`--gui-test-filter` string per shard.

Two things about the ImGui test engine's filter drive the design:

* A filter token is matched against a test's NAME *or* its CATEGORY, never
  "category/name". So a token is `^name$` (anchored, or "select" would also
  pull in "select_delete").
* 14 test names are also category names (`defaults/pattern` vs the `pattern`
  category), and a token cannot say "the name, not the category". Those tests
  are therefore MERGED into a unit with the same-named category, so the
  over-match lands inside one shard instead of running `pattern`'s 58-second
  stress test in two of them.

Times only balance the shards; they never decide who runs. A test missing
from the times file gets the median weight and still runs — a stale file
costs balance, not coverage, which is the failure mode we can live with.
"""

import argparse
import collections
import sys

# Weight for a test with no recorded time (a new one). Deliberately the
# median rather than 0: an unknown test should not look free.
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


def build_units(tests):
    """Group tests into indivisible units: {filter token: [test, ...]}.

    Normally one test per unit, addressed by `^name$`. But when a test's name
    is also a category name, that token selects BOTH — so the test and every
    member of that category become a single unit under the one token they
    share. Only those categories merge; the rest stay per-test, or a 25-test
    category like `defaults` would be one indivisible 42-second lump.
    """
    categories = {t.split("/", 1)[0] for t in tests}
    names = {t.partition("/")[2] for t in tests}
    merged = categories & names  # the ambiguous ones

    units = collections.defaultdict(list)
    for full in tests:
        category, _, name = full.partition("/")
        if category in merged:
            units["^%s$" % category].append(full)
        elif name in merged:
            units["^%s$" % name].append(full)  # joins that category's unit
        else:
            units["^%s$" % name].append(full)
    return units


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", default="-", help="test inventory ('-' = stdin)")
    ap.add_argument("--times", default=None, help="recorded per-test times")
    ap.add_argument("--shards", type=int, default=4)
    ap.add_argument("--slower-than", type=float, default=None,
                    help="skip tests recorded at or above this many seconds")
    args = ap.parse_args()

    src = sys.stdin if args.list == "-" else open(args.list)
    tests = [ln.strip() for ln in src if ln.strip()]
    if not tests:
        sys.exit("gui_shard: empty test list")
    times = load_times(args.times)

    skipped = []
    if args.slower_than is not None:
        # An UNKNOWN test is never skipped: a newly added test must run even
        # in fast mode, or the mode quietly stops testing new work.
        slow = [t for t in tests
                if times.get(t, DEFAULT_WEIGHT) >= args.slower_than]
        categories = {t.split("/", 1)[0] for t in tests}
        for t in slow:
            # Skipping means an exclusion token `-^name$`, and that matches by
            # NAME OR CATEGORY. If the name is also a category, the exclusion
            # would take that whole category down with it — so this one
            # cannot be expressed, and it runs. Better slow than silently
            # dropping its neighbours.
            if t.partition("/")[2] in categories:
                print("# cannot skip %s (its name is a category)" % t,
                      file=sys.stderr)
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
    excludes = "".join(",-^%s$" % t.partition("/")[2] for t in sorted(skipped))
    for tokens in shards:
        print(",".join(sorted(tokens)) + excludes)
    # The runner asserts the shards ran exactly this many tests: a filter
    # that silently selects nothing is the one failure this design must not
    # have.
    print("# tests: %d" % len(tests), file=sys.stderr)
    est = ", ".join("%.0fs" % l for l in load)
    print("# estimated per shard: %s" % est, file=sys.stderr)


if __name__ == "__main__":
    main()
