#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A lost graphics device is a state, not an abort
(docs/rendering/README.md#a-lost-device).

`debug_fault {kind: "device_lost"}` marks the device lost as the frame loop
does on VK_ERROR_DEVICE_LOST. Then, in the windowed app (--hidden, on the
display tools/xvfb-exec.sh gives) and in --offscreen, each in its own scratch
XDG_STATE_HOME (the autosave and the report are the point) with
LOBOLE_NO_DIALOG=1 so the Restart / Quit box is not shown:

  - the edit made before it is on disk in the autosave, at once, though the
    interval is an hour;
  - screenshot, render_still and select_face are refused with diagnostic
    code device_lost, and a UI action too (windowed);
  - every other method still answers: get_state, ping (renderer.lost and its
    reason), an edit;
  - crash_reports lists a report whose reason is "graphics device lost",
    holding that autosave;
  - the process is alive, and a SIGTERM still ends it cleanly (no marker).

Usage: device_loss_smoke.py <path-to-designer-binary>
"""
import os
import signal
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from smoke_app import launch  # noqa: E402

from lobole.errors import Refused  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)
    print(f"  ok: {message}", flush=True)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def refused_as_lost(call, what):
    try:
        call()
    except Refused as e:
        expect(e.diagnostic and e.diagnostic.get("code") == "device_lost",
               f"{what} is refused as device_lost")
        return
    raise AssertionError(f"{what} was not refused")


def one(binary, mode):
    print(f"device loss, {mode}:", flush=True)
    env = dict(os.environ, LOBOLE_ALLOW_DEBUG_CRASH="1", LOBOLE_NO_DIALOG="1")
    # smoke_app's own scratch dirs, never an inherited real one: the autosave
    # and the report land there.
    env.pop("XDG_STATE_HOME", None)
    env.pop("XDG_CONFIG_HOME", None)
    extra = ["--autosave", "3600"]
    if mode == "windowed":
        extra.append("--hidden")
    with launch(binary, extra_args=extra, env=env,
                headless=False if mode == "windowed" else "offscreen") as app:
        d = app.client
        state = os.path.join(app.tmp, "state", "lobole")
        d.call("add_feature", feature={
            "type": "Box", "name": "BeforeTheLoss", "on": "Top",
            "width": 10, "depth": 10, "height": 10})
        sidecar = os.path.join(state, "unsaved.autosave.json")
        expect(not os.path.exists(sidecar), "nothing autosaved yet (1 h)")
        r = d.call("debug_fault", kind="device_lost")
        expect(r["lost"], "debug_fault marks the device lost")

        deadline = time.time() + 20
        while time.time() < deadline and not (
                os.path.exists(sidecar) and "BeforeTheLoss" in read(sidecar)):
            time.sleep(0.1)
        expect(os.path.exists(sidecar) and "BeforeTheLoss" in read(sidecar),
               "the edit is in the autosave at once")

        refused_as_lost(lambda: d.call(
            "screenshot", path=os.path.join(app.tmp, "x.png")), "screenshot")
        refused_as_lost(lambda: d.call(
            "render_still", mode="quick", width=16, height=16,
            path=os.path.join(app.tmp, "q.png")), "render_still")
        refused_as_lost(lambda: d.call("select_face", mesh=0, face=0),
                        "select_face")
        if mode == "windowed":
            refused_as_lost(lambda: d.call("menu_click", path="Edit/Undo"),
                            "a UI action")

        ping = d.call("ping")
        expect(ping["renderer"]["lost"] and
               "debug_fault" in ping["renderer"]["lost_reason"],
               "ping says the renderer is lost, and why")
        expect(len(d.call("get_state")["features"]) >= 1,
               "get_state still answers")
        d.call("add_feature", feature={
            "type": "Box", "name": "AfterTheLoss", "on": "Top",
            "centered": False, "offset": [30, 0],
            "width": 5, "depth": 5, "height": 5})
        expect(len(d.call("get_state")["features"]) >= 2,
               "and an edit still lands: the document is CPU state")

        reports = d.call("crash_reports")["reports"]
        lost = [x for x in reports
                if x["reason"].startswith("graphics device lost")]
        expect(len(lost) == 1, f"one report of the loss ({len(reports)})")
        copy = os.path.join(lost[0]["path"], "autosave",
                            "unsaved.autosave.json")
        expect(os.path.exists(copy) and "BeforeTheLoss" in read(copy),
               "the report holds the autosave")
        expect(app.proc.poll() is None, "the process is alive")

        app.proc.send_signal(signal.SIGTERM)
        app.proc.wait(timeout=30)
        sessions = os.path.join(state, "sessions")
        expect(not [n for n in os.listdir(sessions) if n.endswith(".json")],
               "and SIGTERM still ends it cleanly")


def restart(binary):
    """The box's Restart, through a stand-in `zenity` that answers it: the
    process starts again in place (the same pid, execv), with
    --after-device-loss, and offers that session's work back."""
    print("device loss, windowed, Restart:", flush=True)
    import re
    import tempfile
    bindir = tempfile.mkdtemp(prefix="lobole_fake_zenity_")
    asked = os.path.join(bindir, "asked.txt")
    with open(os.path.join(bindir, "zenity"), "w") as f:
        f.write(f"#!/bin/sh\nprintf '%s\\n' \"$@\" > '{asked}'\nexit 0\n")
    os.chmod(os.path.join(bindir, "zenity"), 0o755)
    env = dict(os.environ, LOBOLE_ALLOW_DEBUG_CRASH="1", LOBOLE_NO_DIALOG="0",
               PATH=bindir)  # the stand-in alone: no real box on the display
    env.pop("XDG_STATE_HOME", None)
    env.pop("XDG_CONFIG_HOME", None)
    with launch(binary, extra_args=["--autosave", "3600", "--hidden"], env=env,
                headless=False) as app:
        pid = app.proc.pid
        app.client.call("add_feature", feature={
            "type": "Box", "name": "BeforeTheRestart", "on": "Top",
            "width": 10, "depth": 10, "height": 10})
        try:
            app.client.call("debug_fault", kind="device_lost")
        except Exception:
            pass  # the restart may beat the answer
        deadline = time.time() + 30
        ports = []
        while time.time() < deadline and len(ports) < 2:
            ports = re.findall(r"listening on 127\.0\.0\.1:(\d+)", app.log())
            time.sleep(0.2)
        expect(os.path.exists(asked) and "--question" in read(asked) and
               "Restart" in read(asked),
               "the box asked Restart or Quit")
        expect(len(ports) >= 2, "a second start announced its port")
        expect(app.proc.poll() is None, f"still pid {pid}: started in place")
        from lobole.client import Client
        d = Client.wait_for(int(ports[-1]))
        ping = d.call("ping")
        expect(ping["pid"] == pid and not ping["renderer"]["lost"],
               "the new start draws again, in the same process")
        expect("Restarted after a lost graphics device" in app.log(),
               "and offers the lost session's work (the notice)")
        d.close()


def main():
    binary = sys.argv[1]
    one(binary, "offscreen")
    if os.environ.get("DISPLAY"):
        one(binary, "windowed")
        restart(binary)
    else:
        print("  skip: the windowed half needs a display (tools/xvfb-exec.sh)")


if __name__ == "__main__":
    main()
