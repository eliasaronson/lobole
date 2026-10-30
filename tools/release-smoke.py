#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Start a packaged Loböle and prove it models, before anyone downloads it.

    tools/release-smoke.py --version 0.1.0 -- ./lobole-x86_64.AppImage
    tools/release-smoke.py --version 0.1.0 -- wine64 app/bin/lobole.exe
    tools/release-smoke.py --attach --port 8437      # a container already up

Every release artifact goes through this in release.yml (and can be run on
one by hand). Answering `ping` proves only that the binary starts; the two
checks after it are the ones a broken package has failed before:

- a 4 mm box regenerates to 64 mm³ — OpenCASCADE is loaded and working;
- a circle sketched at r 1 and CONSTRAINED to r 2, extruded 2 mm, is
  8π mm³ — the PlaneGCS solver is linked. A package built without Boost
  configured with the solver silently off and still drew the r 1 circle.

`--version` also checks that `CMD --version` names the version being
released, so a tag cannot ship a binary built from another one.

`--calculix` (a package that bundles ccx: the GUI AppImage, the Windows zip)
checks that `ping` finds the copy shipped beside the app, not a host's, and
that it solves (docs/packaging/README.md#the-bundled-calculix):

- with Netgen in the build, a bar pulled along its length is solved as a
  static study on CalculiX and in-house, and the two must agree;
- without it (Windows), the ccx `ping` named runs a one-element deck, a unit
  cube in uniform tension, whose answer is exact.

`--system-faults` (Windows, run by packaging/windows/smoke.sh) checks the
crash handler with faults in the system's own code, on a worker thread,
through `debug_crash` (docs/architecture.md#crashes-and-stalls):

- one the system catches itself (`system_probe`: IsBadReadPtr on an unmapped
  address) answers, and the app lives on;
- one it does not (`system_fault`: msvcrt's memcpy from an unmapped address)
  ends the app with a crash file, which the next start turns into a report
  whose frames are the system DLL's, then lobole.exe's. Before the handler
  walked such a fault's frames, mcfgthread's thread entry ended the process
  first and there was no report.
"""
import argparse
import math
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent /
                       "python"))
from lobole.client import Client  # noqa: E402


def wait_for_ping(port, proc, deadline_s):
    end = time.monotonic() + deadline_s
    last = None
    while time.monotonic() < end:
        if proc is not None and proc.poll() is not None:
            sys.exit(f"FAIL: the app exited ({proc.returncode}) before it "
                     "answered")
        try:
            return Client(port, timeout=10).call("ping")
        except OSError as e:
            last = e
        time.sleep(0.5)
    sys.exit(f"FAIL: nothing answered on port {port} in {deadline_s} s "
             f"({last})")


def check_models(port):
    c = Client(port)
    c.call("add_feature", feature={"type": "Box", "name": "B", "on": "Top",
                                   "width": 4, "height": 4, "depth": 4})
    vol = c.call("get_state")["bodies"][0]["volume_mm3"]
    if abs(vol - 64) > 1e-6:
        sys.exit(f"FAIL: a 4 mm box is {vol} mm³, not 64")

    c.call("new_project")
    sk = c.call("add_feature", feature={
        "type": "Sketch", "name": "Disc",
        "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                  "uAxis": [1, 0, 0]},
        "entities": [{"type": "circle", "center": [0, 0], "radius": 1}]})
    c.call("add_constraints", sketch_id=sk["id"], constraints=[
        {"kind": "radius", "refs": [[sk["entity_ids"][0], 0]], "value": 2}])
    c.call("add_feature", feature={"type": "Extrude", "name": "Pad",
                                   "sketch_id": sk["id"], "distance": 2.0,
                                   "mode": "new_body"})
    vol = c.call("get_state")["bodies"][0]["volume_mm3"]
    if abs(vol - 8 * math.pi) > 1e-3:
        sys.exit(f"FAIL: a circle constrained to r 2, extruded 2 mm, is "
                 f"{vol:.4f} mm³, not {8 * math.pi:.4f}: the sketch solver "
                 "is not working in this build")


# A unit cube of one C3D8 element in uniform tension, held on three faces
# only as symmetry needs: E 1000 MPa, nu 0.3, 1 N over the top. The stress
# is uniform, so the full-integration hex is exact: the top rises 1e-3 mm
# and the far corner moves in by 3e-4 mm in x and y.
CUBE_DECK = """*NODE, NSET=NALL
1, 0, 0, 0
2, 1, 0, 0
3, 1, 1, 0
4, 0, 1, 0
5, 0, 0, 1
6, 1, 0, 1
7, 1, 1, 1
8, 0, 1, 1
*ELEMENT, TYPE=C3D8, ELSET=EALL
1, 1, 2, 3, 4, 5, 6, 7, 8
*BOUNDARY
1, 1, 3
2, 2, 3
3, 3, 3
4, 1, 1
4, 3, 3
*MATERIAL, NAME=M
*ELASTIC
1000., 0.3
*SOLID SECTION, ELSET=EALL, MATERIAL=M
*STEP
*STATIC
*CLOAD
5, 3, 0.25
6, 3, 0.25
7, 3, 0.25
8, 3, 0.25
*NODE PRINT, NSET=NALL
U
*END STEP
"""


def bundled_ccx(path):
    """Whether `path` is the ccx a package ships beside the app:
    <prefix>/lib/lobole/ccx, or ccx.exe beside lobole.exe."""
    p = path.replace("\\", "/")
    return p.endswith("/lib/lobole/ccx") or p.lower().endswith("/bin/ccx.exe")


def check_calculix_study(c, ccx):
    c.call("new_project")
    fid = c.call("add_feature", feature={
        "type": "Box", "name": "Bar", "on": "Top", "width": 10, "height": 10,
        "depth": 60})["id"]
    c.call("set_material", feature_id=fid, material="steel")
    els = c.call("list_elements")["bodies"][0]
    faces = {}
    for f in els["faces"]:
        n = f["normal"]
        if abs(abs(n[2]) - 1) < 1e-6:
            faces[n[2] > 0] = f["name"]
    if len(faces) != 2:
        sys.exit(f"FAIL: the bar's two ends were not found: {els['faces']}")

    def solve(solver):
        sid = c.call("add_study", name=solver)["id"]
        # converge: never, so both answer on one mesh: a converging run
        # refines until a time budget, which a slow runner reaches sooner,
        # and two meshes differ by more than the tolerance below.
        c.call("set_study", id=sid, mesh_size=5.0, solver=solver,
               converge="never")
        c.call("add_load", study=sid, type="fixed",
               face={"feature_id": els["created_by"], "element": faces[False]})
        c.call("add_load", study=sid, type="force",
               face={"feature_id": els["created_by"], "element": faces[True]},
               magnitude=1000.0, direction=[0, 0, 1])
        return c.call("run_study", id=sid,
                      **({"timeout_s": 300} if solver == "calculix" else {}))

    own = solve("in_house")
    theirs = solve("calculix")
    if theirs.get("solver") != f"calculix {ccx['version']}":
        sys.exit(f"FAIL: the study was not solved on the bundled ccx: "
                 f"{theirs.get('solver')}")
    a = own["max_displacement_mm"]["value"]
    b = theirs["max_displacement_mm"]["value"]
    if not a > 0 or abs(a - b) / a > 1e-5:
        sys.exit(f"FAIL: CalculiX moved the bar's end {b} mm, in-house {a}")
    return f"a static study on it agrees with in-house to {abs(a - b) / a:.1e}"


def check_calculix_deck(launcher, ccx):
    """Run the ccx the app named on CUBE_DECK, through the same launcher the
    app was started with (wine, for a Windows path)."""
    work = tempfile.mkdtemp(prefix="lobole-ccx-smoke-")
    try:
        with open(os.path.join(work, "cube.inp"), "w") as f:
            f.write(CUBE_DECK)
        run = subprocess.run(launcher + [ccx["path"], "-i", "cube"], cwd=work,
                             capture_output=True, text=True, timeout=300)
        dat = os.path.join(work, "cube.dat")
        if run.returncode != 0 or "*ERROR" in run.stdout or \
                not os.path.exists(dat):
            sys.exit(f"FAIL: the bundled ccx did not run a deck "
                     f"({run.returncode}): {(run.stdout + run.stderr)[-800:]}")
        u = {}
        for line in open(dat, encoding="utf-8", errors="replace"):
            parts = line.split()
            if len(parts) == 4 and parts[0].isdigit():
                u[int(parts[0])] = [float(v) for v in parts[1:]]
        want = [-3e-4, -3e-4, 1e-3]
        got = u.get(7)
        if got is None or any(abs(g - w) > 1e-9 for g, w in zip(got, want)):
            sys.exit(f"FAIL: the bundled ccx moved the cube's far corner "
                     f"{got}, not {want}")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return "it solves a unit cube in tension exactly"


def check_calculix(port, launcher):
    c = Client(port)
    ping = c.call("ping")
    ccx = ping.get("features", {}).get("calculix")
    if not ccx:
        sys.exit("FAIL: ping finds no ccx; the package should bundle one")
    if not bundled_ccx(ccx["path"]):
        sys.exit(f"FAIL: ping finds {ccx['path']}, not the ccx shipped "
                 "beside the app")
    if ping.get("features", {}).get("mesher"):
        how = check_calculix_study(c, ccx)
    else:
        how = check_calculix_deck(launcher, ccx)
    return f"finds the bundled ccx {ccx['version']} ({ccx['path']}); {how}"


def start(cmd, port, env=None):
    return subprocess.Popen(cmd + ["--headless", "--remote", str(port)],
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, env=env)


def stop(proc):
    proc.terminate()
    try:
        proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        proc.kill()


def local_path(path):
    """A path the app reported, as this machine can open it: Wine's are
    Windows paths, which `winepath -u` maps into the prefix."""
    if os.path.exists(path):
        return path
    out = subprocess.run(["winepath", "-u", path], capture_output=True,
                         text=True)
    return out.stdout.strip()


def check_system_faults(cmd, port, timeout):
    env = dict(os.environ, LOBOLE_ALLOW_DEBUG_CRASH="1")
    proc = start(cmd, port, env)
    try:
        wait_for_ping(port, proc, timeout)
        c = Client(port)
        before = len(c.call("crash_reports")["reports"])
        probed = c.call("debug_crash", kind="system_probe")
        if probed.get("unreadable") is not True:
            sys.exit(f"FAIL: IsBadReadPtr on an unmapped address answered "
                     f"{probed}, not unreadable")
        if not c.call("ping").get("ok"):
            sys.exit("FAIL: the app did not answer after a fault the system "
                     "caught itself")
        try:
            Client(port, timeout=60).call("debug_crash", kind="system_fault")
        except Exception:
            pass  # no answer: that is the point
        try:
            proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            sys.exit("FAIL: a fault in msvcrt's memcpy on a worker thread "
                     "did not end the app")
    finally:
        if proc.poll() is None:
            stop(proc)

    proc = start(cmd, port, env)
    try:
        wait_for_ping(port, proc, timeout)
        reports = Client(port).call("crash_reports")["reports"]
        if len(reports) != before + 1:
            sys.exit(f"FAIL: {len(reports) - before} new crash reports, not "
                     "one: the fault the system caught left one, or the "
                     "one it did not left none")
        r = reports[0]
        if not r["crashed"] or "0xC0000005" not in r["reason"]:
            sys.exit(f"FAIL: a fault in msvcrt's memcpy on a worker thread "
                     f"was not reported as one: {r['reason']!r}")
        text = open(os.path.join(local_path(r["path"]), "report.txt"),
                    encoding="utf-8", errors="replace").read()
        frames = re.findall(r"^\s*#(\d+) (.+?)\+0x", text, re.M)
        system = [f for f in frames if "system32" in f[1].lower()]
        ours = [f for f in frames if f[1].lower().endswith("lobole.exe")]
        if (not frames or frames[0] not in system or not ours or
                int(ours[0][0]) < int(system[0][0])):
            sys.exit("FAIL: the report's frames are not the system DLL's "
                     f"and then lobole.exe's:\n{text}")
    finally:
        stop(proc)
    print(f"release-smoke: a fault the system catches itself left no report; "
          f"one it does not, on a worker thread, was reported "
          f"(#0 {frames[0][1]}, #{ours[0][0]} {ours[0][1]})")


def main():
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--port", type=int, default=8437)
    ap.add_argument("--version", help="the version CMD --version must name")
    ap.add_argument("--attach", action="store_true",
                    help="drive an instance already serving on --port")
    ap.add_argument("--timeout", type=float, default=180,
                    help="seconds to wait for the first answer (Wine is slow "
                         "to start)")
    ap.add_argument("--system-faults", action="store_true",
                    help="check faults in system code on a worker thread "
                         "(Windows) instead of modeling")
    ap.add_argument("--calculix", action="store_true",
                    help="the package bundles ccx: ping must find that copy, "
                         "and it must solve")
    ap.add_argument("cmd", nargs="*", help="the app's command line")
    args = ap.parse_args()
    if args.attach == bool(args.cmd):
        ap.error("give either a command after -- or --attach")

    if args.version and args.cmd:
        out = subprocess.run(args.cmd + ["--version"], capture_output=True,
                             text=True, timeout=args.timeout)
        text = out.stdout + out.stderr
        if args.version not in text.split():
            sys.exit(f"FAIL: `--version` says {text.strip()!r}, not "
                     f"{args.version}")

    if args.system_faults:
        if not args.cmd:
            ap.error("--system-faults starts the app: give its command line")
        check_system_faults(args.cmd, args.port, args.timeout)
        return

    proc = None
    if args.cmd:
        proc = start(args.cmd, args.port)
    try:
        ping = wait_for_ping(args.port, proc, args.timeout)
        if ping.get("app") != "lobole":
            sys.exit(f"FAIL: port {args.port} is not Loböle: {ping}")
        check_models(args.port)
        solver = (check_calculix(args.port, args.cmd[:-1])
                  if args.calculix else None)
    finally:
        if proc is not None:
            stop(proc)
    print(f"release-smoke: {' '.join(args.cmd) or f'port {args.port}'} "
          f"answers, models a box and solves a sketch")
    if solver:
        print(f"release-smoke: CalculiX: {solver}")


if __name__ == "__main__":
    main()
