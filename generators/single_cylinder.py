#!/usr/bin/env python3
"""A single-cylinder engine, on the SDK — the SDK's acceptance
test (generators/README.md#single-cylinder-and-air-cooled-engines).

    python3 generators/single_cylinder.py --port <port>

Everything the round built, used together on one part a person would
recognise: named frames so nothing is placed by a transcribed number, the SDK
for the geometry, batches where the calls are independent, a chain relation
driving the camshaft at half crank speed, cam relations lifting two valves,
and a motion study over 720 degrees that checks every pose.

The engine: bore 86, stroke 86 (crank radius 43, rod 150), which is the W16's
own cylinder. Findings go in generators/README.md#single-cylinder-and-air-cooled-engines.
"""
import argparse
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from designer_sdk import Plane, SdkError, V, connect  # noqa: E402

# The engine's own numbers. Everything else is derived from these.
BORE = 86.0
CRANK_R = 43.0          # half the stroke
ROD_L = 150.0
PIN_HEIGHT = 30.0       # wrist pin to crown
DECK = CRANK_R + ROD_L + PIN_HEIGHT + 6.0   # squish at TDC
SKIRT = 25.0            # wrist pin to skirt bottom
WEB_R = 45.0            # crank web radius: under the barrel, beside the rod
BARREL_Z = 82.0         # the barrel's underside, clear of the rod's swing
CAM_LIFT = 9.0
VALVE_D = 34.0
# One list for both halves of the joint: the block's blind holes and the
# head's through holes are the same four centres, not two sets of literals
# that agree until one of them is edited.
BOLT_CIRCLE = [(-55.0, -45.0), (55.0, -45.0), (-55.0, 45.0), (55.0, 45.0)]


def block(app):
    """The barrel: a bored block standing over the crank, with a deck face
    and a bolt circle for the head."""
    p = app.part("block")
    (p.var("bore", BORE).var("deck", DECK).var("crank_r", CRANK_R)
      .var("rod_l", ROD_L))
    # The barrel starts clear of the crank's swing: the webs reach WEB_R from
    # the axis, and the rod's big end swings wider still.
    p.box(position=(-70, -60, BARREL_Z), w=140, h=120, d=DECK - BARREL_Z,
          name="Barrel")
    hole = p.sketch(Plane.xy(BARREL_Z - 5), name="Bore")
    hole.circle((0, 0), d="bore")
    p.extrude(hole.add(), DECK - BARREL_Z + 10, mode="cut", name="Bore")
    # The head bolt circle: four blind holes in the deck, clear of the bore
    # (r 43) and of the head's valve guides.
    deck_holes = p.sketch(Plane.xy(DECK, flip=True), name="Head bolt holes")
    for x, y in BOLT_CIRCLE:
        deck_holes.circle((x, y), d=11.0)
    p.extrude(deck_holes.add(), 25.0, mode="cut", name="Head bolt holes")
    # The frames the assembly mates to: the crank axis (along Y), the bore
    # axis (along Z), and the deck the head bolts to.
    p.frame("crank_axis", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.frame("bore_axis", origin=(0, 0, BARREL_Z), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("deck", origin=(0, 0, DECK), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def crank(app):
    """Main journal along Y, a pin offset by the crank radius, and two webs
    weighted opposite it."""
    p = app.part("crank")
    p.var("crank_r", CRANK_R)
    # Two stub journals, OUTBOARD of the webs — not one shaft through the
    # middle. A journal running the full length passes exactly through where
    # the rod's big end has to be, and the motion study reported it as a
    # crank/rod clash at every angle.
    p.cylinder(center=(0, -70, 0), direction=(0, 1, 0), d=50, h=24,
               name="Journal L")
    p.cylinder(center=(0, 46, 0), direction=(0, 1, 0), d=50, h=24,
               name="Journal R")
    # 2 mm diametral clearance in the rod's 46 bore: a running fit, and well
    # outside the collision mesh's deflection on a part this size.
    p.cylinder(center=(0, -30, CRANK_R), direction=(0, 1, 0), d=42, h=60,
               name="Crank pin")
    for y in (-46, 16):
        # A web either side of the rod, radius WEB_R so it passes under the
        # barrel and 1 mm off the rod's cheeks.
        p.cylinder(center=(0, y, 0), direction=(0, 1, 0), d=2 * WEB_R, h=30,
                   name="Web")
        # The counterweight: material OPPOSITE the pin, which is what makes a
        # crank a crank rather than two discs.
        cw = p.sketch(Plane.xz(y + 30), name="Counterweight")
        cw.polyline([(-38, -18), (38, -18), (30, -WEB_R - 14),
                     (-30, -WEB_R - 14)])
        p.extrude(cw.add(), 30.0, mode="add", name="Counterweight")
    p.frame("main", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.frame("pin", origin=(0, 0, CRANK_R), z=(0, 1, 0), x=(1, 0, 0))
    p.check()
    return p


def rod(app):
    """Big end on its own origin, small end `rod_l` along +Z, and two bolts
    through the big-end cap."""
    p = app.part("rod")
    p.var("rod_l", ROD_L).var("bolt", 8.0)
    # The shank stops just past the small end: at +34 it pushed through the
    # piston's crown, which sits 30 above the wrist pin.
    # 32 across, not 36: at its worst tilt the shank's outer edge came within
    # a millimetre of the bore's mouth, and the study called it.
    p.box(position=(-16, -14, -34), w=32, h=28, d=ROD_L + 50, name="Shank")
    # The big end is wider than the shank: the bolts have to clear its bore
    # (45 dia) and still be inside material. At the shank's own 36 they were
    # in fresh air, and the app said so.
    p.box(position=(-35, -14, -34), w=70, h=28, d=54, name="Big end")
    bores = p.sketch(Plane.xz(14), name="Bores")
    bores.circle((0, 0), d=46)
    bores.circle((0, "rod_l"), d=26)
    p.extrude(bores.add(), 30.0, mode="cut", name="Bores")
    # Two bolts across the big end, clear of its bore.
    bolts = p.sketch(Plane.xz(14), name="Bolt holes")
    bolts.circle((-30, 0), d="bolt").circle((30, 0), d="bolt")
    p.extrude(bolts.add(), 30.0, mode="cut", name="Bolt holes")
    p.frame("big_end", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.frame("small_end", origin=(0, 0, ROD_L), z=(0, 1, 0), x=(1, 0, 0))
    p.check()
    return p


def piston(app):
    """Crown, three ring grooves, wrist-pin bore, skirt. Its origin is the
    wrist pin, so the rod mates to it without an offset."""
    p = app.part("piston")
    p.var("bore", BORE).var("pin_h", PIN_HEIGHT)
    p.cylinder(center=(0, 0, -SKIRT), direction=(0, 0, 1), d=BORE - 0.5,
               h=SKIRT + PIN_HEIGHT, name="Piston")
    # Ring grooves: revolved cuts, the way a lathe makes them.
    for i in range(3):
        z = PIN_HEIGHT - 6.0 - 6.0 * i
        g = p.sketch(Plane.xz(), name=f"Ring {i + 1}")
        g.polyline([("bore / 2 - 3", z), ("bore / 2 + 1", z),
                    ("bore / 2 + 1", z - 2.5), ("bore / 2 - 3", z - 2.5)])
        p.revolve(g.add(), axis="z", mode="cut", name=f"Ring groove {i + 1}")
    # Plane.xz's normal is -Y, and an extrude goes along +normal: starting at
    # y = -60 the cut travels AWAY from the piston. It starts at +60.
    pin = p.sketch(Plane.xz(60), name="Pin bore")
    pin.circle((0, 0), d=30)
    p.extrude(pin.add(), 120.0, mode="cut", name="Pin bore")
    # The slot the rod's small end swings in, open at the skirt: without it
    # the rod is buried in the piston at every angle, which is what the
    # motion study reported.
    slot = p.sketch(Plane.xz(20), name="Rod slot")
    # Tall enough for the rod's top CORNER, which sweeps
    # sqrt(16^2 + 18^2) = 24.1 about the wrist pin as the rod swings.
    # The rod's top corner sweeps sqrt(16^2 + 18^2) = 24.1 about the wrist
    # pin, in BOTH x and z as it swings.
    slot.rect((-26, -SKIRT - 5), (26, 27))
    p.extrude(slot.add(), 40.0, mode="cut", name="Rod slot")
    p.frame("wrist_pin", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.frame("slide", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def bottom_end(app):
    """The four parts and the four mates that make them a mechanism."""
    b, c, r, pi = block(app), crank(app), rod(app), piston(app)
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
    return rig, {"block": bo, "crank": co, "rod": ro, "piston": po}, main


def head(app):
    """The head: a deck plate with two valve guides, and a camshaft bearing
    above them."""
    p = app.part("head")
    p.var("bore", BORE)
    p.box(position=(-70, -60, 0), w=140, h=120, d=60, name="Head")
    # Two valve guides, bored through, either side of the bore's axis.
    guides = p.sketch(Plane.xy(-5), name="Guides")
    for x in (-22, 22):
        # 1.5 mm radial on an 11 stem. The mesh check's deflection on a part
        # this size is 0.39 mm, so a half-millimetre fit is inside its
        # resolution and reads as a clash — a real running clearance is not.
        guides.circle((x, 0), d=14)
    p.extrude(guides.add(), 75.0, mode="cut", name="Guides")
    # The ports the valves seat against: a counterbore under each guide.
    ports = p.sketch(Plane.xy(-5), name="Ports")
    for x in (-22, 22):
        ports.circle((x, 0), d=VALVE_D + 4)
    # Deep enough to swallow the valve's whole seat cone: at 14 the cone was
    # still 7.1 wide where the 7.0 guide began, and the boolean said so —
    # 161 mm3 of steel in the same place twice.
    p.extrude(ports.add(), 20.0, mode="cut", name="Ports")
    # A spring seat around each guide: the spring compresses by the lift, and
    # without somewhere to go its bottom coils are inside the head's top face.
    # flip=True so the plane's normal points DOWN and the cut goes into the
    # head: without it the recess was 20 mm of fresh air above the top face,
    # and the app said it removed nothing.
    seats = p.sketch(Plane.xy(62, flip=True), name="Spring seats")
    for x in (-22, 22):
        seats.circle((x, 0), d=26)
    p.extrude(seats.add(), 20.0, mode="cut", name="Spring seats")
    # The other half of the bolt circle: through the head, on the block's
    # centres, so the two agree by sharing ONE list and not by two literals.
    through = p.sketch(Plane.xy(-5), name="Head bolt holes")
    for x, y in BOLT_CIRCLE:
        through.circle((x, y), d=11.0)
    p.extrude(through.add(), 75.0, mode="cut", name="Head bolt holes")
    p.frame("deck", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    # Above the spring stacks (which end 90 up), so the lobes clear them.
    p.frame("cam_axis", origin=(0, 0, 110), z=(0, 1, 0), x=(1, 0, 0))
    for i, x in enumerate((-22, 22)):
        # Z points DOWN the guide, so the cam's positive lift OPENS the
        # valve into the cylinder rather than pulling it up into the head.
        p.frame(f"guide_{i}", origin=(x, 0, 0), z=(0, 0, -1), x=(1, 0, 0))
    p.check()
    return p


def valve(app):
    """A valve: a stem on the guide's axis with a head at the bottom. Its
    origin is on the seat, so a lift of zero is closed."""
    p = app.part("valve")
    p.var("valve_d", VALVE_D)
    # Everything ABOVE the seat plane: a closed valve must not hang into the
    # cylinder, which is what put it through the piston crown at TDC.
    prof = [(0, 0), ("valve_d / 2", 0), ("valve_d / 2", 3),
            (5.5, 10), (5.5, 118), (0, 118)]
    p.revolve_profile(prof, plane=Plane.xz(), axis="z", name="Valve")
    # Z points DOWN the valve, to match the guide's: aligning two frames
    # matches Z to Z, so a valve whose frame pointed UP was mated upside
    # down and hung its whole length into the cylinder.
    # The spring: a helix around the stem, drawn as a pipe along it. Free
    # length 46, so it is compressed but never coil-bound over the lift.
    p.app.add_feature(feature={
        "type": "Curve3D", "name": "Spring path", "kind": "helix",
        # Seated ON the head (its top face is 60 up), not around the guide:
        # a spring inside the head's material is 11 mm of steel in a 7 mm
        # bore, which the study reported at every angle.
        "origin": [0, 0, 62], "axis": [0, 0, 1], "radius": 11.0,
        "pitch": 7.0, "turns": 4.0})
    coil = p.state()["features"][-1]["id"]
    p.app.add_feature(feature={
        "type": "Pipe", "name": "Spring", "path_id": coil,
        # SOLID wire (no wall_thickness): a spring is round bar coiled up, not
        # tubing. It was a tube, which is both wrong and expensive — the bore
        # is a second swept B-spline surface for the drawing's hidden-line
        # pass to classify against, and dropping it took one spring's HLR from
        # 7.3 s to 2.5 s.
        "outer_diameter": 3.2, "mode": "add"})
    p.frame("seat", origin=(0, 0, 0), z=(0, 0, -1), x=(1, 0, 0))
    p.check()
    return p


def camshaft(app):
    """A shaft along Y with two lobes — drawn round, because the LIFT is the
    cam relation's table, not this geometry. The lobes are here so the part
    looks like a camshaft and so the study has something to check."""
    p = app.part("camshaft")
    p.cylinder(center=(0, -70, 0), direction=(0, 1, 0), d=26, h=140,
               name="Shaft")
    for y in (-30, 14):
        p.cylinder(center=(0, y, 0), direction=(0, 1, 0), d=34, h=16,
                   name="Lobe")
    p.frame("axle", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.check()
    return p


def top_end(app, rig, occ, main_mate):
    """Head, two valves, camshaft. The camshaft runs at half crank speed on a
    CHAIN relation (a chain does not reverse), and each valve is lifted by a
    CAM relation off the camshaft's own rotation."""
    h, v, c = head(app), valve(app), camshaft(app)
    ho = rig.place(h, at=(0, 0, DECK))
    rig.fasten((occ["block"], "deck"), (ho, "deck"), flipped=False)
    # Four real ISO 4762 cap screws, each derived from the hole it goes in:
    # the LENGTH comes from the stack the axis passes through and nothing
    # here says a length. The size is said outright, because 11 mm is a
    # clearance for two threads (M9 Loose and M10 Normal) and the app refuses
    # to pick for us — see generators/README.md#single-cylinder-and-air-cooled-engines. Drilling
    # these with a Hole feature would record the fit and need no `size`.
    top = h.body().edges(circular=True, radius=5.5,
                         where=lambda e: e.center.z > 59.0)
    if len(top) != len(BOLT_CIRCLE):
        raise SdkError(f"{len(top)} bolt rims on the head's top face, "
                       f"expected {len(BOLT_CIRCLE)}")
    for rim in top:
        fit = rig.bolt(rim, ho, standard="iso4762", size="M10")
        print(f"  head bolt: {fit['size']} x {fit['length_mm']} mm "
              f"({fit['summary']})")
    cam_occ = rig.place(c, at=(0, 0, DECK + 110))
    cam_mate = rig.revolute((ho, "cam_axis"), (cam_occ, "axle"), flipped=False)
    # Half crank speed, same direction: a chain, not a gear.
    rig.chain(main_mate, cam_mate, teeth_a=1, teeth_b=2, name="timing chain")

    valves = []
    for i in range(2):
        vo = rig.place(v, at=(-22 + 44 * i, 0, DECK))
        slide = rig.slider((ho, f"guide_{i}"), (vo, "seat"), flipped=False)
        # Inlet opens on the intake stroke, exhaust half a turn later: the
        # phase is the camshaft angle at which each lobe peaks.
        start = 20.0 + 180.0 * i
        rig.cam(cam_mate, slide, rise=CAM_LIFT, duration_deg=150,
                start_deg=start, name=f"lobe {i + 1}")
        valves.append(vo)
    print("  top end solve:", rig.solve())
    return {"head": ho, "camshaft": cam_occ, "valves": valves}, cam_mate


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    ap.add_argument("--exact-drawing", action="store_true",
                    help="project the sheet with exact HLR (~84 s a view) "
                         "instead of the fast polygonal one")
    ap.add_argument("--out", default="/tmp/single_cylinder")
    a = ap.parse_args()
    app = connect(a.port)
    app.new()
    rig, occ, main_mate = bottom_end(app)
    top, cam_mate = top_end(app, rig, occ, main_mate)
    # new() opens an empty default part; it is not one of ours and would ship
    # in the saved project as an empty tab.
    for part in app.list_parts()["parts"]:
        if part["name"] == "Part 1":
            app.delete_part(part=part["name"])

    # ---- the kinematic gate: two revolutions, every 15 degrees ----------
    study = rig.motion(main_mate, dof="rotation", **{"from": 0}, to=720,
                       samples=49, name="two revolutions")
    r = app.run_motion_study(study=study["id"], include_poses=True)
    print(r["table"])

    def pos(sample, occurrence, axis=2):
        return next(p["position"][axis] for p in sample["poses"]
                    if p["occurrence"] == occurrence.id)

    xs = [pos(s, occ["piston"]) for s in r["samples"]]
    stroke = max(xs) - min(xs)
    print(f"  piston stroke {stroke:.3f} mm (2R = {2 * CRANK_R:.3f}), "
          f"clean={r['clean']}, {r['pairs_checked']} pair checks "
          f"({r.get('pairs_rigid', 0)} skipped as rigid) in "
          f"{r['seconds']:.2f}s")
    if abs(stroke - 2 * CRANK_R) > 0.01:
        raise SdkError(f"stroke {stroke:.3f} is not the design stroke")

    # The valvetrain: each valve's travel over the cycle, and the camshaft
    # turning once for the crank's twice — the chain relation, measured.
    for i, vo in enumerate(top["valves"]):
        lift = [pos(s, vo) for s in r["samples"]]
        print(f"  valve {i + 1}: lift {max(lift) - min(lift):.2f} mm "
              f"(cam lobe {CAM_LIFT})")
    turns = [s["value"] for s in r["samples"]]
    print(f"  crank {turns[-1]:.0f} deg over the study; the camshaft runs at "
          f"half that on the chain relation")

    # ---- the exact confirmation: mesh decides fast, the boolean decides ---
    app.set_motion_study(study=study["id"], clash_mode="exact")
    exact = app.run_motion_study(study=study["id"])
    print(f"  exact re-check: clean={exact['clean']} in "
          f"{exact['seconds']:.2f}s (mesh said {r['clean']})")
    if exact["clean"] != r["clean"]:
        raise SdkError("the mesh check and the boolean disagree — see the "
                       "tables above")
    app.set_motion_study(study=study["id"], clash_mode="mesh")
    app.run_motion_study(study=study["id"])

    # ---- the deliverable: the animation ---------------------------------
    os.makedirs(a.out, exist_ok=True)
    glb = os.path.join(a.out, "single_cylinder.glb")
    ex = app.export_motion(study=study["id"], path=glb)
    print(f"  animation: {ex['nodes']} nodes x {ex['frames']} keyframes -> "
          f"{glb} ({os.path.getsize(glb) // 1024} kB)")

    # ---- and a drawing of it ---------------------------------------------
    # `quality="fast"` by default, and the reason is narrower than it looks.
    # Exact HLR of this engine is ~85 s A VIEW — but hide the two valves and
    # it is 0.0 s. The two helical springs are the entire bill; the other
    # seventeen bodies project instantly. Fast is polygonal HLR: the same
    # sheet in about 3 s, and a LOOK — polylines with no source refs, so
    # nothing on it can be dimensioned. --exact-drawing takes the time.
    rig.activate()
    app.create_drawing(name="engine drawing")
    for i, view in enumerate(("front", "right", "iso")):
        app.add_drawing_view(source="engine", orientation=view,
                             position=[70 + 130 * i, 150],
                             quality="exact" if a.exact_drawing else "fast",
                             _timeout=900)
    app.export_drawing(path=os.path.join(a.out, "single_cylinder.pdf"),
                       _timeout=900)
    print(f"  drawing: 3 views ({'exact' if a.exact_drawing else 'fast'}) "
          f"-> {a.out}/single_cylinder.pdf")

    project = os.path.join(a.out, "single_cylinder.dsgn")
    app.save_project(path=project)
    print(f"  project: {project}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
