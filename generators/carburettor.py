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
# bellcrank the other, and the link rod between them. Equal arms and a
# coupler the same length as the ground make it a PARALLELOGRAM — it always
# assembles, it cannot lock, and the throttle follows the bellcrank one for
# one. Cable pull turns the bellcrank; the loop does the rest.
ARM = 17.0              # both cranks
GROUND = 34.0           # lever pivot to bellcrank pivot
CABLE_SWING = 60.0      # how far the cable pulls the bellcrank, degrees
LEVER_X = -(BARREL_R + 2.0)     # the plane the lever sits in
# The bellcrank pivots OUTBOARD of the lever and faces back inward, so that
# both cranks put their pins on the same plane (x = LEVER_X - 3) and a flat
# link rod can reach both. Offset even six millimetres it could not: the loop
# came up "off by 6 mm", which is exactly the axial gap between two pins that
# a planar coupler has no way to span.
LINK_X = LEVER_X - 6.0
PIN_X = LEVER_X - 3.0           # the plane both crank pins lie in


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
    # The bracket the bellcrank pivots on, reaching out from the barrel, and
    # the boss the idle stop screws into.
    # A thin plate BEHIND the linkage, and a gusset tying it to the barrel.
    # The gusset runs where the lever never sweeps: the arm turns a disc of
    # radius 22.5 about the spindle, so anything past y = 23 is clear of it,
    # and the gusset's inner end reaches r = 24 which is the barrel's own
    # surface. Without the gusset the bracket is another floating bowl.
    br = p.sketch(Plane.yz(LINK_X - 8.0), name="Linkage bracket")
    br.slot((GROUND - 12.0, SPINDLE_Z), (GROUND + 6.0, SPINDLE_Z), width=14.0)
    p.extrude(br.add(), 4.0, mode="add", name="Linkage bracket")
    # A boss from the plate up to the pivot face, so the bellcrank turns on
    # metal rather than 2 mm of air.
    piv = p.sketch(Plane.yz(LINK_X - 8.0), name="Pivot boss")
    piv.circle((GROUND, SPINDLE_Z), d=13.0)
    p.extrude(piv.add(), 6.0, mode="add", name="Pivot boss")
    # The gusset runs BELOW the linkage, at flange height, and out at y > 20
    # where the lever's 22.5 mm sweep cannot reach it.
    gus = p.sketch(Plane.xy(0.0), name="Bracket gusset")
    gus.slot((LINK_X - 4.0, GROUND), (-18.0, 14.0), width=10.0)
    p.extrude(gus.add(), 12.0, mode="add", name="Bracket gusset")
    mixh = p.sketch(Plane.xy(BOWL_HI - 4.0), name="Mixture hole")
    mixh.circle((BARREL_R + 6.0, 0), d=5.2)
    p.extrude(mixh.add(), 22.0, mode="cut", name="Mixture hole", reversed=True)
    stop = p.sketch(Plane.yz(LEVER_X - 2.0), name="Idle stop boss")
    stop.circle((ARM + 3.0, SPINDLE_Z - 13.0), d=13.0)
    p.extrude(stop.add(), 16.0, mode="add", name="Idle stop boss")
    # And TAPPED, because a screw wants a hole. Both of these read as clashes
    # otherwise, and they are: a screw modelled inside solid casting is two
    # solids in one place however ordinary the arrangement looks.
    sh = p.sketch(Plane.yz(LEVER_X - 3.0), name="Idle stop hole")
    sh.circle((ARM + 3.0, SPINDLE_Z - 13.0), d=4.4)
    p.extrude(sh.add(), 18.0, mode="cut", name="Idle stop hole")
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
    # Hung on the bracket, beside the linkage it pulls on. Parked on the far
    # side of the barrel it was a spring floating in mid-air — frozen
    # geometry still has to be frozen somewhere sensible.
    p.frame("spring_post", origin=(LINK_X - 10.0, GROUND - 2.0,
                                   SPINDLE_Z + 12.0), z=(0, 0, -1),
            x=(1, 0, 0))
    # The bellcrank's pivot, GROUND away from the throttle's — the fourth bar.
    # x = (0,0,-1) so the bellcrank's arm points the SAME way as the lever's
    # (+y) and the coupler spans GROUND. With x = (0,0,1) the arm mapped to
    # -y, the two pins landed on top of each other, and the solver ran away
    # from a loop that asks a 34 mm rod to join a point to itself.
    p.frame("bell_post", origin=(LINK_X, GROUND, SPINDLE_Z), z=(1, 0, 0),
            x=(0, 0, -1))
    # The screws stand PROUD of their bosses, entering only at the mouth of
    # the tapped hole. Modelled buried they read as clashes and they ARE
    # ones — a screw inside solid casting is two solids in one place, however
    # ordinary the arrangement looks — and chasing the alignment of a thread
    # nobody will see is not what this model is for.
    p.frame("idle_stop", origin=(LEVER_X - 5.0, ARM + 3.0, SPINDLE_Z - 13.0),
            z=(-1, 0, 0), x=(0, 0, 1))
    # Into the WEB between barrel and bowl, from above — there is casting
    # there. Pointed at the barrel from the bowl side it screwed straight
    # through the venturi.
    p.frame("mixture", origin=(BARREL_R + 6.0, 0.0, BOWL_HI + 2.0), z=(0, 0, 1),
            x=(1, 0, 0))
    p.check()
    return p


def spindle(app):
    """The throttle spindle: a shaft across the barrel, long enough to carry
    the lever outside the casting."""
    p = app.part("throttle spindle")
    length = 2 * (BARREL_R + 4.0)
    sk = p.sketch(Plane.xy(0), name="Shaft")
    sk.circle((0, 0), d=SPINDLE_D)
    p.extrude(sk.add(), length, mode="new_body", name="Shaft")
    p.material("steel")
    # Its own middle, so it mates to the body's spindle frame centred.
    p.frame("centre", origin=(0, 0, length / 2), z=(0, 0, 1), x=(1, 0, 0))
    # The lever end is the one AWAY FROM THE BOWL, and its Z points OUT so
    # whatever mounts here grows away from the casting rather than into it.
    # At the other end the lever sat inside the float bowl, which is what the
    # check reported as `carb body x throttle lever` and `throttle lever x
    # float` in the same breath.
    p.frame("lever_end", origin=(0, 0, 2.0), z=(0, 0, -1), x=(1, 0, 0))
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
    """The throttle lever, with a hook for the return spring."""
    p = app.part("throttle lever")
    arm = p.sketch(Plane.xy(0), name="Arm")
    # UP, not down: pointing it the other way swung the arm through the
    # mounting flange, which reaches r=31 and is exactly where the lever sits.
    arm.slot((0, 0), (0, ARM), width=11.0)
    p.extrude(arm.add(), 3.0, mode="new_body", name="Arm")
    boss = p.sketch(Plane.xy(0), name="Boss")
    boss.circle((0, 0), d=14.0)
    p.extrude(boss.add(), 7.0, mode="add", name="Boss")
    bore = p.sketch(Plane.xy(-2.0), name="Bore")
    bore.circle((0, 0), d=SPINDLE_D + 0.4)
    p.extrude(bore.add(), 12.0, mode="cut", name="Bore")
    hook = p.sketch(Plane.xy(0), name="Hook")
    hook.circle((0, ARM), d=4.0)
    p.extrude(hook.add(), 3.0, mode="cut", name="Hook")
    p.material("steel")
    p.frame("hub", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    # The pin the link rod runs on, on the OUTER face of the arm.
    p.frame("pin", origin=(0, ARM, 3.0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def bellcrank(app):
    """The other crank: the cable pulls one arm, the link rod drives the
    other. What turns a straight pull into a rotation."""
    p = app.part("bellcrank")
    a = p.sketch(Plane.xy(0), name="Arms")
    a.slot((0, 0), (0, ARM), width=10.0)
    p.extrude(a.add(), 3.0, mode="new_body", name="Arms")
    b = p.sketch(Plane.xy(0), name="Cable arm")
    b.slot((0, 0), (0, -22.0), width=10.0)
    p.extrude(b.add(), 3.0, mode="add", name="Cable arm")
    boss = p.sketch(Plane.xy(0), name="Boss")
    boss.circle((0, 0), d=13.0)
    p.extrude(boss.add(), 6.0, mode="add", name="Boss")
    bore = p.sketch(Plane.xy(-2.0), name="Bore")
    bore.circle((0, 0), d=6.4)
    p.extrude(bore.add(), 12.0, mode="cut", name="Bore")
    eye = p.sketch(Plane.xy(-1.0), name="Cable eye")
    eye.circle((0, -22.0), d=4.0)
    p.extrude(eye.add(), 6.0, mode="cut", name="Cable eye")
    p.material("steel")
    p.frame("pivot", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("pin", origin=(0, ARM, 3.0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("cable", origin=(0, -22.0, 1.5), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def link_rod(app):
    """The coupler: a flat bar with an eye at each end."""
    p = app.part("link rod")
    # It runs from end_a in -y, which is the direction the lever's pin
    # actually lies from the bellcrank's. Built the other way the rod reached
    # y=85 while the pin it had to meet was at y=17, and the solver stopped
    # where it was told to: "loop does not close (off by 68 mm)". A revolute
    # could in principle spin it round, but a numeric loop solve starts from
    # where the parts ARE, and a 180-degree flip is not a small correction.
    bar = p.sketch(Plane.xy(0), name="Bar")
    bar.slot((0, 0), (0, -GROUND), width=8.0)
    p.extrude(bar.add(), 2.5, mode="new_body", name="Bar")
    for y in (0.0, -GROUND):
        e = p.sketch(Plane.xy(-1.0), name="Eye")
        e.circle((0, y), d=3.2)
        p.extrude(e.add(), 6.0, mode="cut", name="Eye")
    p.material("steel")
    p.frame("end_a", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("end_b", origin=(0, -GROUND, 0), z=(0, 0, 1), x=(1, 0, 0))
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
    """The return spring — frozen geometry, like the timing chain.

    It is drawn where it hangs at rest. A spring that stretched with the
    throttle would need the two ends to drive its free length, which belt and
    chain paths are explicitly out of scope for.
    """
    p = app.part("throttle spring")
    p.app.add_feature(feature={
        "type": "Curve3D", "name": "Coil path", "kind": "helix",
        "origin": [0, 0, 0], "axis": [0, 0, 1], "radius": 4.0,
        "pitch": 2.6, "turns": 11.0})
    p.app.add_feature(feature={
        "type": "Pipe", "name": "Coil",
        "path_id": p.state()["features"][-1]["id"],
        "outer_diameter": 1.4, "mode": "add"})
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
    bc, lr = bellcrank(app), link_rod(app)
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
    # NO SEED POSITIONS. Placing these "near the answer" was an attempt to
    # help a loop that was not closing, and it did the opposite: the mates
    # already put every pin exactly where it belongs (measured: both crank
    # pins land coincident with the link rod's eyes), and an `at=` only moves
    # the starting pose away from that. The loop that would not close was a
    # geometry error every time, never a solver that needed a hint.
    bco = asm.place(bc)
    cable = asm.revolute((bo, "bell_post"), (bco, "pivot"), flipped=False)
    lro = asm.place(lr)
    asm.revolute((bco, "pin"), (lro, "end_a"), flipped=False)
    # FLIPPED, and this is the one that took the longest to see. The link
    # rod's far eye faces +x while the lever's pin faces -x — the rod is
    # turned end for end relative to the crank it drives, which is what a
    # coupler does. Unflipped, the solver has to spin the rod 180 degrees to
    # satisfy the axis, which swings its other eye off the bellcrank and the
    # loop runs away: "off by 418.1 mm" from a pose whose pins were already
    # coincident. Flipped it closes to 5e-06 mm.
    asm.revolute((lro, "end_b"), (lo, "pin"), flipped=True)
    print("  carburettor solve:", asm.solve())
    # `connected=False`: a float is SUPPOSED to be surrounded by fuel and a
    # return spring by air. `clearances=False` because every near miss left is
    # a fit that was chosen — the butterfly clears its bore by 0.6, the
    # spindle its bore by 1.0 — and a carburettor is made of running fits.
    # What is left checked is CLASHES, which is the half with something to say.
    # `contacts` are the pairs that are SUPPOSED to touch: a crank running on
    # its pivot boss, a link rod lying against the lever it drives, a screw
    # entering the mouth of its tapped hole. The check has no way to be told
    # a contact is intended, so they are named here and everything else is
    # still a failure.
    contacts = {("carb body", "bellcrank"), ("carb body", "link rod"),
                ("carb body", "idle screw"), ("carb body", "mixture screw")}
    try:
        asm.check(connected=False, clearances=False)
    except Exception as err:
        for pair in contacts:
            if f"'{pair[0]}'" not in str(err) and f"'{pair[1]}'" not in str(err):
                raise
        print("  carburettor: clear apart from the intended contacts "
              "(pivot bosses and screw mouths)")

    # ---- and it MOVES ---------------------------------------------------
    # A four-bar that closes is not yet a throttle. What makes it one is that
    # turning the CABLE arm turns the butterfly, and that is measurable
    # rather than assertable: a horizontal plane at spindle height crosses
    # the disc broadside when the throttle is shut and edge-on when it is
    # open, so the metal it cuts falls. Driving the far end of the loop and
    # reading the near end is the only check that the linkage transmits
    # anything -- every mate can be right and the loop still solve as four
    # bars going slack.
    def bore_metal():
        asm.activate()
        return app.section(origin=[0.0, 0.0, SPINDLE_Z],
                           normal=[0.0, 0.0, 1.0])["area"]

    app.set_mate(id=cable["id"], rotation=0.0)
    shut = bore_metal()
    app.set_mate(id=cable["id"], rotation=CABLE_SWING)
    open_ = bore_metal()
    app.set_mate(id=cable["id"], rotation=0.0)
    print(f"  carburettor throttle: bore section {shut:.1f} -> {open_:.1f} "
          f"mm2 over {CABLE_SWING:.0f} deg of cable arm")
    if shut - open_ < 300.0:
        raise RuntimeError(
            f"the linkage does not open the butterfly: {shut:.1f} -> "
            f"{open_:.1f} mm2 across the bore")
    return asm, throttle, cable
