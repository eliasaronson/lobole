#!/usr/bin/env python3
"""The frame watchdog reports a stalled main thread, with its stack.

`designer --headless --stall-test` stalls the main loop for 4 s once, a few
iterations in, inside a function named StallSelfTest and under a label. The
watchdog (common/Watchdog.h) must log, from its own thread:

  - one "main thread stalled" line naming the label,
  - the main thread's stack, symbolized, containing StallSelfTest — captured
    by a signal handler that must not allocate, then named on the watchdog's
    thread,
  - one "main thread resumed after" line,

and the app must carry on (it answers `ping` afterwards). A bad
DESIGNER_WATCHDOG_S is refused at start-up rather than defaulted.

Usage: watchdog_smoke.py <path-to-designer-binary>
"""
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)
    print(f"  ok: {message}", flush=True)


def main():
    binary = sys.argv[1]
    scratch = tempfile.mkdtemp(prefix="designer_watchdog_")
    env = dict(os.environ)
    env["XDG_STATE_HOME"] = os.path.join(scratch, "state")
    env["XDG_CONFIG_HOME"] = os.path.join(scratch, "config")
    env.pop("DESIGNER_WATCHDOG_S", None)

    app = launch_app(binary, extra_args=["--stall-test"], env=env)
    try:
        deadline = time.time() + 30
        while "resumed after" not in app.log() and time.time() < deadline:
            time.sleep(0.2)
        log = app.log()
        stalled = [ln for ln in log.splitlines() if "main thread stalled" in ln]
        expect(len(stalled) == 1, f"one stall line ({stalled})")
        expect("stall self-test" in stalled[0], "it names what was running")
        expect("StallSelfTest" in log, "the stack names the stalled function")
        resumed = [ln for ln in log.splitlines() if "resumed after" in ln]
        expect(len(resumed) == 1, f"one resume line ({resumed})")
        d = Designer(app.port, timeout=30)
        expect(d.call("ping").get("ok"), "and the app carries on")
    finally:
        app.terminate()

    env["DESIGNER_WATCHDOG_S"] = "3s"
    r = subprocess.run([binary, "--headless", "--remote", "0"], env=env,
                       capture_output=True, text=True, timeout=30)
    expect(r.returncode == 2 and "number of seconds" in r.stderr,
           "a threshold that is not seconds is refused, not defaulted")
    return 0


if __name__ == "__main__":
    sys.exit(main())
