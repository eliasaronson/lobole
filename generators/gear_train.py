#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

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

What this is: docs/assemblies/relations.md, the demo.
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


def box_on_face(d, face, centre, size_x, size_y):
    """A box's width, depth and offset on `face` (a face facing +Z) for a
    footprint `size_x` by `size_y` centred at the world point `centre`. The
    face's frame -- origin its centroid, u its X -- is asked of the app: a
    face's X is the kernel's to choose, so it is read, not assumed."""
    f = d.call("frame", **face)
    o, x, y = f["origin"], f["x"], f["y"]
    dv = [p - q for p, q in zip(centre, o)]
    along_x = abs(x[0]) > 0.5
    return {"width": size_x if along_x else size_y,
            "depth": size_y if along_x else size_x,
            "offset": [sum(a * b for a, b in zip(dv, x)),
                       sum(a * b for a, b in zip(dv, y))]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    ap.add_argument("--save")
    a = ap.parse_args()
    d = connect(a.port)
    port = a.port
    if port is None:
        from lobole.client import Client
        port = Client.instances()[0]["port"]
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
    # The plate hangs under Top (its top face is z = 0, where the gears
    # seat), so it grows reversed.
    plate = d.call("add_feature", feature={
        "type": "Box", "name": "Plate", "on": "Top", "reversed": True,
        "width": 136, "depth": 100, "height": PLATE, "offset": [48, 0]})["id"]
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
    # The rail block stands on the plate's top face.
    top = planar_face(d, "carrier", (0, 0, 1), (48, 0, 0))
    d.call("add_feature", feature={
        "type": "Box", "name": "Rail block", "on": top, "height": 8,
        **box_on_face(d, top, (114, -43, 0), 4, 8)})
    d.call("set_material", feature_id=plate, material="aluminum_6061_t6")

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
        "type": "Box", "name": "Bar", "on": "Right",
        "width": 8, "depth": 6, "height": 120})

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

    def gear_z(name):
        return next(o for o in d.call("list_occurrences")["occurrences"]
                    if o["name"] == name)["transform"]["position"][2]

    for name in occ:
        # PER GEAR, and then CHECKED. Each hole face carries its own frame,
        # and nothing makes their Z axes agree: measuring the pinion and
        # applying its sign to all three was an assumption about the other
        # two. It happens to hold on this build — the old code seats all
        # three at z = 0 — which is exactly why it needs an assertion rather
        # than a reading. A gear that misses its seat sits a whole `seat`
        # (6.5 mm) off the plate and looks perfectly reasonable in a render.
        d.call("set_mate", id=mates[name], offset=seat)
        if gear_z(name) < -1e-6:
            d.call("set_mate", id=mates[name], offset=-seat)
        if abs(gear_z(name)) > 1e-4:
            raise SystemExit(f"{name} did not seat on the plate: z = "
                             f"{gear_z(name):.3f}")
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
        d.call("set_mate", id=mates["pinion"], rotation_deg=math.degrees(theta))
        pairs = []
        for x, y in (("pinion", "idler"), ("idler", "wheel")):
            r = d.call("interference", a=occ[x], b=occ[y])
            worst = max(worst, r.get("volume_mm3", 0.0))
            pairs.append("%s-%s %s %.3f" % (x, y, r["state"],
                                             r.get("volume_mm3", 0.0)))
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
    d.call("set_mate", id=mates["pinion"], rotation_deg=math.degrees(pitch / 4))
    if a.save:
        d.call("save_project", path=a.save)
        print("saved", a.save)
    d.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
