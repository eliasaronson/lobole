#!/usr/bin/env python3
"""Compare full contact and volume-only sweeps on an already open engine.

Usage: python3 tests/om615_sweep_benchmark.py SCRATCH_PORT [OUT.json]
Does not change geometry. Use an isolated instance with its engine tab open.
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_sdk import connect


def main():
    app = connect(int(sys.argv[1]))
    rig = app.assembly("engine", create=False)
    rig.activate()
    reports = {}
    # ABBA order reduces the influence of warming caches on the comparison.
    for index, mode in enumerate(("contacts", "volume", "volume", "contacts")):
        started = time.perf_counter()
        kw = dict(max_pairs=20000, deadline_s=900, _timeout=1800)
        report = (app.contact_report(**kw) if mode == "contacts" else
                  app.interference(mode="exact", **kw))
        assert not report.get("truncated"), report
        hits = report["clashes"] if mode == "contacts" else report["pairs"]
        clashes = sorted((c["a"], c["b"], c["volume"]) for c in hits)
        if reports:
            assert clashes == next(iter(reports.values()))["clashes"]
        row = dict(wall_s=time.perf_counter() - started,
                   sweep_s=report["elapsed_s"],
                   checked=report["pairs_checked"], clashes=clashes)
        reports[f"{index}_{mode}"] = row
        print(mode, json.dumps(row), flush=True)
    if len(sys.argv) > 2:
        with open(sys.argv[2], "w") as f:
            json.dump(reports, f, indent=2)


if __name__ == "__main__":
    main()
