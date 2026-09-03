#!/usr/bin/env python3
"""A gear train: two generator gears on a carrier, coupled by a relation.

    python3 generators/gear_train.py [--port N] [--save PATH]

The pinion (12 teeth) and wheel (36 teeth), module 2, so the centre distance
is m (z1 + z2) / 2 = 48. Each gear's bore is mated (revolute) to a hole in
the carrier; a gear relation derived from the tooth counts couples the two
axles, phased by half a pitch on the wheel so the teeth interleave. Then
the pinion is turned through a tooth pitch and the interference check says
whether the teeth ever clash — the static tooth check that stands in for
"contact".
"""
import argparse
import math
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from _lib import connect  # noqa: E402

M, ZA, ZB, THICK, BORE = 2.0, 12, 36, 8.0, 6.0
CENTRE = M * (ZA + ZB) / 2.0  # 48


def cylinder_face(d, part, radius, near=None):
    """A cylindrical face of `part` with `radius`, nearest `near` (x, y)."""
    q = d.call("query_geometry", part=part, element_type="face",
               geometry_type="cylinder", radius=radius)
    best = None
    for f in q.get("matches", q.get("elements", [])):
        c = f.get("centroid", [0, 0, 0])
        dist = 0.0 if near is None else math.hypot(c[0] - near[0], c[1] - near[1])
        if best is None or dist < best[0]:
            best = (dist, f)
    if best is None:
        raise SystemExit(f"no cylindrical face of radius {radius} in {part}: {q}")
    f = best[1]
    return {"feature_id": f.get("feature_id", f.get("created_by")),
            "element": f["element"]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    ap.add_argument("--save")
    a = ap.parse_args()
    d = connect(a.port)
    port = a.port
    if port is None:
        from designer_remote import Designer
        port = Designer.instances()[0]["port"]
    d.call("new_project")

    # The carrier: a plate with two Ø6 holes 48 apart.
    d.call("add_part", name="carrier")
    d.call("add_feature", feature={
        "type": "Box", "name": "Plate", "position": [-20, -20, -5],
        "width": CENTRE + 40, "height": 40, "depth": 5})
    d.call("add_feature", feature={
        "type": "Sketch", "name": "Holes",
        "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                  "uAxis": [1, 0, 0]},
        "entities": [{"type": "point", "p": [0, 0]},
                     {"type": "point", "p": [CENTRE, 0]}]})
    holes_sk = d.call("get_state")["features"][-1]["id"]
    d.call("add_feature", feature={
        "type": "Hole", "name": "Axle holes", "sketch_id": holes_sk,
        "hole_kind": "simple", "diameter": BORE, "depth": 5.0,
        "through": True})

    # The gears, each in its own part, from the generator.
    gear = os.path.join(ROOT, "generators", "gear.py")
    for name, z in (("pinion", ZA), ("wheel", ZB)):
        d.call("add_part", name=name)
        d.call("set_active_part", name=name)
        subprocess.run([sys.executable, gear, "--port", str(port),
                        "--module", str(M), "--teeth", str(z),
                        "--thickness", str(THICK), "--bore", str(BORE),
                        "--name", name], check=True)

    # The assembly: carrier fixed, each gear's bore on its hole.
    hole_a = cylinder_face(d, "carrier", BORE / 2, (0, 0))
    hole_b = cylinder_face(d, "carrier", BORE / 2, (CENTRE, 0))
    bore_a = cylinder_face(d, "pinion", BORE / 2)
    bore_b = cylinder_face(d, "wheel", BORE / 2)
    d.call("add_assembly", name="train")
    carrier = d.call("add_occurrence", part="carrier", fixed=True)["id"]
    pinion = d.call("add_occurrence", part="pinion")["id"]
    wheel = d.call("add_occurrence", part="wheel")["id"]
    ma = d.call("add_mate", type="revolute", name="pinion axle",
                a={"occurrence": carrier, "face": hole_a},
                b={"occurrence": pinion, "face": bore_a})["id"]
    mb = d.call("add_mate", type="revolute", name="wheel axle",
                a={"occurrence": carrier, "face": hole_b},
                b={"occurrence": wheel, "face": bore_b})["id"]
    rel = d.call("add_relation", type="gear", a=ma, b=mb, teeth_a=ZA,
                 teeth_b=ZB, phase=math.pi / ZB, name=f"{ZA}:{ZB}")
    print("relation:", rel["kind"], "ratio %.4f" % rel["ratio"],
          "phase %.4f" % rel["phase"], rel.get("error", ""))
    for m in d.call("list_mates")["mates"]:
        print("mate", m["name"], "error:", m.get("error", "-"))

    # Turn the pinion through one tooth pitch and watch the teeth.
    worst = 0.0
    pitch = 2 * math.pi / ZA
    for i in range(9):
        theta = pitch * i / 8
        d.call("set_mate", id=ma, rotation=theta)
        wheel_rot = next(m for m in d.call("list_mates")["mates"]
                         if m["id"] == mb)["rotation"]
        r = d.call("interference", a=pinion, b=wheel)
        print("pinion %.3f rad -> wheel %.4f rad: %s, clearance %.3f mm, "
              "clash volume %.3f" % (theta, wheel_rot, r["state"],
                                     r.get("distance", 0.0),
                                     r.get("volume", 0.0)))
        worst = max(worst, r.get("volume", 0.0))
    print("worst clash volume over a pitch: %.4f mm3" % worst)
    if a.save:
        d.call("save_project", path=a.save)
        print("saved", a.save)
    d.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
