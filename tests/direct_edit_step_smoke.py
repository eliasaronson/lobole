#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The direct edits on real STEP files (docs/modeling/README.md#direct-edit):
every face of each file deleted alone, and offset out and in at its derived
default. Each edit either passes its own checks and is kept, or is REFUSED
with a face named and the document unchanged -- never stored as a broken
feature, never a refusal that names nothing, never a write before one.

The files are tests/fixtures/step's (LICENSES.txt there); more can be named
on the command line, which is how parts whose licence keeps them out of the
tree are run by hand.

Usage: direct_edit_step_smoke.py <path-to-designer-binary> [file.step ...]
"""
import math
import os
import sys
import time

from lobole.client import Client  # noqa: E402
from lobole.errors import Refused  # noqa: E402
from smoke_app import launch_app  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = [os.path.join(HERE, "fixtures", "step", f) for f in (
    "topup_ball_knob_axial_hole_rev_orig.step",
    "battery_holder_box_y_f130_orig.step",
    "ball_knob_cylinder_radius_f130_orig.step",
)]


def round125(raw):
    """1, 2 or 5 x 10^n at or below raw: how the app rounds a default."""
    decade = 10.0 ** math.floor(math.log10(raw))
    mant = raw / decade
    return decade * (5.0 if mant >= 5.0 else 2.0 if mant >= 2.0 else 1.0)


def version(d):
    return d.call("get_state")["revision"]["version"]


def run_file(d, path, tally):
    d.call("new_project")
    d.call("import_step", path=path, keep_origin=True)
    els = d.call("list_elements", max_faces=1000000)
    bodies = d.call("mass_properties")["bodies"]
    for b in els["bodies"]:
        cb = b["created_by"]
        mp = bodies[b["body"]]
        ext = [h - l for h, l in zip(mp["bbox_max"], mp["bbox_min"])]
        least = min(ext)
        for f in b["faces"]:
            ref = {"feature_id": cb, "element": f["name"]}
            reach = (f.get("inset_mm") if f.get("kind") == "plane" else
                     f.get("radius") if f.get("kind") == "cylinder" else
                     least)
            edits = [("delete", {"type": "DeleteFace", "name": "S",
                                 "body_id": cb, "faces": [ref]})]
            if reach and reach > 0:
                dist = round125(0.1 * min(reach, least))
                for sign, what in ((1, "offset out"), (-1, "offset in")):
                    edits.append((what, {
                        "type": "OffsetFace", "name": "S", "body_id": cb,
                        "faces": [ref], "distance_mm": sign * dist}))
            for what, feat in edits:
                before = version(d)
                label = f"{os.path.basename(path)} {f['name']} {what}"
                try:
                    r = d.call("add_feature", feature=feat)
                except Refused as e:
                    msg = str(e)
                    # A refusal names the face it is about, and changed
                    # nothing.
                    assert "`" in msg or f["name"] in msg, \
                        f"{label}: refused naming no face: {msg}"
                    assert version(d) == before, \
                        f"{label}: the refusal moved the revision"
                    tally["refused"] += 1
                    continue
                regen = r.get("regen", {})
                assert regen.get("ok", False) and not regen.get("failed"), \
                    f"{label}: accepted and broken: {regen}"
                tally["kept"] += 1
                d.call("undo")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    files = FIXTURES + sys.argv[2:]
    app = launch_app(binary)
    try:
        d = Client.wait_for(app.port)
        for path in files:
            tally = {"kept": 0, "refused": 0}
            t = time.time()
            run_file(d, path, tally)
            print(f"direct_edit_step_smoke: {os.path.basename(path)}: "
                  f"{tally['kept']} kept, {tally['refused']} refused "
                  f"({time.time() - t:.1f} s)")
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()
    print("direct_edit_step_smoke: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
