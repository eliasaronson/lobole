#!/usr/bin/env python3
"""A gear train: three generator gears on a carrier, coupled by relations,
and a rack on a rail.

    python3 generators/gear_train.py [--port N] [--save PATH]

pinion z12 -> idler z24 -> wheel z36, module 2, so the centre distances are
36 and 60 mm. Each gear's bore is mated (revolute) to a hole in the carrier
and seated ON the plate by the mate's axial offset (derived from the two
thicknesses). Two gear relations chain the axles, the ratios derived from
the tooth counts and the idler phased by half a pitch so the teeth
interleave; a rack relation slides a bar along a rail block by the wheel's
pitch radius per radian. Then the pinion is turned through a tooth pitch
and the interference check says whether the teeth ever clash — the static
tooth check that stands in for "contact".

What this is: docs/assemblies/relations-plan.md, the demo.
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

M, ZA, ZB, THICK, BORE, PLATE = 2.0, 12, 36, 8.0, 6.0, 5.0
BACKLASH = 0.15  # mm of extra centre distance per mesh


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


def planar_face(d, part, normal, near=None):
    """A planar face of `part` facing `normal`, nearest `near` (x, y, z)."""
    q = d.call("query_geometry", part=part, element_type="face",
               geometry_type="plane", normal=list(normal), angle_tol_deg=1.0)
    best = None
    for f in q["matches"]:
        c = f["centroid"]
        dist = 0.0 if near is None else math.dist(c, near)
        if best is None or dist < best[0]:
            best = (dist, f)
    if best is None:
        raise SystemExit(f"no planar face facing {normal} in {part}: {q}")
    f = best[1]
    return {"feature_id": f["created_by"], "element": f["element"]}


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

    # The carrier: a plate with three axle holes, and a rail block whose +X
    # face carries the rack (a slider mated face-to-face slides along the
    # faces' normal, which is what makes the rack go sideways and not up).
    # Centre distances m(z1+z2)/2 plus a backlash allowance: drawn with zero
    # backlash the flanks coincide and every checker is left deciding
    # whether a coincident face is a touch or a clash. 0.15 mm per mesh is
    # in the range a hobbed m2 pair is cut with.
    gears = (("pinion", ZA, 0.0), ("idler", 24, 36.0 + BACKLASH),
             ("wheel", ZB, 96.0 + 2 * BACKLASH))
    d.call("add_part", name="carrier")
    d.call("add_feature", feature={
        "type": "Box", "name": "Plate", "position": [-20, -50, -PLATE],
        "width": 136, "height": 100, "depth": PLATE})
    d.call("add_feature", feature={
        "type": "Sketch", "name": "Axle holes",
        "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                  "uAxis": [1, 0, 0]},
        "entities": [{"type": "point", "p": [x, 0]} for _, _, x in gears]})
    holes_sk = d.call("get_state")["features"][-1]["id"]
    d.call("add_feature", feature={
        "type": "Hole", "name": "Axles", "sketch_id": holes_sk,
        "hole_kind": "simple", "diameter": BORE, "depth": PLATE,
        "through": True})
    d.call("add_feature", feature={
        "type": "Box", "name": "Rail block", "position": [112, -47, 0],
        "width": 4, "height": 8, "depth": 8})
    d.call("set_material", feature_id=d.call("get_state")["features"][0]["id"],
           material="aluminum")

    gear = os.path.join(ROOT, "generators", "gear.py")
    for name, z, _ in gears:
        d.call("add_part", name=name)
        d.call("set_active_part", name=name)
        subprocess.run([sys.executable, gear, "--port", str(port),
                        "--module", str(M), "--teeth", str(z),
                        "--thickness", str(THICK), "--bore", str(BORE),
                        "--name", name], check=True)
        d.call("set_material",
               feature_id=d.call("get_state")["features"][1]["id"],
               material="steel")

    # The rack: a bar with no teeth (the relation is the test), whose -X end
    # face mates to the rail block.
    d.call("add_part", name="rack")
    d.call("add_feature", feature={
        "type": "Box", "name": "Bar", "position": [0, -4, -3],
        "width": 120, "height": 8, "depth": 6})

    d.call("add_assembly", name="train")
    carrier = d.call("add_occurrence", part="carrier", fixed=True)["id"]
    occ, mates = {}, {}
    for name, z, x in gears:
        occ[name] = d.call("add_occurrence", part=name)["id"]
        mates[name] = d.call("add_mate", type="revolute", name=f"{name} axle",
                             a={"occurrence": carrier,
                                "face": cylinder_face(d, "carrier", BORE / 2,
                                                      (x, 0))},
                             b={"occurrence": occ[name],
                                "face": cylinder_face(d, name, BORE / 2)})["id"]
    # Seat each gear ON the plate: a face mate puts the bore's centre on the
    # hole's centre, half a plate down. The axial offset is derived from the
    # two thicknesses; its sign follows the hole frame's Z, which is measured
    # rather than assumed.
    seat = PLATE / 2 + THICK / 2
    for name in occ:
        d.call("set_mate", id=mates[name], offset=seat)
    z_now = next(o for o in d.call("list_occurrences")["occurrences"]
                 if o["name"] == "pinion")["transform"]["position"][2]
    if z_now < -1e-6:
        for name in occ:
            d.call("set_mate", id=mates[name], offset=-seat)
    rack = d.call("add_occurrence", part="rack")["id"]
    slider = d.call("add_mate", type="slider", name="rack slide",
                    a={"occurrence": carrier,
                       "face": planar_face(d, "carrier", (1, 0, 0),
                                           (116, -43, 4))},
                    b={"occurrence": rack,
                       "face": planar_face(d, "rack", (-1, 0, 0))})["id"]

    # Phases: tooth 0 of every gear points +x. A pair meshes when one has a
    # tooth and the other a gap on the line between them.
    rels = [
        d.call("add_relation", type="gear", a=mates["pinion"], b=mates["idler"],
               teeth_a=ZA, teeth_b=24, phase=math.pi / 24, name="12:24"),
        d.call("add_relation", type="gear", a=mates["idler"], b=mates["wheel"],
               teeth_a=24, teeth_b=ZB, phase=0.0, name="24:36"),
        d.call("add_relation", type="rack", a=mates["wheel"], b=slider,
               pitch_radius=M * ZB / 2, name="wheel rack"),
    ]
    for r in rels:
        print("relation", r["name"], "ratio %.4f phase %.4f" % (r["ratio"],
                                                                r["phase"]),
              r.get("error", ""))

    # Turn the pinion through one tooth pitch and watch every pair.
    worst = 0.0
    pitch = 2 * math.pi / ZA
    for i in range(9):
        theta = pitch * i / 8
        d.call("set_mate", id=mates["pinion"], rotation=theta)
        pairs = []
        for x, y in (("pinion", "idler"), ("idler", "wheel")):
            r = d.call("interference", a=occ[x], b=occ[y])
            worst = max(worst, r.get("volume", 0.0))
            pairs.append("%s-%s %s %.3f" % (x, y, r["state"],
                                             r.get("volume", 0.0)))
        rack_x = next(o for o in d.call("list_occurrences")["occurrences"]
                      if o["name"] == "rack")["transform"]["position"][0]
        print("pinion %.3f rad: %s; rack x %.2f" % (theta, ", ".join(pairs),
                                                     rack_x))
    print("worst clash volume over a pitch: %.4f mm3" % worst)
    # The whole assembly, not just the pairs this script thought to ask
    # about: the first version of it seated every gear INSIDE the plate and
    # only measured gear against gear. A clash here is a mate saying
    # something other than what was meant.
    rep = d.call("contact_report")
    clashes = [(c["a"], c["b"]) for c in rep.get("clashes", [])]
    print("contact report: %d component(s), clashes: %s" % (
        len(rep.get("components", [])), clashes or "none"))
    d.call("set_mate", id=mates["pinion"], rotation=pitch / 4)
    if a.save:
        d.call("save_project", path=a.save)
        print("saved", a.save)
    d.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
