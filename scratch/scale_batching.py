#!/usr/bin/env python3
"""Where does the time in a big part actually go?

The scale round found that adding the 100th feature to a plate costs ~70x what
the 4th did. Two candidate explanations:

  (a) OCC boolean cost grows with the target solid's face count — inherent,
      and the answer is "batch your cuts";
  (b) per-FEATURE overhead (naming, caching, bookkeeping) dominates — fixable.

This cuts the same 480 holes two ways — 60 Hole features of 8 points, and one
Hole feature of 480 points — and compares. Same geometry, same boolean work,
wildly different feature counts.

    python3 scratch/scale_batching.py <port>
"""
import sys
import os
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402

ROWS, PER_ROW = 60, 8


def plate(d):
    d.call("new_project")
    d.call("add_feature", feature={
        "type": "Sketch", "name": "Plate",
        "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1], "uAxis": [1, 0, 0]},
        "entities": [
            {"type": "line", "a": [0, 0], "b": [400, 0]},
            {"type": "line", "a": [400, 0], "b": [400, 300]},
            {"type": "line", "a": [400, 300], "b": [0, 300]},
            {"type": "line", "a": [0, 300], "b": [0, 0]}]})
    sk = d.call("get_state")["features"][0]["id"]
    d.call("add_feature", feature={
        "type": "Extrude", "name": "Plate solid", "sketch_id": sk,
        "distance": 12.0, "mode": "new_body"})


def points(r):
    band, row = divmod(r, 9)
    y = 20.0 + row * 30.0
    x0 = 25.0 + band * 6.0
    return [{"type": "point", "p": [x0 + c * 45.0, y]} for c in range(PER_ROW)]


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8952
    d = Designer(port, timeout=900.0)

    print(f"A: {ROWS} Hole features x {PER_ROW} points")
    plate(d)
    t0 = time.time()
    for r in range(ROWS):
        sk = d.call("add_feature", feature={
            "type": "Sketch", "name": f"H{r}",
            "plane": {"origin": [0, 0, 12], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]}, "entities": points(r)})
        d.call("add_feature", feature={
            "type": "Hole", "name": f"B{r}", "sketch_id": sk["id"],
            "kind": "simple", "diameter": 6.0, "through": True})
    a_build = time.time() - t0
    st = d.call("get_state")
    a_features, a_vol = len(st["features"]), st["bodies"][0]["volume"]
    ext = st["features"][1]
    t0 = time.time()
    d.call("modify_feature", index=1, feature=dict(ext, distance=12.5))
    a_regen = time.time() - t0
    print(f"   {a_features} features, volume {a_vol:.0f}")
    print(f"   build {a_build:.1f} s | full rebuild {a_regen:.1f} s")

    print(f"\nB: 1 Hole feature x {ROWS * PER_ROW} points (same geometry)")
    plate(d)
    allpts = [p for r in range(ROWS) for p in points(r)]
    t0 = time.time()
    sk = d.call("add_feature", feature={
        "type": "Sketch", "name": "All holes",
        "plane": {"origin": [0, 0, 12], "normal": [0, 0, 1],
                  "uAxis": [1, 0, 0]}, "entities": allpts})
    d.call("add_feature", feature={
        "type": "Hole", "name": "All bolts", "sketch_id": sk["id"],
        "kind": "simple", "diameter": 6.0, "through": True})
    b_build = time.time() - t0
    st = d.call("get_state")
    b_features, b_vol = len(st["features"]), st["bodies"][0]["volume"]
    ext = st["features"][1]
    t0 = time.time()
    d.call("modify_feature", index=1, feature=dict(ext, distance=12.5))
    b_regen = time.time() - t0
    print(f"   {b_features} features, volume {b_vol:.0f}")
    print(f"   build {b_build:.1f} s | full rebuild {b_regen:.1f} s")

    print(f"\nsame {ROWS * PER_ROW} holes, same material removed "
          f"({abs(a_vol - b_vol) < 1.0})")
    print(f"  build:   {a_build:.1f} s spread over {ROWS} features"
          f"  vs  {b_build:.1f} s in one  -> {a_build / max(b_build, 1e-6):.0f}x")
    print(f"  rebuild: {a_regen:.1f} s  vs  {b_regen:.1f} s"
          f"  -> {a_regen / max(b_regen, 1e-6):.0f}x")
    return 0


if __name__ == "__main__":
    sys.exit(main())
