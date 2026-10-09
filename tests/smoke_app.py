#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

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

THE APP DIES WITH ITS LAUNCHER
------------------------------
Every launch sets DESIGNER_EXIT_WITH_PARENT to this process's pid, so the
kernel sends the app SIGTERM when this process dies — however it dies. The
`finally:` and `with` cleanup here never runs on SIGKILL (a ctest timeout, an
agent stopping a test), and two `--headless --remote 0` instances orphaned
that way were found still serving nothing 16 hours later
(src/app/ParentWatch.h). The death signal follows the THREAD that forked, so
a launch from anything but the main thread is refused rather than left to
kill its app when that thread ends. A launcher that is PID 1 (a container's
entry point) sets nothing: the app refuses 1, and a pid namespace's other
processes die with its init anyway. The app's pid is printed to stderr — kill
it by that, never by a pattern.
"""
import contextlib
import os
import re
import shutil
import subprocess
import sys
import traceback
import tempfile
import threading
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

    @property
    def tmp(self):
        """This run's own directory: its XDG_STATE_HOME and XDG_CONFIG_HOME
        are under here unless the caller passed its own."""
        return self._tmp

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
        _archive_failure(said)
        raise
    finally:
        app.close()


def _archive_failure(app_log):
    """Keep a failing run's evidence somewhere the next run cannot destroy.

    ctest writes a test's output to Testing/Temporary/LastTest.log and the
    NEXT run overwrites it. So the natural response to an intermittent failure
    — run it again — deletes the only record of what happened, and the answer
    is another round of guessing. `fem_async_smoke` has failed three times
    across a month and has never once been diagnosed, for exactly this reason.

    The GUI suite hit the same wall and solved it the same way
    (tools/gui-tests-xvfb.sh archives a failing shard with its filter and
    seed). This is that, for the python smokes: the app's own log plus the
    traceback, under a per-run stamp, capped so old ones cannot accumulate.

    Never raises: a test that fails must report ITS failure, not a filesystem
    error from the code trying to write the failure down.
    """
    try:
        name = os.path.basename(sys.argv[0]).removesuffix(".py") or "smoke"
        root = os.path.join(
            os.environ.get("XDG_STATE_HOME",
                           os.path.expanduser("~/.local/state")),
            "lobole", "smoke-failures")
        os.makedirs(root, exist_ok=True)
        # Keep the last 20; past that the oldest are noise, same rule the GUI
        # archive follows.
        entries = sorted(
            (os.path.join(root, d) for d in os.listdir(root)),
            key=lambda p: os.path.getmtime(p), reverse=True)
        for old in entries[19:]:
            shutil.rmtree(old, ignore_errors=True)
        stamp = time.strftime("%Y%m%d-%H%M%S")
        out = os.path.join(root, f"{stamp}-{name}")
        os.makedirs(out, exist_ok=True)
        if app_log:
            with open(os.path.join(out, "designer.log"), "w") as f:
                f.write(app_log)
        with open(os.path.join(out, "traceback.txt"), "w") as f:
            traceback.print_exc(file=f)
        with open(os.path.join(out, "reproduce.sh"), "w") as f:
            f.write("#!/bin/sh\n# The command that produced this failure.\n"
                    f"# Intermittent? Run it in a loop, and under load:\n"
                    f"#   tools/with-cpu-load.sh -n $(nproc) -- <this>\n"
                    + " ".join(sys.argv) + "\n")
        os.chmod(os.path.join(out, "reproduce.sh"), 0o755)
        print(f"--- evidence kept in {out} ---", file=sys.stderr)
    except Exception:
        pass


OWNER_FILE = "smoke_app.owner"
SWEEP_MIN_AGE_S = 600


def _alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _sweep_orphaned_scratch():
    """Remove scratch dirs whose launcher AND app are both gone.

    A launcher that was SIGKILLed never ran its rmtree; its app now exits with
    it, but the directory stays. Only a directory that names its owners is
    touched — one without the owner file is not provably ours to delete — and
    a pid that has been reused merely keeps a directory one sweep longer.
    """
    root = tempfile.gettempdir()
    try:
        names = os.listdir(root)
    except OSError:
        return
    for name in names:
        if not name.startswith("designer_smoke_"):
            continue
        path = os.path.join(root, name)
        try:
            with open(os.path.join(path, OWNER_FILE)) as f:
                pids = [int(p) for p in f.read().split()]
        except (OSError, ValueError):
            continue
        # Dead owners alone are not enough: a launcher killed a moment ago
        # (exit_with_parent_smoke does it on purpose) still has a watcher
        # about to read its app.log, and a parallel launch's sweep removed it
        # under the test. Ten minutes is past any reader of a fresh orphan.
        try:
            age = time.time() - os.path.getmtime(os.path.join(path, OWNER_FILE))
        except OSError:
            continue
        if age > SWEEP_MIN_AGE_S and pids and not any(_alive(p) for p in pids):
            shutil.rmtree(path, ignore_errors=True)


def _spawn(binary, extra_args, env, timeout, headless):
    """Launch, wait for the app to name its port, return (proc, port, log, tmp).

    `headless` is a tri-state: True runs --headless, False the windowed app,
    and the string "offscreen" runs --offscreen (real renderer, no window) —
    which, like headless, must work with no display server at all.
    """
    # The pid named below must be the parent the kernel watches, and that is
    # the forking THREAD: a worker thread that returns would take the app
    # with it, mid-test, for no visible reason.
    if threading.current_thread() is not threading.main_thread():
        raise RuntimeError("smoke_app launches only from the main thread: "
                           "the app exits when the thread that started it "
                           "does (DESIGNER_EXIT_WITH_PARENT)")
    _sweep_orphaned_scratch()
    tmp = tempfile.mkdtemp(prefix="designer_smoke_")
    e = dict(os.environ if env is None else env)
    # Dies with THIS process, even one killed by SIGKILL (see the docstring).
    # Except as PID 1 -- a container's entry point, as in ci-local's pixel
    # gate: the app refuses 1, since an orphan's parent reads 1 too, and it
    # needs no asking, since when a pid namespace's init exits the kernel
    # kills every process left in it.
    if os.getpid() != 1:
        e["DESIGNER_EXIT_WITH_PARENT"] = str(os.getpid())
    else:
        e.pop("DESIGNER_EXIT_WITH_PARENT", None)
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
    # ...and its STATE dir, where the recently-opened list, the autosaves and
    # the instance registry live. Without it a suite run pushed temp projects
    # (rig.dsgn, p.dsgn, single.dsgn) into the developer's "recently opened"
    # menu, and an unsaved document's recovery copy sits in that same
    # directory. A test that wants the real one — the MCP pair, which reads
    # the registry from the client side — sets it itself, and setdefault
    # leaves that alone.
    e.setdefault("XDG_STATE_HOME", os.path.join(tmp, "state"))

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
    # Who owns this directory, for the sweep in the next launch.
    with open(os.path.join(tmp, OWNER_FILE), "w") as f:
        f.write(f"{os.getpid()} {proc.pid}\n")
    print(f"smoke_app: designer pid {proc.pid}, scratch {tmp}",
          file=sys.stderr)

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

