#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Two instances on ONE state directory keep each other's recovery copies
(docs/versioning/README.md#autosave-and-recovery).

The human's app and every agent instance started by headless-preview.sh share
~/.local/state/lobole. The unsaved document's recovery copy used to be one
shared file there, so an agent's autosave wrote over the human's, and an
agent's save -- which clears "the unsaved slot" -- deleted it.

In a scratch XDG_STATE_HOME shared by the two launches and by nothing else:

  1. A: an unsaved edit, autosaved;
  2. B: an unsaved edit of its own, autosaved -- A's copy still holds A's
     edit, and B's is a file of its own;
  3. B saves: B's copy goes, A's stays;
  4. A saves: A's copy goes too.

Usage: autosave_instances_smoke.py <path-to-designer-binary>
"""
import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from smoke_app import launch  # noqa: E402

UNSAVED = re.compile(r"^unsaved.*\.autosave\.json$")


def expect(condition, message):
    if not condition:
        raise AssertionError(message)
    print(f"  ok: {message}", flush=True)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def unsaved_copies(lobole):
    """{file name: contents} of every unsaved-document recovery copy."""
    if not os.path.isdir(lobole):
        return {}
    return {n: read(os.path.join(lobole, n))
            for n in os.listdir(lobole) if UNSAVED.match(n)}


def holding(copies, name):
    return [n for n, text in copies.items() if name in text]


def box(d, name):
    d.call("add_feature", feature={
        "type": "Box", "name": name, "on": "Top",
        "width": 10, "depth": 10, "height": 10})


def main():
    binary = sys.argv[1]
    scratch = tempfile.mkdtemp(prefix="lobole_autosave_instances_")
    state = os.path.join(scratch, "state")
    lobole = os.path.join(state, "lobole")
    env = dict(os.environ, XDG_STATE_HOME=state,
               XDG_CONFIG_HOME=os.path.join(scratch, "config"))
    # An hour: only autosave_now writes, so nothing races the checks.
    extra = ["--autosave", "3600"]
    try:
        with launch(binary, extra_args=extra, env=env) as a, \
                launch(binary, extra_args=extra, env=env) as b:
            # 1 ---------------------------------------------------------
            box(a.client, "HumansBox")
            expect(a.client.call("autosave_now")["documents"] == 1,
                   "A autosaved its unsaved document")
            copies = unsaved_copies(lobole)
            expect(len(holding(copies, "HumansBox")) == 1,
                   f"A's recovery copy holds A's edit ({sorted(copies)})")
            mine = holding(copies, "HumansBox")[0]

            # 2 ---------------------------------------------------------
            box(b.client, "AgentsBox")
            expect(b.client.call("autosave_now")["documents"] == 1,
                   "B autosaved its unsaved document")
            copies = unsaved_copies(lobole)
            expect(mine in copies and "HumansBox" in copies[mine] and
                   "AgentsBox" not in copies[mine],
                   "B's autosave did not write over A's copy")
            theirs = holding(copies, "AgentsBox")
            expect(len(theirs) == 1 and theirs[0] != mine,
                   f"B's copy is a file of its own ({sorted(copies)})")

            # 3 ---------------------------------------------------------
            b.client.call("save_project",
                          path=os.path.join(scratch, "agent.json"))
            copies = unsaved_copies(lobole)
            expect(theirs[0] not in copies, "B's save cleared B's copy")
            expect(mine in copies and "HumansBox" in copies[mine],
                   "B's save left A's unsaved copy alone")
            expect(a.client.call("autosave_status")["single_file"],
                   "A still has its unsaved work to recover")

            # 4 ---------------------------------------------------------
            a.client.call("save_project",
                          path=os.path.join(scratch, "human.json"))
            expect(not unsaved_copies(lobole),
                   "A's save cleared A's copy, and none is left")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    print("autosave_instances_smoke: PASS")


if __name__ == "__main__":
    main()
