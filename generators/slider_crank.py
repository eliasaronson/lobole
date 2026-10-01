#!/usr/bin/env python3
"""A slider-crank on the SDK — the engine's atom, and the motion study's
acceptance rig (docs/assemblies/README.md#motion-studies).

A block with a bore, a crank disc on a revolute at the origin with a pin
boss at radius R, a connecting rod, and a piston that slides in the bore:
crank -> rod -> piston -> block closes a loop, which the A6 solver closes
numerically. The motion study turns the crank through 360 degrees and the
piston's stroke must be exactly 2R, with no clash at any sample.

    python3 generators/slider_crank.py --port <port> [--crank 20 --rod 70]

Every dimension is a variable; every mate is between named frames.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from designer_sdk import Plane, SdkError, connect  # noqa: E402
from _lib import uv_on  # noqa: E402


def build(app, crank_r=20.0, rod_l=70.0, bore_d=30.0):
    """The mechanism, geometrically consistent.

    Two things the motion study caught in the first version of this rig, and
    both are what a kinematic gate is FOR:

    * the rod's tip hit the piston body ahead of its slot (an exact 360 mm3
      at every angle), because the slot did not open at the piston's back
      face; and
    * at 90 and 270 degrees the rod's swing left the bore and hit the
      barrel, because the barrel started at x = crank_r + 10 and the rod is
      still 11 mm off the axis there.

    So the barrel starts clear of the swing, and the piston is slotted deep
    enough for the rod's small end to rotate inside it. Nothing here is
    asserted to be right: `run_motion_study` checks all 49 poses.
    """
    app.new()
    # The wrist pin travels crank_r*cos t + sqrt(rod_l^2 - (crank_r sin t)^2):
    # from rod_l - crank_r to rod_l + crank_r, a stroke of exactly 2*crank_r.
    tdc = rod_l + crank_r
    bdc = rod_l - crank_r
    # The rod's small end swings a corner of radius ~10 about the wrist pin
    # (8 past it, 6 half-height); the barrel must start where the rod is
    # still inside the bore, which at 90 degrees is about a third of the way.
    barrel_x0 = crank_r + 25.0

    # ---- The barrel: bored along +X on the axis, covering the piston's
    # whole travel.
    block = app.part("block")
    block.var("bore_d", bore_d).var("crank_r", crank_r).var("rod_l", rod_l)
    # The block stands on the plane the barrel starts at and runs along +X.
    barrel_end = block.datum_plane("yz", offset=barrel_x0, name="Barrel end")
    block.box(barrel_end, width=50, depth=50, height=tdc + 35 - barrel_x0,
              name="Block")
    bore_sk = block.sketch(Plane.yz(barrel_x0), name="Bore")
    bore_sk.circle((0, 0), d="bore_d")
    block.extrude(bore_sk.add(), tdc + 40 - barrel_x0, mode="cut", name="Bore")
    block.frame("crank_axis", origin=(0, 0, 0), z=(0, 1, 0))
    block.frame("bore_axis", origin=(barrel_x0, 0, 0), z=(1, 0, 0))
    block.check()

    # ---- The crank: a disc BEHIND the rod plane, and a pin that reaches
    # through it, so the rod's big end runs on the pin and clears the disc.
    crank = app.part("crank")
    crank.var("crank_r", crank_r)
    # The disc's back is 30 behind the rod plane (an XZ datum faces -Y, so
    # the disc grows reversed, towards +Y); the pin stands on its front.
    disc_back = crank.datum_plane("xz", offset=30, name="Disc back")
    disc = crank.cylinder(disc_back, d=2 * crank_r + 16, height=8,
                          reversed=True, name="Disc")
    front = disc.feature.end_cap()
    crank.cylinder(front, d=8, height=25, at=uv_on(front, (crank_r, -22, 0)),
                   name="Pin")
    crank.frame("axle", origin=(0, 0, 0), z=(0, 1, 0))
    crank.frame("pin", origin=("crank_r", 0, 0), z=(0, 1, 0))
    crank.check()

    # ---- The rod: in the plane y -3..3, big end at its own origin.
    rod = app.part("rod")
    rod.var("rod_l", rod_l)
    # The rod lies in the plane y -3..3, its underside 6 below the axis.
    underside = rod.datum_plane("xy", offset=-6, name="Rod underside")
    rod.box(underside, width=rod_l + 16, depth=6, height=12,
            at=(rod_l / 2, 0), name="Rod")
    hole = rod.sketch(Plane.xz(3.0), name="Pin hole")
    hole.circle((0, 0), d=9)
    rod.extrude(hole.add(), 8.0, mode="cut", name="Pin hole")
    rod.frame("big_end", origin=(0, 0, 0), z=(0, 1, 0))
    rod.frame("small_end", origin=("rod_l", 0, 0), z=(0, 1, 0))
    rod.check()

    # ---- The piston: the wrist pin near its crown end, and a slot that
    # OPENS at the back face so the rod's small end swings inside it.
    piston = app.part("piston")
    piston.var("bore_d", bore_d)
    # Both start on YZ datums behind the wrist pin and run along +X: the
    # piston from its back face, the slot from 2 mm behind it, so it opens.
    piston.cylinder(piston.datum_plane("yz", offset=-6, name="Piston back"),
                    d=bore_d - 0.5, height=32, name="Piston")
    piston.box(piston.datum_plane("yz", offset=-8, name="Slot start"),
               width=8, depth=22, height=20, mode="cut", name="Slot")
    piston.frame("wrist_pin", origin=(0, 0, 0), z=(0, 1, 0))
    piston.frame("slide", origin=(0, 0, 0), z=(1, 0, 0))
    piston.check()

    # ---- The mechanism: every mate between named frames.
    rig = app.assembly("slider_crank")
    b = rig.place(block, fixed=True)
    c = rig.place(crank)
    r = rig.place(rod)
    p = rig.place(piston, at=(tdc, 0, 0))
    axle = rig.revolute((b, "crank_axis"), (c, "axle"), flipped=False)
    rig.revolute((c, "pin"), (r, "big_end"), flipped=False)
    rig.revolute((r, "small_end"), (p, "wrist_pin"), flipped=False)
    rig.slider((b, "bore_axis"), (p, "slide"), flipped=False)
    rep = rig.solve()
    return rig, axle["id"], rep, {"tdc": tdc, "bdc": bdc}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    ap.add_argument("--crank", type=float, default=20.0)
    ap.add_argument("--rod", type=float, default=70.0)
    a = ap.parse_args()
    app = connect(a.port)
    rig, axle, rep, geom = build(app, a.crank, a.rod)
    print("solve:", rep)
    study = app.add_motion_study(driver=axle, dof="rotation", **{"from": 0},
                                 to=360, samples=49, name="crank")
    r = app.run_motion_study(study=study["id"], include_poses=True)
    print(r["table"])
    piston = rig.occurrence("piston")
    xs = [next(pp["position"][0] for pp in s["poses"]
               if pp["occurrence"] == piston.id) for s in r["samples"]]
    stroke = max(xs) - min(xs)
    print(f"piston stroke {stroke:.3f} mm (2R = {2 * a.crank:.3f}), "
          f"clean={r['clean']}, {r['pairs_checked']} pair checks in "
          f"{r['seconds']:.2f}s")
    if abs(stroke - 2 * a.crank) > 0.05:
        raise SdkError(f"stroke {stroke:.3f} is not 2R = {2 * a.crank:.3f}")
    if not r["clean"]:
        raise SdkError("the mechanism collides — see the table above")
    return 0


if __name__ == "__main__":
    sys.exit(main())
