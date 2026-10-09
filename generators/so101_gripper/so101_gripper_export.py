#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Write the deliverables: STLs to print, STEPs to open, renders to look at."""
import argparse
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)                          # this generator's own files
sys.path.insert(0, os.path.join(HERE, ".."))      # generators/: _lib, gear
import so101_gripper_spec as P                         # noqa: E402
from _lib import connect                               # noqa: E402
from so101_gripper_assembly import placements          # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                   "scratch", "so101-pg52-gripper")

# what gets printed, and how many
PRINTED = [("Chassis", 1), ("DrivePlate L", 2), ("DrivePlate R", 2),
           ("CouplerPlate", 4), ("Finger L", 1), ("Finger R", 1),
           ("Jaw Pad", 2), ("Camera Pod", 1)]


def pose(d, theta):
    d.call("set_active_part", part="Gripper")
    occ = {o["name"]: o["id"]
           for o in d.call("list_occurrences")["occurrences"]}
    calls = [{"method": "set_occurrence_transform",
              "params": {"id": occ[n], "transform": {"position": list(p),
                                                     "orientation": list(q)}}}
             for n, (p, q) in placements(theta).items() if n in occ]
    d.call("batch", calls=calls)
    return occ


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    a = ap.parse_args()
    d = connect(a.port)
    d.sock.settimeout(900)
    for sub in ("stl", "step", "renders"):
        os.makedirs(os.path.join(OUT, sub), exist_ok=True)

    for name, _ in PRINTED:
        d.call("set_active_part", part=name)
        slug = name.lower().replace(" ", "_")
        d.call("export_stl", path=f"{OUT}/stl/{slug}.stl", deflection=0.02)
        d.call("export_step", path=f"{OUT}/step/{slug}.step")
        print("  ", name)

    # the whole thing, at both ends of the stroke
    for label, theta in (("closed", P.THETA_CLOSED), ("open", P.THETA_OPEN)):
        pose(d, theta)
        d.call("export_step", path=f"{OUT}/step/gripper_{label}.step")
        d.call("render_sheet", path=f"{OUT}/renders/sheet_{label}.png",
               views=["iso", "front", "right", "top"])
        print("   assembly", label)

    # a hero render, jaws around a 30 mm block
    theta = P.theta_for_opening(30.0)
    pose(d, theta)
    d.call("set_camera", eye=[150, -190, 150], target=[0, 0, 55],
           up=[0, 0, 1])
    d.call("render_still", path=f"{OUT}/renders/hero.png", width=1600,
           height=1000, samples=256)
    print("   hero")
    d.call("save_project", path=f"{OUT}/so101-pg52.dproj", force=True,
           message="SO-101 PG-52 parallel gripper with wrist camera")
    print("saved to", OUT)
    d.close()


if __name__ == "__main__":
    main()
