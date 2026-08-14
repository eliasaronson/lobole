#!/usr/bin/env python3
"""Scale round: how does Designer behave on a part with hundreds of features?

Every dogfooding round so far used toy parts — five features, 2 mm boxes. This
builds a realistic mounting plate: a plate, a grid of bolt holes, pockets,
ribs and fillets, ~200 features, and times the operations a user actually
waits on.

    python3 scratch/scale_part.py <port> [feature-target]

Drive it against `build/designer --headless --remote <port>`.
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
    print(f"  {label:<44} {dt*1000:8.0f} ms")
    return result, dt


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8951
    target = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    d = Designer(port, timeout=600.0)
    d.call("new_project")

    print(f"building a ~{target}-feature plate")
    # The base plate: 400 x 300 x 12.
    d.call("add_feature", feature={
        "type": "Sketch", "name": "Plate",
        "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1], "uAxis": [1, 0, 0]},
        "entities": [
            {"type": "line", "a": [0, 0], "b": [400, 0]},
            {"type": "line", "a": [400, 0], "b": [400, 300]},
            {"type": "line", "a": [400, 300], "b": [0, 300]},
            {"type": "line", "a": [0, 300], "b": [0, 0]},
        ]})
    plate = d.call("get_state")["features"][0]["id"]
    d.call("add_feature", feature={
        "type": "Extrude", "name": "Plate solid", "sketch_id": plate,
        "distance": 12.0, "mode": "new_body"})

    # Bolt holes and pockets, added until the feature target OR a wall-clock
    # budget is spent — whichever comes first. The GROWTH CURVE is the real
    # question ("does the 100th feature cost more than the 10th?"), so every
    # addition is timed against the count at the time.
    curve = []          # (feature count after, seconds for this pair)
    budget = time.time() + 240.0
    made = 2
    r = 0
    while made < target and time.time() < budget:
        # A realistic grid: 8 holes per row, rows 30 mm apart, wrapping into a
        # second column band rather than piling holes on top of each other.
        band, row = divmod(r, 9)
        y = 20.0 + row * 30.0
        x0 = 25.0 + band * 6.0
        t0 = time.time()
        sk = d.call("add_feature", feature={
            "type": "Sketch", "name": f"Holes {r + 1}",
            "plane": {"origin": [0, 0, 12], "normal": [0, 0, 1],
                      "uAxis": [1, 0, 0]},
            "entities": [{"type": "point", "p": [x0 + c * 45.0, y]}
                         for c in range(8)]})
        d.call("add_feature", feature={
            "type": "Hole", "name": f"Bolts {r + 1}", "sketch_id": sk["id"],
            "kind": "counterbore", "diameter": 6.0, "through": True,
            "cb_diameter": 11.0, "cb_depth": 4.0})
        made += 2
        curve.append((made, time.time() - t0))
        r += 1

    st = d.call("get_state")
    n = len(st["features"])
    print(f"\n{n} features, {len(st['bodies'])} body, "
          f"volume {st['bodies'][0]['volume']:.0f} mm^3")
    if curve:
        print("\n  growth: cost of adding one sketch+hole pair, by feature count")
        step = max(1, len(curve) // 12)
        for i in range(0, len(curve), step):
            count, dt = curve[i]
            print(f"    at {count:4} features: {dt*1000:7.0f} ms  "
                  f"{'#' * int(dt * 20)}")
        print(f"    total build: {sum(c[1] for c in curve):.1f} s for "
              f"{len(curve)} pairs")

    print("\nthe operations a user waits on")
    # A real rebuild has to CHANGE something: re-posting identical JSON hashes
    # the same and the prefix cache correctly skips the work (4 ms, and not a
    # measurement of anything). Edit the plate's thickness — feature #2, which
    # everything downstream depends on — then edit the last feature, which
    # should cost only itself.
    ext = st["features"][1]
    _, t_full = timed("rebuild ALL (change the plate's thickness)",
                      lambda: d.call("modify_feature", index=1,
                                     feature=dict(ext, distance=12.5)))
    _, t_full_back = timed("  and back", lambda: d.call(
        "modify_feature", index=1, feature=dict(ext, distance=12.0)))
    last_idx = n - 1
    last = st["features"][last_idx]
    tail_edit = dict(last)
    if "distance" in tail_edit:
        tail_edit["distance"] = tail_edit["distance"] + 0.1
    elif "diameter" in tail_edit:
        tail_edit["diameter"] = tail_edit["diameter"] + 0.1
    _, t_tail = timed("edit the LAST feature (prefix cache should help)",
                      lambda: d.call("modify_feature", index=last_idx,
                                     feature=tail_edit))
    _, t_state = timed("get_state", lambda: d.call("get_state"))
    _, t_check = timed("check_geometry", lambda: d.call("check_geometry"))
    _, t_mass = timed("mass_properties", lambda: d.call("mass_properties"))
    _, t_elems = timed("list_elements (all faces/edges)",
                       lambda: d.call("list_elements", body=0))
    out = "/tmp/scale-part.dsgn"
    _, t_save = timed("save_project (directory format)",
                      lambda: d.call("save_project", path=out))
    _, t_open = timed("open_project", lambda: d.call("open_project", path=out))
    _, t_undo = timed("undo", lambda: d.call("undo"))
    _, t_stl = timed("export_stl", lambda: d.call(
        "export_stl", path="/tmp/scale-part.stl"))
    _, t_step = timed("export_step", lambda: d.call(
        "export_step", path="/tmp/scale-part.step"))

    files = sum(len(fs) for _, _, fs in os.walk(out))
    print(f"\nproject on disk: {files} files, "
          f"{sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(out) for f in fs) / 1024:.0f} KiB")
    print(f"STL {os.path.getsize('/tmp/scale-part.stl')/1024:.0f} KiB, "
          f"STEP {os.path.getsize('/tmp/scale-part.step')/1024:.0f} KiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
