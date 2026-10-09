#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Pick a scratch --remote port that is free AND unclaimed.

Parallel work in worktrees needs one instance per worktree, and the convention
has been prose: "pass a distinct one per worktree" (tools/worktree-build.sh).
That is a value a human has to hold, in a repo that everywhere else refuses to
let one — free_port() races were fixed by having the app bind its own port,
and every running instance already announces itself in
~/.local/state/lobole/instances/<port>.json.

Two sources, because neither alone is enough:

  - the kernel, for "is anything bound here right now";
  - the announced instances, for "does an app own this even though it is
    momentarily unbound" — a relaunching dev-watch is exactly that.

8799 is excluded whatever both say. It is the human's dev-watch: the windowed
build under review, whose open document an agent must never mutate (CLAUDE.md).

Usage:
    tools/scratch_port.py           # prints one port, or nothing if none free
    tools/scratch_port.py --range 9000 9100
Exit status is 1 when nothing is free, so a caller can branch on it.
"""

import argparse
import glob
import json
import os
import socket
import sys

# The port the human's dev-watch owns. Never handed out.
DEV_WATCH_PORT = 8799


def announced_ports():
    """Ports claimed by instances that have announced themselves."""
    claimed = set()
    pattern = os.path.expanduser("~/.local/state/lobole/instances/*.json")
    for path in glob.glob(pattern):
        # The filename is the port, and the file also carries it. Read the
        # file first (it is the source `ping` uses) and fall back to the name,
        # so a truncated or half-written file still reserves its port rather
        # than being handed to a second instance.
        try:
            with open(path) as f:
                claimed.add(int(json.load(f)["port"]))
                continue
        except (OSError, ValueError, KeyError, json.JSONDecodeError):
            pass
        try:
            claimed.add(int(os.path.basename(path).removesuffix(".json")))
        except ValueError:
            pass
    return claimed


def bindable(port):
    """Whether the kernel will give us this port right now."""
    with socket.socket() as s:
        try:
            s.bind(("127.0.0.1", port))
        except OSError:
            return False
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--range", nargs=2, type=int, metavar=("LO", "HI"),
                    default=[8800, 8899],
                    help="inclusive port range to search (default 8800 8899)")
    args = ap.parse_args()

    claimed = announced_ports() | {DEV_WATCH_PORT}
    for port in range(args.range[0], args.range[1] + 1):
        if port not in claimed and bindable(port):
            print(port)
            return 0
    print(f"no free port in {args.range[0]}-{args.range[1]}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
