#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A merge waits for a running suite; two suites do not wait for each other.

tools/ctest holds a build directory's lock SHARED for as long as its suite
runs; tools/build.sh and tools/merge-branch.sh take it EXCLUSIVELY
(tools/build-lock.sh) before they change the tree. Without the shared side a
merge relinked designer_tests mid-suite and tests failed once and passed on
the rerun (docs/architecture.md#the-development-loop).

The suite here is a scratch ctest tree whose one test sleeps; the merge is
the exclusive lock exactly as merge-branch.sh and build.sh take it, and both
scripts are checked to take it through that function. Headless, no app.
"""
import os
import pathlib
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
SLEEP_S = 3.0


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def scratch_suite(d: pathlib.Path, name: str):
    d.mkdir(parents=True)
    (d / "CTestTestfile.cmake").write_text(
        f'add_test({name} "/bin/sh" "-c" '
        f'"touch {d}/started; sleep {SLEEP_S}; touch {d}/ended")\n')


def wait_for(path: pathlib.Path, deadline_s=30.0):
    t0 = time.monotonic()
    while not path.exists():
        if time.monotonic() - t0 > deadline_s:
            fail(f"{path} never appeared")
        time.sleep(0.05)


def run_suite(d: pathlib.Path):
    return subprocess.Popen([str(TOOLS / "ctest"), "--test-dir", str(d)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)


def merge_lock(d: pathlib.Path):
    """The lock as merge-branch.sh takes it, then the time it got it."""
    script = (f'. "{TOOLS}/build-lock.sh"; build_lock_exclusive "{d}" 9; '
              'python3 -c "import time; print(time.time())"')
    return subprocess.run(["bash", "-c", script], capture_output=True,
                          text=True, timeout=60)


def main():
    for script in ("build.sh", "merge-branch.sh"):
        if "build_lock_exclusive" not in (TOOLS / script).read_text():
            fail(f"tools/{script} does not take the lock exclusively")
    for script in ("ctest", "gui-tests-xvfb.sh"):
        if "build_lock_shared" not in (TOOLS / script).read_text():
            fail(f"tools/{script} does not take the lock shared")

    with tempfile.TemporaryDirectory() as tmp:
        a = pathlib.Path(tmp) / "a"
        scratch_suite(a, "sleeper")
        suite = run_suite(a)
        wait_for(a / "started")
        merged = merge_lock(a)
        if merged.returncode != 0:
            fail(f"the merge's lock failed: {merged.stderr}")
        got = float(merged.stdout.strip().splitlines()[-1])
        ended = (a / "ended")
        if not ended.exists():
            fail("the merge took the lock while the suite was still running")
        if got < ended.stat().st_mtime - 0.01:
            fail("the merge took the lock before the suite ended")
        if "waiting for the lock" not in merged.stderr:
            fail(f"the merge did not say it waited: {merged.stderr!r}")
        out, _ = suite.communicate(timeout=60)
        if suite.returncode != 0:
            fail(f"the scratch suite failed:\n{out}")

        # Two suites at once: shared, so the second starts while the first
        # still runs.
        b = pathlib.Path(tmp) / "b"
        scratch_suite(b, "sleeper")
        first, second = run_suite(b), None
        wait_for(b / "started")
        os.remove(b / "started")
        second = run_suite(b)
        wait_for(b / "started", deadline_s=SLEEP_S - 0.5)
        if (b / "ended").exists():
            fail("the second suite waited for the first")
        for p in (first, second):
            p.communicate(timeout=60)
    print("build_lock_smoke: ok (a merge waited for the suite; "
          "two suites ran together)")


if __name__ == "__main__":
    main()
