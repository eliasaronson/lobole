#!/usr/bin/env python3
"""Integration test: record a session, replay it into a fresh headless
instance, and assert the replayed geometry matches (S4 v1).

Usage: replay_smoke.py <path-to-designer-binary>
"""
import os
import socket
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402
import designer_replay  # noqa: E402


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def launch(binary, port, env, record=None):
    cmd = [binary, "--headless", "--remote", str(port), "--autosave", "0"]
    if record:
        cmd += ["--record", record]
    return subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)


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
    port = free_port()
    app = launch(binary, port, env, record=session)
    try:
        d = Designer.wait_for(port)
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
        d.call("add_feature", feature={
            "type": "Box", "name": "Oops", "position": [50, 0, 0],
            "width": 1, "height": 1, "depth": 1})
        d.call("undo")  # the recording carries the mistake AND its undo
        d.call("set_variable", name="w", expression="6")
        recorded = d.call("get_state")
        expect(len(recorded["bodies"]) == 1, "recorded state: one body")
        recorded_volume = recorded["bodies"][0]["volume"]
        expect(abs(recorded_volume - 8 * 3) < 1e-6, "recorded volume 24")
    finally:
        app.terminate()
        app.wait(timeout=10)
    expect(os.path.getsize(session) > 0, "session file written")

    # Replay into a FRESH instance; assert the same geometry falls out.
    port2 = free_port()
    app2 = launch(binary, port2, env)
    try:
        expect(designer_replay.replay(port2, session) == 0, "replay clean")
        d2 = Designer.wait_for(port2)
        st = d2.call("get_state")
        expect(len(st["bodies"]) == 1, "replayed state: one body")
        expect(abs(st["bodies"][0]["volume"] - recorded_volume) < 1e-9,
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
