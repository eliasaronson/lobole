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

A third source covers the gap between a worktree being built and its
instance announcing itself: a claim. `--claim PATH` (tools/worktree-build.sh
passes the worktree) records the port it picks in
~/.local/state/lobole/port-claims/<port>.json, and every later pick skips it
for as long as PATH exists, so two worktrees built minutes apart are not both
told 8801. Asked again for the same PATH, it answers the same port. A claim
whose PATH is gone (a merged worktree is deleted) is stale and removed. Run
from inside a claimed worktree without `--claim`, it answers that worktree's
port, so "which port is mine" has one answer.

8799 is excluded whatever both say. It is the human's dev-watch: the windowed
build under review, whose open document an agent must never mutate (CLAUDE.md).

Usage:
    tools/scratch_port.py           # prints one port, or nothing if none free
    tools/scratch_port.py --range 9000 9100
    tools/scratch_port.py --claim /path/to/worktree
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


CLAIMS = os.path.expanduser("~/.local/state/lobole/port-claims")


def live_claims():
    """{port: path} for every claim whose worktree still exists. A stale one
    (its path deleted) is removed as it is found."""
    claims = {}
    for path in glob.glob(os.path.join(CLAIMS, "*.json")):
        try:
            with open(path) as f:
                c = json.load(f)
            port, owner = int(c["port"]), c["path"]
        except (OSError, ValueError, KeyError, TypeError,
                json.JSONDecodeError):
            continue  # unreadable: leave it, and do not hand its port out
        if os.path.isdir(owner):
            claims[port] = owner
        else:
            try:
                os.remove(path)
            except OSError:
                pass
    return claims


def claim_for(claims, path):
    """The port claimed for the worktree PATH is in, if any."""
    path = os.path.realpath(path)
    for port, owner in claims.items():
        owner = os.path.realpath(owner)
        if path == owner or path.startswith(owner + os.sep):
            return port
    return None


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
    ap.add_argument("--claim", metavar="PATH",
                    help="record the port for the worktree at PATH, so no "
                         "later pick hands it out while PATH exists")
    args = ap.parse_args()

    claims = live_claims()
    if args.claim is not None:
        if not os.path.isdir(args.claim):
            print(f"--claim: {args.claim} is not a directory", file=sys.stderr)
            return 2
        mine = claim_for(claims, args.claim)
    else:
        mine = claim_for(claims, os.getcwd())
    if mine is not None:
        print(mine)
        return 0

    claimed = announced_ports() | set(claims) | {DEV_WATCH_PORT}
    for port in range(args.range[0], args.range[1] + 1):
        if port not in claimed and bindable(port):
            if args.claim is not None:
                os.makedirs(CLAIMS, exist_ok=True)
                with open(os.path.join(CLAIMS, f"{port}.json"), "w") as f:
                    json.dump({"port": port,
                               "path": os.path.realpath(args.claim)}, f)
            print(port)
            return 0
    print(f"no free port in {args.range[0]}-{args.range[1]}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
