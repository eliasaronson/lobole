#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The big generators, run as tests: each one's cheapest checking stage.

    generator_runs.py <path-to-designer-binary> <generator> <budget-seconds>

A core change broke the OM615 generator and nobody saw it for three days:
the unit tests pin what each feature does alone, and only a whole generator
puts hundreds of them in one document. So each generator here runs its own
cheapest stage that still CHECKS, from nothing -- against a private instance
launched by smoke_app.py (its own XDG_STATE_HOME and XDG_CONFIG_HOME),
writing to a private directory (LOBOLE_SCRATCH, or the generator's
--out), never the repo's scratch/ -- and passes only when it exits 0 AND
its own success marker is in its output. A run that exits 0 without the
marker stopped early, which is not a pass.

What each one runs, its markers, its instance and its budget are declared in
its own directory, generators/<name>/test.json (tools/generator_tests.py
reads them; tests/CMakeLists.txt registers one test per file). The budget
is passed on the command line, a minute under the test's ctest TIMEOUT:
past it the generator's process group is killed and the test fails saying
so, rather than ctest killing this script and orphaning the app. Registered
for the ctest configuration `generators` only, so a plain ctest never runs
them, and labelled `generators` and `slow`: `ctest -C generators -L
generators` runs them, and
tools/merge-branch.sh --full does after the headless suite
(generators/README.md#the-big-generators-as-tests).
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
from smoke_app import launch  # noqa: E402
import generator_tests  # noqa: E402

PY = sys.executable


# name -> (builder, instance kind, extra environment), from each
# generator's own generators/<name>/test.json (tools/generator_tests.py). The
# offscreen ones take screenshots or renders, which a --headless instance
# refuses.
def _runs():
    runs = {}
    for name, t in generator_tests.load().items():
        runs[name] = ((lambda port, out, t=t:
                       generator_tests.command(t, port, out, PY)),
                      True if t["instance"] == "headless" else "offscreen",
                      dict(t.get("env", {})))
    return runs


RUNS = _runs()


def tail(path, n=60):
    try:
        with open(path, errors="replace") as f:
            return "".join(f.readlines()[-n:])
    except OSError:
        return "  (no output)\n"


def main():
    if len(sys.argv) != 4 or sys.argv[2] not in RUNS:
        sys.exit(f"usage: {sys.argv[0]} BINARY {{{','.join(RUNS)}}} "
                 "BUDGET_S")
    binary, name, budget = sys.argv[1], sys.argv[2], float(sys.argv[3])
    build, kind, extra = RUNS[name]
    out = tempfile.mkdtemp(prefix=f"designer-gen-{name}-")
    env = dict(os.environ, LOBOLE_SCRATCH=out, PYTHONUNBUFFERED="1",
               **extra)
    with launch(binary, headless=kind) as app:
        cmd, markers, own_log = build(app.port, out)
        log = os.path.join(out, f"{name}.log")

        def said():
            text = tail(log)
            return text + (tail(own_log) if own_log else "")

        t0 = time.monotonic()
        with open(log, "w") as f:
            f.write(f"$ {' '.join(cmd)}\n")
            f.flush()
            proc = subprocess.Popen(cmd, env=env, stdout=f,
                                    stderr=subprocess.STDOUT,
                                    start_new_session=True)
            try:
                code = proc.wait(timeout=budget)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
                raise SystemExit(f"{name}: over its budget of {budget:.0f} s"
                                 f" -- killed. Output in {out}\n{said()}")
        took = time.monotonic() - t0
        if code != 0:
            raise SystemExit(f"{name}: exit {code} after {took:.0f} s. "
                             f"Output in {out}\n{said()}")
        text = open(log, errors="replace").read()
        if own_log:
            text += open(own_log, errors="replace").read()
        missing = [m for m in markers if not re.search(m, text)]
        if missing:
            raise SystemExit(f"{name}: exit 0 but no success marker "
                             f"{missing}: it stopped early. Output in {out}\n"
                             f"{said()}")
        print(f"{name}: passed in {took:.0f} s (budget {budget:.0f} s)")
    # Passed: the output is regenerable and runs to hundreds of MB. A
    # failure keeps it, named in the message.
    shutil.rmtree(out, ignore_errors=True)


if __name__ == "__main__":
    main()
