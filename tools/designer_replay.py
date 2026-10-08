#!/usr/bin/env python3
"""Replay a recorded Designer session (--record JSONL) against a live
instance — every reported bug becomes a replayable regression (S4).

Usage:
    designer_replay.py <port> <session.jsonl>

Sends each recorded semantic request in order over the JSON-RPC remote
(start the target with `lobole --headless --remote <port>` for a
deterministic environment). Exits non-zero on the first failing call.
"""
import json
import sys

from designer_remote import Designer


def replay(port, path):
    # Close when done: the remote takes at most eight clients at once, and a
    # held connection is one of them for as long as it stays open.
    d = Designer.wait_for(port)
    try:
        count = 0
        with open(path) as f:
            for lineno, line in enumerate(f, 1):
                line = line.strip()
                if not line:
                    continue
                entry = json.loads(line)
                try:
                    d.call(entry["method"], **entry.get("params", {}))
                except RuntimeError as e:
                    print(f"replay failed at line {lineno} "
                          f"({entry['method']}): {e}", file=sys.stderr)
                    return 1
                count += 1
        print(f"replayed {count} requests from {path}")
        return 0
    finally:
        d.close()


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    return replay(int(sys.argv[1]), sys.argv[2])


if __name__ == "__main__":
    sys.exit(main())
