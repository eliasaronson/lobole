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
import subprocess
import sys
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
    finally:
        if proc is not None:
            stop(proc)
    print(f"release-smoke: {' '.join(args.cmd) or f'port {args.port}'} "
          f"answers, models a box and solves a sketch")


if __name__ == "__main__":
    main()
