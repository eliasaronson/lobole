#!/usr/bin/env python3
"""Record per-GUI-test wall times from a serial suite run.

Reads the suite's stdout, writes "seconds<TAB>category/name" to the file named
on the command line, and passes the output through so the run still reports
normally. Used by `gui-tests-xvfb.sh --record-times`; the file it writes only
BALANCES the shards, so a stale one costs speed, never coverage.
"""

import re
import sys
import time

PATTERN = re.compile(r"Test: '([^']+)' '([^']+)'")


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: gui_shard_record.py <times-file>")
    rows = []
    current = None
    last = time.time()
    for line in sys.stdin:
        sys.stdout.write(line)
        match = PATTERN.search(line)
        if not match:
            continue
        now = time.time()
        if current:
            rows.append((now - last, current))
        current = "%s/%s" % (match.group(1), match.group(2))
        last = now
    if current:
        rows.append((time.time() - last, current))
    if not rows:
        sys.exit("gui_shard_record: no tests seen in the output")
    with open(sys.argv[1], "w") as out:
        for secs, name in rows:
            out.write("%.2f\t%s\n" % (secs, name))
    print("recorded %d test times to %s" % (len(rows), sys.argv[1]))


if __name__ == "__main__":
    main()
