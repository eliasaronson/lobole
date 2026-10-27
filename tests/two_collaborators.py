#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Two people, one shared project, no network (F9).

A bare repository in a temp directory is a real remote as far as git is
concerned, so two clones of it are a real two-person scenario — and the whole
thing runs in CI with nothing to reach.

The bar is the same one F7 set: after everything, the project LOADS AND
REGENERATES on the other person's machine.
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402

_PHASE = "startup"


def phase(name):
    global _PHASE
    _PHASE = name
    print(f"  [{name}]")


def expect(condition, message):
    if not condition:
        raise AssertionError(f"[phase: {_PHASE}] {message}")


def start(binary, env):
    # launch_app waits for the app to NAME its port, so there is nothing left
    # to retry against here — it either reports one or raises with the app's
    # own log attached.
    app = launch_app(binary, env=env)
    return app, Client(app.port)


def box(name, height):
    """A 20 x 20 block, `height` tall. It is stored as a Sketch and an
    Extrude (docs/modeling/README.md#primitives); the height is the
    Extrude's distance, which is the field the two people below fight over."""
    return {"type": "Box", "name": name, "on": "Top", "centered": False,
            "offset": [0, 0], "width": 20, "depth": 20, "height": height}


def extrude(client):
    """(index, JSON) of the one Extrude in the client's part."""
    feats = client.call("get_state")["features"]
    (i,) = [i for i, f in enumerate(feats) if f["type"] == "Extrude"]
    return i, feats[i]


def set_height(client, height):
    i, f = extrude(client)
    client.call("modify_feature", index=i, feature=dict(f, distance=height))


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "build/lobole"
    tmp = tempfile.mkdtemp(prefix="designer-collab-")
    env = dict(os.environ)
    # Two instances, and neither should touch the developer's real config.
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    env["XDG_STATE_HOME"] = os.path.join(tmp, "state")

    bare = os.path.join(tmp, "shared.git")
    subprocess.run(["git", "init", "--bare", "-q", bare], check=True)

    alice_proc = bob_proc = None
    alice = bob = None
    try:
        alice_proc, alice = start(binary, env)
        bob_proc, bob = start(binary, env)

        phase("Alice makes a project and shares it")
        alice_dir = os.path.join(tmp, "alice.lob")
        alice.call("new_project")
        alice.call("add_feature", feature=box("Body", 40))
        alice.call("save_project", path=alice_dir)
        alice.call("remote_add", url=bare)
        # C4: a shared project stops committing built geometry.
        expect(not os.path.isdir(os.path.join(alice_dir, "geometry")),
               "built geometry is no longer committed once shared")
        expect(alice.call("push")["status"] == "pushed", "first push")

        phase("Bob clones it")
        bob_dir = os.path.join(tmp, "bob.lob")
        bob.call("clone_project", url=bare, path=bob_dir)
        state = bob.call("get_state")
        expect([f["type"] for f in state["features"]] == ["Sketch", "Extrude"],
               f"Bob has Alice's features: {state['features']}")
        expect(abs(extrude(bob)[1]["distance"] - 40.0) < 1e-6, "and its size")

        phase("Both change the same feature")
        set_height(alice, 50)
        alice.call("save_project", path=alice_dir)
        expect(alice.call("push")["status"] == "pushed", "Alice pushes first")

        set_height(bob, 60)
        bob.call("save_project", path=bob_dir)

        phase("Bob's push is rejected, because Alice got there first")
        try:
            bob.call("push")
            raise AssertionError("push should have been rejected")
        except RuntimeError as e:
            expect("someone else" in str(e), f"rejected for the right reason: {e}")

        phase("Bob pulls, and the conflict is described in CAD terms")
        pulled = bob.call("pull")
        expect(pulled["status"] == "conflicted", f"conflicted: {pulled}")
        conflicts = bob.call("conflict_list")
        items = [c for c in conflicts["conflicts"] if c["kind"] == "item"]
        expect(len(items) == 1, f"one item conflict: {conflicts}")
        expect(items[0]["item_kind"] == "Extrude", f"an Extrude: {items[0]}")
        fields = {f["field"]: (f["mine"], f["theirs"])
                  for f in items[0]["fields"]}
        expect("distance" in fields, f"about the distance: {items[0]}")
        expect(fields["distance"] == ("60", "50"), f"his vs hers: {fields}")

        phase("Bob keeps his, finishes, and pushes")
        bob.call("conflict_resolve", path=items[0]["path"], take="mine")
        bob.call("merge_finish")
        expect(bob.call("push")["status"] == "pushed", "Bob pushes the merge")

        phase("Alice pulls, and her project still works")
        pulled = alice.call("pull")
        expect(pulled["status"] in ("fast_forward", "merged", "up_to_date"),
               f"clean for Alice: {pulled}")
        state = alice.call("get_state")
        expect(abs(extrude(alice)[1]["distance"] - 60.0) < 1e-6,
               f"Bob's resolution arrived: {state['features']}")
        # THE BAR: not "git exited 0" but "the project still works".
        expect(len(state["bodies"]) == 1, "and it regenerates to a body")
        expect(state["bodies"][0]["volume_mm3"] > 0, "with real volume")

        phase("Both name a version the same thing, and both names survive")
        alice.call("checkpoint_create", name="before the lid")
        alice.call("push")
        bob.call("pull")
        set_height(bob, 70)
        bob.call("save_project", path=bob_dir)
        bob.call("checkpoint_create", name="before the lid")
        named = [c for c in bob.call("checkpoint_list")["checkpoints"]
                 if c["name"] == "before the lid"]
        expect(len(named) == 2, f"two versions, one name: {named}")
        expect(named[0]["ref"] != named[1]["ref"],
               f"different addresses: {named}")

        phase("Commits say who made them")
        log = subprocess.run(
            ["git", "-C", bob_dir, "log", "--pretty=format:%ae"],
            capture_output=True, text=True, check=True).stdout.split()
        expect(all(a for a in log), "every commit has an author")

        print("two_collaborators: all assertions passed")
        return 0
    finally:
        for client in (alice, bob):
            try:
                client.close()
            except Exception:
                pass
        for proc in (alice_proc, bob_proc):
            if proc and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=10)
                except Exception:
                    proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
