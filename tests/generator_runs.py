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
writing to a private directory (DESIGNER_SCRATCH, or the generator's
--out), never the repo's scratch/ -- and passes only when it exits 0 AND
its own success marker is in its output. A run that exits 0 without the
marker stopped early, which is not a pass.

The budget is set where the test is registered (tests/CMakeLists.txt, a
minute under its ctest TIMEOUT): past it the generator's process group is
killed and the test fails saying so, rather than ctest killing this script
and orphaning the app. Registered for the ctest configuration `generators`
only, so a plain ctest never runs them, and labelled `generators` and
`slow`: `ctest -C generators -L generators` runs them, and
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
GEN = os.path.join(ROOT, "generators")
sys.path.insert(0, HERE)
from smoke_app import launch  # noqa: E402

PY = sys.executable


def om615(port, out):
    # --stage preview: strict single-solid validity of every part, feature
    # status, the accessory mounting structure. An add that fuses the cam
    # cover's cavity into its shell (the body-mode fix) fails here.
    return ([PY, "-u", os.path.join(GEN, "om615", "om615.py"),
             "--port", str(port), "--stage", "preview",
             "--out", os.path.join(out, "om615")],
            [r"preview passed; motion NOT checked"], None)


def pes_pump(port, out):
    # The whole run but the path-traced still: the build, the standing exact
    # sweep, the four studies with their strokes, firing order and rack
    # turn, the GLB and the screenshots. `--no-studies` would check nothing,
    # and the rest costs about 15 s more.
    return ([PY, "-u", os.path.join(GEN, "pes_pump", "pes_pump.py"),
             "--port", str(port), "--no-still",
             "--out", os.path.join(out, "pes_pump")],
            [r"GLB: ", r"render: .*exterior_drive_end"], None)


def chess(port, out):
    # Every design stage from nothing, then one CAM job (the board: pockets,
    # chamfer, engraving, cut-out, verified and posted). All the jobs are
    # another six minutes.
    return (["bash", os.path.join(GEN, "chess", "run_all.sh"), str(port),
             "--only", "board"],
            [r"(?m)^done$"], os.path.join(out, "chess", "cam_all.log"))


def pantilt(port, out):
    # Every stage from nothing through the CAM job and the drawings, so each
    # name a stage picks by is resolved by this binary. Left out: yoke_topo
    # (25 minutes of statics, and a topology result kept in a 25 MB
    # project; no later stage opens its output) and renders (path-traced
    # stills, which check nothing).
    return (["bash", os.path.join(GEN, "pantilt", "run_all.sh"), str(port),
             "--skip", "yoke_topo", "renders"],
            [r"cam: every setup ok and complete", r"(?m)^done$"],
            os.path.join(out, "pantilt", "run_all.log"))


def planetary(port, out):
    # The print plate with its exact mesh clearances and the STL's shell
    # count, the assembly and both motion studies. Left out: the sun's static
    # study (--study: a GPU, or two minutes of CPU) and the hero still.
    return ([PY, "-u", os.path.join(GEN, "planetary", "build.py"),
             "--port", str(port), "--no-render",
             "--out", os.path.join(out, "planetary")],
            [r"motion, one orbit .*: passed", r"planetary: 5 shells"], None)


def hook(port, out):
    # The whole run: both prints checked on the bed, the study solved at two
    # meshes and read for both orientations, the test card written.
    return ([PY, "-u", os.path.join(GEN, "hook", "build.py"),
             "--port", str(port), "--out", os.path.join(out, "hook")],
            [r"mesh: the prediction moved", r"hook: done"], None)

def jar(port, out):
    # The jar, lid and gasket with modelled threads, every check and both
    # negative controls, the unscrewing study and the three STLs; no still.
    return ([PY, "-u", os.path.join(GEN, "jar", "build.py"),
             "--port", str(port), "--no-render",
             "--out", os.path.join(out, "jar")],
            [r"unscrew 2 turns .*: passed", r"jar: done"], None)


# name -> (builder, instance kind, extra environment). The offscreen ones
# take screenshots or renders, which a --headless instance refuses.
RUNS = {
    "om615": (om615, True, {}),
    "pes_pump": (pes_pump, "offscreen", {}),
    "chess": (chess, "offscreen", {}),
    # Six samples a motion study, each still checked exactly.
    "pantilt": (pantilt, "offscreen", {"PANTILT_SAMPLES": "6"}),
    "planetary": (planetary, True, {}),
    "hook": (hook, True, {}),
    "jar": (jar, True, {}),
}


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
    env = dict(os.environ, DESIGNER_SCRATCH=out, PYTHONUNBUFFERED="1",
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
