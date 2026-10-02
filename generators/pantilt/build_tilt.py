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
        ("cam_socket", "0.3 in", "mm, the camera's 1/4-20 socket depth"),
        ("cam_screw_eng", "0.2 in", "mm, the screw's engagement in it"),
        ("cam_screw_clear", "0.266 in", "mm, ASME close clearance for 1/4"),
        # The cradle and the arms it turns between.
        ("cheek_t", "2 * print_wall", "mm"),
        ("boss_r", "main_brg_d / 2 + 2 * print_wall", "mm, round the shaft"),
        ("slot_l", "com_fwd / 2", "mm, the camera slides +-com_fwd/4 to "
                                  "balance"),
        ("main_collar_d", "main_brg_d + 0.25 * (main_brg_D - main_brg_d)",
         "mm, bears on the inner ring only"),
        ("relief_d", "(main_brg_d + main_brg_D) / 2",
         "mm, in the arms under the tilt bearings' inner rings, so they "
         "turn free of the shoulders"),
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
    F = S["wheel_face"]
    blank = part.cylinder("Top", d="wheel_rim_d", height="wheel_face",
                          name="Blank")
    rim = blank.faces(kind="cylinder")[0]
    c = S["tilt_collar"]
    sk = part.sketch(Plane.xy(0, flip=True), name="Collar")
    sk.circle((0, 0), d=Param(S["main_collar_d"], "main_collar_d"))
    part.extrude(sk.add(), Param(c, "tilt_collar"), mode="add", name="Collar")
    sk = part.sketch(Plane.xy(-c, flip=True), name="Shaft")
    sk.circle((0, 0), d=model("shaft", S["main_brg_d"]))
    part.extrude(sk.add(), Param(2 * S["arm_out"], "2 * arm_out"),
                 mode="add", name="Shaft")
    r = part.worm_wheel(rim, teeth=int(S["wheel_teeth"]),
                        starts=int(S["worm_starts"]), hand="right",
                        name="Tilt wheel")
    part.material("petg")
    common.frame(part, "axis", (0, 0, 0), (0, 0, 1), (1, 0, 0))
    common.frame(part, "mesh", (0, 0, F / 2), (0, 0, 1), (1, 0, 0))
    common.frame(part, "cradle", (0, 0, -(S["arm_out"] + c)), (0, 0, 1),
                 (1, 0, 0))
    part.check()
    return part, r


def cradle(app, S):
    """The camera plate on two cheeks, in tilt-axis coordinates."""
    part = app.part("cradle")
    model = common.printed(part)
    w, sx, sz, pt = S["cam_w"], S["seat_x"], S["seat_z"], S["plate_t"]
    sk = part.sketch(Plane.xy(sz - pt), name="Plate")
    sk.rect((sx - S["cam_d"] / 2, -w / 2), (sx + S["cam_d"] / 2, w / 2))
    part.extrude(sk.add(), Param(pt, "plate_t"), name="Plate")
    br = S["boss_r"]
    for sign in (1, -1):
        # the cheek's outer face is the plate's end; drawn on it, extruded in
        pl = Plane.xz(sign * w / 2, flip=sign > 0)
        sk = part.sketch(pl, name="Cheek")
        u = (lambda x: -x) if sign > 0 else (lambda x: x)
        sk.polyline([(u(-br), 0), (u(-br), sz - pt), (u(br), sz - pt),
                     (u(br), 0)], close=False)
        sk.arc((0, 0), br, 180, 360)
        part.extrude(sk.add(), Param(S["cheek_t"], "cheek_t"), mode="add",
                     reversed=True, name="Cheek")
    sk = part.sketch(Plane.xz(w / 2, flip=True), name="Shaft bore")
    sk.circle((0, 0), d=model("hole", S["main_brg_d"]))
    part.cut(sk.add(), 1.0, end_mode="through_all", reversed=True,
             name="Shaft bore")
    sk = part.sketch(Plane.xy(sz), name="Camera slot")
    half = S["slot_l"] / 2
    sk.slot((sx - half, 0), (sx + half, 0),
            model("hole", S["cam_screw_clear"]))
    part.cut(sk.add(), Param(pt, "plate_t"), reversed=True,
             name="Camera slot")
    part.material("petg")
    # Z along the tilt axis, X down: at zero tilt the shaft's X is down.
    common.frame(part, "shaft", (0, 0, 0), (0, 1, 0), (0, 0, -1))
    common.frame(part, "seat", (sx, 0, sz), (0, 0, 1), (1, 0, 0))
    common.frame(part, "screw", (sx, 0, sz - pt), (0, 0, 1), (1, 0, 0))
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
