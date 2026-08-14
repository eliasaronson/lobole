#!/usr/bin/env python3
"""Scale round, part 2: a many-occurrence assembly and a many-view drawing.

Assemblies and drawings have only ever been dogfooded at toy size (two parts,
one view). This builds a bolted-plate assembly with dozens of occurrences and
mates, then a drawing of it, timing what a user waits on.

    python3 scratch/scale_assembly.py <port> [occurrences]
"""
import sys
import os
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402


def timed(label, fn):
    t0 = time.time()
    result = fn()
    dt = time.time() - t0
    print(f"  {label:<44} {dt*1000:8.0f} ms", flush=True)
    return result, dt


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8952
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 50
    d = Designer(port, timeout=900.0)
    d.call("new_project")

    # Two parts to instance: a plate and a bolt.
    d.call("add_feature", feature={
        "type": "Box", "name": "Plate", "position": [0, 0, 0],
        "width": 60, "height": 8, "depth": 40})
    d.call("add_part", name="Bolt")
    d.call("set_active_part", index=1)
    d.call("add_feature", feature={
        "type": "Cylinder", "name": "Shank", "center": [0, 0, 0],
        "axis": [0, 1, 0], "radius": 3, "height": 20})
    parts = d.call("list_parts")["parts"]
    plate_id = parts[0]["id"]
    bolt_id = parts[1]["id"]

    d.call("add_assembly", name="Rig")
    asm = len(d.call("list_parts")["parts"]) - 1
    d.call("set_active_part", index=asm)

    print(f"inserting {count} occurrences")
    curve = []
    for i in range(count):
        pid = plate_id if i % 5 == 0 else bolt_id
        t0 = time.time()
        d.call("add_occurrence", part=pid,
               position=[(i % 10) * 70.0, 0.0, (i // 10) * 50.0])
        curve.append((i + 1, time.time() - t0))
    print("  growth: cost of one add_occurrence, by occurrence count")
    step = max(1, len(curve) // 8)
    for i in range(0, len(curve), step):
        n, dt = curve[i]
        print(f"    at {n:3} occurrences: {dt*1000:7.1f} ms  {'#' * int(dt * 200)}")

    print("\nthe operations a user waits on (assembly)")
    _, _ = timed("list_occurrences", lambda: d.call("list_occurrences"))
    _, _ = timed("solve_assembly", lambda: d.call("solve_assembly"))
    _, _ = timed("bom", lambda: d.call("bom"))
    _, _ = timed("mass_properties", lambda: d.call("mass_properties"))
    _, _ = timed("interference (all pairs)", lambda: d.call("interference"))
    _, _ = timed("auto_explode", lambda: d.call("auto_explode"))
    out = "/tmp/scale-asm.dsgn"
    _, _ = timed("save_project", lambda: d.call("save_project", path=out))
    _, _ = timed("open_project", lambda: d.call("open_project", path=out))

    print("\nthe operations a user waits on (drawing of that assembly)")
    d.call("create_drawing", name="Sheet", size="A2")
    dwg = len(d.call("list_parts")["parts"]) - 1
    d.call("set_active_part", index=dwg)
    src = d.call("list_parts")["parts"][asm]["id"]
    views = []
    for i, ori in enumerate(["front", "top", "right", "iso"] * 3):
        t0 = time.time()
        v = d.call("add_drawing_view", source=src, orientation=ori,
                   position=[40 + (i % 4) * 130, 380 - (i // 4) * 130],
                   scale=0.25)
        views.append((i + 1, time.time() - t0, v))
    print("  growth: cost of one add_drawing_view (HLR projection)")
    for n, dt, _ in views:
        print(f"    view {n:2}: {dt*1000:7.0f} ms  {'#' * int(dt * 20)}")
    _, _ = timed("update_drawing (re-project every view)",
                 lambda: d.call("update_drawing"))
    _, _ = timed("list_drawing_geometry (one view)",
                 lambda: d.call("list_drawing_geometry",
                                view=views[0][2]["id"]))
    _, _ = timed("export_drawing SVG", lambda: d.call(
        "export_drawing", path="/tmp/scale-sheet.svg"))
    _, _ = timed("export_drawing PDF", lambda: d.call(
        "export_drawing", path="/tmp/scale-sheet.pdf"))
    for f in ("/tmp/scale-sheet.svg", "/tmp/scale-sheet.pdf"):
        print(f"  {os.path.basename(f)}: {os.path.getsize(f)/1024:.0f} KiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
