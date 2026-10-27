#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""SIGTERM ends --headless promptly, whatever it is doing.

docs/architecture.md#stopping. The handler sets a flag the loop reads once an
iteration, and the shutdown after the loop joins every job; a main thread in
one long call never read the flag, and a worker in an OpenCASCADE call with
no interruption point held the join. An import of a 100k-triangle mesh
needed `kill -9`. Now the ordinary shutdown has a few seconds
(stopsignal::kStopGraceSeconds) and after that the process ends anyway:

- an import job that can be cancelled: the clean exit, status 0;
- an import job whose worker cannot be stopped (LOBOLE_TEST_IMPORT_HOLD_DEAF,
  what a step with no interruption point is): ended after the grace, status
  128 + SIGTERM, saying what the main thread was doing;
- the main thread itself inside a long call (an `add_feature` of a mesh
  Import, which builds on the frame): the same;
- a `--file` import at startup: the same as a job;
- a second SIGTERM: at once.

Each must be gone within the grace plus a margin.

Usage: stop_signal_smoke.py <path-to-designer-binary>
"""
import os
import signal
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "python"))
from smoke_app import launch_app  # noqa: E402
from smoke_meshes import torus  # noqa: E402
from lobole.client import Client  # noqa: E402

GRACE_S = 3.0  # stopsignal::kStopGraceSeconds
MARGIN_S = 5.0  # a loaded machine's slack on top of it


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def wait_for(cond, what, timeout=60.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        got = cond()
        if got:
            return got
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {what}")


def import_running(client):
    return any(j["kind"] == "import"
               for j in client.call("background_jobs")["jobs"])


def stopped_within(app, label, bound, second=False):
    """SIGTERM `app`, return (exit status, seconds) once it is gone."""
    t = time.monotonic()
    app.send_signal(signal.SIGTERM)
    if second:
        time.sleep(0.3)
        app.send_signal(signal.SIGTERM)
    try:
        status = app.wait(timeout=bound + 30)
    except Exception:  # noqa: BLE001
        app.kill()
        raise AssertionError(f"{label}: still running {bound + 30:.0f} s "
                             "after SIGTERM")
    took = time.monotonic() - t
    print(f"{label}: exited {status} after {took:.2f} s")
    expect(took < bound, f"{label}: SIGTERM took {took:.1f} s, past "
           f"{bound:.1f} s\n--- log ---\n{app.log()[-3000:]}")
    return status, took


def background(fn):
    """`fn` on a thread; the app it calls is about to be stopped, so the
    connection it loses is expected, not a failure."""
    def run():
        try:
            fn()
        except Exception:  # noqa: BLE001
            pass
    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t


def main():
    binary = sys.argv[1]
    bound = GRACE_S + MARGIN_S
    with tempfile.TemporaryDirectory(prefix="lobole-stop-") as tmp:
        small = os.path.join(tmp, "small.stl")
        torus(small, 2000)
        heavy = os.path.join(tmp, "heavy.stl")
        torus(heavy, 80000)

        hold = {"LOBOLE_TEST_IMPORT_HOLD_MS": "120000"}
        deaf = dict(hold, LOBOLE_TEST_IMPORT_HOLD_DEAF="1")

        # A job that can be cancelled: a clean exit.
        app = launch_app(binary, env=hold)
        d = Client.wait_for(app.port)
        background(lambda: d.request("import_step", {"path": small},
                                     timeout=600))
        poll = Client.wait_for(app.port)
        wait_for(lambda: import_running(poll), "the held import")
        status, _ = stopped_within(app, "a cancellable import job", bound)
        expect(status == 0, f"a cancellable job's stop exited {status}, not 0")

        # A worker that cannot be stopped: ended after the grace.
        app = launch_app(binary, env=deaf)
        d = Client.wait_for(app.port)
        background(lambda: d.request("import_step", {"path": small},
                                     timeout=600))
        poll = Client.wait_for(app.port)
        wait_for(lambda: import_running(poll), "the deaf import")
        status, took = stopped_within(app, "a deaf import job", bound)
        expect(status == 128 + signal.SIGTERM,
               f"a forced stop exited {status}, not {128 + signal.SIGTERM}")
        expect(took >= GRACE_S - 0.5,
               f"forced after {took:.1f} s, before the {GRACE_S} s grace")

        # The main thread inside one call: an Import built on the frame.
        app = launch_app(binary)
        d = Client.wait_for(app.port)
        background(lambda: d.request(
            "add_feature", {"feature": {"type": "Import", "path": heavy}},
            timeout=600))
        wait_for(lambda: "rpc: add_feature" in app.log() or
                 "Add Import" in app.log(), "the add_feature to start")
        time.sleep(0.5)
        status, _ = stopped_within(app, "a busy main thread", bound)
        expect(status in (0, 128 + signal.SIGTERM),
               f"a busy main thread's stop exited {status}")

        # A --file import at startup, held deaf: ended after the grace.
        app = launch_app(binary, ["--file", small], env=deaf)
        poll = Client.wait_for(app.port)
        wait_for(lambda: import_running(poll), "the --file import")
        status, _ = stopped_within(app, "a --file import", bound)
        expect(status == 128 + signal.SIGTERM,
               f"a forced --file stop exited {status}")

        # A second SIGTERM does not wait for the grace.
        app = launch_app(binary, env=deaf)
        d = Client.wait_for(app.port)
        background(lambda: d.request("import_step", {"path": small},
                                     timeout=600))
        poll = Client.wait_for(app.port)
        wait_for(lambda: import_running(poll), "the deaf import")
        status, took = stopped_within(app, "two SIGTERMs", GRACE_S,
                                      second=True)
        expect(status == 128 + signal.SIGTERM, f"two SIGTERMs exited {status}")
    print("stop_signal_smoke: ok")


if __name__ == "__main__":
    main()
