#!/usr/bin/env python3
"""Compare two arms from a sweep, with an interval instead of a vibe.

    python3 benchmarks/analysis/compare.py results/<dir> --a full --b rpc-only

WHY AN INTERVAL
---------------
"129/129 versus 127/129" is not a finding at this sample size, and reporting it
as one is how a benchmark starts lying. Correctness is resampled by TASK (the
unit that actually varies — repetitions of the same task are not independent
evidence about the API), and the difference is reported with a 95% percentile
bootstrap interval. If that interval straddles zero, the honest sentence is
"no detectable difference in correctness", however far apart the totals look.

Efficiency is a different animal: cost and tool-call gaps are usually large and
consistent enough to read straight off the ratio, which is why Chamfer could
claim 5.9x from N=3 while being careful about correctness. Ratios are printed
with their own interval anyway.
"""
import argparse
import json
import os
import random
import statistics
import sys


def is_broken(r):
    """Did the agent never get off the ground?

    Detected from the RECORD, not from a flag, so it applies retroactively to
    sweeps written before the flag existed — no rewriting an old runs.jsonl to
    make an analysis come out tidier.
    """
    return bool(r.get("harness_error")) or (r.get("num_turns") is None
                                            and r.get("error"))


def load(paths):
    """Read one or more sweeps. Arms run in separate sweeps compare fine as
    long as they ran on the same machine against the same build — meta.json
    records the commit for exactly that argument."""
    rows, broken = [], []
    for path in paths:
        with open(os.path.join(path, "runs.jsonl")) as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                r = json.loads(line)
                (broken if is_broken(r) else rows).append(r)
    if broken:
        print(f"excluded {len(broken)} cell(s) where the agent never started: "
              + ", ".join(f"{r['task']}/{r['arm']}/r{r['rep']}" for r in broken)
              + "\n")
    return rows


def per_task(rows, arm, field):
    """Mean of `field` per task for one arm — the paired unit."""
    out = {}
    for r in rows:
        if r["arm"] != arm:
            continue
        out.setdefault(r["task"], []).append(field(r))
    return {t: statistics.fmean(v) for t, v in out.items()}


def boot(pairs, draws=20000, seed=20260827):
    """Percentile bootstrap over tasks. Fixed seed: a benchmark that reports a
    different interval every time it is run is one more thing to argue about."""
    rng = random.Random(seed)
    n = len(pairs)
    diffs = []
    for _ in range(draws):
        s = [pairs[rng.randrange(n)] for _ in range(n)]
        diffs.append(statistics.fmean(b - a for a, b in s))
    diffs.sort()
    return (statistics.fmean(b - a for a, b in pairs),
            diffs[int(0.025 * draws)], diffs[int(0.975 * draws)])


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("results", nargs="+")
    ap.add_argument("--a", required=True, help="baseline arm")
    ap.add_argument("--b", required=True, help="arm compared against it")
    args = ap.parse_args()

    rows = load(args.results)
    arms = sorted({r["arm"] for r in rows})
    for a in (args.a, args.b):
        if a not in arms:
            sys.exit(f"no arm {a!r} in this sweep (have: {', '.join(arms)})")

    rate = lambda r: r["passed"] / r["total"] if r["total"] else 0.0  # noqa: E731
    metrics = [("check pass rate", rate, "{:+.3f}"),
               ("cost USD / run", lambda r: r.get("cost_usd") or 0.0, "{:+.3f}"),
               ("tool calls / run", lambda r: r.get("tool_calls") or 0, "{:+.1f}"),
               ("wall s / run", lambda r: r.get("wall_s") or 0.0, "{:+.1f}")]

    print(f"{args.a}  ->  {args.b}   ({len(rows)} runs, "
          f"{len({r['task'] for r in rows})} tasks)\n")
    print(f"{'metric':18s} {args.a:>10s} {args.b:>10s} {'delta':>10s}  95% CI")
    for label, field, fmt in metrics:
        A, B = per_task(rows, args.a, field), per_task(rows, args.b, field)
        tasks = sorted(set(A) & set(B))
        pairs = [(A[t], B[t]) for t in tasks]
        if not pairs:
            continue
        mean, lo, hi = boot(pairs)
        ma = statistics.fmean(a for a, _ in pairs)
        mb = statistics.fmean(b for _, b in pairs)
        note = "" if (lo > 0 or hi < 0) else "   (straddles zero)"
        print(f"{label:18s} {ma:10.3f} {mb:10.3f} {fmt.format(mean):>10s}  "
              f"[{lo:+.3f}, {hi:+.3f}]{note}")

    print()
    for a in (args.a, args.b):
        rs = [r for r in rows if r["arm"] == a]
        over = [r for r in rs if r.get("over_claim")]
        clean = sum(1 for r in rs if r["passed"] == r["total"])
        print(f"{a:10s} clean runs {clean}/{len(rs)}   over-claims {len(over)}"
              + (f"   -> {', '.join(sorted({r['task'] for r in over}))}"
                 if over else ""))

    # Which tools each arm actually reached for. This explains results that the
    # totals only report.
    print()
    for a in (args.a, args.b):
        used = {}
        for r in rows:
            if r["arm"] == a:
                for k, v in (r.get("tools_used") or {}).items():
                    used[k] = used.get(k, 0) + v
        top = sorted(used.items(), key=lambda kv: -kv[1])
        print(f"{a:10s} " + ", ".join(f"{k}x{v}" for k, v in top[:10]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
