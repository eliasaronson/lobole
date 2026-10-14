#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: record a session, replay it into a fresh headless
instance, and assert the replayed geometry matches (S4 v1).

Usage: replay_smoke.py <path-to-designer-binary>
"""
import os
import socket
import subprocess
import sys
import tempfile

from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402
import lobole.replay  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def launch(binary, env, record=None):
    extra = ["--autosave", "0"]
    if record:
        extra += ["--record", record]
    return launch_app(binary, extra_args=extra, env=env)


def main():
    binary = sys.argv[1]
    tmp = tempfile.mkdtemp(prefix="designer_replay_")
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env["XDG_STATE_HOME"] = tmp
    # ...and its own CONFIG dir, or the run edits the developer's real
    # preferences: set_settings persists immediately (headless runs no GUI
    # loop to debounce it), so a suite reset their font size to 120%.
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    session = os.path.join(tmp, "session.jsonl")

    # Record: a small parametric model, including an undone step.
    app = launch(binary, env, record=session)
    try:
        d = Client.wait_for(app.port)
        d.call("new_project")
        d.call("set_variable", name="w", expression="4")
        sk = d.call("add_feature", feature={
            "type": "Sketch", "name": "S",
            "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "rect", "c1": [0, 0], "c2": [4, 2]}]})
        d.call("add_feature", feature={
            "type": "Extrude", "name": "Pad", "sketch_id": sk["id"],
            "distance": 1.0, "distance_expr": "w / 2", "mode": "new_body"})
        # One call and one undo step, though it adds a sketch and an extrude.
        box(d, (50, 0, 0), (1, 1, 1), "Oops")
        d.call("undo")  # the recording carries the mistake AND its undo
        d.call("set_variable", name="w", expression="6")
        recorded = d.call("get_state")
        expect(len(recorded["bodies"]) == 1, "recorded state: one body")
        recorded_volume = recorded["bodies"][0]["volume_mm3"]
        expect(abs(recorded_volume - 8 * 3) < 1e-6, "recorded volume 24")
    finally:
        app.terminate()
        app.wait(timeout=10)
    expect(os.path.getsize(session) > 0, "session file written")

    # Replay into a FRESH instance; assert the same geometry falls out.
    app2 = launch(binary, env)
    try:
        expect(lobole.replay.replay(app2.port, session) == 0, "replay clean")
        d2 = Client.wait_for(app2.port)
        st = d2.call("get_state")
        expect(len(st["bodies"]) == 1, "replayed state: one body")
        expect(abs(st["bodies"][0]["volume_mm3"] - recorded_volume) < 1e-9,
               "replayed volume matches the recording")
        lv = d2.call("list_variables")["variables"]
        expect(lv[0]["expression"] == "6", "replayed variable expression")
    finally:
        app2.terminate()
        app2.wait(timeout=10)

    print("replay_smoke: all assertions passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
