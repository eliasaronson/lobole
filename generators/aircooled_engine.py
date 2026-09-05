#!/usr/bin/env python3
"""An air-cooled single, built from SKETCHES ONLY — no Box, Sphere or Cylinder.

The same mechanism as generators/single_cylinder.py (86 x 86 bore and stroke,
150 mm rod, two valves on a 2:1 chain), so the kinematics are already proven
and this round is about the SHAPES: every body here starts as a drawn profile
that is extruded or revolved, the way the part is actually described.

That is the point of the exercise. A primitive is fast and wrong-looking: a
piston is not a cylinder with grooves subtracted, it is a turned profile; a
barrel is not a tube, it is a finned section revolved; a crank web is not a
disc, it is the shape between two journals. Building it the second way is
more drawing and less boolean, and it is the way the model stays editable —
change FIN_PITCH and the barrel re-fins itself.

    python3 generators/aircooled_engine.py --port <port> [--out DIR]

Findings from the round go in docs/modeling/aircooled-dogfood.md.
"""
import argparse
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from designer_sdk import Plane, SdkError, connect  # noqa: E402

# ---- the mechanism, unchanged from v1 so the motion is already proven -----
BORE = 86.0
CRANK_R = 43.0          # half the stroke
ROD_L = 150.0
PIN_HEIGHT = 30.0       # wrist pin to crown
SKIRT = 25.0            # wrist pin to skirt bottom
DECK = CRANK_R + ROD_L + PIN_HEIGHT + 6.0   # squish at TDC
BARREL_Z = 82.0         # the barrel's underside, clear of the rod's swing
WEB_R = 45.0
CAM_LIFT = 9.0
VALVE_D = 34.0

# ---- what makes it air-cooled --------------------------------------------
BORE_R = BORE / 2       # 43
WALL_R = 52.0           # the barrel's own wall, outside the bore
FIN_R = 72.0            # the fins reach this far
FIN_T = 5.0             # fin thickness
FIN_PITCH = 15.0        # fin every this far up
FLANGE_R = 78.0         # the deck flange the head bolts through
FLANGE_T = 14.0
BOLT_R = 65.0           # bolt circle radius, inside the flange


def bolt_xy():
    """Four head bolts at 45 degrees, derived from the bolt circle rather
    than written out — the flange radius is the only number to change."""
    return [(BOLT_R * math.cos(math.radians(a)),
             BOLT_R * math.sin(math.radians(a)))
            for a in (45, 135, 225, 315)]


def finned_wall(z0, z1, inner, outer, first=6.0, last=6.0):
    """The (radius, z) points of a finned outer wall, bottom to top.

    One fin every FIN_PITCH, each standing FIN_T proud — the teeth of a
    revolve profile. Returned as points rather than drawn, so a caller can
    put them in the middle of a bigger outline.
    """
    pts = []
    z = z0 + first
    while z + FIN_T <= z1 - last:
        pts += [(inner, z), (outer, z), (outer, z + FIN_T), (inner, z + FIN_T)]
        z += FIN_PITCH
    return pts


def barrel(app):
    """The finned barrel: ONE revolved profile for the bore, the wall, every
    fin and the deck flange, plus four blind bolt holes."""
    p = app.part("barrel")
    (p.var("bore", BORE).var("deck", DECK).var("crank_r", CRANK_R)
      .var("rod_l", ROD_L).var("fin_pitch", FIN_PITCH))
    # Bottom face outward, up the finned outside, across the flange, back
    # down the bore. `close=True` returns the last point to the first.
    prof = ([(BORE_R, BARREL_Z), (WALL_R, BARREL_Z)]
            + finned_wall(BARREL_Z, DECK - FLANGE_T, WALL_R, FIN_R)
            + [(WALL_R, DECK - FLANGE_T), (FLANGE_R, DECK - FLANGE_T),
               (FLANGE_R, DECK), (BORE_R, DECK)])
    p.revolve_profile(prof, plane=Plane.xz(), axis="z", name="Barrel")
    # A FULL-HEIGHT boss at each bolt, before the holes are drilled or they
    # would be filled in again.
    #
    # It has to reach INSIDE the wall to be part of the barrel. A boss at
    # r=65 spans r=50..80, which overlaps the r=52 wall by two millimetres and
    # merges; the fins, which reach r=72, run into it. That is what an
    # air-cooled barrel's through-bolt spigot looks like.
    #
    # The first attempt relieved the fins with a d=38 cut and then added a
    # d=30 boss. The relief reaches r=46 and the boss only r=50, so it ate
    # four millimetres of the wall the boss needed to land on: four pillars
    # hanging off the deck flange with daylight behind them, one solid body
    # (so nothing complained) and obviously wrong the moment you look at it.
    # Nothing here is a substitute for looking at the part.
    #
    # Full height rather than 70: below the boss the fins would cross the
    # bolt axis again, which is the geometry that made `fit_fastener` refuse
    # in the first place.
    boss = p.sketch(Plane.xy(DECK, flip=True), name="Bolt bosses")
    for x, y in bolt_xy():
        boss.circle((x, y), d=30.0)
    p.extrude(boss.add(), DECK - BARREL_Z, mode="add", name="Bolt bosses")
    holes = p.sketch(Plane.xy(DECK, flip=True), name="Head bolt holes")
    for x, y in bolt_xy():
        holes.circle((x, y), d=11.0)
    p.extrude(holes.add(), 30.0, mode="cut", name="Head bolt holes")
    p.frame("crank_axis", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.frame("bore_axis", origin=(0, 0, BARREL_Z), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("deck", origin=(0, 0, DECK), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def head(app):
    """A finned head: a revolved body with a bolt flange, then the valve
    gear cut into it."""
    p = app.part("head")
    p.var("bore", BORE)
    prof = ([(0.0, 0.0), (FLANGE_R, 0.0), (FLANGE_R, FLANGE_T),
             (WALL_R, FLANGE_T)]
            + finned_wall(FLANGE_T, 60.0, WALL_R, FIN_R, first=2.0, last=2.0)
            + [(WALL_R, 60.0), (0.0, 60.0)])
    p.revolve_profile(prof, plane=Plane.xz(), axis="z", name="Head")
    # Two valve guides, bored through, either side of the bore's axis.
    guides = p.sketch(Plane.xy(-5), name="Guides")
    for x in (-22, 22):
        # 1.5 mm radial on an 11 stem. The mesh check's deflection on a part
        # this size is 0.39 mm, so a half-millimetre fit reads as a clash.
        guides.circle((x, 0), d=14)
    p.extrude(guides.add(), 75.0, mode="cut", name="Guides")
    # The ports the valves seat against: a counterbore under each guide, deep
    # enough to swallow the whole seat cone.
    ports = p.sketch(Plane.xy(-5), name="Ports")
    for x in (-22, 22):
        ports.circle((x, 0), d=VALVE_D + 4)
    p.extrude(ports.add(), 20.0, mode="cut", name="Ports")
    # A seat for each spring, so its bottom coils are not inside the head.
    seats = p.sketch(Plane.xy(62, flip=True), name="Spring seats")
    for x in (-22, 22):
        seats.circle((x, 0), d=26)
    p.extrude(seats.add(), 20.0, mode="cut", name="Spring seats")
    bolts = p.sketch(Plane.xy(-1), name="Bolt holes")
    for x, y in bolt_xy():
        bolts.circle((x, y), d=11.0)
    p.extrude(bolts.add(), FLANGE_T + 2, mode="cut", name="Bolt holes")
    p.frame("deck", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("cam_axis", origin=(0, 0, 110), z=(0, 1, 0), x=(1, 0, 0))
    for i, x in enumerate((-22, 22)):
        # Z points DOWN the guide, so the cam's positive lift OPENS the valve
        # into the cylinder rather than pulling it up into the head.
        p.frame(f"guide_{i}", origin=(x, 0, 0), z=(0, 0, -1), x=(1, 0, 0))
    p.check()
    return p


def crank(app):
    """Two stub journals, a pin, and webs shaped between them.

    A web is a SLOT — the rounded shape spanning two journal centres — which
    is what a crank web is, rather than a disc with a lump stuck on it.
    Plane.xz's normal is -Y, so an extrude from Plane.xz(a) travels toward
    -Y and every sketch below is placed at the +Y end of the feature it makes.
    """
    p = app.part("crank")
    p.var("crank_r", CRANK_R)
    for name, y, length, d in (("Journal L", -46.0, 24.0, 50.0),
                               ("Journal R", 70.0, 24.0, 50.0)):
        sk = p.sketch(Plane.xz(y), name=name)
        sk.circle((0, 0), d=d)
        p.extrude(sk.add(), length, mode="add" if name != "Journal L"
                  else "new_body", name=name)
    # The pin: 2 mm diametral clearance in the rod's 46 bore.
    pin = p.sketch(Plane.xz(30), name="Crank pin")
    pin.circle((0, "crank_r"), d=42)
    p.extrude(pin.add(), 60.0, mode="add", name="Crank pin")
    for y in (-16, 46):
        # TWO sketches, because a sketch REFUSES loops that overlap ("profile
        # loops overlap or nest inconsistently") — it will not union them for
        # you. A silhouette drawn as overlapping shapes is therefore one
        # extrude per shape, each adding to the last. More calls, and no
        # guess about which loop was meant to be a hole.
        # 64 across, not 90: a slot's end cap is HALF ITS WIDTH past the
        # centre it ends on, so a 90-wide web round a pin at z=43 reached
        # z=85 — and the barrel's underside is at 82. The motion study called
        # it `barrel x crank` at seven of the 49 samples. A disc centred on
        # the axis (what v1 used) hides this: the shape only grows where you
        # look at it.
        w = p.sketch(Plane.xz(y), name="Web")
        w.slot((0, 0), (0, CRANK_R), width=64.0)
        p.extrude(w.add(), 30.0, mode="add", name="Web")
        cw = p.sketch(Plane.xz(y), name="Counterweight")
        cw.slot((0, 0), (0, -34), width=2 * WEB_R)
        p.extrude(cw.add(), 30.0, mode="add", name="Counterweight")
    p.frame("main", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.frame("pin", origin=(0, 0, "crank_r"), z=(0, 1, 0), x=(1, 0, 0))
    p.check()
    return p


def rod(app):
    """A rod drawn as its own silhouette: two round ends and a waisted shank
    between them, extruded to thickness."""
    p = app.part("rod")
    p.var("rod_l", ROD_L).var("bolt", 8.0)
    # One extrude per overlapping shape (see the crank's note): the shank
    # first, then a boss at each end.
    shank = p.sketch(Plane.xz(14), name="Shank")
    # Waisted in the middle, so it reads as a rod rather than a bar. Narrow
    # enough that its outer edge clears the bore mouth at full tilt, which
    # the v1 study caught at 36 across.
    shank.polyline([(-16, 0), (-11, ROD_L * 0.45), (-13, ROD_L),
                    (13, ROD_L), (11, ROD_L * 0.45), (16, 0)])
    p.extrude(shank.add(), 28.0, mode="new_body", name="Shank")
    for name, z, d in (("Big end", 0.0, 70.0), ("Small end", ROD_L, 40.0)):
        boss = p.sketch(Plane.xz(14), name=name)
        boss.circle((0, z), d=d)
        p.extrude(boss.add(), 28.0, mode="add", name=name)
    bores = p.sketch(Plane.xz(14), name="Bores")
    bores.circle((0, 0), d=46)
    bores.circle((0, "rod_l"), d=26)
    p.extrude(bores.add(), 30.0, mode="cut", name="Bores")
    bolts = p.sketch(Plane.xz(14), name="Bolt holes")
    bolts.circle((-30, 0), d="bolt").circle((30, 0), d="bolt")
    p.extrude(bolts.add(), 30.0, mode="cut", name="Bolt holes")
    p.frame("big_end", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.frame("small_end", origin=(0, 0, ROD_L), z=(0, 1, 0), x=(1, 0, 0))
    p.check()
    return p


def piston(app):
    """A turned piston: crown, three ring grooves, a dished underside and a
    skirt, all in ONE revolved profile. Its origin is the wrist pin."""
    p = app.part("piston")
    p.var("bore", BORE).var("pin_h", PIN_HEIGHT)
    r = BORE / 2 - 0.25   # running clearance
    prof = [(0.0, PIN_HEIGHT), (r, PIN_HEIGHT)]
    # Three ring grooves cut into the outside as the profile descends.
    for i in range(3):
        z = PIN_HEIGHT - 6.0 - 6.0 * i
        prof += [(r, z), (r - 4.0, z), (r - 4.0, z - 2.5), (r, z - 2.5)]
    prof += [(r, -SKIRT), (r - 6.0, -SKIRT),      # skirt bottom
             (r - 6.0, -12.0), (20.0, -12.0),     # up inside to the pin boss
             (20.0, 14.0), (0.0, 14.0)]           # dished under the crown
    p.revolve_profile(prof, plane=Plane.xz(), axis="z", name="Piston")
    pin = p.sketch(Plane.xz(60), name="Pin bore")
    pin.circle((0, 0), d=30)
    p.extrude(pin.add(), 120.0, mode="cut", name="Pin bore")
    # The slot the rod's small end swings in, open at the skirt.
    slot = p.sketch(Plane.xz(20), name="Rod slot")
    slot.rect((-26, -SKIRT - 5), (26, 24))
    p.extrude(slot.add(), 40.0, mode="cut", name="Rod slot")
    p.frame("wrist_pin", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.frame("slide", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def valve(app):
    """A valve turned from one profile, with its spring. Its origin is on the
    seat, so a lift of zero is closed."""
    p = app.part("valve")
    p.var("valve_d", VALVE_D)
    prof = [(0, 0), ("valve_d / 2", 0), ("valve_d / 2", 3),
            (5.5, 10), (5.5, 118), (0, 118)]
    p.revolve_profile(prof, plane=Plane.xz(), axis="z", name="Valve")
    p.app.add_feature(feature={
        "type": "Curve3D", "name": "Spring path", "kind": "helix",
        "origin": [0, 0, 62], "axis": [0, 0, 1], "radius": 11.0,
        "pitch": 7.0, "turns": 4.0})
    coil = p.state()["features"][-1]["id"]
    p.app.add_feature(feature={
        "type": "Pipe", "name": "Spring", "path_id": coil,
        # SOLID wire: a spring is round bar coiled up, not tubing.
        "outer_diameter": 3.2, "mode": "add"})
    # Z points DOWN the valve, to match the guide's.
    p.frame("seat", origin=(0, 0, 0), z=(0, 0, -1), x=(1, 0, 0))
    p.check()
    return p


def camshaft(app):
    """A shaft with two real EGG-shaped lobes — drawn, not a disc. The lift
    the valves actually follow is the cam relation's table; this is the
    shape that says so."""
    p = app.part("camshaft")
    shaft = p.sketch(Plane.xz(70), name="Shaft")
    shaft.circle((0, 0), d=26)
    p.extrude(shaft.add(), 140.0, mode="new_body", name="Shaft")
    for y in (-14, 30):
        # Base circle, then the nose on top of it — two extrudes, because
        # they overlap (see the crank's note).
        base = p.sketch(Plane.xz(y), name="Lobe base")
        base.circle((0, 0), d=30)
        p.extrude(base.add(), 16.0, mode="add", name="Lobe base")
        nose = p.sketch(Plane.xz(y), name="Lobe nose")
        nose.slot((0, 0), (0, CAM_LIFT), width=22.0)
        p.extrude(nose.add(), 16.0, mode="add", name="Lobe nose")
    p.frame("axle", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.check()
    return p


def bottom_end(app):
    """The four parts and the four mates that make them a mechanism."""
    b, c, r, pi = barrel(app), crank(app), rod(app), piston(app)
    rig = app.assembly("engine")
    bo = rig.place(b, fixed=True)
    co = rig.place(c)
    ro = rig.place(r)
    po = rig.place(pi, at=(0, 0, CRANK_R + ROD_L))
    main = rig.revolute((bo, "crank_axis"), (co, "main"), flipped=False)
    rig.revolute((co, "pin"), (ro, "big_end"), flipped=False)
    rig.revolute((ro, "small_end"), (po, "wrist_pin"), flipped=False)
    rig.slider((bo, "bore_axis"), (po, "slide"), flipped=False)
    print("  bottom end solve:", rig.solve())
    return rig, {"barrel": bo, "crank": co, "rod": ro, "piston": po}, main


def top_end(app, rig, occ, main_mate):
    """Head, two valves, camshaft. The camshaft runs at half crank speed on a
    CHAIN relation; each valve is lifted by a CAM relation off it."""
    h, v, c = head(app), valve(app), camshaft(app)
    b = app.part("barrel")
    ho = rig.place(h, at=(0, 0, DECK))
    rig.fasten((occ["barrel"], "deck"), (ho, "deck"), flipped=False)
    # Four ISO 4762 cap screws, each derived from the hole it goes in, and
    # each told WHERE THE JOINT ENDS.
    #
    # The second pick is not optional here and the app is right to want it.
    # A head bolt's axis carries on down past the barrel and clips a crank
    # web — the webs reach r=45 and the bolt circle is at r=65, so the probe
    # bundle finds crank material 158 mm further down. Without a far end the
    # traversal has the blind hole blocked in a segment that is not the last
    # one, and refuses: "its hole does not go all the way through, so a
    # fastener cannot reach the parts beyond it". True, and the fix is to say
    # the joint stops at the bottom of the blind hole, which is a rim on the
    # barrel directly under the one being picked.
    top = h.body().edges(circular=True, radius=5.5,
                         where=lambda e: e.center.z > 13.0)
    floors = b.body().edges(circular=True, radius=5.5,
                            where=lambda e: e.center.z < DECK - 1.0)
    if len(top) != 4 or len(floors) != 4:
        raise SdkError(f"{len(top)} bolt rims on the head flange and "
                       f"{len(floors)} hole floors in the barrel, expected 4")
    for rim in top:
        floor = min(floors, key=lambda e: (e.center.x - rim.center.x) ** 2
                    + (e.center.y - rim.center.y) ** 2)
        fit = rig.bolt(rim, ho, standard="iso4762", size="M10",
                       far_end={**floor.ref, "occurrence": occ["barrel"].id})
        print(f"  head bolt: {fit['size']} x {fit['length_mm']} mm "
              f"({fit['summary']})")
    cam_occ = rig.place(c, at=(0, 0, DECK + 110))
    cam_mate = rig.revolute((ho, "cam_axis"), (cam_occ, "axle"), flipped=False)
    rig.chain(main_mate, cam_mate, teeth_a=1, teeth_b=2, name="timing chain")
    valves = []
    for i in range(2):
        vo = rig.place(v, at=(-22 + 44 * i, 0, DECK))
        slide = rig.slider((ho, f"guide_{i}"), (vo, "seat"), flipped=False)
        start = 20.0 + 180.0 * i
        rig.cam(cam_mate, slide, rise=CAM_LIFT, duration_deg=150,
                start_deg=start, name=f"lobe {i + 1}")
        valves.append(vo)
    print("  top end solve:", rig.solve())
    return {"head": ho, "camshaft": cam_occ, "valves": valves}, cam_mate


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    ap.add_argument("--out", default="/tmp/aircooled")
    a = ap.parse_args()
    app = connect(a.port)
    app.new()
    rig, occ, main_mate = bottom_end(app)
    top, cam_mate = top_end(app, rig, occ, main_mate)
    for part in app.list_parts()["parts"]:
        if part["name"] == "Part 1":
            app.delete_part(part=part["name"])

    # ---- the kinematic gate: two revolutions, every 15 degrees ----------
    rig.activate()
    study = rig.motion(main_mate, dof="rotation", **{"from": 0}, to=720,
                       samples=49, name="two revolutions")
    r = app.run_motion_study(study=study["id"], include_poses=True)
    print(r["table"])
    piston_occ = rig.occurrence("piston")
    zs = [next(pp["position"][2] for pp in s["poses"]
               if pp["occurrence"] == piston_occ.id) for s in r["samples"]]
    stroke = max(zs) - min(zs)
    print(f"  piston stroke {stroke:.3f} mm (2R = {2 * CRANK_R:.3f}), "
          f"clean={r['clean']}, {r['pairs_checked']} pair checks "
          f"({r['pairs_rigid']} skipped as rigid) in {r['seconds']:.2f}s")
    if abs(stroke - 2 * CRANK_R) > 0.05:
        raise SdkError(f"stroke {stroke:.3f} is not 2R")
    for i, vo in enumerate(top["valves"]):
        lift = [next(pp["position"][2] for pp in s["poses"]
                     if pp["occurrence"] == vo.id) for s in r["samples"]]
        print(f"  valve {i + 1}: lift {max(lift) - min(lift):.2f} mm "
              f"(cam lobe {CAM_LIFT})")
    if not r["clean"]:
        raise SdkError("the mechanism collides — see the table above")

    os.makedirs(a.out, exist_ok=True)
    glb = os.path.join(a.out, "aircooled.glb")
    ex = app.export_motion(study=study["id"], path=glb)
    print(f"  animation: {ex['nodes']} nodes x {ex['frames']} keyframes -> "
          f"{glb} ({os.path.getsize(glb) // 1024} kB)")
    project = os.path.join(a.out, "aircooled.dsgn")
    app.save_project(path=project)
    print(f"  project: {project}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
