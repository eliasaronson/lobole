"""What every stage of the pan/tilt head shares: where its output goes, the
connection, and the few helpers that are several RPCs."""
import argparse
import json
import math
import os
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
# What the run makes -- projects, renders, reports -- is regenerable, so it
# goes to the untracked scratch/, not beside the code.
# DESIGNER_SCRATCH moves it (tests/generator_runs.py runs it in a private
# directory); unset, the repo's scratch/.
OUT = Path(os.environ.get("DESIGNER_SCRATCH")
           or HERE.parent.parent / "scratch") / "pantilt"
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(HERE.parent.parent / "tools"))
sys.path.insert(0, str(HERE))
from designer_sdk import connect, Plane, Param  # noqa: E402,F401

PRINT = {"process": "fdm", "material": "petg"}


def uv_on(face, point):
    """Where a world point lands in the frame a primitive standing on
    `face` is drawn in (origin the face's centroid, u its frame's X):
    `at=uv_on(face, p)`. The face's X is the kernel's to choose, so it is
    asked, not assumed."""
    f = face.frame()
    d = [p - o for p, o in zip(point, f.origin)]
    return (sum(a * b for a, b in zip(d, f.x)),
            sum(a * b for a, b in zip(d, f.y)))


def args():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    return ap.parse_args()


class Clock:
    """Wall time per step, for the friction list: where the run waits."""

    def __init__(self):
        self.t0 = time.monotonic()
        self.last = self.t0
        self.rows = []

    def __call__(self, what):
        now = time.monotonic()
        self.rows.append((what, now - self.last))
        print(f"  [{now - self.t0:7.1f} s] {what} ({now - self.last:.1f} s)",
              flush=True)
        self.last = now


def printed(part):
    """Declare a part printed in PETG (set_print_process) and give it the
    material; returns a function mapping a size that must PRINT to the size
    to MODEL, asked of the app's allowance table (never typed here)."""
    part.activate()
    part.app.set_print_process(**PRINT)
    cache = {}

    def model(kind, size):
        key = (kind, round(float(size), 4))
        if key not in cache:
            cache[key] = part.app.print_allowance(
                kind=kind, size_mm=float(size))["model_mm"]
        return cache[key]
    return model


def frame(part, name, origin, z, x):
    """An explicit named frame: what a mate anchors to by name."""
    return part.frame(name, origin=tuple(float(c) for c in origin),
                      z=tuple(z), x=tuple(x))


def save(app, name):
    path = OUT / name
    app.save_project(path=str(path))
    print("saved", path)


def check_regen(app, what):
    st = app.call("get_state")
    if not st["regen"]["ok"]:
        raise SystemExit(f"REGEN FAILED in {what}: {json.dumps(st['regen'])}")
    return st


def part_named(app, name):
    for p in app.call("list_parts")["parts"]:
        if p["name"] == name:
            return p
    raise SystemExit(f"no part {name!r}")


def drop_default_part(app):
    """The empty part a new project opens with is not the head's."""
    for p in app.call("list_parts")["parts"]:
        if p["name"] == "Part 1":
            app.call("delete_part", part=p["id"])


def bolt_points(radius, n=4, start_deg=45.0):
    """`n` points on a circle, the first `start_deg` off +X."""
    return [(radius * math.cos(math.radians(start_deg + 360.0 * i / n)),
             radius * math.sin(math.radians(start_deg + 360.0 * i / n)))
            for i in range(n)]
