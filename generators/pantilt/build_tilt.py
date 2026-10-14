#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Stage 3: what turns about the tilt axis -- the tilt wheel on its shaft,
the cradle (the camera plate) and the camera. Opens stages/worms.dsgn,
saves stages/tilt.dsgn. The camera screw is bought, an ASME B18.3 standard
part, and the assembly places it (build_assembly.py).

    python3 generators/pantilt/build_tilt.py --port N

Each part is modelled about the tilt axis: the cradle and the camera in
tilt-axis coordinates (X forward, Z up, the axis along Y), the wheel along
its own Z. The camera is the LOAD, drawn as one: a mirrorless body with a
grip, a viewfinder hump and a hot shoe on a zoom with its rings and front
element, in the black plastic it is drawn in, its lens long enough to make
1.5 kg (spec.py) and its hood hollow by what the details add, so it
still weighs the load. The load's CoM is spec.py's, the body and plain
lens it is sized as: 80 mm up and 40 mm forward of the tilt axis when the
cradle seats it -- the cradle's seat is placed from that CoM, not the other
way round, and the studies load it there.
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
    # Params, so the plate and its slot follow the seat when the camera
    # moves it (seat_x is the camera's CoM, and so its load's).
    w, sx = X.cam_w, X.seat_x
    # Its planes are the variables', in tilt-axis coordinates: the plate's
    # underside, the cheeks' outer faces at the camera's width, the plate's
    # top (the plate's own faces are off the axis the sketches are drawn
    # about, so the planes are frames placed by the variables).
    under = X.seat_z - X.plate_t
    sk = part.sketch(on=common.plane(part, "Plate", "xy", under),
                     name="Plate")
    sk.rect((sx - X.cam_d / 2, -w / 2), (sx + X.cam_d / 2, w / 2))
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
    half = X.slot_l / 2
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


# The camera's look, every size over the body's and the lens's own (spec.py's
# CAMERA): a full-frame mirrorless body with a grip, a viewfinder hump and a
# hot shoe, two dials and a shutter button, and a zoom on a metal mount with
# its zoom and focus rings and a front element in a hood. None of it moves
# the load: the hollow behind the front element is as deep as gives back
# what the rest adds (`cam_hood_depth`), so the camera still weighs load_kg.
CAMERA_LOOK = [
    ("cam_r", "cam_h / 16", "mm, the body's edges, rounded"),
    ("cam_mount_d", "lens_d * 1.06", "mm, the lens mount's ring"),
    ("cam_mount_l", "lens_d / 12", "mm, the mount ring's length"),
    ("cam_grip_t", "cam_d / 3", "mm, how far the grip stands off the front"),
    ("cam_grip_w", "cam_w / 2 - cam_r - cam_mount_d / 2 - cam_r / 2",
     "mm, the grip's width: from the body's side to beside the mount"),
    ("cam_grip_h", "cam_h - 2 * cam_r", "mm, the grip's height"),
    ("cam_grip_r", "cam_grip_w / 4", "mm, the grip's edges"),
    ("cam_hump_l", "cam_d * 0.6", "mm, the viewfinder hump, front to back"),
    ("cam_hump_w", "lens_d / 2", "mm, the hump's width"),
    ("cam_hump_h", "cam_h / 5", "mm, the hump over the top plate"),
    ("cam_hump_r", "cam_hump_h / 4", "mm, the hump's edges"),
    ("cam_hump_x", "-cam_d / 10", "mm, the hump's centre behind the socket"),
    ("cam_shoe_l", "cam_hump_l / 2", "mm, the hot shoe"),
    ("cam_shoe_w", "cam_hump_w / 2", "mm"),
    ("cam_shoe_h", "cam_r / 2", "mm"),
    ("cam_dial_d", "cam_h / 4.5", "mm, the top plate's two dials"),
    ("cam_dial_h", "cam_r", "mm"),
    ("cam_dial_x", "-cam_d / 6", "mm, the dials' centres behind the socket"),
    ("cam_dial_y", "cam_w / 2 - 2 * cam_r - cam_dial_d / 2",
     "mm, each dial's centre off the lens axis"),
    ("cam_btn_d", "cam_dial_d / 2", "mm, the shutter button, over the grip"),
    ("cam_btn_h", "cam_r / 2", "mm"),
    ("cam_btn_x", "cam_d / 2 - cam_r - cam_btn_d", "mm"),
    ("cam_zoom_at", "lens_l * 0.15", "mm, the zoom ring's back from the body"),
    ("cam_groove_at", "lens_l * 0.45", "mm, the groove between the rings"),
    ("cam_groove_l", "lens_d / 25", "mm"),
    ("cam_groove_t", "lens_d / 50", "mm, the groove's depth"),
    ("cam_front_at", "lens_l * 0.7", "mm, the focus ring's front"),
    ("cam_bore_d", "lens_d * 0.84", "mm, the hood's bore"),
    ("cam_glass_t", "lens_d / 20", "mm, the front element"),
    ("cam_glass_rho", "2.5", "g/cm3: glass, what the front element is"),
]


def _rounded_box(a, b, c, r):
    """The volume of an a x b x c box with every edge rounded r, as an
    expression: the inner box swept by a ball of r."""
    i = [f"(({x}) - 2 * ({r}))" for x in (a, b, c)]
    return (f"({i[0]} * {i[1]} * {i[2]} + 2 * ({r}) * ({i[0]} * {i[1]} + "
            f"{i[1]} * {i[2]} + {i[0]} * {i[2]}) + pi * ({r})^2 * ({i[0]} + "
            f"{i[1]} + {i[2]}) + 4 / 3 * pi * ({r})^3)")


def _disc(d, h):
    return f"(pi * ({d})^2 / 4 * ({h}))"


def _hood_depth():
    """How deep the hollow behind the front element is: deep enough that
    the camera, every addition to the body and lens counted, weighs what
    the box and the plain lens weighed -- load_kg (spec.py's lens_l)."""
    added = " + ".join([
        _rounded_box("cam_d", "cam_w", "cam_h", "cam_r")
        + " - cam_d * cam_w * cam_h",
        _rounded_box("cam_grip_t", "cam_grip_w", "cam_grip_h", "cam_grip_r"),
        _rounded_box("cam_hump_l", "cam_hump_w", "cam_hump_h", "cam_hump_r"),
        "cam_shoe_l * cam_shoe_w * cam_shoe_h",
        "2 * " + _disc("cam_dial_d", "cam_dial_h"),
        _disc("cam_btn_d", "cam_btn_h"),
        _disc("cam_mount_d", "cam_mount_l") + " - "
        + _disc("lens_d", "cam_mount_l"),
        _disc("lens_d - 2 * cam_groove_t", "cam_groove_l") + " - "
        + _disc("lens_d", "cam_groove_l"),
    ])
    return (f"({added}) / (pi * cam_bore_d^2 / 4) + "
            f"cam_glass_rho / cam_rho * cam_glass_t")


def _look(part, body, color, roughness, metallic=0.0):
    part.app.call("set_appearance", feature_id=body.created_by,
                  color=list(color), roughness=roughness, metallic=metallic)


# Linear colours (set_appearance): plastic_black alone renders a mid grey.
BLACK = ((0.018, 0.018, 0.02), 0.5)
RUBBER = ((0.008, 0.008, 0.008), 0.92)
METAL = ((0.55, 0.55, 0.57), 0.3, 1.0)
DARK_METAL = ((0.05, 0.05, 0.055), 0.35, 0.8)


def camera(app, S):
    """The load: a mirrorless body and a zoom, socket at the origin on its
    underside. It looks like a camera and weighs what spec.py says the load
    is; the studies read the load from spec.py (load_kg, its CoM), not from
    this part."""
    part = app.part("camera")
    part.activate()
    for name, expr, comment in CAMERA_LOOK:
        part.param(name, expr, comment=comment, scope="project")
    part.param("cam_hood_depth", _hood_depth(),
               comment="mm, the hollow behind the front element: what the "
                       "grip, hump, dials, mount and rounded edges add, "
                       "taken out again, so the camera weighs load_kg",
               scope="project")
    S.update(spec.values(part))
    X = common.named(S)
    if not 0 < S["cam_hood_depth"] < S["lens_l"] - S["cam_front_at"]:
        raise SystemExit(f"camera: the hood's hollow is "
                         f"{S['cam_hood_depth']:.2f} mm, outside the lens's "
                         f"front {S['lens_l'] - S['cam_front_at']:.2f} mm")
    body = part.box("Top", width="cam_d", depth="cam_w", height="cam_h",
                    name="Body")
    part.fillet(body.edges(), "cam_r", name="Body edges")
    # The front, top and lens stations are planes the variables place
    # (tilt-axis-free camera coordinates: X forward, Z up, the socket at the
    # origin). On the front plane u is +Y and v +Z; on the top, u +X, v +Y.
    front = common.plane(part, "Camera front", "yz", X.cam_d / 2)
    top = common.plane(part, "Camera top", "xy", X.cam_h)
    grip = part.box(front, width="cam_grip_w", depth="cam_grip_h",
                    height="cam_grip_t",
                    at=("cam_grip_w / 2 + cam_r - cam_w / 2", "cam_h / 2"),
                    name="Grip")
    part.fillet(grip.edges(), "cam_grip_r", name="Grip edges")
    hump = part.box(top, width="cam_hump_l", depth="cam_hump_w",
                    height="cam_hump_h", at=("cam_hump_x", "0"),
                    name="Viewfinder")
    part.fillet(hump.edges(), "cam_hump_r", name="Viewfinder edges")
    shoe_on = common.plane(part, "Hot shoe", "xy", X.cam_h + X.cam_hump_h)
    shoe = part.box(shoe_on, width="cam_shoe_l", depth="cam_shoe_w",
                    height="cam_shoe_h", at=("cam_hump_x", "0"),
                    name="Hot shoe")
    dials = [part.cylinder(top, d="cam_dial_d", height="cam_dial_h",
                           at=("cam_dial_x", y), name=f"Dial {k}")
             for k, y in (("left", "cam_dial_y"), ("right", "-cam_dial_y"))]
    button = part.cylinder(top, d="cam_btn_d", height="cam_btn_h",
                           at=("cam_btn_x", "-cam_dial_y"),
                           name="Shutter button")
    # The lens, station by station along +X from the body's front.
    axis = ("0", "cam_cz")

    def station(name, at):
        return common.plane(part, f"Lens {name}", "yz", X.cam_d / 2 + at)

    mount = part.cylinder(front, d="cam_mount_d", height="cam_mount_l",
                          at=axis, name="Lens mount")
    rear = part.cylinder(station("rear", X.cam_mount_l), d="lens_d",
                         height="cam_zoom_at - cam_mount_l", at=axis,
                         name="Lens barrel")
    zoom = part.cylinder(station("zoom ring", X.cam_zoom_at), d="lens_d",
                         height="cam_groove_at - cam_zoom_at", at=axis,
                         name="Zoom ring")
    groove = part.cylinder(station("groove", X.cam_groove_at),
                           d="lens_d - 2 * cam_groove_t",
                           height="cam_groove_l", at=axis,
                           name="Lens groove")
    focus = part.cylinder(station("focus ring",
                                  X.cam_groove_at + X.cam_groove_l),
                          d="lens_d",
                          height="cam_front_at - cam_groove_at - cam_groove_l",
                          at=axis, name="Focus ring")
    nose = part.cylinder(station("front", X.cam_front_at), d="lens_d",
                         height="lens_l - cam_front_at", at=axis,
                         name="Lens front")
    # The hood's hollow, cut back from the lens's front, and the front
    # element at its floor.
    tip = station("tip", X.lens_l)
    sk = part.sketch(on=tip, name="Hood")
    sk.circle((0, X.cam_cz), d=X.cam_bore_d)
    part.cut(sk.add(), X.cam_hood_depth, reversed=True, name="Hood")
    glass = part.cylinder(station("glass", X.lens_l - X.cam_hood_depth),
                          d="cam_bore_d", height="cam_glass_t", at=axis,
                          name="Front element")
    sk = part.sketch(Plane.xy(0, flip=True), name="Socket")
    sk.point((0, 0))
    sock = part.hole(sk.add(), d=1.0, depth=Param(S["cam_socket"],
                                                  "cam_socket"),
                     fit={"thread": "1/4-20 UNC", "kind": "tapped"},
                     name="Socket")
    part.thread(sock.bores()[0], "1/4-20 UNC-2B", allow_engagement=True)
    part.material("plastic_black")
    part.app.set_material(feature_id=glass.created_by, material="glass")
    for b in (body, hump, rear, groove, nose):
        _look(part, b, *BLACK)
    for b in (grip, zoom, focus):
        _look(part, b, *RUBBER)
    for b in (mount, shoe):
        _look(part, b, *METAL)
    for b in dials + [button]:
        _look(part, b, *DARK_METAL)
    common.frame(part, "socket", (0, 0, 0), (0, 0, 1), (1, 0, 0))
    part.check()
    mp = part.app.mass_properties(part=part.id)["total"]
    print(f"camera: {mp['mass_g']:.1f} g (load_kg {S['load_kg']:g}), CoM "
          f"{mp['center_of_mass_mm']} (spec.py: x {S['cam_cx']:.2f}, z "
          f"{S['cam_cz']:.2f})")
    # The socket's tapped hole is the one thing not given back, as before.
    if abs(mp["mass_g"] - 1000 * S["load_kg"]) > 1.0:
        raise SystemExit(f"camera: {mp['mass_g']:.3f} g, not load_kg")
    return part, mp


def main():
    a = common.args()
    app = common.connect(a.port)
    clock = common.Clock()
    app.call("open_project", path=str(common.stage("worms")))
    S = {}
    tilt_wheel(app, S)
    clock("tilt wheel")
    cradle(app, S)
    clock("cradle")
    camera(app, S)
    clock("camera")
    common.check_regen(app, "tilt")
    common.save(app, "tilt")


if __name__ == "__main__":
    main()
