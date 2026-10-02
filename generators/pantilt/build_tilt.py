#!/usr/bin/env python3
"""Stage 3: what turns about the tilt axis -- the tilt wheel on its shaft,
the cradle (the camera plate) and the camera. Opens worms.dsgn, saves
tilt.dsgn. The camera screw is bought, an ASME B18.3 standard part, and
the assembly places it (build_assembly.py).

    python3 generators/pantilt/build_tilt.py --port N

Each part is modelled about the tilt axis: the cradle and the camera in
tilt-axis coordinates (X forward, Z up, the axis along Y), the wheel along
its own Z. The camera is the LOAD, not a camera: a box and a lens of the
stated mass, its density the black plastic it is drawn in and its lens long
enough to make 1.5 kg, so its CoM lands 80 mm up and 40 mm forward of the
tilt axis when the cradle seats it -- the cradle's seat is placed from the
camera's own CoM, not the other way round.
"""
import common
from common import Plane, Param
import spec


def declare(part, S):
    # The camera's own sizes are spec.py's (CAMERA): how far it swings
    # under the tilt axis sizes the pan journal, in the first stage.
    for name, expr, comment in [
        # cam_socket, the screw's engagement and its length are spec.py's:
        # the cradle's plate is sized by them.
        ("cam_screw_clear", "0.266 in", "mm, ASME close clearance for 1/4"),
        # The cradle and the arms it turns between.
        ("cheek_t", "2 * print_wall", "mm"),
        ("boss_r", "main_brg_d / 2 + 2 * print_wall", "mm, round the shaft"),
        ("slot_l", "com_fwd / 2", "mm, the camera slides +-com_fwd/4 to "
                                  "balance"),
        ("main_collar_d", "(main_brg_da_min + main_brg_da_max) / 2",
         "mm, bears on the inner ring only: mid-way in SKF's d_a"),
        ("relief_d", "(main_brg_da_max + main_brg_Da_max) / 2",
         "mm, the arms' shoulders round the tilt bearings' outer rings: "
         "inside D_a max, clear of the inner ring (d_a max) so it turns "
         "free"),
        ("arm_t", "main_brg_B + print_wall", "mm, a bearing and a shoulder"),
        ("arm_in", "cam_w / 2 + run_gap", "mm, the arms clear the camera"),
        ("arm_out", "arm_in + arm_t", "mm, the arms' outer faces"),
        ("tilt_mesh_y", "max(arm_out + run_gap + wheel_face / 2, "
                        "arm_in + block_w / 2 + run_gap)",
         "mm, the tilt wheel's mid-plane and the tilt worm's axis: out far "
         "enough that the worm's blocks clear the camera between the arms"),
        ("tilt_collar", "tilt_mesh_y - wheel_face / 2 - arm_out",
         "mm, the wheel's collar out from the arm's bearing"),
    ]:
        part.param(name, expr, comment=comment, scope="project")
    S.update(spec.values(part))


def tilt_wheel(app, S):
    """The wheel and its shaft, one print, along part Z: the wheel's
    inboard face at z = 0, the shaft down through both arms."""
    part = app.part("tilt_wheel")
    model = common.printed(part)
    declare(part, S)
    X = common.named(S)
    blank = part.cylinder("Top", d="wheel_rim_d", height="wheel_face",
                          name="Blank")
    rim = blank.faces(kind="cylinder")[0]
    sk = part.sketch(Plane.xy(0, flip=True), name="Collar")
    sk.circle((0, 0), d=X.main_collar_d)
    collar = part.extrude(sk.add(), X.tilt_collar, mode="add",
                          name="Collar")
    # The shaft stands on the collar's end.
    # Through both arms, and past the -Y arm by the land a DIN 471 ring
    # needs: its groove and the edge distance beyond it.
    sk = part.sketch(on=collar.end_cap(), name="Shaft")
    shaft_d = model("shaft", S["main_brg_d"])
    sk.circle((0, 0), d=shaft_d)
    part.extrude(sk.add(), 2 * X.arm_out + X.main_ring_land, mode="add",
                 name="Shaft")
    r = part.worm_wheel(rim, teeth=int(S["wheel_teeth"]),
                        starts=int(S["worm_starts"]), hand="right",
                        name="Tilt wheel")
    # The -Y bearing's inner ring is held by a ring in a groove at the arm's
    # outer face: the groove's near flank two arm_outs from the collar.
    common.groove(part, shaft_d / 2,
                  rim_z=-X.tilt_collar, offset=2 * X.arm_out,
                  name="Ring groove")
    part.material("petg")
    common.frame(part, "axis", (0, 0, 0), (0, 0, 1), (1, 0, 0))
    common.frame(part, "mesh", (0, 0, X.wheel_face / 2), (0, 0, 1),
                 (1, 0, 0))
    common.frame(part, "cradle", (0, 0, -(X.arm_out + X.tilt_collar)),
                 (0, 0, 1), (1, 0, 0))
    part.check()
    return part, r


def cradle(app, S):
    """The camera plate on two cheeks, in tilt-axis coordinates."""
    part = app.part("cradle")
    model = common.printed(part)
    X = common.named(S)
    w, sx, sz, pt = S["cam_w"], S["seat_x"], S["seat_z"], S["plate_t"]
    # Its planes are the variables', in tilt-axis coordinates: the plate's
    # underside, the cheeks' outer faces at the camera's width, the plate's
    # top (the plate's own faces are off the axis the sketches are drawn
    # about, so the planes are frames placed by the variables).
    under = X.seat_z - X.plate_t
    sk = part.sketch(on=common.plane(part, "Plate", "xy", under),
                     name="Plate")
    sk.rect((sx - S["cam_d"] / 2, -w / 2), (sx + S["cam_d"] / 2, w / 2))
    part.extrude(sk.add(), X.plate_t, name="Plate")
    br = X.boss_r
    for sign in (1, -1):
        # the cheek's outer face is the plate's end; drawn on it, extruded in
        side = common.plane(part, "Cheek " + ("+Y" if sign > 0 else "-Y"),
                            "xz", X.cam_w / 2 if sign > 0 else -X.cam_w / 2,
                            flip=sign > 0)
        sk = part.sketch(on=side, name="Cheek")
        u = (lambda x: -x) if sign > 0 else (lambda x: x)
        sk.polyline([(u(-br), 0.0), (u(-br), under), (u(br), under),
                     (u(br), 0.0)], close=False)
        sk.arc((0, 0), br, 180, 360)
        part.extrude(sk.add(), X.cheek_t, mode="add", reversed=True,
                     name="Cheek")
        if sign > 0:
            outer = side
    sk = part.sketch(on=outer, name="Shaft bore")
    sk.circle((0, 0), d=model("hole", S["main_brg_d"]))
    part.cut(sk.add(), 1.0, end_mode="through_all", reversed=True,
             name="Shaft bore")
    sk = part.sketch(on=common.plane(part, "Camera slot", "xy", X.seat_z),
                     name="Camera slot")
    half = S["slot_l"] / 2
    sk.slot((sx - half, 0), (sx + half, 0),
            model("hole", S["cam_screw_clear"]))
    part.cut(sk.add(), X.plate_t, reversed=True, name="Camera slot")
    part.material("petg")
    # Z along the tilt axis, X down: at zero tilt the shaft's X is down.
    common.frame(part, "shaft", (0, 0, 0), (0, 1, 0), (0, 0, -1))
    common.frame(part, "seat", (X.seat_x, 0, X.seat_z), (0, 0, 1), (1, 0, 0))
    common.frame(part, "screw", (X.seat_x, 0, under), (0, 0, 1), (1, 0, 0))
    part.check()
    return part


def camera(app, S):
    """The load: a body and a lens, socket at the origin on its underside."""
    part = app.part("camera")
    part.activate()
    w, h, d = S["cam_w"], S["cam_h"], S["cam_d"]
    body = part.box("Top", width="cam_d", depth="cam_w", height="cam_h",
                    name="Body")
    # The lens stands on the body's front face, at the lens centre height.
    front = body.face(normal=(1, 0, 0))
    lens = part.cylinder(front, d="lens_d", height="lens_l",
                         at=common.uv_on(front, (d / 2, 0, S["cam_cz"])),
                         name="Lens")
    part.app.add_feature(feature={"type": "Boolean", "name": "Camera",
                                  "target_id": body.created_by,
                                  "tool_id": lens.created_by, "op": "union"})
    sk = part.sketch(Plane.xy(0, flip=True), name="Socket")
    sk.point((0, 0))
    sock = part.hole(sk.add(), d=1.0, depth=Param(S["cam_socket"],
                                                  "cam_socket"),
                     fit={"thread": "1/4-20 UNC", "kind": "tapped"},
                     name="Socket")
    part.thread(sock.bores()[0], "1/4-20 UNC-2B", allow_engagement=True)
    part.material("plastic_black")
    common.frame(part, "socket", (0, 0, 0), (0, 0, 1), (1, 0, 0))
    part.check()
    mp = part.app.mass_properties(part=part.id)["total"]
    print(f"camera: {mp['mass_g']:.1f} g, CoM {mp['center_of_mass']}")
    return part, mp


def main():
    a = common.args()
    app = common.connect(a.port)
    clock = common.Clock()
    app.call("open_project", path=str(common.OUT / "worms.dsgn"))
    S = {}
    tilt_wheel(app, S)
    clock("tilt wheel")
    cradle(app, S)
    clock("cradle")
    camera(app, S)
    clock("camera")
    common.check_regen(app, "tilt")
    common.save(app, "tilt.dsgn")


if __name__ == "__main__":
    main()
