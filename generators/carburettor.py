#!/usr/bin/env python3
"""A fixed-venturi carburettor, as a SUBASSEMBLY.

Seven parts and the mates that hold them, in an assembly of its own that the
engine then places as one occurrence — which is the point of the exercise.
A carburettor is the natural shape for it: nobody thinks of a float bowl as
part of a cylinder head, and the throttle spindle turns inside the body
whether or not the engine it is bolted to is running.

Styled on a Solex H-type: a bell mouth over a fixed venturi, a butterfly
throttle on a spindle below it, an integral float bowl beside the barrel with
a main jet in its floor, and an external lever with a return spring.

    from carburettor import carburettor
    carb = carburettor(app)          # -> an Asm, ready to place

Its `mount` frame is the flange face, Z pointing the way the body grows, so
it bolts to a port frame with nothing to remember.
"""
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from designer_sdk import Plane  # noqa: E402

# ---- the one set of numbers everything else is derived from --------------
FLANGE_R = 31.0         # matches the head's port flange
FLANGE_T = 10.0
OUTLET_R = 15.0         # the bore where it meets the port
VENTURI_R = 11.0        # the waist: what makes it a carburettor
BARREL_R = 24.0         # outside of the barrel
BELL_R = 30.0           # the bell mouth's outer lip
TOP_Z = 70.0
# The throttle sits BELOW the venturi — and above the FLANGE, which is the
# constraint that actually sets this number. The lever's arm is 11 across, so
# at 12 it swept from z=6.5 to 17.5 and clipped the top corner of a flange
# that reaches r=31 and z=10: 278 mm3, and moving the arm around the spindle
# did not help because the arm is 26 long in every direction that matters.
SPINDLE_Z = 18.0
SPINDLE_D = 6.0
DISC_R = OUTLET_R - 0.6 # a butterfly clears its bore by a whisker
BOWL_X = 58.0           # the float bowl, beside the barrel — far enough
                        # out that the spindle never reaches it
BOWL_R = 22.0
BOWL_LO, BOWL_HI = 8.0, 48.0

# ---- the throttle linkage ------------------------------------------------
# A four-bar: the body is the ground, the throttle lever one crank, the
# bellcrank the other, and the link rod between them.
#
# A PARALLELOGRAM, and staying one: equal arms and a coupler the length of
# the ground link. That is the four-bar that always assembles and cannot
# lock, and it is worth keeping — the version that abandoned it for a
# general four-bar solved at rest and then jumped between branches at every
# step of the sweep, 2550 mm2 of bore at one angle and 3080 at the next.
#
# The first version's mistake was not the parallelogram. It was pointing
# both arms ALONG the ground line, which puts the coupler on the line
# through both pivots — over the bellcrank's own boss, its pin, and anything
# else that pivot needs. Both arms point PERPENDICULAR to the ground now, so
# the coupler runs 17 mm clear of both pivots and stays there through the
# whole swing.
ARM = 17.0              # the throttle lever's
BELL_ARM = 18.0         # ...and the bellcrank's, deliberately NOT equal
GROUND = 34.0           # lever pivot to bellcrank pivot
ROD_L = math.hypot(GROUND, BELL_ARM - ARM)
CABLE_ARM = 16.0        # the arm the cable pulls on, reaching back inboard
# NEGATIVE, and a butterfly is why that is a free choice: a disc across a
# bore opens whichever way it turns, so the sign decides where the LINKAGE
# goes, not whether the throttle opens. Positive swung the cable arm down
# through the flange and the bracket rib; negative takes it up and outboard,
# where there is nothing.
CABLE_SWING = -60.0     # how far the cable pulls the bellcrank, degrees

# ---- and where each piece sits ALONG the spindle -------------------------
# Every one of these is a face, measured outward from the barrel, and the
# linkage's whole axial stack is written here rather than deduced at each
# sketch. Three parts once shared the plane x = -29 and interpenetrated by
# 130, 162 and 4 mm3 while the loop closed to 1.6e-07: a solved loop says
# the pins line up, and says nothing whatever about thickness.
PLATE_T = 3.0           # both crank plates
ROD_T = 2.5
PIN_D = 3.0             # the studs the coupler's eyes run on
CRANK_X = -(BARREL_R + 10.0)        # -34: inboard face of both crank plates
PIN_STAND = 5.0                     # the coupler's plane, off that face
ROD_X = CRANK_X - PIN_STAND         # -39
BRACKET_X = ROD_X - 9.0             # -48: the bracket's outer face
PIVOT_D = 11.0                      # the boss the bellcrank turns against
# The idle stop's boss is d=13, so its centre has to clear its own radius
# above the MOUNTING FACE. At 4 the boss reached z = -2.5 — behind the
# flange, inside whatever the carburettor is bolted to, which is exactly
# where it turned up once the engine grew a clash check: 205 mm3 of boss
# inside the cylinder head's intake port.
IDLE_Z = 8.0
SPRING_X = -(BARREL_R + 1.0)        # -25: where the return spring starts
SPRING_L = SPRING_X - CRANK_X - 1.0  # ...and how much room it has: 8


def body(app):
    """The casting: bell mouth, venturi, outlet, flange and float bowl.

    One revolved profile for the barrel — a carburettor's barrel IS a turned
    shape — then the bowl beside it and the passages through.
    """
    p = app.part("carb body")
    (p.var("venturi_r", VENTURI_R).var("outlet_r", OUTLET_R)
      .var("bowl_r", BOWL_R))
    # Outside from the flange up to the bell lip, then back DOWN the bore:
    # venturi at the waist, opening out below it to the outlet. The profile
    # never touches the axis, which is what leaves a bore through the middle.
    p.revolve_profile(
        [(OUTLET_R, 0.0), (FLANGE_R, 0.0), (FLANGE_R, FLANGE_T),
         (BARREL_R, FLANGE_T), (BARREL_R, 58.0), (BELL_R, TOP_Z),
         (BELL_R - 4.0, TOP_Z), (OUTLET_R + 1.0, 56.0),
         (OUTLET_R + 1.0, 46.0), (VENTURI_R, 36.0), (VENTURI_R, 30.0),
         (OUTLET_R, 20.0)],
        plane=Plane.xz(), axis="z", name="Barrel")
    # The float bowl, and then the void in it.
    bowl = p.sketch(Plane.xy(BOWL_LO), name="Float bowl")
    bowl.circle((BOWL_X, 0), d=2 * BOWL_R)
    p.extrude(bowl.add(), BOWL_HI - BOWL_LO, mode="add", name="Float bowl")
    # A WEB joining bowl to barrel — without it the bowl is a separate lump
    # 12 mm clear of the casting it belongs to. `islands()` said so: two
    # regions at every height, the barrel and the bowl, exactly the way four
    # bolt bosses once hung off a deck flange
    # (docs/dogfood-friction-plan.md, F1).
    web = p.sketch(Plane.xy(FLANGE_T), name="Bowl web")
    web.rect((BARREL_R - 6.0, -9.0), (BOWL_X - BOWL_R + 6.0, 9.0))
    p.extrude(web.add(), BOWL_HI - FLANGE_T - 6.0, mode="add", name="Bowl web")
    # The void, stopping short of the top so the bowl has a lid.
    void = p.sketch(Plane.xy(BOWL_LO + 6.0), name="Bowl void")
    void.circle((BOWL_X, 0), d=2 * BOWL_R - 12.0)
    p.extrude(void.add(), BOWL_HI - BOWL_LO - 12.0, mode="cut",
              name="Bowl void")
    # EACH BASE PLANE EXTRUDES ITS OWN WAY, and they do not agree: xy goes
    # +Z, yz goes +X, xz goes -Y. Measured, because assuming the two
    # "positive" ones behaved like the third put both of these cuts outside
    # the part — the spindle bore ran from x=44 to x=132 and touched nothing,
    # so the spindle was mated into solid metal, and the jet duct said so out
    # loud ("cut removed no material").
    sp = p.sketch(Plane.yz(-(BARREL_R + 20.0)), name="Spindle bore")
    sp.circle((0, SPINDLE_Z), d=SPINDLE_D + 2.0)
    p.extrude(sp.add(), 2 * (BARREL_R + 20.0), mode="cut", name="Spindle bore")
    # The passage the fuel takes: venturi throat out to the bowl floor.
    duct = p.sketch(Plane.yz(VENTURI_R - 2.0), name="Jet duct")
    duct.circle((0, 33.0), d=5.0)
    p.extrude(duct.add(), BOWL_X + BOWL_R - VENTURI_R + 2.0, mode="cut",
              name="Jet duct")
    # ---- the linkage bracket --------------------------------------------
    # A plate reaching out past the coupler's plane, carrying the pivot the
    # bellcrank turns on, and a RIB tying it back into the flange. Without
    # the rib it is another float bowl: metal that exists, checks clean and
    # is joined to nothing.
    br = p.sketch(Plane.yz(BRACKET_X), name="Linkage bracket")
    br.slot((GROUND - 12.0, 7.0), (GROUND + 6.0, SPINDLE_Z), width=14.0)
    p.extrude(br.add(), 4.0, mode="add", name="Linkage bracket")
    piv = p.sketch(Plane.yz(BRACKET_X + 4.0), name="Pivot boss")
    piv.circle((GROUND, SPINDLE_Z), d=PIVOT_D)
    p.extrude(piv.add(), 6.0, mode="add", name="Pivot boss")
    # The rib runs at FLANGE height and comes in to r=23, where there is
    # flange to land on. Higher up there is nothing to reach: the barrel is
    # r=24 and its bore is r=15, so a rib that reached the casting above the
    # flange would be standing in the airstream.
    rib = p.sketch(Plane.xy(0.0), name="Bracket rib")
    rib.slot((BRACKET_X + 2.0, GROUND), (-18.0, 14.0), width=10.0)
    p.extrude(rib.add(), 8.0, mode="add", name="Bracket rib")
    # ---- the two adjuster screws' holes ----------------------------------
    # The mixture screw goes down into the WEB between barrel and bowl, and
    # its mouth is the web's own top face. Seated 6 mm above that it hung in
    # the air with its head inside the bell mouth, which is the one place on
    # a carburettor nothing may stand.
    mixh = p.sketch(Plane.xy(BOWL_HI - 6.0), name="Mixture hole")
    mixh.circle((BARREL_R + 6.0, 0), d=5.0)
    p.extrude(mixh.add(), 20.0, mode="cut", name="Mixture hole", reversed=True)
    stop = p.sketch(Plane.yz(-(BARREL_R + 4.0)), name="Idle stop boss")
    stop.circle((ARM + 3.0, IDLE_Z), d=13.0)
    p.extrude(stop.add(), 16.0, mode="add", name="Idle stop boss")
    # Tapped, and cut LAST so it goes through the rib as well as the boss —
    # a screw modelled inside solid casting is two solids in one place,
    # however ordinary the arrangement looks.
    # FROM THE BRACKET'S OUTER FACE, not from somewhere near it. Started 2 mm
    # inboard, the screw's shank ran through the 2 mm of undrilled plate that
    # left: 8*pi mm3 exactly, which is what a r=2 shank buried 2 mm comes to.
    sh = p.sketch(Plane.yz(BRACKET_X - 1.0), name="Idle stop hole")
    sh.circle((ARM + 3.0, IDLE_Z), d=4.4)
    p.extrude(sh.add(), 44.0, mode="cut", name="Idle stop hole")
    # And the bore for the bellcrank's pivot pin, through plate and boss.
    ph = p.sketch(Plane.yz(BRACKET_X - 1.0), name="Pivot bore")
    ph.circle((GROUND, SPINDLE_Z), d=6.2)
    p.extrude(ph.add(), 13.0, mode="cut", name="Pivot bore")
    p.material("aluminum")
    # Z points the way the BODY grows, so mating it to a port frame needs no
    # flip — the same rule the head's own ports follow.
    p.frame("mount", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("spindle", origin=(0, 0, SPINDLE_Z), z=(1, 0, 0), x=(0, 0, 1))
    p.frame("jet_seat", origin=(BOWL_X, 0, BOWL_LO + 6.0), z=(0, 0, 1),
            x=(1, 0, 0))
    # The float rides ON THE FUEL, above the jet in the floor. Mating both to
    # one frame put a 5 mm jet inside a float bored 4 — 31 mm3 of two brass
    # parts in the same place.
    p.frame("float_seat", origin=(BOWL_X, 0, BOWL_LO + 16.0), z=(0, 0, 1),
            x=(1, 0, 0))
    # The return spring is a coil ROUND THE SPINDLE, in the gap between the
    # barrel and the lever's hub — which is where a throttle return spring
    # actually lives. Hung off the bracket instead it was 115 mm3 inside the
    # plate at one end and in clear air at the other; a frozen spring still
    # has to be frozen somewhere it could physically be.
    p.frame("spring_post", origin=(SPRING_X, 0, SPINDLE_Z), z=(-1, 0, 0),
            x=(0, 0, 1))
    # The bellcrank's pivot. z faces OUT so the crank grows away from the
    # casting; x = (0,-1,0) then maps its local +y to world +z, which is the
    # quarter turn that keeps the coupler off this pivot.
    p.frame("bell_post", origin=(CRANK_X, GROUND, SPINDLE_Z), z=(-1, 0, 0),
            x=(0, -1, 0))
    # The pin the bellcrank turns on, seated against the bracket's outer face.
    p.frame("pivot_pin", origin=(BRACKET_X - 3.2, GROUND, SPINDLE_Z),
            z=(1, 0, 0), x=(0, 0, 1))
    # Both adjusters sit with their tips INSIDE their tapped holes: 4 mm of
    # engagement, 0.1 mm of clearance, which is a screw in a hole rather
    # than a screw near one.
    p.frame("idle_stop", origin=(-(BARREL_R + 8.0), ARM + 3.0, IDLE_Z),
            z=(-1, 0, 0), x=(0, 0, 1))
    p.frame("mixture", origin=(BARREL_R + 6.0, 0.0, BOWL_HI - 10.0),
            z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def spindle(app):
    """The throttle spindle: a shaft across the barrel, long enough to carry
    the lever outside the casting."""
    p = app.part("throttle spindle")
    # ASYMMETRIC on purpose. The lever end has to reach out past the return
    # spring to x = -34, and a shaft that long at BOTH ends runs into the
    # float bowl, which starts at x = 36. So the mating frame is not the
    # shaft's middle: it is the point on the shaft that belongs on the
    # barrel's axis.
    out = -CRANK_X + 4.0                 # 38 to the lever end
    length = out + (BARREL_R + 4.0)      # ...and 28 to the other
    sk = p.sketch(Plane.xy(0), name="Shaft")
    sk.circle((0, 0), d=SPINDLE_D)
    p.extrude(sk.add(), length, mode="new_body", name="Shaft")
    p.material("steel")
    p.frame("centre", origin=(0, 0, out), z=(0, 0, 1), x=(1, 0, 0))
    # The lever end is the one AWAY FROM THE BOWL, and its Z points OUT so
    # whatever mounts here grows away from the casting rather than into it.
    # At the other end the lever sat inside the float bowl, which is what the
    # check reported as `carb body x throttle lever` and `throttle lever x
    # float` in the same breath.
    # x = (0,1,0) turns the lever's own +y into the world's +z, which is what
    # points its arm UP the barrel instead of along the ground link.
    p.frame("lever_end", origin=(0, 0, 4.0), z=(0, 0, -1), x=(0, 1, 0))
    p.check()
    return p


def disc(app):
    """The butterfly: a disc the spindle runs across, not through."""
    p = app.part("throttle disc")
    sk = p.sketch(Plane.xy(0), name="Disc")
    sk.circle((0, 0), d=2 * DISC_R)
    p.extrude(sk.add(), 1.6, mode="new_body", name="Disc")
    # A butterfly is SLOTTED for its spindle, not drilled — the spindle lies
    # across the disc's face, and without the slot the two solids share the
    # same 271 mm3.
    slot = p.sketch(Plane.xy(-1.0), name="Spindle slot")
    slot.rect((-DISC_R - 1.0, -(SPINDLE_D / 2 + 0.6)),
              (DISC_R + 1.0, SPINDLE_D / 2 + 0.6))
    p.extrude(slot.add(), 4.0, mode="cut", name="Spindle slot")
    p.material("brass")
    # Z ALONG THE SPINDLE, which lies IN the disc's plane — a butterfly is a
    # diameter of its bore, not a lid on it. Giving this frame the disc's own
    # normal would mate it broadside across the barrel.
    p.frame("hub", origin=(0, 0, 0.8), z=(1, 0, 0), x=(0, 0, 1))
    p.check()
    return p


def lever(app):
    """The throttle lever: hub on the spindle, one arm, one pin."""
    p = app.part("throttle lever")
    arm = p.sketch(Plane.xy(0), name="Arm")
    # UP, not down: pointing it the other way swung the arm through the
    # mounting flange, which reaches r=31 and is exactly where the lever sits.
    arm.slot((0, 0), (0, ARM), width=11.0)
    p.extrude(arm.add(), PLATE_T, mode="new_body", name="Arm")
    # A SHORT hub. At 7 mm it reached into the coupler's plane and left the
    # rod 1 mm of daylight; the rod passes 12 mm from this pivot and is 8
    # wide, so the hub has to stop before x = -38.
    boss = p.sketch(Plane.xy(0), name="Boss")
    boss.circle((0, 0), d=14.0)
    p.extrude(boss.add(), 4.0, mode="add", name="Boss")
    bore = p.sketch(Plane.xy(-2.0), name="Bore")
    bore.circle((0, 0), d=SPINDLE_D + 0.4)
    p.extrude(bore.add(), 12.0, mode="cut", name="Bore")
    # The pin the coupler's eye runs on, standing proud of the arm's face.
    pin = p.sketch(Plane.xy(PLATE_T), name="Pin")
    pin.circle((0, ARM), d=PIN_D)
    p.extrude(pin.add(), PIN_STAND, mode="add", name="Pin")
    p.material("steel")
    p.frame("hub", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    # HALFWAY UP the stud, which is the coupler's own plane.
    p.frame("pin", origin=(0, ARM, PIN_STAND), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def bellcrank(app):
    """The other crank, and an L: the coupler on one arm, the cable on the
    other, a quarter turn round. What turns a straight pull into rotation."""
    p = app.part("bellcrank")
    a = p.sketch(Plane.xy(0), name="Rod arm")
    a.slot((0, 0), (0, BELL_ARM), width=10.0)
    p.extrude(a.add(), PLATE_T, mode="new_body", name="Rod arm")
    # PERPENDICULAR to the rod arm, not opposite it. Opposite, it swept the
    # throttle lever; along +y in the world it has the whole outboard side
    # of the carburettor to itself.
    # 16, not 22. The cable arm reaches back INBOARD past the throttle
    # lever's own hub, and at 22 its rounded end overlapped that hub by
    # 66 mm3 — with both arms in one plane there is only 34 mm of ground
    # between the two pivots to share.
    b = p.sketch(Plane.xy(0), name="Cable arm")
    b.slot((0, 0), (-CABLE_ARM, 0), width=10.0)
    p.extrude(b.add(), PLATE_T, mode="add", name="Cable arm")
    # The hub runs INBOARD, onto the bracket's pivot boss. Outboard it stood
    # in the coupler's plane.
    boss = p.sketch(Plane.xy(0), name="Boss")
    boss.circle((0, 0), d=13.0)
    p.extrude(boss.add(), 6.0, mode="add", name="Boss", reversed=True)
    bore = p.sketch(Plane.xy(-8.0), name="Bore")
    bore.circle((0, 0), d=6.4)
    p.extrude(bore.add(), 14.0, mode="cut", name="Bore")
    pin = p.sketch(Plane.xy(PLATE_T), name="Pin")
    pin.circle((0, BELL_ARM), d=PIN_D)
    p.extrude(pin.add(), PIN_STAND, mode="add", name="Pin")
    eye = p.sketch(Plane.xy(-1.0), name="Cable eye")
    eye.circle((-CABLE_ARM, 0), d=4.0)
    p.extrude(eye.add(), 6.0, mode="cut", name="Cable eye")
    p.material("steel")
    p.frame("pivot", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("pin", origin=(0, BELL_ARM, PIN_STAND), z=(0, 0, 1),
            x=(1, 0, 0))
    p.frame("cable", origin=(-CABLE_ARM, 0, 1.5), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def pivot_pin(app):
    """The shouldered pin the bellcrank turns on, pressed into the bracket."""
    p = app.part("pivot pin")
    p.revolve_profile(
        [(0.0, 0.0), (5.5, 0.0), (5.5, 3.0), (3.0, 3.0), (3.0, 23.0),
         (0.0, 23.0)],
        plane=Plane.xz(), axis="z", name="Pin")
    p.material("steel")
    p.frame("head", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def link_rod(app):
    """The coupler: a flat bar with an eye at each end.

    ROD_L, not GROUND. Made the length of the ground link it was a
    parallelogram, and a parallelogram's coupler lies along the line through
    both pivots — over the bellcrank's boss, its pin and anything else that
    pivot needs.
    """
    p = app.part("link rod")
    bar = p.sketch(Plane.xy(0), name="Bar")
    bar.slot((0, 0), (0, -ROD_L), width=8.0)
    p.extrude(bar.add(), ROD_T, mode="new_body", name="Bar")
    for y in (0.0, -ROD_L):
        e = p.sketch(Plane.xy(-1.0), name="Eye")
        e.circle((0, y), d=PIN_D + 0.2)
        p.extrude(e.add(), 6.0, mode="cut", name="Eye")
    p.material("steel")
    p.frame("end_a", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("end_b", origin=(0, -ROD_L, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def idle_screw(app):
    """The stop the closed throttle rests against — what sets idle speed."""
    p = app.part("idle screw")
    p.revolve_profile(
        [(0.0, 0.0), (2.0, 0.0), (2.0, 16.0), (5.0, 16.0), (5.0, 20.0),
         (0.0, 20.0)],
        plane=Plane.xz(), axis="z", name="Screw")
    p.material("steel")
    p.frame("seat", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def mixture_screw(app):
    """The tapered needle that meters the idle circuit."""
    p = app.part("mixture screw")
    p.revolve_profile(
        [(0.0, 0.0), (1.6, 4.0), (2.4, 4.0), (2.4, 14.0), (5.0, 14.0),
         (5.0, 18.0), (0.0, 18.0)],
        plane=Plane.xz(), axis="z", name="Needle")
    p.material("brass")
    p.frame("seat", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def spring(app):
    """The return spring: a coil round the spindle, between the barrel and
    the lever's hub.

    Frozen geometry, like the timing chain — it is drawn where it sits at
    rest. A spring that wound up with the throttle would need its two ends to
    drive its own free length, which is explicitly out of scope.
    """
    p = app.part("throttle spring")
    turns = 6.0
    p.app.add_feature(feature={
        "type": "Curve3D", "name": "Coil path", "kind": "helix",
        # 6.5, not 8: the coil sits at z = 18 +/- its radius, and the
        # mounting flange is r=31 out to z=10. At radius 8 the bottom of
        # every turn was 0.8 mm inside the flange -- 594 mm3 of spring in
        # solid aluminium, in the one place a side view does not show it.
        "origin": [0, 0, 0], "axis": [0, 0, 1], "radius": 6.5,
        "pitch": SPRING_L / turns, "turns": turns})
    p.app.add_feature(feature={
        "type": "Pipe", "name": "Coil",
        "path_id": p.state()["features"][-1]["id"],
        "outer_diameter": 1.6, "mode": "add"})
    p.material("steel")
    p.frame("top", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def float_(app):
    """The float: an annulus that rides on the fuel and shuts the needle."""
    p = app.part("float")
    p.revolve_profile(
        [(4.0, 2.0), (BOWL_R - 9.0, 0.0), (BOWL_R - 9.0, 12.0), (4.0, 14.0)],
        plane=Plane.xz(), axis="z", name="Float")
    p.material("brass")
    p.frame("seat", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def jet(app):
    """The main jet: the calibrated hole everything else exists to serve."""
    p = app.part("main jet")
    p.revolve_profile(
        [(1.1, 0.0), (5.0, 0.0), (5.0, 3.0), (3.0, 3.0), (3.0, 16.0),
         (1.1, 16.0)],
        plane=Plane.xz(), axis="z", name="Jet")
    p.material("brass")
    p.frame("seat", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def carburettor(app, name="carburettor"):
    """The parts, the assembly and the mates — returned as an Asm to place."""
    b, sp, d = body(app), spindle(app), disc(app)
    lv, spr, fl, jt = lever(app), spring(app), float_(app), jet(app)
    bc, lr, pp = bellcrank(app), link_rod(app), pivot_pin(app)
    idl, mix = idle_screw(app), mixture_screw(app)

    asm = app.assembly(name)
    bo = asm.place(b, fixed=True)
    spo = asm.place(sp)
    # The spindle TURNS in the body; everything on it is fastened to it, so
    # one revolute carries the disc and the lever round with it.
    throttle = asm.revolute((bo, "spindle"), (spo, "centre"), flipped=False)
    do = asm.place(d)
    asm.fasten((spo, "centre"), (do, "hub"), flipped=False)
    lo = asm.place(lv)
    asm.fasten((spo, "lever_end"), (lo, "hub"), flipped=False)
    # The spring and the bowl's contents hang off the CASTING: frozen, and
    # honestly so.
    sro = asm.place(spr)
    asm.fasten((bo, "spring_post"), (sro, "top"), flipped=False)
    fo = asm.place(fl)
    asm.fasten((bo, "float_seat"), (fo, "seat"), flipped=False)
    jo = asm.place(jt)
    asm.fasten((bo, "jet_seat"), (jo, "seat"), flipped=False)
    io = asm.place(idl)
    asm.fasten((bo, "idle_stop"), (io, "seat"), flipped=False)
    mo = asm.place(mix)
    asm.fasten((bo, "mixture"), (mo, "seat"), flipped=False)

    # ---- and the linkage, which CLOSES A LOOP ---------------------------
    # body -> spindle -> lever -> link rod -> bellcrank -> body. Four bars
    # and four revolutes, so the assembly stops being a tree and the A6 loop
    # solver has something to do: turn the bellcrank and the butterfly
    # follows, because the geometry says it must.
    # SEEDED, and the earlier note here saying never to seed was drawn from
    # a different failure. A seed cannot help a loop that has no solution —
    # there the residual is a geometry error and a hint only moves the start
    # further from an answer that does not exist. But this loop has TWO
    # solutions at every angle, the linkage and its mirror image, and both
    # close it exactly. Unseeded, the solver took whichever was nearer to
    # the pose the occurrences happened to be placed in, and picked the
    # crossed one: it assembled, residual 1e-06, dof 1, and the butterfly
    # sat edge-on in the bore at rest. The seed is how the branch is chosen.
    bco = asm.place(bc, at=(CRANK_X, GROUND, SPINDLE_Z))
    cable = asm.revolute((bo, "bell_post"), (bco, "pivot"), flipped=False)
    ppo = asm.place(pp)
    asm.fasten((bo, "pivot_pin"), (ppo, "head"), flipped=False)
    lro = asm.place(lr, at=(ROD_X, GROUND, SPINDLE_Z + BELL_ARM))
    asm.revolute((bco, "pin"), (lro, "end_a"), flipped=False)
    # Both crank pins now face the same way — the bellcrank's frame is the
    # one that got turned round — so the coupler needs no flip at either end.
    # The flip it used to need was the whole reason the parts fought for one
    # plane: the two cranks grew in opposite directions along the spindle.
    asm.revolute((lro, "end_b"), (lo, "pin"), flipped=False)
    print("  carburettor solve:", asm.solve())
    # `connected=False`: a float is SUPPOSED to be surrounded by fuel, and a
    # return spring by air. `clearances=False` because every near miss left
    # is a fit that was chosen — the butterfly clears its bore by 0.6, the
    # spindle its bore by 1.0 — and a carburettor is made of running fits.
    #
    # NO ALLOWANCE FOR "INTENDED CONTACTS". There was one, and it hid six
    # real clashes: the test asked whether NEITHER name in a pair appeared in
    # the message, and since every pair named the body, nothing ever raised.
    # An escape hatch that wide is not a check with exceptions, it is no
    # check — and it passed a carburettor whose coupler ran through both the
    # cranks it joins. Clashes are now a failure, all of them.
    asm.check(connected=False, clearances=False)
    # ---- and it MOVES ---------------------------------------------------
    # A four-bar that closes is not yet a throttle. What makes it one is that
    # turning the CABLE arm turns the butterfly, and that is measurable
    # rather than assertable: a horizontal plane at spindle height crosses
    # the disc broadside when the throttle is shut and edge-on when it is
    # open, so the metal it cuts falls.
    #
    # Through a MOTION STUDY. `set_mate` reads the same range as a series of
    # unrelated poses -- 2589, 2558, 2719, 2533 mm2 at what were meant to be
    # ten-degree steps -- but that was a UNIT bug, not the solver: `rotation`
    # is RADIANS and named no unit, so 60 meant 197.7 and the walk asked for
    # 213, 66, 279, 132. There is a `rotation_deg` now (docs/known-bugs.md).
    # The study is
    # still the right instrument here, because it reports a clash verdict at
    # every one of its thirteen poses instead of the two a pair of set_mate
    # calls can reach.
    def disc_angle(sample, occ_id):
        q = next(pp["orientation"] for pp in sample["poses"]
                 if pp["occurrence"] == occ_id)
        return q

    study = asm.motion(cable, dof="rotation", **{"from": 0.0},
                       to=CABLE_SWING, samples=13, name="throttle travel")
    r = app.run_motion_study(study=study["id"], include_poses=True)
    if not r["clean"]:
        raise RuntimeError("the throttle linkage fouls somewhere in its "
                           f"travel:\n{r['table']}")
    # The BUTTERFLY's own rotation, read off the poses. A section through
    # the bore was the first measure and it cannot be taken here: the study
    # puts the assembly back where it found it, so the plane sees the shut
    # throttle twice. The angle is the better quantity anyway — it is the
    # thing the linkage exists to deliver.
    do_id = next(o.id for o in asm.occurrences() if o.name == "throttle disc")
    q0, q1 = (disc_angle(r["samples"][i], do_id) for i in (0, -1))
    dot = abs(sum(a * b for a, b in zip(q0, q1)))
    swept = math.degrees(2.0 * math.acos(min(1.0, dot)))
    print(f"  carburettor throttle: butterfly turns {swept:.1f} deg for "
          f"{abs(CABLE_SWING):.0f} of cable, clear at all "
          f"{len(r['samples'])} poses")
    if swept < 40.0:
        raise RuntimeError(
            f"the linkage does not open the butterfly: {swept:.1f} deg "
            f"of disc for {abs(CABLE_SWING):.0f} of cable arm")
    return asm, throttle, cable
