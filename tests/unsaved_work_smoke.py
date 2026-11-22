#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A stop with never-saved work offers it back at the next start
(docs/versioning/README.md#a-stop-offers-unsaved-work-back).

A stop is not a crash: SIGTERM -- tools/dev-watch.sh relaunching on a
rebuild -- deleted the session marker, so the next start offered nothing and
the unsaved slot sat in the state dir unseen. The windowed app now renames
its marker to a stop note instead, and the next windowed start offers the
slot the note names.

In a scratch XDG_STATE_HOME shared by these launches and nothing else:

  1. a windowed app (a visible window, on the display tools/xvfb-exec.sh
     gives) makes an unsaved edit and autosaves it; SIGTERM stops it, and
     the stop note is left;
  2. a --headless start and a --hidden window offer nothing, and leave the
     note for the start it is for;
  3. the next visible windowed start offers it (autosave_status), with no
     crash report;
  4. open_project on the offered slot recovers it: untitled, dirty, never
     saved, the offer answered, autosaving into the new instance's own slot.

Usage: unsaved_work_smoke.py <path-to-designer-binary>
"""
import glob
import os
import shutil
import signal
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from smoke_app import launch  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)
    print(f"  ok: {message}", flush=True)


def wait_until(what, condition, seconds=30.0):
    deadline = time.time() + seconds
    while time.time() < deadline:
        if condition():
            return
        time.sleep(0.1)
    raise AssertionError(f"timed out waiting for {what}")


def main():
    binary = sys.argv[1]
    if not os.environ.get("DISPLAY"):
        print("unsaved_work_smoke: no display (run it under "
              "tools/xvfb-exec.sh); skipped")
        sys.exit(77)
    scratch = tempfile.mkdtemp(prefix="lobole_unsaved_work_")
    state = os.path.join(scratch, "state")
    lobole = os.path.join(state, "lobole")
    sessions = os.path.join(lobole, "sessions")
    env = dict(os.environ, XDG_STATE_HOME=state,
               XDG_CONFIG_HOME=os.path.join(scratch, "config"))
    env.pop("WAYLAND_DISPLAY", None)
    try:
        # 1 -------------------------------------------------------------
        with launch(binary, extra_args=["--autosave", "1"], env=env,
                    headless=False) as app:
            d = app.client
            expect(d.call("ping")["mode"] != "headless",
                   "the first launch is the windowed app")
            d.call("add_feature", feature={
                "type": "Box", "name": "StoppedBox", "on": "Top",
                "width": 10, "depth": 10, "height": 10})
            slots = lambda: glob.glob(  # noqa: E731
                os.path.join(lobole, "unsaved-*.autosave.json"))
            wait_until("the autosave", lambda: any(
                "StoppedBox" in open(p, errors="replace").read()
                for p in slots()))
            slot = slots()[0]
            expect(d.call("ping")["dirty"], "the edit is unsaved")
            app.proc.send_signal(signal.SIGTERM)
            app.proc.wait(timeout=30)
        notes = glob.glob(os.path.join(sessions, "*.stopped"))
        expect(len(notes) == 1, f"the stop left a note ({notes})")
        expect(not glob.glob(os.path.join(sessions, "*.json")),
               "and no session marker, so no crash report")
        expect(os.path.exists(slot), "the unsaved slot is still there")

        # 2 -------------------------------------------------------------
        with launch(binary, env=env) as headless:
            status = headless.client.call("autosave_status")
            expect(status["unsaved_offer"] is None,
                   "a headless start offers nothing")
            expect([s["path"] for s in status["unsaved_slots"]] == [slot],
                   "but lists the work as no live session's")
        with launch(binary, extra_args=["--hidden"], env=env,
                    headless=False) as hidden:
            expect(hidden.client.call("autosave_status")["unsaved_offer"]
                   is None, "a hidden window offers nothing")
        expect(len(glob.glob(os.path.join(sessions, "*.stopped"))) == 1,
               "and both left the note for a visible start")

        # 3 -------------------------------------------------------------
        with launch(binary, extra_args=["--autosave", "3600"], env=env,
                    headless=False) as app:
            d = app.client
            status = d.call("autosave_status")
            offer = status["unsaved_offer"]
            expect(offer is not None and offer["path"] == slot,
                   f"the next windowed start offers it ({offer})")
            expect(offer["documents"] == 1, "one document")
            expect(not d.call("crash_reports").get("reports"),
                   "and calls nothing a crash")
            expect(not glob.glob(os.path.join(sessions, "*.stopped")),
                   "the note is taken: said once")

            # 4 ---------------------------------------------------------
            reply = d.call("open_project", path=slot)
            expect(reply.get("unsaved_work") is True,
                   "open_project says it opened unsaved work")
            ping = d.call("ping")
            expect(ping["project"] == "", "untitled: no project path")
            expect(ping["dirty"], "and dirty: none of it is saved")
            names = [f.get("name") for f in d.call("get_state")["features"]]
            expect("StoppedBox" in names, f"the edit is back ({names})")
            expect(d.call("autosave_status")["unsaved_offer"] is None,
                   "recovering answered the offer")
            own = reply["autosave_path"]
            expect(os.path.basename(own) != os.path.basename(slot) and
                   os.path.dirname(own) == lobole,
                   "it autosaves into this instance's own slot")
            expect(not glob.glob(os.path.join(lobole, "*.autosave.json."
                                              "autosave")),
                   "never into '…autosave.json.autosave'")
            d.call("autosave_now")
            expect(os.path.exists(own) and
                   "StoppedBox" in open(own, errors="replace").read(),
                   "and the work is in that slot")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    print("unsaved_work_smoke: PASS")


if __name__ == "__main__":
    main()
