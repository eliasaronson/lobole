#!/usr/bin/env python3
"""Stage 1: the spec, the base plate and the pan wheel. Saves base.dsgn.

    python3 generators/pantilt/build_base.py --port N

The base plate is aluminium (cut on the 6090 in phase 7): a disc that stays
inside the pan worm's swing, the lower pan bearing in a pocket in its top,
the 3/8-16 tripod thread tapped up into its underside, and four M4 tapped
holes for the wheel. The pan wheel is printed and does not turn: it is
screwed to the plate, carries the upper pan bearing in a counterbore, and
the yoke's worm walks round it.
"""
import common
from common import Plane, Param
import spec

SCREW = "M4"  # one screw size across the head, one hex key


def base_plate(app, S):
    part = app.part("base_plate")
    part.activate()
    P = {}
    for name, expr, comment in [
        ("base_r", "centre_dist - worm_tip_d / 2 - run_gap",
         "mm, inside the pan worm's swing"),
        ("tripod_len", "0.375 in", "mm, 3/8-16 engaged one diameter deep"),
        ("tripod_pitch", "0.0625 in", "mm, 16 TPI"),
        ("base_t", "tripod_len + 2 * tripod_pitch + pan_brg_B / 3 + "
                   "pan_brg_B", "mm: tap drill, a web, the bearing pocket"),
        ("screw_pcd", "base_r + pan_brg_D / 2",
         "mm, the wheel's screws, midway between pocket and rim"),
        ("edge_break", "0.5", "mm, the chamfer on every machined rim: "
         "modelled, so the CAM's chamfers are the part's"),
    ]:
        P[name] = part.param(name, expr, comment=comment, scope="project")
    S.update(spec.values(part))
    body = part.cylinder("Top", r=P["base_r"], height=P["base_t"],
                         name="Plate")
    t = S["base_t"]
    sk = part.sketch(Plane.xy(t), name="Bearing pocket")
    sk.circle((0, 0), d=Param(S["pan_brg_D"], "pan_brg_D"))
    part.cut(sk.add(), Param(S["pan_brg_B"], "pan_brg_B"), reversed=True,
             name="Bearing pocket")
    sk = part.sketch(Plane.xy(t), name="Relief")
    sk.circle((0, 0), d=Param(S["pan_relief_d"], "pan_relief_d"))
    part.cut(sk.add(), Param(S["pan_brg_B"] + S["run_gap"],
                             "pan_brg_B + run_gap"),
             reversed=True, name="Relief")
    # The tripod thread, tapped up from the underside.
    sk = part.sketch(Plane.xy(0, flip=True), name="Tripod")
    sk.point((0, 0))
    tri = part.hole(sk.add(), d=1.0, depth=P["tripod_len"] +
                    P["tripod_pitch"] * 2,
                    fit={"thread": "3/8-16 UNC", "kind": "tapped"},
                    # Bored flat with the 6 mm end mill on the 6090, not
                    # drilled (cam.py): a drill's point here would break
                    # through the web into the relief.
                    drill_point_deg=180.0,
                    name="Tripod hole")
    bore = [f for f in tri.bores()][0]
    part.thread(bore, "3/8-16 UNC-2B")
    sk = part.sketch(Plane.xy(t), name="Wheel screws")
    for x, y in common.bolt_points(S["screw_pcd"] / 2):
        sk.point((x, y))
    part.hole(sk.add(), d=1.0, depth=8.0,
              fit={"thread": SCREW, "kind": "tapped"}, name="Wheel screws")
    # The edges the 6090 breaks (generators/pantilt/cam.py), in the model:
    # a chamfer the model does not have is a gouge to the CAM check
    # (docs/cam/simulation.md#verdict). The rim top and bottom, the bearing
    # seat's mouth (a lead-in for the outer ring) and the tripod thread's.
    plate = part.bodies()[0]

    def rims(r, z):
        return plate.edges(circular=True, radius=r,
                           where=lambda e: abs(e.midpoint[2] - z) < 1e-3)
    tripod_r = bore.radius
    edges = (rims(S["base_r"], t) + rims(S["base_r"], 0.0) +
             rims(S["pan_brg_D"] / 2, t) + rims(tripod_r, 0.0))
    assert len(edges) >= 4, edges
    part.chamfer(edges, P["edge_break"], name="Edge breaks")
    # The grade it is cut from (cam.py's blank, the drawing's title block).
    part.material("aluminum_6082_t6")
    common.frame(part, "seat", (0, 0, t), (0, 0, 1), (1, 0, 0))
    part.check()
    return part


def pan_wheel(app, S):
    part = app.part("pan_wheel")
    model = common.printed(part)
    face = S["wheel_face"]
    blank = part.cylinder("Top", d="wheel_rim_d", height="wheel_face",
                          name="Blank")
    rim = blank.faces(kind="cylinder")[0]
    sk = part.sketch(Plane.xy(face), name="Bearing counterbore")
    sk.circle((0, 0), d=model("hole", S["pan_brg_D"]))
    part.cut(sk.add(), Param(S["pan_brg_B"], "pan_brg_B"), reversed=True,
             name="Bearing counterbore")
    sk = part.sketch(Plane.xy(face), name="Relief")
    sk.circle((0, 0), d=model("hole", S["pan_relief_d"]))
    part.cut(sk.add(), 1.0, reversed=True, end_mode="through_all",
             name="Relief")
    sk = part.sketch(Plane.xy(face), name="Screws")
    for x, y in common.bolt_points(S["screw_pcd"] / 2):
        sk.point((x, y))
    # ISO 4762 M4: head 7 across, 4 high; the counterbore clears it.
    part.hole(sk.add(), d=1.0, through=True, kind="counterbore",
              fit={"thread": SCREW, "kind": "normal"},
              cb_diameter=model("hole", 7.0 + 2 * S["nozzle"]), cb_depth=4.5,
              name="Screws")
    r = part.worm_wheel(rim, teeth=int(S["wheel_teeth"]),
                        starts=int(S["worm_starts"]), hand="right",
                        name="Pan wheel")
    print("pan wheel report:", {k: r.get("report", {}).get(k) for k in (
        "centre_distance", "ratio", "throat_diameter", "tip_crest",
        "hob_seconds")})
    part.material("petg")
    common.frame(part, "seat", (0, 0, 0), (0, 0, 1), (1, 0, 0))
    # The yoke turns on the wheel's top face: the upper bearing's outer side.
    common.frame(part, "top", (0, 0, face), (0, 0, 1), (1, 0, 0))
    # The mesh reference: the blank's X (where a tooth stands) at mid-face.
    common.frame(part, "mesh", (0, 0, face / 2), (0, 0, 1), (1, 0, 0))
    part.check()
    return part, r


def main():
    a = common.args()
    app = common.connect(a.port)
    clock = common.Clock()
    app.new()
    first = app.part("base_plate")
    first.activate()
    S = spec.declare(app, first)
    clock("spec")
    base_plate(app, S)
    clock("base plate")
    pan_wheel(app, S)
    clock("pan wheel")
    common.drop_default_part(app)
    common.check_regen(app, "base")
    common.save(app, "base.dsgn")


if __name__ == "__main__":
    main()
