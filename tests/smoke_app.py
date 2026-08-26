#!/usr/bin/env python3
"""Launch a Designer instance for a smoke test, and keep what it said.

WHY THIS EXISTS
---------------
Thirty smoke scripts each carried their own copy of this:

    def free_port():
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            return s.getsockname()[1]      # ...and the socket closes HERE

    app = subprocess.Popen([binary, "--headless", "--remote", str(port)],
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)

Two problems, and both of them cost real time.

**The port is a race.** The socket is closed before the child binds, so
between those two moments the kernel is free to hand the same port to
somebody else — another one of these scripts, running in parallel under
`ctest -j`. It is a narrow window and a rare failure, which is the worst
combination: rare enough to be dismissed as a flake, common enough to keep
costing a rerun. The app now takes `--remote 0`, which binds a
kernel-chosen port IN THE PROCESS THAT KEEPS IT and reports which. There is
no window left to lose.

**The output went to DEVNULL.** So when one of these tests failed, all
anyone ever saw was an assertion or a socket timeout, with no idea what the
app itself had said — and `fem_smoke` has been failing intermittently for
long enough to earn a line in CLAUDE.md without anyone once seeing its
cause. The log is kept now and printed on failure.

USAGE
-----
    from smoke_app import launch

    with launch(sys.argv[1]) as app:          # exits non-zero and prints the
        d = app.designer                      # app's log if anything raises
        d.call("ping")

`launch` yields an object with `.designer` (connected), `.port`, `.log()`
and `.proc`. Extra CLI arguments and environment go in as keyword
arguments; a temporary XDG_CONFIG_HOME is set up by default so a suite run
cannot rewrite the developer's own preferences (tests/fem_smoke.py learned
that one the hard way).
"""
import contextlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from designer_remote import Designer  # noqa: E402

# Both the windowed and the headless path log this; matching either means the
# helper does not care which one it launched.
PORT_RE = re.compile(r"(?:listening on 127\.0\.0\.1:|remote port )(\d+)")


class LaunchedApp:
    def __init__(self, proc, port, log_path, tmp):
        self.proc = proc
        self.port = port
        self._log_path = log_path
        self._tmp = tmp
        self.designer = Designer.wait_for(port)

    def log(self):
        try:
            with open(self._log_path) as f:
                return f.read()
        except OSError:
            return ""

    def close(self):
        with contextlib.suppress(Exception):
            self.designer.close()
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        shutil.rmtree(self._tmp, ignore_errors=True)


class AppProcess:
    """What `launch_app` hands back: Popen's cleanup surface, plus a port.

    `terminate`/`wait`/`kill` are mirrored deliberately so the existing
    `finally: app.terminate() ...` block in every smoke script keeps working
    untouched. Migrating those scripts is then a three-line change instead of
    a rewrite, which matters when there are twenty-nine of them.
    """

    def __init__(self, proc, port, log_path, tmp):
        self._proc = proc
        self.port = port
        self._log_path = log_path
        self._tmp = tmp

    def log(self):
        try:
            with open(self._log_path) as f:
                return f.read()
        except OSError:
            return ""

    def terminate(self):
        self._proc.terminate()

    def kill(self):
        self._proc.kill()

    def wait(self, timeout=None):
        try:
            return self._proc.wait(timeout=timeout)
        finally:
            shutil.rmtree(self._tmp, ignore_errors=True)

    def poll(self):
        return self._proc.poll()

    def send_signal(self, sig):
        self._proc.send_signal(sig)

    @property
    def pid(self):
        return self._proc.pid


def launch_app(binary, extra_args=(), env=None, timeout=30.0, headless=True):
    """Start the app on a kernel-chosen port; returns an AppProcess.

    Also installs an excepthook that prints the app's log before an uncaught
    traceback — so a migrated script gains failure diagnosis without a single
    line of its own changing. That is the half nobody had: every one of these
    scripts sent the app's output to DEVNULL, so an intermittent failure
    (fem_smoke, for months) showed an assertion and nothing about why.
    """
    proc, port, log_path, tmp = _spawn(binary, extra_args, env, timeout,
                                       headless)
    app = AppProcess(proc, port, log_path, tmp)

    previous = sys.excepthook

    def hook(exc_type, exc, tb):
        said = app.log().strip()
        if said:
            print("--- designer log ---\n" + said + "\n--- end ---",
                  file=sys.stderr)
        previous(exc_type, exc, tb)

    sys.excepthook = hook
    return app


@contextlib.contextmanager
def launch(binary, extra_args=(), env=None, timeout=30.0, headless=True):
    """Start `binary`, wait for it to name its port, yield a LaunchedApp.

    Raises RuntimeError with the app's own log attached if it never reports a
    port — which is the case that used to present as a bare socket timeout.
    """
    proc, port, log_path, tmp = _spawn(binary, extra_args, env, timeout,
                                       headless)
    app = LaunchedApp(proc, port, log_path, tmp)
    try:
        yield app
    except BaseException:
        # The whole point: say what the app said, before the traceback.
        said = app.log().strip()
        if said:
            print("--- designer log ---\n" + said + "\n--- end ---",
                  file=sys.stderr)
        raise
    finally:
        app.close()


def _spawn(binary, extra_args, env, timeout, headless):
    """Launch, wait for the app to name its port, return (proc, port, log, tmp).

    `headless` is a tri-state: True runs --headless, False the windowed app,
    and the string "offscreen" runs --offscreen (real renderer, no window) —
    which, like headless, must work with no display server at all.
    """
    tmp = tempfile.mkdtemp(prefix="designer_smoke_")
    e = dict(os.environ if env is None else env)
    if headless:
        # No display: these paths run under ctest with no X, and a stray
        # DISPLAY makes it look like the windowed path is wanted. ONLY for
        # them — mcp_smoke launches the windowed app with --hidden under
        # Xvfb, and stripping DISPLAY there gets it "GLFW Error 65550:
        # Failed to detect any supported platform".
        e.pop("DISPLAY", None)
        e.pop("WAYLAND_DISPLAY", None)
    # set_settings persists immediately headless (no GUI loop to debounce it),
    # so without this a suite run rewrites the developer's real preferences.
    e.setdefault("XDG_CONFIG_HOME", os.path.join(tmp, "config"))

    log_path = os.path.join(tmp, "app.log")
    cmd = [binary]
    if headless == "offscreen":
        cmd.append("--offscreen")
    elif headless:
        cmd.append("--headless")
    cmd += ["--remote", "0", *extra_args]
    with open(log_path, "w") as log:
        proc = subprocess.Popen(cmd, env=e, stdout=log,
                                stderr=subprocess.STDOUT)

    port = None
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            break                      # died; fall through to the report
        try:
            with open(log_path) as f:
                m = PORT_RE.search(f.read())
            if m:
                port = int(m.group(1))
                break
        except OSError:
            pass
        time.sleep(0.05)

    if port is None:
        with contextlib.suppress(Exception):
            proc.kill()
        try:
            with open(log_path) as f:
                said = f.read().strip()
        except OSError:
            said = ""
        shutil.rmtree(tmp, ignore_errors=True)
        raise RuntimeError(
            f"designer never reported a port within {timeout:g}s "
            f"(exit={proc.poll()}). It said:\n"
            + (said or "  (nothing)"))

    return proc, port, log_path, tmp

