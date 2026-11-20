#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Record per-GUI-test wall times, for the shard balancer (gui_shard.py).

Three modes, all of which only BALANCE the shards: a stale time costs
speed, never coverage.

  gui_shard_record.py TIMES          a serial run on stdin -> TIMES, whole
                                     (gui-tests-xvfb.sh --record-times)
  gui_shard_record.py --tee ROWS     a shard's output on stdin, passed through
                                     to stdout; each test's time -> ROWS
  gui_shard_record.py --merge TIMES ROWS...
                                     fold the rows of a run into TIMES

The runner tees and merges on EVERY sharded run. The balance file was
written only by --record-times, which nobody ran: it knew 190 of 517 tests,
and the 328 it did not carry 76% of the time at 5.7 s apiece against the
1.7 s it assumed, so four shards finished 517, 549, 642 and 779 s into the
run. A test's time is the gap between its "Test:" line and the next one's
(the engine flushes each). A merged time is the mean of the old and the
new, so one run on a loaded machine moves the balance halfway, not all the
way; the shards are compared with each other, so a load that slows them
all alike cancels out.
"""

import os
import re
import sys
import tempfile
import time

PATTERN = re.compile(r"Test: '([^']+)' '([^']+)'")


def timed_lines(src, sink):
    """Pass src through to sink; return [(seconds, "category/name")]."""
    rows = []
    current = None
    last = time.time()
    for line in src:
        sink.write(line)
        sink.flush()
        match = PATTERN.search(line)
        if not match:
            continue
        now = time.time()
        name = "%s/%s" % (match.group(1), match.group(2))
        if name == current:
            continue  # the engine prints a test's line again on a failure
        if current:
            rows.append((now - last, current))
        current = name
        last = now
    if current:
        rows.append((time.time() - last, current))
    return rows


def load(path):
    times = {}
    try:
        with open(path) as f:
            for line in f:
                secs, _, name = line.rstrip("\n").partition("\t")
                if not name or secs.startswith("#"):
                    continue
                try:
                    times[name] = float(secs)
                except ValueError:
                    continue
    except FileNotFoundError:
        pass
    return times


def write(path, times):
    """Atomically, so a run reading it never sees half a file."""
    folder = os.path.dirname(os.path.abspath(path))
    os.makedirs(folder, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=folder)
    with os.fdopen(fd, "w") as out:
        for name in sorted(times):
            out.write("%.2f\t%s\n" % (times[name], name))
    os.replace(tmp, path)


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == "--tee":
        rows = timed_lines(sys.stdin, sys.stdout)
        with open(args[1], "w") as out:
            for secs, name in rows:
                out.write("%.2f\t%s\n" % (secs, name))
        return 0
    if len(args) >= 2 and args[0] == "--merge":
        times = load(args[1])
        seen = 0
        for rows in args[2:]:
            for name, secs in load(rows).items():
                old = times.get(name)
                times[name] = secs if old is None else (old + secs) / 2
                seen += 1
        if seen:
            write(args[1], times)
        print("  balance: %d test time(s) folded into %s" % (seen, args[1]))
        return 0
    if len(args) != 1 or args[0].startswith("-"):
        sys.exit("usage: gui_shard_record.py TIMES | --tee ROWS | "
                 "--merge TIMES ROWS...")
    rows = timed_lines(sys.stdin, sys.stdout)
    if not rows:
        sys.exit("gui_shard_record: no tests seen in the output")
    write(args[0], {name: secs for secs, name in rows})
    print("recorded %d test times to %s" % (len(rows), args[0]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
