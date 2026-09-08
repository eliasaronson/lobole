#!/usr/bin/env python3
"""Compare live circular-edge centers with frozen snapshots on an existing project.

Read-only measurements run in an isolated headless process; no human instance is
used. Run through nix-shell, with --project and --parts for a realistic model.
Startup/loading is outside the timed regions. Alternate measurement order and
use fresh Body handles. This measures inspection, not full generator throughput.
"""
import argparse
import json
import os
from pathlib import Path
import statistics
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from designer_sdk import App
from smoke_app import launch


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default=str(ROOT / 'build/designer'))
    parser.add_argument('--project', required=True)
    parser.add_argument('--parts', nargs='+', required=True)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error('--repeats must be positive')
    results = []
    with tempfile.TemporaryDirectory(prefix='inspection-benchmark-') as tmp:
        env = dict(os.environ, XDG_STATE_HOME=tmp, XDG_CONFIG_HOME=tmp+'/config')
        with launch(args.binary, env=env) as running:
            app = App(running.designer)
            app.open_project(path=str(Path(args.project).resolve()))
            for name in args.parts:
                part = app.part(name)
                timings = dict(live_s=[], snapshot_s=[])
                max_difference = 0.0

                def live():
                    return {e.ref['element']: e.center
                            for e in part.body().edges(circular=True)}

                def snapshot():
                    snap = part.body().snapshot(circular=True, max_elements=10000)
                    return {e.element: e.center for e in snap.edges}

                for iteration in range(args.repeats):
                    measured = {}
                    calls = [('live_s', live), ('snapshot_s', snapshot)]
                    for key, call in calls[::1 if iteration % 2 == 0 else -1]:
                        start = time.perf_counter()
                        measured[key] = call()
                        timings[key].append(time.perf_counter() - start)
                    assert measured['live_s'].keys() == measured['snapshot_s'].keys()
                    difference = max(((v - measured['snapshot_s'][k]).length()
                                      for k, v in measured['live_s'].items()), default=0.0)
                    assert difference < 1e-5, (name, difference)
                    max_difference = max(max_difference, difference)
                row = dict(part=name, edges=len(measured['live_s']),
                           max_center_difference_mm=max_difference, **timings,
                           medians={k: statistics.median(v) for k, v in timings.items()})
                results.append(row)
                print(json.dumps(row), flush=True)
    Path(args.output).write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
