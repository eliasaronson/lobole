#!/usr/bin/env python3
"""Integration test: V4 compare_geometry — boolean-diff similarity metrics,
golden-file regression compare, diff-solid export, occurrence compare.

Usage: compare_smoke.py <path-to-designer-binary>
"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402


def free_port():
    import socket
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]
    port = free_port()
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_compare_")
    env["XDG_STATE_HOME"] = tmp
    app = subprocess.Popen([binary, "--headless", "--remote", str(port)],
                           env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    try:
        d = Designer.wait_for(port)
        d.call("new_project")

        # Two 10-cubes overlapping by 8 in x: IoU 800/1200, symmetric
        # precision/recall, CoM offset exactly the 2 mm shift.
        d.call("add_feature", feature={
            "type": "Box", "name": "A", "position": [0, 0, 0],
            "width": 10, "height": 10, "depth": 10})
        d.call("add_feature", feature={
            "type": "Box", "name": "B", "position": [2, 0, 0],
            "width": 10, "height": 10, "depth": 10})
        r = d.call("compare_geometry", a=0, b=1)
        expect(abs(r["iou"] - 800.0 / 1200.0) < 1e-3, f"iou: {r['iou']}")
        expect(abs(r["precision"] - 0.8) < 1e-3, f"precision: {r}")
        expect(abs(r["recall"] - 0.8) < 1e-3, f"recall: {r}")
        expect(abs(r["volume_ratio"] - 1.0) < 1e-3, f"ratio: {r}")
        expect(abs(r["center_offset"] - 2.0) < 1e-3, f"offset: {r}")
        expect(abs(r["volume_missing"] - 200.0) < 1e-3, f"missing: {r}")
        expect(abs(r["volume_extra"] - 200.0) < 1e-3, f"extra: {r}")
        expect(any("partial match" in v for v in r["verdicts"]),
               f"verdicts: {r['verdicts']}")
        expect(any("offset by 2.0 mm" in v for v in r["verdicts"]),
               f"verdicts: {r['verdicts']}")

        # Diff solids export as BREP.
        miss = os.path.join(tmp, "missing.brep")
        extra = os.path.join(tmp, "extra.brep")
        r = d.call("compare_geometry", a=0, b=1, missing_path=miss,
                   extra_path=extra)
        expect(os.path.getsize(miss) > 100, "missing.brep written")
        expect(os.path.getsize(extra) > 100, "extra.brep written")

        # materialize: the delta becomes a drawable overlay on the part.
        # Headless has no renderer, so it reports that instead of claiming
        # something is on screen.
        r = d.call("compare_geometry", a=0, b=1, materialize=True)
        expect(r["materialized"] is False, f"headless materialize: {r}")
        expect("no renderer" in r.get("note", ""), f"note: {r}")
        expect(abs(r["volume_missing"] - 200.0) < 1e-3,
               f"materialize still measures: {r}")
        expect(d.call("clear_geometry_delta")["cleared"] is True, "cleared")
        # Clearing when there is nothing to clear is not an error.
        expect(d.call("clear_geometry_delta")["cleared"] is True, "re-cleared")

        # Golden-file regression: export the doc, compare against itself.
        golden = os.path.join(tmp, "golden.brep")
        d.call("export_brep", path=golden)
        r = d.call("compare_geometry", golden_path=golden)
        expect(r["iou"] > 0.999, f"golden iou: {r['iou']}")
        expect(any("identical" in v for v in r["verdicts"]),
               f"golden verdicts: {r['verdicts']}")

        # ...and a real deviation against the same golden: shrink box B.
        d.call("modify_feature", index=1, feature={
            "type": "Box", "name": "B", "position": [2, 0, 0],
            "width": 5, "height": 10, "depth": 10})
        r = d.call("compare_geometry", golden_path=golden)
        expect(r["iou"] < 0.999, f"deviation iou: {r['iou']}")
        expect(any("under-generating" in v or "undersized" in v
                   for v in r["verdicts"]), f"verdicts: {r['verdicts']}")

        # Occurrence compare on an assembly tab.
        part = d.call("list_parts")["parts"][0]["name"]
        d.call("add_assembly", name="Rig")
        d.call("set_active_part", name="Rig")
        for name, x in [("O1", 0), ("O2", 100)]:
            d.call("add_occurrence", part=part, name=name, fixed=True,
                   transform={"position": [x, 0, 0],
                              "orientation": [0, 0, 0, 1]})
        r = d.call("compare_geometry", a="O1", b="O2")
        expect(r["iou"] < 0.01, f"disjoint occurrences iou: {r['iou']}")
        expect(abs(r["center_offset"] - 100.0) < 1e-3,
               f"occurrence offset: {r['center_offset']}")
        expect(abs(r["volume_ratio"] - 1.0) < 1e-3, f"same part: {r}")

        # materialize is drawn in the active PART's space, so an assembly tab
        # refuses rather than putting the overlay somewhere plausible-looking.
        try:
            d.call("compare_geometry", a="O1", b="O2", materialize=True)
            raise AssertionError("materialize should refuse on an assembly tab")
        except RuntimeError as e:
            expect("part tab" in str(e), f"refusal wording: {e}")

        print("compare_smoke: all checks passed")
        return 0
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
