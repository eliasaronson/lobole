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
tilt, plus a clearance. That reach is `cam_swing`, computed by spec.swing
from the camera's and cradle's own variables in the first stage.
"""
import math

import common
from common import Plane, Param
import spec


def declare(part, S):
    S.update(spec.values(part))
    # cam_swing, cam_clear and yoke_base_t are spec.py's: the pan journal
    # is sized by how high they stand the load over it.
    for name, expr, comment in [
        ("z_mesh_pan", "base_t + wheel_face / 2",
         "mm, the pan wheel's mid-plane: the pan worm's axis"),
        ("yoke_zb", "z_mesh_pan + block_h / 2",
         "mm, the yoke's underside: the pan blocks hang from it"),
        ("yoke_zt", "yoke_zb + yoke_base_t", "mm, the yoke's top"),
        ("tilt_z", "yoke_zt + cam_swing + cam_clear", "mm, the tilt axis"),
        ("arm_w", "main_brg_D + 4 * print_wall", "mm, fore and aft"),
        ("ledge_z", "tilt_z - centre_dist - block_h / 2",
         "mm, the ledge's top: the tilt blocks stand on it"),
        # Phase 6: the yoke re-modelled over its topology result
        # (build_yoke_topo.py).
        ("yoke_xb", "centre_dist + block_w / 2",
         "mm, the base's and the arms' feet's half-width: the pan blocks' "
         "far edge, the room the A-frame arms stand on"),
        ("yoke_strut", "arm_t",
         "mm, a strut of the lightened arms, web and base: an arm's thickness"),
        ("joint_fw", "arm_t", "mm, the arm's foot flange, outward: an M4 "
                              "head (7) and a wall each side"),
        ("arm_up_x", "worm_span - run_gap",
         "mm, the +Y arm's half-width above the ledge: inside the tilt "
         "worm's blocks"),
    ]:
        part.param(name, expr, comment=comment, scope="project")
    S.update(spec.values(part))
    lever = S["tilt_z"] + S["lever"] - (S["base_t"] + S["wheel_face"])
    if abs(lever - S["pan_lever"]) > 1e-6:
        raise SystemExit(f"yoke: the load stands {lever:.3f} mm over the pan "
                         f"collar, and spec.py sized the journal for "
                         f"{S['pan_lever']:.3f}: the two layouts disagree")


def arm_side(S, design, sign):
    """An arm's +X edge, bottom to top, ending where the boss's arc starts
    (the outline is symmetric about the tilt axis). The lightened +Y arm
    keeps inside the tilt worm's blocks (and their screws' nuts under the
    ledge) above `zk`. The envelope's arms are both whole plates: the
    blocks are the topology study's keep-outs (build_yoke_topo.keep_outs),
    not a step drawn into the envelope by hand."""
    zb, zt, tz = S["yoke_zb"], S["yoke_zt"], S["tilt_z"]
    aw, xb, xu = S["arm_w"], S["yoke_xb"], S["arm_up_x"]
    zk = S["ledge_z"] - 2 * S["yoke_base_t"]
    if design == "solid":
        return [(aw / 2, zb), (aw / 2, tz)]
    if design == "envelope":
        return [(xb, zb), (xb, tz), (aw / 2, tz)]
    # lightened: an A-frame, its legs straight from the foot to the boss
    if sign > 0:
        return [(xb, zb), (xb, zt), (xu, zk), (aw / 2, tz)]
    return [(xb, zb), (xb, zt), (aw / 2, tz)]


def _inset_x(side, z, w):
    """The x a strut `w` wide leaves inside the edge `side` at height z:
    the edge's x less w across its slope."""
    for (x0, z0), (x1, z1) in zip(side, side[1:]):
        if z0 <= z <= z1 and z1 > z0:
            k = (z - z0) / (z1 - z0)
            slope = (x1 - x0) / (z1 - z0)
            return x0 + (x1 - x0) * k - w * math.sqrt(1 + slope * slope)
    raise ValueError(f"z {z} is not on the arm's edge")


def windows(S, sign):
    """The arm's windows, as (x, z) polygons on its +X half (mirrored for
    -X): the A-frame's legs, a centre strut and the boss left standing,
    and on the +Y arm the ledge as a cross strut -- what the density kept.
    Each band starts a strut over the foot flange and stops a half-strut
    short of the boss and of the ledge."""
    ws, tb, tz = S["yoke_strut"], S["yoke_base_t"], S["tilt_z"]
    side = arm_side(S, "lightened", sign)[1:]
    lz = S["ledge_z"]
    top = tz - S["arm_w"] / 2 - ws / 2
    bottom = S["yoke_zt"] + tb + ws / 2
    # On +Y nothing is cut above the ledge: that 65 mm of arm is the whole
    # path from the worm to the wheel's bearing, and windows there cost
    # the camera's pitch more than any other gram.
    bands = ([(bottom, lz - tb - ws / 2)] if sign > 0
             else [(bottom, top)])
    out = []
    for z0, z1 in bands:
        zs = [z0] + [z for _, z in side if z0 < z < z1] + [z1]
        edge = [(_inset_x(side, z, ws), z) for z in zs]
        out.append([(ws / 2, z0)] + edge + [(ws / 2, z1)])
    return out


def lighten(part, S, model):
    """The topology result, modelled (build_yoke_topo.py): windows in the
    arms, the foot flanges the arms are bolted down by, and the bolt holes.
    The yoke prints as three pieces split at the base's top (the base, and
    each arm lying on its inner face), so the joint is real: three ISO 4762
    M4 screws through each flange and the base, nuts underneath."""
    yi, yo, ws = S["arm_in"], S["arm_out"], S["yoke_strut"]
    zb, zt, tb, xb, fw = S["yoke_zb"], S["yoke_zt"], S["yoke_base_t"], \
        S["yoke_xb"], S["joint_fw"]
    lz, yw = S["ledge_z"], S["tilt_mesh_y"]
    lx = S["worm_span"] + S["block_t"] + S["print_wall"]
    for sign in (1, -1):
        pl = Plane.xz(sign * yi, flip=sign < 0)
        sk = part.sketch(pl, name="Arm windows")
        u = (lambda x: x) if sign > 0 else (lambda x: -x)
        for poly in windows(S, sign):
            for m in (1, -1):
                sk.polyline([(u(m * x), z) for x, z in poly], close=True)
        # Through the arm and a millimetre into the air past it.
        part.cut(sk.add(), S["arm_t"] + 1.0, reversed=True,
                 name="Arm windows")
    # The web: a second A-frame under the tilt worm, from the base to the
    # ledge. With the arm, the ledge and the base it closes a box, so the
    # worm's thrust on the ledge goes straight down a plate in its own plane
    # instead of twisting the arm (a 12 mm plate twisted by a load 24 mm
    # off it was most of the solid yoke's camera pitch).
    web = [(xb, zb), (xb, zt), (lx, lz - tb / 2)]
    sk = part.sketch(Plane.xz(yw - ws / 2), name="Web")
    sk.polyline([(x, z) for x, z in reversed(web)] +
                [(-x, z) for x, z in web], close=True)
    part.extrude(sk.add(), Param(ws, "yoke_strut"), mode="add",
                 reversed=True, name="Web")
    side = web[1:]
    z0, z1 = zt + ws, lz - tb - ws / 2
    zs = [z0, z1]
    poly = [(ws / 2, z0)] + [(_inset_x(side, z, ws), z) for z in zs] + \
        [(ws / 2, z1)]
    sk = part.sketch(Plane.xz(yw - ws / 2), name="Web windows")
    for m in (1, -1):
        sk.polyline([(m * x, z) for x, z in poly], close=True)
    part.cut(sk.add(), ws + 1.0, reversed=True, name="Web windows")
    # The arms' foot flanges, a pad at each end of each foot, outward (on
    # +Y into the web, which closes the gap between them).
    sk = part.sketch(Plane.xy(zt), name="Foot flanges")
    for sign, y1 in ((1, yw), (-1, -(yo + fw))):
        for m in (1, -1):
            sk.rect((m * xb, sign * yo), (m * (xb - 2 * fw), y1))
    part.extrude(sk.add(), Param(tb, "yoke_base_t"), mode="add",
                 name="Foot flanges")
    # The base's windows: it is a frame -- a rim under the arms' feet and
    # the ends, a cross of struts, the hub's ring -- with a window in each
    # quadrant, clear of the hub, the arms' feet and the pan blocks (whose
    # footprint, from the worm's span out, stays solid on +X).
    rh = S["base_r"] + ws / 2
    sk = part.sketch(Plane.xy(zt), name="Base windows")
    for mx in (1, -1):
        for my in (1, -1):
            y1 = (S["worm_span"] - ws / 2) if mx > 0 else (yi - ws)
            sk.polyline([(mx * x, my * y)
                         for x, y in _window_round_hub(ws / 2, xb - ws,
                                                       ws / 2, y1, rh)],
                        close=True)
    # Keels: a grillage under the base down to the hub's depth, inside the
    # pan worm's swing (x < base_r): a band under each arm's foot and two
    # ribs across, through the hub, so an arm's moment reaches the journal
    # through a 20 mm section. Held at its underside, the yoke's lens moved
    # a quarter of what it does on the journal: the base was the soft path.
    keel_t = zb - S["base_t"] - S["wheel_face"] - S["run_gap"]
    sk2 = part.sketch(Plane.xy(zb, flip=True), name="Keels")
    for y0, y1 in ((yi - ws, yw + ws / 2), (-(yo + fw), -(yi - ws))):
        sk2.rect((-xb, y0), (S["base_r"], y1))
    part.extrude(sk2.add(), keel_t, mode="add", name="Keels")
    # (the ribs overlap the bands' and the hub's outlines: their own sketch,
    # since one sketch's loops may not overlap)
    sk2 = part.sketch(Plane.xy(zb, flip=True), name="Keel ribs")
    for x in (0.0, -(xb - ws / 2)):
        sk2.rect((x - ws / 2, -(yi - ws / 2)), (x + ws / 2, yi - ws / 2))
    part.extrude(sk2.add(), keel_t, mode="add", name="Keel ribs")
    part.cut(sk.add(), tb + 1.0, reversed=True, name="Base windows")
    # Last: down through the flange, the base and the keel under it.
    sk = part.sketch(Plane.xy(zt + tb), name="Joint screws")
    for y in ((yo + yw - ws / 2) / 2, -(yo + fw / 2)):
        for m in (1, -1):
            sk.point((m * (xb - fw), y))
    part.hole(sk.add(), d=1.0, through=True,
              fit={"thread": "M4", "kind": "normal"}, name="Joint screws")


def _window_round_hub(x0, x1, y0, y1, r, n=6):
    """The rectangle [x0, x1] x [y0, y1] in the first quadrant, its corner
    near the origin rounded off along the circle `r` so the window clears
    the hub."""
    a0 = math.asin(min(1.0, y0 / r))
    a1 = math.acos(min(1.0, x0 / r))
    if r * math.sin(a1) >= y1:  # too narrow to reach round the hub
        return [(r, y0), (r, y1), (x1, y1), (x1, y0)]
    arc = [(r * math.cos(a0 + (a1 - a0) * i / n),
            r * math.sin(a0 + (a1 - a0) * i / n)) for i in range(n + 1)]
    return arc + [(x0, y1), (x1, y1), (x1, y0)]


def yoke(app, S, name="yoke", design="solid"):
    """The yoke, or (design="envelope") the room it may fill: the base out
    to the pan blocks' far edge on both sides and each arm a plate across
    that whole width up to the tilt axis -- what the topology study of
    build_yoke_topo.py removes material from. The tilt worm's blocks stand
    in the +Y arm's slab above the ledge, and their nuts under it: the
    study keeps out of them (keep-outs), so the plate need not."""
    part = app.part(name)
    model = common.printed(part)
    if name == "yoke":
        declare(part, S)
    a, s = S["centre_dist"], S["worm_span"]
    zb, zt, tb = S["yoke_zb"], S["yoke_zt"], S["yoke_base_t"]
    zt_tilt, yo, yi = S["tilt_z"], S["arm_out"], S["arm_in"]
    aw, bw, bh, bt = S["arm_w"], S["block_w"], S["block_h"], S["block_t"]
    xb = a + bw / 2  # the pan blocks' far edge
    x0 = -aw / 2 if design == "solid" else -xb
    lz, yw = S["ledge_z"], S["tilt_mesh_y"]
    # The base, out under the arms' foot flanges when they have them.
    # Lightened, it reaches under the -Y arm's foot flange and, on +Y, under
    # the web that stands under the tilt worm (lighten()).
    y_neg, y_pos = yo, yo
    if design == "lightened":
        y_neg = yo + S["joint_fw"]
        y_pos = yw + S["yoke_strut"] / 2
    sk = part.sketch(Plane.xy(zb), name="Base")
    sk.rect((x0, -y_neg), (xb, y_pos))
    part.extrude(sk.add(), Param(tb, "yoke_base_t"), name="Base")
    # The pan shaft: a collar on the upper bearing's inner ring, then the
    # journal down through both bearings.
    top = S["base_t"] + S["wheel_face"]
    sk = part.sketch(Plane.xy(zb, flip=True), name="Pan collar")
    sk.circle((0, 0), d=Param(S["pan_collar_d"], "pan_collar_d"))
    part.extrude(sk.add(), zb - top, mode="add", name="Pan collar")
    # Through the wheel's bearing AND the plate's under it: a journal only
    # the wheel's long (as phase 5 had it) hangs the whole yoke, and its
    # moment, on one bearing's inner ring.
    sk = part.sketch(Plane.xy(top, flip=True), name="Pan journal")
    sk.circle((0, 0), d=model("shaft", S["pan_brg_d"]))
    part.extrude(sk.add(), Param(S["wheel_face"] + S["pan_brg_B"]
                                 - S["nozzle"],
                                 "wheel_face + pan_brg_B - nozzle"),
                 mode="add", name="Pan journal")
    if design != "solid":
        # The hub: the base deepened down to the wheel's top (a running gap
        # over it), inside the pan worm's swing -- the room between the
        # collar and the arms that the base plate alone does not use.
        sk = part.sketch(Plane.xy(zb, flip=True), name="Hub")
        sk.circle((0, 0), r=Param(S["base_r"], "base_r"))
        part.extrude(sk.add(), zb - top - S["run_gap"], mode="add",
                     name="Hub")
    # The arms, from inside the base to a round top about the tilt axis.
    for sign in (1, -1):
        pl = Plane.xz(sign * yi, flip=sign < 0)
        sk = part.sketch(pl, name="Arm")
        u = (lambda x: x) if sign > 0 else (lambda x: -x)
        right = arm_side(S, design, sign)
        pts = [(x, z) for x, z in reversed(right)] + \
              [(-x, z) for x, z in right]
        sk.polyline([(u(x), z) for x, z in pts], close=False)
        sk.arc((0, zt_tilt), aw / 2, 0, 180)
        part.extrude(sk.add(), Param(S["arm_t"], "arm_t"), mode="add",
                     reversed=True, name="Arm")
    if design == "lightened":
        lighten(part, S, model)
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
    yoke(app, S, design="lightened")
    clock("yoke")
    common.check_regen(app, "yoke")
    common.save(app, "yoke.dsgn")


if __name__ == "__main__":
    main()
