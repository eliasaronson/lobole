#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A crash leaves a report the next start collects (core/io/CrashReports.h,
docs/architecture.md#crashes-and-stalls).

All in a scratch XDG_STATE_HOME, shared by the launches below and by nothing
else, so no planted session or report reaches the developer's real one:

  1. headless, two edits by RPC, an autosave (--autosave 1), then
     `debug_crash segv`: the process dies by SIGSEGV;
  2. the next start lists one report through `crash_reports`: report.txt
     says SIGSEGV and its stack names the debug_crash handler; session.log
     has both edits' descriptions; autosave/ holds the sidecar;
  3. `kill -9` gives a report that ended "without a crash report";
  4. a clean quit gives no report, and leaves no marker in sessions/;
  5. tools/symbolize-crash.py names the handler from the RAW frames alone;
  6. an exception nothing catches is reported as SIGABRT with its message,
     and `debug_crash` without LOBOLE_ALLOW_DEBUG_CRASH=1 is refused.

Usage: crash_report_smoke.py <path-to-designer-binary>
"""
import os
import re
import resource
import shutil
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from smoke_app import launch_app  # noqa: E402

from lobole.client import Client  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)
    print(f"  ok: {message}", flush=True)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def wait_exit(app, timeout=20):
    deadline = time.time() + timeout
    while app.poll() is None and time.time() < deadline:
        time.sleep(0.05)
    return app.poll()


def main():
    binary = sys.argv[1]
    # Crashing on purpose is not worth a core file each run.
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    scratch = tempfile.mkdtemp(prefix="lobole_crash_smoke_")
    state = os.path.join(scratch, "state")
    lobole = os.path.join(state, "lobole")
    env = dict(os.environ, XDG_STATE_HOME=state,
               XDG_CONFIG_HOME=os.path.join(scratch, "config"),
               LOBOLE_ALLOW_DEBUG_CRASH="1")
    apps = []

    def launch(extra=(), environment=env):
        app = launch_app(binary, extra_args=list(extra), env=environment)
        apps.append(app)
        return app, Client(app.port, timeout=30)

    try:
        # 1 -------------------------------------------------------------
        app, d = launch(["--autosave", "1"])
        d.call("add_feature", feature={
            "type": "Box", "name": "FirstBox", "on": "Top",
            "width": 10, "depth": 10, "height": 10})
        d.call("add_feature", feature={
            "type": "Box", "name": "SecondBox", "on": "Top",
            "centered": False, "offset": [30, 0],
            "width": 5, "depth": 5, "height": 5})
        # The unsaved slot is the session's own, named by its stem
        # (core/io/Autosaver.h).
        sessions = os.path.join(lobole, "sessions")
        stem = [n[:-len(".json")] for n in os.listdir(sessions)
                if n.endswith(".json")]
        expect(len(stem) == 1, f"one live session marker ({stem})")
        slot = f"unsaved-{stem[0]}.autosave.json"
        sidecar = os.path.join(lobole, slot)
        deadline = time.time() + 20
        while time.time() < deadline and "SecondBox" not in (
                read(sidecar) if os.path.exists(sidecar) else ""):
            time.sleep(0.1)
        expect("SecondBox" in read(sidecar), "both edits were autosaved")
        try:
            d.call("debug_crash", kind="segv")
        except Exception:
            pass  # no answer: that is the point
        code = wait_exit(app)
        expect(code == -signal.SIGSEGV, f"it died by SIGSEGV ({code})")
        expect(any(n.endswith(".crash") for n in os.listdir(sessions)),
               "the handler wrote a crash file beside the marker")

        # 2 -------------------------------------------------------------
        app, d = launch()
        reports = d.call("crash_reports")["reports"]
        expect(len(reports) == 1, f"one report ({len(reports)})")
        r = reports[0]
        expect(r["reason"] == "crashed: SIGSEGV" and r["crashed"],
               f"it says SIGSEGV ({r['reason']})")
        text = read(os.path.join(r["path"], "report.txt"))
        expect("Nothing has been sent" in text, "it says nothing was sent")
        expect("RPC debug_crash" in text, "it names the RPC in flight")
        raw = text.split("raw frames (module+offset):")[1].split("symbolized:")[0]
        expect(re.search(r"#\d+ \S+\+0x[0-9a-f]+", raw),
               "raw frames are module+offset")
        expect("DebugCrash" in text, "the stack names the debug_crash handler")
        log = read(os.path.join(r["path"], "session.log"))
        expect("Add Extrude 'FirstBox'" in log and
               "Add Extrude 'SecondBox'" in log,
               "session.log holds both edits")
        expect("rpc: debug_crash" in log, "and the call that crashed it")
        expect(r["autosaves"] == [slot] and
               "SecondBox" in read(os.path.join(r["path"], "autosave", slot)),
               "autosave/ holds the session's own unsaved slot")
        expect(not os.path.exists(os.path.join(
            sessions, r["stem"] + ".crash")), "the crash file was consumed")
        first = r["path"]

        # 3 -------------------------------------------------------------
        os.kill(app.pid, signal.SIGKILL)
        wait_exit(app)
        app, d = launch()
        reports = d.call("crash_reports")["reports"]
        expect(len(reports) == 2, f"two reports ({len(reports)})")
        expect(reports[0]["reason"].startswith(
            "ended without a crash report") and not reports[0]["crashed"],
            f"kill -9 ended without one ({reports[0]['reason']})")
        expect(reports[1]["path"] == first, "newest first")

        # 4 -------------------------------------------------------------
        app.terminate()
        expect(wait_exit(app) == 0, "a clean quit")
        expect(not [n for n in os.listdir(sessions) if n.endswith(".json")],
               "a clean quit leaves no marker")
        app, d = launch()
        expect(len(d.call("crash_reports")["reports"]) == 2,
               "and no report")

        # 6 -------------------------------------------------------------
        try:
            d.call("debug_crash", kind="exception")
        except Exception:
            pass
        expect(wait_exit(app) == -signal.SIGABRT, "an uncaught exception aborts")
        noenv = dict(env)
        noenv.pop("LOBOLE_ALLOW_DEBUG_CRASH")
        app, d = launch(environment=noenv)
        reports = d.call("crash_reports")["reports"]
        expect(reports[0]["reason"] == "crashed: SIGABRT", "reported SIGABRT")
        text = read(os.path.join(reports[0]["path"], "report.txt"))
        expect("uncaught exception: debug_crash: an exception nothing catches"
               in text, "with what was thrown")
        try:
            d.call("debug_crash", kind="segv")
            refused = ""
        except Exception as e:
            refused = str(e)
        expect("LOBOLE_ALLOW_DEBUG_CRASH" in refused,
               "debug_crash is refused without the variable")
        expect(d.call("ping")["ok"], "and the app carries on")
        app.terminate()
        wait_exit(app)

        # 5 -------------------------------------------------------------
        if shutil.which("addr2line"):
            out = subprocess.run(
                [sys.executable, os.path.join(HERE, "..", "tools",
                                              "symbolize-crash.py"),
                 os.path.join(first, "report.txt"), binary],
                capture_output=True, text=True, check=True).stdout
            raw = out.split("raw frames (module+offset):")[1].split("symbolized:")[0]
            expect(re.search(r"#\d+ .*DebugCrash.*\(lobole\S*\+0x", raw),
                   "symbolize-crash.py names the handler from raw frames")
        else:
            print("  skip: no addr2line on PATH for symbolize-crash.py")
    finally:
        for app in apps:
            if app.poll() is None:
                app.kill()
                app.wait(5)
        shutil.rmtree(scratch, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
