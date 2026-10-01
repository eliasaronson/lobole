#!/usr/bin/env python3
"""Stage 4: the yoke. Opens tilt.dsgn, saves yoke.dsgn.

    python3 generators/pantilt/build_yoke.py --port N

The yoke is modelled where it stands at zero pan (world coordinates, the
base plate's underside at z = 0), so its frames ARE the layout: a base that
hangs the pan worm's blocks under it and turns on the pan bearings by a
shaft down its middle, two arms carrying the tilt bearings, and a ledge on
the +Y arm that stands the tilt worm's blocks under the tilt wheel.

How tall the arms are is the camera's: the tilt axis sits as far above the
base as the camera and cradle reach below it anywhere in +-90 degrees of
tilt, plus a clearance. That reach is computed here from the camera's and
cradle's own variables and declared as `cam_swing`.
"""
import math

import common
from common import Plane, Param
import spec


def swing(S):
    """The deepest the camera or the cradle reaches below the tilt axis,
    anywhere in +-90 degrees: max over the tilt of x sin(t) - z cos(t), over
    the corners of their outlines in the plane the tilt turns them in (the
    lens is a cylinder along X, so its outline there is a rectangle)."""
    sx, sz, d = S["seat_x"], S["seat_z"], S["cam_d"]
    pts = []
    for x in (sx - d / 2, sx + d / 2):
        for z in (sz - S["plate_t"], sz + S["cam_h"]):
            pts.append((x, z))
    lx0 = sx + d / 2
    for x in (lx0, lx0 + S["lens_l"]):
        for z in (sz + S["cam_cz"] - S["lens_d"] / 2,
                  sz + S["cam_cz"] + S["lens_d"] / 2):
            pts.append((x, z))
    deepest = 0.0
    for i in range(-900, 901):
        t = math.radians(i / 10.0)
        for x, z in pts:
            deepest = max(deepest, x * math.sin(t) - z * math.cos(t))
    return deepest


def declare(part, S):
    S.update(spec.values(part))
    part.param("cam_swing", round(swing(S), 3),
               comment="mm, the deepest the camera or cradle reaches below "
                       "the tilt axis over +-90 deg (build_yoke.swing)",
               scope="project")
    for name, expr, comment in [
        ("cam_clear", "4 * run_gap", "mm, the camera over the yoke at full "
                                     "tilt"),
        ("z_mesh_pan", "base_t + wheel_face / 2",
         "mm, the pan wheel's mid-plane: the pan worm's axis"),
        ("yoke_zb", "z_mesh_pan + block_h / 2",
         "mm, the yoke's underside: the pan blocks hang from it"),
        ("yoke_base_t", "4 * print_wall", "mm"),
        ("yoke_zt", "yoke_zb + yoke_base_t", "mm, the yoke's top"),
        ("tilt_z", "yoke_zt + cam_swing + cam_clear", "mm, the tilt axis"),
        ("arm_w", "main_brg_D + 4 * print_wall", "mm, fore and aft"),
        ("ledge_z", "tilt_z - centre_dist - block_h / 2",
         "mm, the ledge's top: the tilt blocks stand on it"),
    ]:
        part.param(name, expr, comment=comment, scope="project")
    S.update(spec.values(part))


def yoke(app, S):
    part = app.part("yoke")
    model = common.printed(part)
    declare(part, S)
    a, s = S["centre_dist"], S["worm_span"]
    zb, zt, tb = S["yoke_zb"], S["yoke_zt"], S["yoke_base_t"]
    zt_tilt, yo, yi = S["tilt_z"], S["arm_out"], S["arm_in"]
    aw, bw, bh, bt = S["arm_w"], S["block_w"], S["block_h"], S["block_t"]
    # The base.
    sk = part.sketch(Plane.xy(zb), name="Base")
    sk.rect((-aw / 2, -yo), (a + bw / 2, yo))
    part.extrude(sk.add(), Param(tb, "yoke_base_t"), name="Base")
    # The pan shaft: a collar on the upper bearing's inner ring, then the
    # journal down through both bearings.
    top = S["base_t"] + S["wheel_face"]
    sk = part.sketch(Plane.xy(zb, flip=True), name="Pan collar")
    sk.circle((0, 0), d=Param(S["main_collar_d"], "main_collar_d"))
    part.extrude(sk.add(), zb - top, mode="add", name="Pan collar")
    sk = part.sketch(Plane.xy(top, flip=True), name="Pan journal")
    sk.circle((0, 0), d=model("shaft", S["main_brg_d"]))
    part.extrude(sk.add(), S["wheel_face"] - S["nozzle"], mode="add",
                 name="Pan journal")
    # The arms, from inside the base to a round top about the tilt axis.
    for sign in (1, -1):
        pl = Plane.xz(sign * yi, flip=sign < 0)
        sk = part.sketch(pl, name="Arm")
        u = (lambda x: x) if sign > 0 else (lambda x: -x)
        sk.polyline([(u(aw / 2), zt_tilt), (u(aw / 2), zb),
                     (u(-aw / 2), zb), (u(-aw / 2), zt_tilt)], close=False)
        sk.arc((0, zt_tilt), aw / 2, 0, 180)
        part.extrude(sk.add(), Param(S["arm_t"], "arm_t"), mode="add",
                     reversed=True, name="Arm")
    # The tilt bearings, pressed in from outside onto a shoulder.
    for sign in (1, -1):
        pl = Plane.xz(sign * yo, flip=sign > 0)
        sk = part.sketch(pl, name="Tilt bearing")
        sk.circle((0, zt_tilt), d=model("hole", S["main_brg_D"]))
        part.cut(sk.add(), Param(S["main_brg_B"], "main_brg_B"),
                 reversed=True, name="Tilt bearing")
    sk = part.sketch(Plane.xz(yo, flip=True), name="Tilt relief")
    sk.circle((0, zt_tilt), d=model("hole", S["relief_d"]))
    part.cut(sk.add(), 1.0, end_mode="through_all", reversed=True,
             name="Tilt relief")
    # The ledge the tilt worm's blocks stand on, out from the +Y arm.
    lz, yw = S["ledge_z"], S["tilt_mesh_y"]
    lx = s + bt + S["print_wall"]
    sk = part.sketch(Plane.xy(lz - tb), name="Ledge")
    sk.rect((-lx, yi), (lx, yw + bw / 2))
    part.extrude(sk.add(), Param(tb, "yoke_base_t"), mode="add",
                 name="Ledge")
    # The blocks' screws: down through the base (pan) and the ledge (tilt).
    lat = bh / 2 + S["block_flange"] / 2
    sk = part.sketch(Plane.xy(zt), name="Pan block screws")
    for y in (s + bt / 2, -(s + bt / 2)):
        for dx in (lat, -lat):
            sk.point((a + dx, y))
    part.hole(sk.add(), d=1.0, through=True,
              fit={"thread": "M4", "kind": "normal"}, name="Pan block screws")
    sk = part.sketch(Plane.xy(lz), name="Tilt block screws")
    for x in (s + bt / 2, -(s + bt / 2)):
        for dy in (lat, -lat):
            sk.point((x, yw + dy))
    # Not through: the base lies under the ledge's inner screws.
    part.hole(sk.add(), d=1.0, depth=Param(tb + S["print_wall"],
                                           "yoke_base_t + print_wall"),
              fit={"thread": "M4", "kind": "normal"},
              name="Tilt block screws")
    part.material("petg")
    zm, zw = S["z_mesh_pan"], zt_tilt - a
    for name, o, z, x in [
        ("pan", (0, 0, top), (0, 0, 1), (1, 0, 0)),
        ("pan_worm", (a, 0, zm), (0, 1, 0), (-1, 0, 0)),
        ("pan_block_a", (a, s, zm), (0, 1, 0), (0, 0, 1)),
        ("pan_block_b", (a, -s, zm), (0, -1, 0), (0, 0, 1)),
        ("tilt", (0, yo + S["tilt_collar"], zt_tilt), (0, 1, 0), (0, 0, -1)),
        ("tilt_worm", (0, yw, zw), (-1, 0, 0), (0, 0, 1)),
        # upside down: the screws' counterbores face up, the heads on top
        ("tilt_block_a", (-s, yw, zw), (-1, 0, 0), (0, 0, -1)),
        ("tilt_block_b", (s, yw, zw), (1, 0, 0), (0, 0, -1)),
    ]:
        common.frame(part, name, o, z, x)
    part.check()
    mp = app.mass_properties(part=part.id)["total"]
    print(f"yoke: {mp['mass_g']:.0f} g, tilt axis at z {zt_tilt:.1f}, "
          f"camera swing {S['cam_swing']:.1f}")
    return part


def main():
    a = common.args()
    app = common.connect(a.port)
    clock = common.Clock()
    app.call("open_project", path=str(common.OUT / "tilt.dsgn"))
    S = {}
    yoke(app, S)
    clock("yoke")
    common.check_regen(app, "yoke")
    common.save(app, "yoke.dsgn")


if __name__ == "__main__":
    main()
