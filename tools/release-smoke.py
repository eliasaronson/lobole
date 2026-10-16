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
"""
import argparse
import math
import pathlib
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


def main():
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--port", type=int, default=8437)
    ap.add_argument("--version", help="the version CMD --version must name")
    ap.add_argument("--attach", action="store_true",
                    help="drive an instance already serving on --port")
    ap.add_argument("--timeout", type=float, default=180,
                    help="seconds to wait for the first answer (Wine is slow "
                         "to start)")
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

    proc = None
    if args.cmd:
        proc = subprocess.Popen(
            args.cmd + ["--headless", "--remote", str(args.port)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        ping = wait_for_ping(args.port, proc, args.timeout)
        if ping.get("app") != "lobole":
            sys.exit(f"FAIL: port {args.port} is not Loböle: {ping}")
        check_models(args.port)
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                proc.kill()
    print(f"release-smoke: {' '.join(args.cmd) or f'port {args.port}'} "
          f"answers, models a box and solves a sketch")


if __name__ == "__main__":
    main()
