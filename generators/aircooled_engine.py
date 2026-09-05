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
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from carburettor import carburettor  # noqa: E402

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

# ---- the bottom end and the timing drive ---------------------------------
CASE_Y = 78.0           # crankcase half-width (outside)
CASE_WALL = 24.0        # end-wall thickness, where the mains run
# Half-depth, DERIVED: the rod's big end orbits at CRANK_R and is 35 in
# radius itself, so it sweeps a circle of 78 — and the void has to clear that,
# not merely the crank. At 82 the walls were inside the swing and the study
# said `crankcase x rod` at 20 of 49 samples.
CASE_X = CRANK_R + 35.0 + 14.0 + 4.0
SUMP_Z = -128.0         # the bottom of the sump
MAIN_D = 50.0           # main journal diameter
PIN_D = 42.0            # crank pin diameter
# The plane the timing drive runs in — outboard of the case's left wall AND
# of the intake, which sticks out to y = -68 and carries a float bowl beyond
# that. At -104 the carburettor sat in the cam sprocket at every sample.
DRIVE_Y = -150.0
CRANK_SPROCKET_R = 32.0 # 1 turn of the crank...
CAM_SPROCKET_R = 64.0   # ...is half a turn of the cam: 2:1, drawn as it is
                        # geared
CHAIN_T = 7.0           # chain thickness, radially
CHAIN_W = 12.0          # chain width, along Y

# ---- the valve gear ------------------------------------------------------
# The valves sit ALONG the camshaft, not across it. A cam can only press what
# is under it, and the cam has to be parallel to the crank for a flat chain to
# reach both sprockets — so the two constraints together put the valves fore
# and aft at x = 0, y = +-22, and a lobe at each of those y's is over a stem.
VALVE_XY = [(0.0, -22.0), (0.0, 22.0)]
CAM_Z = 110.0           # camshaft axis, above the head's top face
CAM_BASE_R = 15.0       # lobe base circle
# Cold clearance, drawn big. A real one is 0.15 mm; the collision mesh's
# deflection is 0.39, so anything that small reads as contact at every sample.
# 2.0 rather than 1.0 because the drawn lobe is a 120-point polygon of the
# analytic cosine while the RELATION interpolates its 19-point table with a
# monotone cubic — the same curve to a few tenths, not to the millimetre, and
# on the flanks that difference was 13 mm3 of contact at 2.0 and 8 of the 49
# samples still touched. 3.5 clears it, and the cam still visibly rides the
# stem: the gap is LASH at every angle, because the drawn profile and the
# table are the same function.
LASH = 3.5
STEM_TOP = CAM_Z - CAM_BASE_R - LASH   # where a closed valve's stem ends
LOBE_START = 20.0       # cam degrees at which valve 1 starts to open...
LOBE_DURATION = 150.0   # ...and how long it is open. Shared with the drawn
                        # lobe, so the picture and the relation agree.
CAM_SHAFT_D = 26.0
CAM_BEARING_W = 14.0
NOSE_W = 22.0           # the lobe nose's width across

# ---- breathing -----------------------------------------------------------
# A port per valve, leaving the head sideways along the CAM axis — the valves
# sit fore and aft, so that is the direction each one's throat can run without
# crossing the other's. Intake at -Y with the carburettor, exhaust at +Y.
PORT_D = 30.0           # throat, a shade under the 34 valve head
# Height up the head for each throat, and they are DIFFERENT on purpose.
# Both at 30 made the two cylinders coaxial — same axis, same radius, a 20 mm
# plug of head between them — and OCC's second subtract failed outright: not
# an error, an empty result, so the head silently ceased to exist and the only
# body left was a cam tower. Staggered they are two independent cuts.
PORT_Z = (26.0, 36.0)
FLANGE_OD = 62.0
FLANGE_T2 = 10.0
# Outboard of everything on the shaft, and CENTRED on these numbers: the
# tower is drawn at y + W/2 and extruded -W, so it spans y +- 7. The lobes
# occupy y -30..-14 and 14..30, and each valve's spring RETAINER is 17 in
# radius about a stem at y = +-22, so it reaches +-39. A bearing at 46 spans
# 39..53 and touched that retainer exactly — `head x valve` at all 49
# samples. 52 spans 45..59 and clears it by six.
CAM_BEARING_Y = (-58.0, 52.0)


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
    # Aluminium, which is WHY it is finned like that: the fins are there to
    # move heat into the air, and the alloy is half the reason that works.
    p.material("aluminum")
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
    # ---- the ports, BEFORE the valve gear is cut -------------------------------------------------------
    # Each valve breathes SIDEWAYS: a throat from the valve's own port up out
    # through the head wall, and a bolting flange around where it comes out.
    # The valves are fore and aft on the cam axis, so each throat runs along
    # its own valve's side and the two never meet.
    #
    # CUT FIRST, before the guides and ports. Done afterwards, the throat's
    # cylinder meets the guide bore's at right angles through the same point
    # and OCC's subtract failed outright — not an error, just an empty result,
    # so the head silently ceased to exist and the only body left was a cam
    # tower. Against the plain revolve both cuts are clean.
    for i, (_, vy) in enumerate(VALVE_XY):
        # An extrude from Plane.xz(y) always travels toward -Y, so each side
        # is sketched at the HIGHER end of the span it makes and given the
        # span's length. Written once for +Y and mirrored by hand, the -Y
        # throat ran from y=-56 out to -106 — entirely outside a head that
        # ends at 78 — and cut the head to pieces: it came back as a single
        # 23885 mm3 body called "Cam tower".
        sign = 1.0 if vy > 0 else -1.0
        root = sign * (WALL_R + 6.0)          # where the flange starts
        face = root + sign * FLANGE_T2        # ...and its outer face
        span = lambda a, b: (max(a, b), abs(b - a))

        at, length = span(root, face)
        boss = p.sketch(Plane.xz(at), name="Port flange")
        boss.circle((0, PORT_Z[i]), d=FLANGE_OD)
        p.extrude(boss.add(), length, mode="add", name="Port flange")

        # The throat, from the valve's own axis out through that face — it
        # meets the guide bore on the way, which is what joins port to valve.
        at, length = span(vy - sign * 12.0, face)
        thr = p.sketch(Plane.xz(at), name="Port throat")
        thr.circle((0, PORT_Z[i]), d=PORT_D)
        p.extrude(thr.add(), length, mode="cut", name="Port throat")
        p.frame(f"port_{i}", origin=(0, face, PORT_Z[i]), z=(0, sign, 0),
                x=(1, 0, 0))
    # Two valve guides, bored through, either side of the bore's axis.
    guides = p.sketch(Plane.xy(-5), name="Guides")
    for x, y in VALVE_XY:
        # 1.5 mm radial on an 11 stem. The mesh check's deflection on a part
        # this size is 0.39 mm, so a half-millimetre fit reads as a clash.
        guides.circle((x, y), d=14)
    p.extrude(guides.add(), 75.0, mode="cut", name="Guides")
    # The ports the valves seat against: a counterbore under each guide, deep
    # enough to swallow the whole seat cone.
    ports = p.sketch(Plane.xy(-5), name="Ports")
    for x, y in VALVE_XY:
        ports.circle((x, y), d=VALVE_D + 4)
    p.extrude(ports.add(), 20.0, mode="cut", name="Ports")
    # A seat for each spring, so its bottom coils are not inside the head.
    seats = p.sketch(Plane.xy(62, flip=True), name="Spring seats")
    for x, y in VALVE_XY:
        seats.circle((x, y), d=26)
    p.extrude(seats.add(), 20.0, mode="cut", name="Spring seats")
    bolts = p.sketch(Plane.xy(-1), name="Bolt holes")
    for x, y in bolt_xy():
        bolts.circle((x, y), d=11.0)
    p.extrude(bolts.add(), FLANGE_T + 2, mode="cut", name="Bolt holes")
    p.material("aluminum")
    # ---- cam towers -----------------------------------------------------
    # Pillars from the head's top face up to the cam axis, each ending in an
    # OPEN saddle. Open because the lobes are bigger than the shaft (20 mm
    # radius against 13), so a closed bore is a camshaft you could never fit:
    # the shaft drops in from above and a cap closes it, which is how it is
    # actually done.
    for y in CAM_BEARING_Y:
        tower = p.sketch(Plane.xz(y + CAM_BEARING_W / 2), name="Cam tower")
        tower.polyline([(-20.0, 56.0), (20.0, 56.0), (20.0, CAM_Z),
                        (-20.0, CAM_Z)], close=True)
        p.extrude(tower.add(), CAM_BEARING_W, mode="add", name="Cam tower")
        saddle = p.sketch(Plane.xz(y + CAM_BEARING_W / 2), name="Cam saddle")
        saddle.circle((0, CAM_Z), d=CAM_SHAFT_D + 8.0)
        p.extrude(saddle.add(), CAM_BEARING_W, mode="cut", name="Cam saddle")
    p.frame("deck", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("cam_axis", origin=(0, 0, CAM_Z), z=(0, 1, 0), x=(1, 0, 0))
    for i, y in enumerate(CAM_BEARING_Y):
        p.frame(f"cap_{i}", origin=(0, y, CAM_Z), z=(0, 0, 1), x=(1, 0, 0))
    for i, (x, y) in enumerate(VALVE_XY):
        # Z points DOWN the guide, so the cam's positive lift OPENS the valve
        # into the cylinder rather than pulling it up into the head.
        p.frame(f"guide_{i}", origin=(x, y, 0), z=(0, 0, -1), x=(1, 0, 0))
    p.check()
    return p


def exhaust(app):
    """The exhaust side: a flange and a header pipe that turns down.

    The bend is a PIPE swept along a drawn path — an arc into a straight —
    which is what a header is, and it is the one part here that could not be
    drawn as a profile.
    """
    p = app.part("exhaust")
    p.revolve_profile(
        [(0.0, 0.0), (FLANGE_OD / 2, 0.0), (FLANGE_OD / 2, FLANGE_T2),
         (24.0, FLANGE_T2), (24.0, 18.0), (18.0, 18.0), (18.0, 0.0)],
        plane=Plane.xz(), axis="z", name="Flange")
    # The path: straight out of the flange, then a quarter turn away.
    #
    # Sampled and fitted, because Curve3D offers `fit_spline`, `helix` and
    # `control_poly` and not an arc — so the bend is 16 points on a circle
    # and the spline through them. Close enough for a header, and it is the
    # feature doing what it is for rather than a shape wished into existence.
    R = 70.0
    pts = [[0.0, 0.0, 16.0], [0.0, 0.0, 40.0]]
    for j in range(1, 17):
        t = math.radians(90.0 * j / 16.0)
        pts.append([R * (1.0 - math.cos(t)), 0.0, 40.0 + R * math.sin(t)])
    p.app.add_feature(feature={
        "type": "Curve3D", "name": "Header path", "kind": "fit_spline",
        "points": pts})
    p.app.add_feature(feature={
        "type": "Pipe", "name": "Header", "path_id": p.state()["features"][-1]["id"],
        "outer_diameter": 36.0, "wall_thickness": 3.0, "mode": "add"})
    p.material("stainless_steel")
    p.frame("flange", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def cam_cap(app):
    """The half that closes a cam saddle, bolted down over the shaft."""
    p = app.part("cam cap")
    prof = p.sketch(Plane.xz(CAM_BEARING_W / 2), name="Cap")
    prof.polyline([(-20.0, 0.0), (20.0, 0.0), (20.0, 14.0), (-20.0, 14.0)],
                  close=True)
    p.extrude(prof.add(), CAM_BEARING_W, mode="new_body", name="Cap")
    bore = p.sketch(Plane.xz(CAM_BEARING_W + 2), name="Saddle")
    # 4 mm radial. An exact boolean calls 3 mm clear and the study's MESH
    # called it a clash at 3 of 49 samples — the tessellation of a 186 mm
    # shaft, not the fit. Same lesson as the main bearings.
    bore.circle((0, 0), d=CAM_SHAFT_D + 8.0)
    p.extrude(bore.add(), 2 * CAM_BEARING_W + 4, mode="cut", name="Saddle")
    p.material("aluminum")
    # Its own origin is ON the shaft axis, so it mates to the head's cap frame
    # without an offset anybody has to remember.
    p.frame("seat", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def crankcase(app):
    """The case and sump: the part everything else hangs off.

    A side silhouette drawn in XZ and extruded along Y — round at the bottom
    where the sump is, square at the top where the barrel bolts on — then
    hollowed, then bored for the mains. The hollow is a second silhouette
    inset from the first, which is what makes the walls a thickness rather
    than a number typed twice.
    """
    p = app.part("crankcase")
    p.var("case_y", CASE_Y).var("main_d", MAIN_D)
    deck = BARREL_Z

    def shell(sk, inset):
        """The case outline, `inset` in from the outside all round: three
        sides and a round bottom.

        The polyline STOPS at the sump's shoulders and the arc carries on
        from there. Running it across the bottom as well drew the chord the
        arc already spans, which is two closed loops sharing their endpoints
        rather than one outline — "sketch has no closed profile", and quite
        right.
        """
        x = CASE_X - inset
        sk.polyline([(x, -60.0), (x, deck - inset), (-x, deck - inset),
                     (-x, -60.0)], close=False)
        # CCW from 180 to 360 goes through 270 — under the sump. From 0 to
        # 180 would arc over the top of it, which is a different part.
        sk.arc((0.0, -60.0), x, 180.0, 360.0)

    # Outside, then the void inside it: one extrude each, because the two
    # loops would nest and a cut says what is meant more plainly than a
    # nested profile does.
    out = p.sketch(Plane.xz(CASE_Y), name="Case outline")
    shell(out, 0.0)
    p.extrude(out.add(), 2 * CASE_Y, mode="new_body", name="Case")
    void = p.sketch(Plane.xz(CASE_Y - CASE_WALL), name="Case void")
    shell(void, 14.0)
    p.extrude(void.add(), 2 * (CASE_Y - CASE_WALL), mode="cut", name="Case void")
    # The bore the barrel's spigot drops into, and the mains.
    mouth = p.sketch(Plane.xy(deck, flip=True), name="Barrel mouth")
    mouth.circle((0, 0), d=2 * WALL_R + 1.0)
    p.extrude(mouth.add(), 30.0, mode="cut", name="Barrel mouth")
    mains = p.sketch(Plane.xz(CASE_Y + 1), name="Main bearings")
    # 1.5 mm RADIAL, not 0.25. A running fit is a few tenths in metal, but
    # the collision mesh's deflection on a part this size is 0.39 mm, so
    # anything tighter than that reads as contact — `crankcase x crank` at all
    # 49 samples, which is the mesh telling the truth about its own
    # resolution rather than about the engine. Same rule as the valve guides.
    mains.circle((0, 0), d=MAIN_D + 3.0)
    p.extrude(mains.add(), 2 * CASE_Y + 2, mode="cut", name="Main bearings")
    # Head-bolt bosses land on the deck, so the studs pull the whole stack
    # together rather than the barrel alone.
    holes = p.sketch(Plane.xy(deck, flip=True), name="Barrel bolt holes")
    for x, y in bolt_xy():
        holes.circle((x, y), d=11.0)
    p.extrude(holes.add(), 26.0, mode="cut", name="Barrel bolt holes")
    p.material("aluminum")
    p.frame("main", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.frame("deck", origin=(0, 0, deck), z=(0, 0, 1), x=(1, 0, 0))
    # ON THE Y AXIS. Written (0, 0, DRIVE_Y) first, which put the timing
    # drive a metre BELOW the engine instead of outboard of it — the study
    # reported the chain hitting the valves, which was true of where it was.
    p.frame("drive", origin=(0, DRIVE_Y, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.check()
    return p


def tooth_tip(radius, teeth):
    """How far a sprocket's teeth actually reach.

    Shared with the chain, because a chain drawn to the PITCH radius has the
    teeth standing through it — the study said `crank sprocket x timing chain`
    at all 49 samples, which was true of the geometry as drawn.
    """
    return radius + (2 * math.pi * radius / teeth * 0.62) / 2


def sprocket(app, name, radius, bore_d, teeth):
    """A timing sprocket: a rim, a hub, and teeth from a circular pattern.

    The teeth are drawn because the ratio is the RELATION's job, not this
    geometry's — but a sprocket with no teeth beside a chain is a disc, and
    the point of drawing the drive is that you can see it is a drive.
    """
    p = app.part(name)
    body = p.sketch(Plane.xz(CHAIN_W / 2), name="Rim")
    body.circle((0, 0), d=2 * radius)
    p.extrude(body.add(), CHAIN_W, mode="new_body", name="Rim")
    # All the teeth in ONE sketch, laid out from the count.
    #
    # A circular Pattern was the obvious way and it is refused: a Pattern
    # repeats a feature that MAKES A BODY, and a tooth added to the rim makes
    # none — "source body (feature 4) not found". Drawing them is no less
    # parametric (the count is still the only number) and it is one extrude
    # instead of a body per tooth and a Boolean to put them back together.
    tooth_d = 2 * (tooth_tip(radius, teeth) - radius)
    ring = p.sketch(Plane.xz(CHAIN_W / 2), name="Teeth")
    for i in range(teeth):
        a = 2 * math.pi * i / teeth
        ring.circle((radius * math.sin(a), radius * math.cos(a)), d=tooth_d)
    p.extrude(ring.add(), CHAIN_W, mode="add", name="Teeth")
    # THROUGH: the rim spans -CHAIN_W/2..+CHAIN_W/2 about its own origin, so
    # a cut starting at +CHAIN_W and running CHAIN_W deep stopped short and
    # left a solid back for the shaft to run into.
    bore = p.sketch(Plane.xz(CHAIN_W), name="Bore")
    bore.circle((0, 0), d=bore_d)
    p.extrude(bore.add(), 2 * CHAIN_W, mode="cut", name="Bore")
    p.material("steel")
    p.frame("hub", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.check()
    return p


def chain(app, span, tip_a, tip_b):
    """The timing chain, as the band that wraps both sprockets.

    Two arcs and two tangent lines, which is what a chain path IS — and the
    tangents are computed rather than eyeballed: for circles of radius r1 and
    r2 whose centres are `span` apart, the external tangent's normal makes
    asin((r2 - r1) / span) with the centre line, and it touches each circle at
    that normal.

    FROZEN geometry: the relation drives the camshaft, and belt paths that
    follow their sprockets are explicitly out of scope. So it is drawn with
    running clearance on the sprocket teeth rather than sitting on them — a
    chain modelled where it really rides would be reported as a clash at every
    sample, which would be a true statement about geometry that is a
    deliberate simplification.
    """
    p = app.part("timing chain")
    # 3 mm off the tooth tips: a real chain rides ON them, and at 1 mm the
    # faceting of a 68 mm radius was enough for the mesh to find contact at
    # 36 of 49 samples.
    r1, r2 = tip_a + 3.0, tip_b + 3.0

    def loop(sk, grow):
        a, b = r1 + grow, r2 + grow
        beta = math.asin((b - a) / span)
        n = (math.cos(beta), math.sin(beta))
        deg = math.degrees(beta)
        sk.arc((0.0, 0.0), a, 180.0 - deg, 360.0 + deg)
        sk.line((a * n[0], a * n[1]), (b * n[0], span + b * n[1]))
        sk.arc((0.0, span), b, deg, 180.0 - deg)
        sk.line((-b * n[0], span + b * n[1]), (-a * n[0], a * n[1]))

    band = p.sketch(Plane.xz(CHAIN_W / 2), name="Chain")
    loop(band, CHAIN_T)   # outer
    loop(band, 0.0)       # inner: NESTED, which a sketch allows — it is a
                          # hole, not an overlap
    p.extrude(band.add(), CHAIN_W, mode="new_body", name="Chain")
    p.material("steel")
    p.frame("crank_end", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
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
    # Journals long enough to RUN IN THE CASE rather than stop in mid-air:
    # each spans its end wall and the drive side carries on to the sprocket.
    # The webs sit inboard of them, so the shaft reads main-web-pin-web-main
    # the way a single's crank actually does.
    # An extrude from Plane.xz(y) travels toward -Y, so each of these is
    # placed at the +Y end of what it makes. The mains fill the case's end
    # walls exactly: wall from CASE_Y - CASE_WALL out to CASE_Y.
    for name, y, length in (("Main L", -CASE_Y + CASE_WALL, CASE_WALL),
                            ("Main R", CASE_Y, CASE_WALL)):
        sk = p.sketch(Plane.xz(y), name=name)
        sk.circle((0, 0), d=MAIN_D)
        p.extrude(sk.add(), length, mode="add" if name != "Main L"
                  else "new_body", name=name)
    # The nose the timing sprocket keys onto, from the case's outer face out
    # past the drive plane.
    # THROUGH the sprocket and out the far side. Stopping level with it, or
    # short of it, leaves a bore with daylight in it and the drive reads as
    # unconnected however correct the mate is.
    nose = p.sketch(Plane.xz(-CASE_Y), name="Nose")
    nose.circle((0, 0), d=34.0)
    p.extrude(nose.add(), -CASE_Y - (DRIVE_Y - CHAIN_W), mode="add",
              name="Nose")
    # The pin: 2 mm diametral clearance in the rod's 46 bore.
    pin = p.sketch(Plane.xz(20), name="Crank pin")
    pin.circle((0, "crank_r"), d=PIN_D)
    p.extrude(pin.add(), 40.0, mode="add", name="Crank pin")
    # Beside the pin (which spans -20..20), NOT out at the walls: an extrude
    # from Plane.xz(-54) travels to -84, straight into the case's end wall.
    #
    # And no wider than 44 either side, because the webs rise to z=75 and the
    # case's top plate starts at 68 — they clear it only through the barrel
    # mouth, which is a 52.5 radius CYLINDER. At +-54 their corners were
    # under solid plate: 876 mm3 of it, at every one of the 49 samples.
    for y in (-20, 44):
        # TWO sketches, because a sketch REFUSES loops that overlap ("profile
        # loops overlap or nest inconsistently") — it will not union them for
        # you. A silhouette drawn as overlapping shapes is therefore one
        # extrude per shape, each adding to the last. More calls, and no
        # guess about which loop was meant to be a hole.
        # WIDTHS SET BY THE SWING, not by taste. A slot's end cap is half its
        # width past the centre it ends on, so what matters is how far the
        # shape reaches from the crank AXIS — it sweeps that circle once a
        # turn. The case's void stops at BARREL_Z - 14 = 68, so:
        #
        #   web           43 + 44/2 = 65, clears by 3
        #   counterweight 34 + 60/2 = 64, clears by 4
        #
        # At 64 and 90 they swept 75 and 79 and ground through the case's top
        # plate at 22 of the 49 samples. A disc centred on the axis (what v1
        # used) hides this class of error, because the shape only grows where
        # you are looking at it.
        w = p.sketch(Plane.xz(y), name="Web")
        w.slot((0, 0), (0, CRANK_R), width=44.0)
        p.extrude(w.add(), 24.0, mode="add", name="Web")
        cw = p.sketch(Plane.xz(y), name="Counterweight")
        cw.slot((0, 0), (0, -34), width=60.0)
        p.extrude(cw.add(), 24.0, mode="add", name="Counterweight")
    p.material("steel")
    p.frame("main", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.frame("pin", origin=(0, 0, "crank_r"), z=(0, 1, 0), x=(1, 0, 0))
    p.frame("drive", origin=(0, DRIVE_Y, 0), z=(0, 1, 0), x=(1, 0, 0))
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
    p.material("steel")
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
    # Aluminium: a piston is the one reciprocating part where mass costs
    # you twice, at both ends of every stroke.
    p.material("aluminum")
    p.frame("wrist_pin", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.frame("slide", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def valve(app):
    """A valve turned from one profile, with its spring. Its origin is on the
    seat, so a lift of zero is closed."""
    p = app.part("valve")
    p.var("valve_d", VALVE_D)
    # The stem ends where the CAM's base circle begins, less the lash: a
    # closed valve is LASH below the lobe, and the relation's 9 mm of lift
    # takes it down to exactly the same clearance under the nose.
    prof = [(0, 0), ("valve_d / 2", 0), ("valve_d / 2", 3),
            (5.5, 10), (5.5, STEM_TOP), (0, STEM_TOP)]
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
    # Stainless: a valve sits in the exhaust stream and has to stay hard hot.
    p.material("stainless_steel")
    # The retainer the spring pushes up against, and the collar it seats on.
    # Without it the spring ends in mid-air and nothing says which way it
    # pushes.
    p.revolve_profile([(5.5, STEM_TOP - 16), (17.0, STEM_TOP - 16),
                       (17.0, STEM_TOP - 10), (11.0, STEM_TOP - 6),
                       (5.5, STEM_TOP - 6)],
                      plane=Plane.xz(), axis="z", mode="add", name="Retainer")
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
    # Out THROUGH the drive plane. At DRIVE_Y - 4 it stopped two millimetres
    # inside the sprocket, so the sprocket's bore showed daylight and the cam
    # drive looked unconnected — which is what a person spotted in the render
    # while every mate and every clearance measured correct.
    p.extrude(shaft.add(), 70.0 - (DRIVE_Y - CHAIN_W), mode="new_body",
              name="Shaft")
    # A lobe over each valve, DRAWN FROM THE RELATION'S OWN PROFILE.
    #
    # This is the only way the picture and the table can agree. Drawn as a
    # base circle plus a slot the two disagreed twice over: a slot reaches
    # offset + width/2, so the tip stood at 20 against a 15 base circle and
    # the drawn lift was 5 mm where the relation lifts 9; and even sized
    # right, a slot's flanks are arcs while the relation's lift is a cosine,
    # so at intermediate angles the drawn cam pushed lower than the valve had
    # moved — `camshaft x valve` at 16 of 49 samples.
    #
    # So the lobe IS the lift function: at each cam angle a, a point at
    # radius CAM_BASE_R + lift(a). It is placed at local angle 180 - a,
    # because that is the angle which has rotated to point at the valve by
    # the time the shaft reaches a. The SIGN was determined by trying both
    # and reading the study: 180 - a put the nose down at the wrong moment
    # and fouled a closed valve at 26 of 49 samples.
    def lift_at(a, start):
        f = ((a - start) % 360.0) / LOBE_DURATION
        return 0.0 if f > 1.0 else CAM_LIFT * 0.5 * (1.0 - math.cos(2 * math.pi * f))

    for i, (_, vy) in enumerate(VALVE_XY):
        start = LOBE_START + 180.0 * i
        pts = []
        for k in range(120):
            a = 360.0 * k / 120.0
            r = CAM_BASE_R + lift_at(a, start)
            phi = math.radians(180.0 - a)
            pts.append((r * math.sin(phi), r * math.cos(phi)))
        lobe = p.sketch(Plane.xz(vy + 8), name="Lobe")
        lobe.polyline(pts, close=True)
        p.extrude(lobe.add(), 16.0, mode="add", name="Lobe")
    p.material("steel")
    p.frame("axle", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.frame("drive", origin=(0, DRIVE_Y, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.check()
    return p


def bottom_end(app):
    """Case, barrel, crank, rod, piston — and the mates that make them run.

    The CASE is the fixed part now, not the barrel: a crankshaft runs in its
    main bearings and the barrel bolts to its deck, which is the order the
    real thing goes together in and the order the loads take.
    """
    k = crankcase(app)
    b, c, r, pi = barrel(app), crank(app), rod(app), piston(app)
    rig = app.assembly("engine")
    ko = rig.place(k, fixed=True)
    bo = rig.place(b, at=(0, 0, 0))
    co = rig.place(c)
    ro = rig.place(r)
    po = rig.place(pi, at=(0, 0, CRANK_R + ROD_L))
    rig.fasten((ko, "deck"), (bo, "bore_axis"), flipped=False)
    main = rig.revolute((ko, "main"), (co, "main"), flipped=False)
    rig.revolute((co, "pin"), (ro, "big_end"), flipped=False)
    rig.revolute((ro, "small_end"), (po, "wrist_pin"), flipped=False)
    rig.slider((bo, "bore_axis"), (po, "slide"), flipped=False)
    print("  bottom end solve:", rig.solve())
    return rig, {"crankcase": ko, "barrel": bo, "crank": co, "rod": ro,
                 "piston": po}, main


def top_end(app, rig, occ, main_mate):
    """Head, two valves, camshaft. The camshaft runs at half crank speed on a
    CHAIN relation; each valve is lifted by a CAM relation off it."""
    h, v, c, cap = head(app), valve(app), camshaft(app), cam_cap(app)
    # The carburettor is a SUBASSEMBLY of its own — eleven parts and a
    # throttle LINKAGE that closes a loop — placed here as one occurrence.
    carb, throttle, cable = carburettor(app)
    exh = exhaust(app)
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
    # ---- breathing -------------------------------------------------------
    # Each bolts to its own valve's port frame, so which side they land on is
    # the HEAD's business and not a coordinate repeated here.
    # A SUB-ASSEMBLY MATES BY ITS OWN ORIGIN. It has no frames or faces of
    # its own — the app says so outright — and a mate connector will not take
    # a path into one either, so the origin is the datum and the thing has to
    # be BUILT around the origin you mean to mate. The carburettor's body is
    # fixed at its assembly's origin with the flange face there, which is why
    # this needs nothing but the port frame on the other side.
    co = rig.place(carb, at=(0, 0, DECK + PORT_Z[0]), name="carburettor")
    rig.fasten((ho, "port_0"), co, flipped=False)
    eo = rig.place(exh, at=(0, 0, DECK + PORT_Z[1]))
    rig.fasten((ho, "port_1"), (eo, "flange"), flipped=False)

    for i in range(len(CAM_BEARING_Y)):
        capo = rig.place(cap, at=(0, CAM_BEARING_Y[i], DECK + CAM_Z))
        rig.fasten((ho, f"cap_{i}"), (capo, "seat"), flipped=False)

    # ---- the timing drive, drawn ----------------------------------------
    # The relation above is what makes the cam turn at half crank speed. This
    # is what makes it VISIBLE: two sprockets in the same ratio and the chain
    # that wraps them, so the drive is a thing you can look at rather than a
    # rule in a table.
    span = DECK + 110.0
    # Bores three millimetres over their shafts, for the same reason the
    # mains are: the mate holds them, and anything inside the collision
    # mesh's deflection reads as two solids in one place.
    cs = sprocket(app, "crank sprocket", CRANK_SPROCKET_R, 37.0, 18)
    ms = sprocket(app, "cam sprocket", CAM_SPROCKET_R, 29.0, 36)
    ch = chain(app, span, tooth_tip(CRANK_SPROCKET_R, 18),
               tooth_tip(CAM_SPROCKET_R, 36))
    rig.activate()
    cso = rig.place(cs, at=(0, DRIVE_Y, 0))
    mso = rig.place(ms, at=(0, DRIVE_Y, span))
    cho = rig.place(ch, at=(0, DRIVE_Y, 0))
    # The sprockets ride their shafts, so they turn with them and the study
    # sees the drive move. The chain is fastened to the CASE: frozen, and
    # honestly so.
    rig.revolute((occ["crank"], "drive"), (cso, "hub"), flipped=False)
    rig.revolute((cam_occ, "drive"), (mso, "hub"), flipped=False)
    rig.fasten((occ["crankcase"], "drive"), (cho, "crank_end"), flipped=False)
    valves = []
    for i in range(2):
        vo = rig.place(v, at=(VALVE_XY[i][0], VALVE_XY[i][1], DECK))
        slide = rig.slider((ho, f"guide_{i}"), (vo, "seat"), flipped=False)
        rig.cam(cam_mate, slide, rise=CAM_LIFT, duration_deg=LOBE_DURATION,
                start_deg=LOBE_START + 180.0 * i, name=f"lobe {i + 1}")
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
    # The mesh default is enough again. It used to report `camshaft x cam cap`
    # where an exact boolean said clear, which is what turned up the
    # box-centre containment bug in MeshCollide — a cap that STRADDLES its
    # shaft has its box nested inside the shaft's and its box centre inside
    # the shaft itself. Fixed there rather than worked around here.
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
