#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""An app a test launched dies with the test, even one killed by SIGKILL.

Two `lobole --headless --remote 0` instances were found 16 hours after
their smokes were killed (a ctest timeout, an agent stopping a run): parented
to init, holding a scratch state dir and their memory. No `finally:` runs on
SIGKILL, so the app has to notice by itself — src/app/ParentWatch.h, armed
for every launch by tests/smoke_app.py.

  1. A child python launches through smoke_app and reports the app's pid; the
     child is SIGKILLed; the app must be gone within a few seconds, and by its
     CLEAN stop (the SIGTERM handler's log line), not a default-signal death.
  2. The race the kernel cannot cover: a launcher already dead when the app
     starts. Named by LOBOLE_EXIT_WITH_PARENT, the app exits at once.
  3. A malformed value is refused, not ignored.

The app is found by the pid smoke_app announced, never by a pattern, and
killed by that pid if the test fails with it still running.
"""
import os
import shutil
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

CHILD = r"""
import sys, time
sys.path.insert(0, sys.argv[2])
from smoke_app import launch_app
app = launch_app(sys.argv[1])
print(app.pid, app._tmp, flush=True)
time.sleep(600)
"""


def expect(cond, what):
    if not cond:
        raise AssertionError(what)
    print(f"  ok: {what}")


def start_time(pid):
    """/proc/<pid>/stat field 22, or None once the pid is gone (or a zombie,
    which is gone for this purpose: it holds no memory and serves nothing)."""
    try:
        with open(f"/proc/{pid}/stat") as f:
            fields = f.read().rsplit(")", 1)[1].split()
    except OSError:
        return None
    return None if fields[0] == "Z" else fields[19]


def main():
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} BINARY")
    binary = sys.argv[1]

    print("1. launcher SIGKILLed -> app exits")
    child = subprocess.Popen([sys.executable, "-c", CHILD, binary, HERE],
                             stdout=subprocess.PIPE, text=True)
    app_pid, tmp, born = None, None, None
    try:
        line = child.stdout.readline().split()
        expect(len(line) == 2, f"the child reported the app's pid ({line})")
        app_pid, tmp = int(line[0]), line[1]
        born = start_time(app_pid)
        expect(born is not None, f"app {app_pid} is running")
        with open(f"/proc/{app_pid}/stat") as f:
            ppid = int(f.read().rsplit(")", 1)[1].split()[1])
        expect(ppid == child.pid, "its parent is the launcher itself")

        child.send_signal(signal.SIGKILL)
        child.wait(timeout=10)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and start_time(app_pid) == born:
            time.sleep(0.05)
        expect(start_time(app_pid) != born,
               "the app is gone within 10 s of its launcher")
        with open(os.path.join(tmp, "app.log")) as f:
            log = f.read()
        expect("stop requested" in log,
               "it stopped through its SIGTERM handler (a clean shutdown)")
    finally:
        if child.poll() is None:
            child.kill()
            child.wait()
        # Ours, by pid AND start time: never a reused pid, never a pattern.
        if app_pid and born and start_time(app_pid) == born:
            os.kill(app_pid, signal.SIGKILL)
        if tmp and os.path.basename(tmp).startswith("designer_smoke_"):
            shutil.rmtree(tmp, ignore_errors=True)

    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)

    print("2. launcher dead before startup -> app exits at once")
    gone = subprocess.Popen(["true"])
    gone.wait()                         # reaped: its pid names no process
    env["LOBOLE_EXIT_WITH_PARENT"] = str(gone.pid)
    r = subprocess.run([binary, "--headless", "--remote", "0"], env=env,
                       capture_output=True, text=True, timeout=30)
    expect(r.returncode == 0 and "exited before startup" in r.stderr,
           f"exit 0 naming the dead launcher (exit {r.returncode}: "
           f"{r.stderr.strip()[-200:]})")

    print("3. a value that is not a pid is refused")
    for bad in ("yes", "1", "-5"):
        env["LOBOLE_EXIT_WITH_PARENT"] = bad
        r = subprocess.run([binary, "--headless", "--remote", "0"], env=env,
                           capture_output=True, text=True, timeout=30)
        expect(r.returncode == 2 and "launcher's pid" in r.stderr,
               f"'{bad}' refused with exit 2")
    return 0


if __name__ == "__main__":
    sys.exit(main())
