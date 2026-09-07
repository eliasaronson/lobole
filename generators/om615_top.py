#!/usr/bin/env python3
"""The top end of the diesel: the head, its valve gear, and the pre-chambers
that make it a pre-chamber diesel rather than a petrol four.

Everything is drawn in ENGINE coordinates — the same z the block's deck is in
— so a frame that only translates puts each part where it was drawn, and the
few frames that also rotate are the ones something actually turns on.
"""
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from designer_sdk import Plane, SdkError, V  # noqa: E402
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from om615_spec import *  # noqa: E402,F401,F403
import om615_bottom as B  # noqa: E402  (ring(), shared by both ends)

# The rocker chamber's floor. It used to BE the spring seat; the springs now
# sit in pockets sunk below it, and tying the two together made every pocket
# zero deep.
CHAMBER_FLOOR = FIRE_FACE + 62.0
CHAMBER_X = 78.0
GUIDE_BOSS_D = 24.0
GUIDE_TOP = GUIDE_Z + GUIDE_LEN
HEAD_BOLT_BOSS_D = 24.0
HEAD_BOLT_SEAT = CHAMBER_FLOOR + 20.0
CAM_PEDESTAL_X = 34.0
CAM_CAP_BOLT_X = 26.0
CAM_CAP_BOLT_D = 9.0
# The head's water jacket is a LADDER, not a slab: a channel up each side
# high in the casting, and a column between every pair of cylinders carrying
# the coolant up to it from the block's own transfer holes.
#
# A slab is what a jacket looks like in section and it cannot be built that
# way here: it crosses every port, every guide, every bolt and both plugs,
# and each of those then needs a sleeve put back through it — and a sleeve
# round an INCLINED port is a disc that pokes out of the face it starts on
# and out of the fire face it ends near, adding metal 8 mm below the gasket.
# The ladder never crosses any of them, so nothing has to be put back.
JACKET_Z = (FIRE_FACE + 47.5, FIRE_FACE + 57.5)   # the side channels
JACKET_X = (48.0, 70.0)
COLUMN_X = (30.0, 50.0)
COLUMN_HY = 12.0
TRANSFER_TOP = FIRE_FACE + 18.0
HEAD_GALLERY_X = -50.0
HEAD_GALLERY_Z = CHAMBER_FLOOR + 6.0
OUTLET_Y = 150.0
OUTLET_D = 28.0
PEDESTAL_TOP = ROCKER_Z + 16.0


def head(app):
    """The cylinder head: flat fire face (a pre-chamber engine's combustion
    space is in the piston and the chamber, not the head), staggered valves,
    a port each side, a pre-chamber per cylinder with its injector and glow
    plug, a water jacket around all of it, and an open rocker chamber split
    at the camshaft's own centreline.

    The ORDER matters and is the whole difficulty of a head: the jacket is a
    slab void, so everything that must stay dry — the ports, the guides, the
    pre-chambers, the head bolts, the glow plugs — has a sleeve put back
    through it BEFORE any of them is drilled. Drill first and the water is in
    the inlet tract, which nothing in a solid model will tell you.
    """
    p = app.part("cylinder head")
    p.var("bore", BORE).var("valve_lift", VALVE_LIFT)
    B.slab(p, -HEAD_X, BLOCK_Y0, FIRE_FACE, HEAD_X, BLOCK_Y1, HEAD_TOP,
           "Head")

    # The chamber the springs, rockers and camshaft live in.
    ch = p.sketch(Plane.xy(CHAMBER_FLOOR), name="Rocker chamber")
    ch.rect((-CHAMBER_X, BLOCK_Y0 + 8.0), (CHAMBER_X, BLOCK_Y1 - 8.0))
    p.extrude(ch.add(), HEAD_H, mode="cut", name="Rocker chamber")
    for i, y in enumerate(CAM_BEARING_Y):
        ped = p.sketch(Plane.xy(CHAMBER_FLOOR), name=f"Cam saddle {i + 1}")
        ped.rect((-CAM_PEDESTAL_X, y - CAM_BEARING_W / 2.0),
                 (CAM_PEDESTAL_X, y + CAM_BEARING_W / 2.0))
        p.extrude(ped.add(), CAM_Z - CHAMBER_FLOOR, mode="add",
                  name=f"Cam saddle {i + 1}")
        for sx in (-1, 1):
            r = p.sketch(Plane.xy(CHAMBER_FLOOR), name=f"Rocker post {i}{sx}")
            # Out to the chamber wall, not past it: the posts stand above
            # the head's own top face, and 4 mm proud of the wall is 4 mm
            # into the cam cover.
            r.rect((sx * (ROCKER_PIVOT_X - 12.0), y - CAM_BEARING_W / 2.0),
                   (sx * CHAMBER_X, y + CAM_BEARING_W / 2.0))
            p.extrude(r.add(), PEDESTAL_TOP - CHAMBER_FLOOR, mode="add",
                      name=f"Rocker post {i}{sx}")

    # ---- the water jacket ------------------------------------------------
    for sx in (-1, 1):
        jk = p.sketch(Plane.xy(JACKET_Z[0]), name=f"Coolant channel {sx}")
        jk.rect((sx * JACKET_X[0], BLOCK_Y0 + 16.0),
                (sx * JACKET_X[1], BLOCK_Y1 - 16.0))
        p.extrude(jk.add(), JACKET_Z[1] - JACKET_Z[0], mode="cut",
                  name=f"Coolant channel {sx}")
    for i, y in enumerate(sorted({y for _, y in COOLANT_HOLES})):
        for sx in (-1, 1):
            col = p.sketch(Plane.xy(TRANSFER_TOP - 2.0),
                           name=f"Coolant column {i}{sx}")
            col.rect((sx * COLUMN_X[0], y - COLUMN_HY),
                     (sx * COLUMN_X[1], y + COLUMN_HY))
            p.extrude(col.add(), JACKET_Z[1] - TRANSFER_TOP + 2.0, mode="cut",
                      name=f"Coolant column {i}{sx}")
    # Guide bosses and head bolt bosses stand through it all.
    for k, cy in enumerate(CYL_Y):
        for kind, (vx, dy) in VALVE.items():
            B.disc(p, (vx, cy + dy), GUIDE_BOSS_D, FIRE_FACE, GUIDE_TOP,
                   f"Guide boss {k + 1} {kind}")
    for x, y in HEAD_BOLT:
        B.disc(p, (x, y), HEAD_BOLT_BOSS_D, FIRE_FACE, HEAD_BOLT_SEAT,
               "Head bolt boss")

    # The spring pockets: an ANNULUS, so the cut leaves each guide's own
    # boss standing in the middle of it.
    for k, cy in enumerate(CYL_Y):
        for kind, (vx, dy) in VALVE.items():
            sp = p.sketch(Plane.xy(SPRING_SEAT_Z), name=f"Spring pocket {k}")
            sp.circle((vx, cy + dy), d=SPRING_POCKET_D)
            sp.circle((vx, cy + dy), d=GUIDE_BOSS_D)
            p.extrude(sp.add(), CHAMBER_FLOOR - SPRING_SEAT_Z, mode="cut",
                      name=f"Spring pocket {k + 1} {kind}")


    # ---- and only now, the holes ----------------------------------------
    tun = p.sketch(Plane.xz(BLOCK_Y1 + 1.0), name="Cam tunnel")
    tun.circle((0, CAM_Z), d=CAM_JOURNAL_D + FIT)
    p.extrude(tun.add(), BLOCK_Y1 - BLOCK_Y0 + 2.0, mode="cut",
              name="Cam tunnel")
    cb = p.sketch(Plane.xy(CAM_Z, flip=True), name="Cam cap bolt holes")
    for y in CAM_BEARING_Y:
        for sx in (-1, 1):
            cb.circle((sx * CAM_CAP_BOLT_X, y), d=CAM_CAP_BOLT_D - 1.5)
    p.extrude(cb.add(), 26.0, mode="cut", name="Cam cap bolt holes")
    for sx in (-1, 1):
        rb = p.sketch(Plane.xz(BLOCK_Y1 + 1.0), name=f"Rocker bore {sx}")
        rb.circle((sx * ROCKER_PIVOT_X, ROCKER_Z), d=ROCKER_SHAFT_D + FIT)
        p.extrude(rb.add(), BLOCK_Y1 - BLOCK_Y0 + 2.0, mode="cut",
                  name=f"Rocker bore {sx}")

    for k, cy in enumerate(CYL_Y):
        for kind, (vx, dy) in VALVE.items():
            y = cy + dy
            seat = p.sketch(Plane.xy(FIRE_FACE), name=f"Seat {k}{kind}")
            seat.circle((vx, y), d=SEAT_OD[kind])
            p.extrude(seat.add(), SEAT_T, mode="cut", name=f"Seat {k}{kind}")
            thr = p.sketch(Plane.xy(FIRE_FACE + SEAT_T), name=f"Throat {k}")
            thr.circle((vx, y), d=VALVE_HEAD_D[kind] - 3.0)
            p.extrude(thr.add(), GUIDE_Z - FIRE_FACE - SEAT_T, mode="cut",
                      name=f"Throat {k}{kind}")
            g = p.sketch(Plane.xy(GUIDE_Z), name=f"Guide bore {k}")
            g.circle((vx, y), d=GUIDE_OD)
            p.extrude(g.add(), GUIDE_LEN, mode="cut",
                      name=f"Guide bore {k}{kind}")
            # ...and a clearance bore ABOVE the guide, up to the chamber.
            # The spring pocket is an annulus, so it leaves an island where
            # the guide boss stands — and the boss now ends below the
            # chamber floor, so without this the last few millimetres of the
            # island are solid metal around a moving stem.
            c = p.sketch(Plane.xy(GUIDE_Z + GUIDE_LEN), name=f"Stem bore {k}")
            c.circle((vx, y), d=STEM_SEAL_D + 2.0)
            p.extrude(c.add(), CHAMBER_FLOOR + 1.0 - GUIDE_Z - GUIDE_LEN,
                      mode="cut", name=f"Stem bore {k}{kind}")
            sx = -1 if kind == "intake" else 1
            B.bore(p, (sx * HEAD_X, y, PORT_Z), (vx, y, GUIDE_Z - 4.0),
                   PORT_D[kind], f"{kind} port {k + 1}")
        pre = p.sketch(Plane.xy(PRE_Z[0]), name=f"Pre-chamber {k + 1}")
        pre.circle((PRE_X, cy + PRE_DY), d=PRE_D)
        p.extrude(pre.add(), PRE_Z[1] - PRE_Z[0], mode="cut",
                  name=f"Pre-chamber {k + 1}")
        thr = p.sketch(Plane.xy(FIRE_FACE), name=f"Pre-chamber throat {k + 1}")
        thr.circle((PRE_X, cy + PRE_DY), d=PRE_THROAT_D)
        p.extrude(thr.add(), PRE_Z[0] - FIRE_FACE, mode="cut",
                  name=f"Pre-chamber throat {k + 1}")
        for nm, face, target, d in (("injector", INJ_FACE, INJ_TARGET, INJ_D),
                                    ("glow plug", GLOW_FACE, GLOW_TARGET,
                                     GLOW_D)):
            B.bore(p, (face[0], cy + face[1], face[2]),
                   (target[0], cy + target[1], target[2]), d,
                   f"{nm} bore {k + 1}")

    # The manifolds' studs, two per port.
    for cy in CYL_Y:
        for kind, (vx, dy) in VALVE.items():
            sx = -1 if kind == "intake" else 1
            for s2 in (-1, 1):
                B.bore(p, (sx * HEAD_X, cy + dy + s2 * MANIFOLD_BOLT_DY,
                           PORT_Z),
                       (sx * (HEAD_X - MANIFOLD_STUD_DEPTH),
                        cy + dy + s2 * MANIFOLD_BOLT_DY, PORT_Z),
                       MANIFOLD_BOLT_D - 1.5, f"{kind} stud {cy}{s2}",
                       over=0.0)

    hb = p.sketch(Plane.xy(FIRE_FACE), name="Head bolt holes")
    for x, y in HEAD_BOLT:
        hb.circle((x, y), d=HEAD_BOLT_D + 1.0)
    p.extrude(hb.add(), HEAD_BOLT_SEAT - FIRE_FACE, mode="cut",
              name="Head bolt holes")

    # ---- the galleries ---------------------------------------------------
    # Coolant crosses the gasket at the block's own ten holes; oil comes up
    # the same riser the block drilled, and is fed along the rocker chamber
    # to each cam saddle.
    ct = p.sketch(Plane.xy(FIRE_FACE), name="Coolant transfers")
    for x, y in COOLANT_HOLES:
        ct.circle((x, y), d=COOLANT_D)
    p.extrude(ct.add(), TRANSFER_TOP - FIRE_FACE + 2.0, mode="cut",
              name="Coolant transfers")
    orf = p.sketch(Plane.xy(FIRE_FACE), name="Oil riser")
    orf.circle((OIL_RISER_X, CYL_Y[0]), d=OIL_RISER_D)
    p.extrude(orf.add(), CHAMBER_FLOOR - FIRE_FACE, mode="cut",
              name="Oil riser")
    gal = p.sketch(Plane.xz(BLOCK_Y1 - 6.0), name="Cam oil gallery")
    gal.circle((HEAD_GALLERY_X, HEAD_GALLERY_Z), d=8.0)
    p.extrude(gal.add(), BLOCK_Y1 - BLOCK_Y0 - 12.0, mode="cut",
              name="Cam oil gallery")
    for i, y in enumerate(CAM_BEARING_Y):
        B.bore(p, (HEAD_GALLERY_X, y, HEAD_GALLERY_Z), (0, y, CAM_Z), 5.0,
               f"Cam bearing feed {i + 1}", over=0.0)
    # ...and the coolant's way out, on the intake side ahead of the chain
    # case, which is the only face of this head that is not covered.
    B.bore(p, (-HEAD_X, BLOCK_Y0 + OUTLET_Y, sum(JACKET_Z) / 2.0),
           (-JACKET_X[1] + 4.0, BLOCK_Y0 + OUTLET_Y, sum(JACKET_Z) / 2.0),
           OUTLET_D, "Coolant outlet", over=0.0)

    fr = p.sketch(Plane.xz(BLOCK_Y0 + 22.0), name="Cover bolt holes")
    for x, z in COVER_BOLTS:
        if z >= DECK - 20.0:
            fr.circle((x, z), d=7.5)
    p.extrude(fr.add(), 22.0, mode="cut", name="Cover bolt holes")
    nose = p.sketch(Plane.xz(BLOCK_Y0 + 24.0), name="Cam nose clearance")
    nose.circle((0, CAM_Z), d=CAM_JOURNAL_D + 8.0)
    p.extrude(nose.add(), 24.0, mode="cut", name="Cam nose clearance")

    p.material("steel")
    p.frame("deck", origin=(0, 0, FIRE_FACE), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("top", origin=(0, 0, CAM_Z), z=(0, 0, 1), x=(1, 0, 0))
    # The cover sits a gasket ABOVE the head's face, not on it — and the
    # gasket sits on the face, not under it. Mated to the same frame, the
    # gasket was 3 mm below the head's top face, which is inside the head,
    # the camshaft and every rocker boss.
    p.frame("cover_rail", origin=(0, 0, CAM_Z + CAM_GASKET_T), z=(0, 0, 1),
            x=(1, 0, 0))
    p.frame("front_face", origin=(0, BLOCK_Y0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("cam_axis", origin=(0, 0, CAM_Z), z=(0, 1, 0), x=(1, 0, 0))
    for sx, nm in ((-1, "intake_face"), (1, "exhaust_face")):
        p.frame(nm, origin=(sx * HEAD_X, 0, PORT_Z), z=(0, 0, 1),
                x=(1, 0, 0))
    p.frame("outlet", origin=(-HEAD_X, BLOCK_Y0 + OUTLET_Y,
                              sum(JACKET_Z) / 2.0), z=(-1, 0, 0),
            x=(0, 1, 0))
    for i, y in enumerate(CAM_BEARING_Y):
        p.frame(f"cam_cap_{i}", origin=(0, y, CAM_Z), z=(0, 0, 1),
                x=(1, 0, 0))
    for sx, nm in ((-1, "rocker_shaft_intake"), (1, "rocker_shaft_exhaust")):
        p.frame(nm, origin=(sx * ROCKER_PIVOT_X, 0, ROCKER_Z), z=(0, 1, 0),
                x=(1, 0, 0))
    for k, cy in enumerate(CYL_Y):
        for kind, (vx, dy) in VALVE.items():
            y = cy + dy
            # Z points DOWN the guide, so the cam relation's positive lift
            # OPENS the valve into the cylinder instead of pulling it up.
            p.frame(f"guide_{k}_{kind}", origin=(vx, y, FIRE_FACE),
                    z=(0, 0, -1), x=(1, 0, 0))
            p.frame(f"guide_seat_{k}_{kind}", origin=(vx, y, GUIDE_Z),
                    z=(0, 0, 1), x=(1, 0, 0))
            p.frame(f"seat_{k}_{kind}", origin=(vx, y, FIRE_FACE),
                    z=(0, 0, 1), x=(1, 0, 0))
            p.frame(f"spring_{k}_{kind}", origin=(vx, y, SPRING_SEAT_Z),
                    z=(0, 0, 1), x=(1, 0, 0))
            # The rocker's own pivot. The arm runs INBOARD from its shaft to
            # the cam, so the frame's x is -sx: the rocker is drawn once,
            # pivot at the origin and arm along +x, and the exhaust side gets
            # it turned about z rather than a second, mirrored part.
            sx = -1 if kind == "intake" else 1
            p.frame(f"rocker_{k}_{kind}",
                    origin=(sx * ROCKER_PIVOT_X, y, ROCKER_Z),
                    z=(0, -sx, 0), x=(-sx, 0, 0))
        p.frame(f"pre_{k}", origin=(PRE_X, cy + PRE_DY, PRE_Z[1]),
                z=(0, 0, -1), x=(1, 0, 0))
        for nm, face, target in (("injector", INJ_FACE, INJ_TARGET),
                                 ("glow_plug", GLOW_FACE, GLOW_TARGET)):
            start = V((face[0], cy + face[1], face[2]))
            d = (V((target[0], cy + target[1], target[2])) - start).unit()
            p.frame(f"{nm}_{k}", origin=tuple(start), z=tuple(d),
                    x=tuple((V((0, 1, 0)) - d * d.dot(V((0, 1, 0)))).unit()))
    p.check()
    return p


def head_gasket(app):
    """The gasket: the block's deck outline, the four bores, and every hole
    that has to pass through it — drawn from the same lists the two faces are.
    """
    p = app.part("head gasket")
    sk = p.sketch(Plane.xy(DECK), name="Gasket")
    sk.rect((-DECK_X, BLOCK_Y0), (DECK_X, BLOCK_Y1))
    p.extrude(sk.add(), GASKET_T, mode="new_body", name="Gasket")
    holes = p.sketch(Plane.xy(DECK - 1.0), name="Holes")
    for y in CYL_Y:
        holes.circle((0, y), d=BORE + 1.0)
    for x, y in HEAD_BOLT:
        holes.circle((x, y), d=HEAD_BOLT_D + 1.0)
    p.extrude(holes.add(), GASKET_T + 2.0, mode="cut", name="Holes")
    p.material("copper")
    p.frame("deck", origin=(0, 0, DECK), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


# --------------------------------------------------------------- the valves
def valve(app, kind):
    """A valve, turned, with everything ABOVE its seat plane: a closed valve
    that hangs into the cylinder is through the piston crown at TDC, and the
    motion study is right to say so."""
    d = VALVE_HEAD_D[kind]
    p = app.part(f"{kind} valve")
    p.revolve_profile([(0, 0), (d / 2.0, 0), (d / 2.0, 3.0),
                       (STEM_D / 2.0, 11.0), (STEM_D / 2.0, VALVE_LEN),
                       (0, VALVE_LEN)],
                      plane=Plane.xz(), axis="z", name=f"{kind} valve")
    # The collet groove, near the top of the stem.
    g = p.sketch(Plane.xz(), name="Collet groove")
    g.polyline([(STEM_D / 2.0 - 1.2, VALVE_LEN - RETAINER_DROP + 3.5),
                (STEM_D / 2.0 + 0.5, VALVE_LEN - RETAINER_DROP + 3.5),
                (STEM_D / 2.0 + 0.5, VALVE_LEN - RETAINER_DROP),
                (STEM_D / 2.0 - 1.2, VALVE_LEN - RETAINER_DROP)])
    p.revolve(g.add(), axis="z", mode="cut", name="Collet groove")
    p.material("stainless_steel")
    # Z DOWN the valve, to match the guide's: a mate aligns z to z, and a
    # valve whose frame pointed up is mated upside down.
    p.frame("seat", origin=(0, 0, 0), z=(0, 0, -1), x=(1, 0, 0))
    p.frame("retainer", origin=(0, 0, VALVE_LEN - RETAINER_DROP - 0.5),
            z=(0, 0, 1), x=(1, 0, 0))
    for nm, sx in (("collet_a", 1), ("collet_b", -1)):
        p.frame(nm, origin=(0, 0, VALVE_LEN - RETAINER_DROP), z=(0, 0, 1),
                x=(sx, 0, 0))
    p.check()
    return p


def valve_guide(app):
    p = B.ring(app, "valve guide", GUIDE_OD, STEM_D + FIT, GUIDE_LEN,
               material="bronze")
    p.frame("bottom", origin=(0, 0, -GUIDE_LEN / 2.0), z=(0, 0, 1),
            x=(1, 0, 0))
    p.frame("top", origin=(0, 0, GUIDE_LEN / 2.0), z=(0, 0, 1), x=(1, 0, 0))
    return p


def valve_seat(app, kind):
    """The seat insert: a hard ring shrunk into the head's counterbore, which
    is what a valve actually closes against."""
    p = B.ring(app, f"{kind} valve seat", SEAT_OD[kind],
               VALVE_HEAD_D[kind] + FIT, SEAT_T, material="stainless_steel")
    p.frame("bottom", origin=(0, 0, -SEAT_T / 2.0), z=(0, 0, 1), x=(1, 0, 0))
    return p


def stem_seal(app):
    p = B.ring(app, "valve stem seal", STEM_SEAL_D, STEM_D + FIT,
               STEM_SEAL_H, material="rubber")
    p.frame("bottom", origin=(0, 0, -STEM_SEAL_H / 2.0), z=(0, 0, 1),
            x=(1, 0, 0))
    return p


def valve_spring(app):
    """A helix of round wire, seated on the head and compressed by the
    retainer — drawn as it is wound, not as a tube."""
    p = app.part("valve spring")
    # Drawn at its SHORTEST — the length it is at full valve lift, less a
    # couple of millimetres. The geometry is rigid, so a spring drawn at its
    # free length is inside the retainer as soon as the valve opens, and the
    # clash check is right to say so.
    free = ((VALVE_LEN - RETAINER_DROP - 0.5 + FIRE_FACE - SPRING_SEAT_Z)
            - VALVE_LIFT - 2.0)
    # Four turns, not five: the pitch is the free length over the turns, and
    # on this valve five put the coils closer together than the wire is
    # thick — a spring drawn coil-bound before it has compressed at all.
    turns = 4.0
    if free / turns <= SPRING_WIRE:
        raise SdkError(f"the valve spring is {free:.1f} long over {turns} "
                       f"turns: its coils are closer than the {SPRING_WIRE} "
                       f"mm wire is thick")
    p.app.add_feature(feature={
        "type": "Curve3D", "name": "Spring path", "kind": "helix",
        "origin": [0, 0, 0], "axis": [0, 0, 1], "radius": SPRING_D / 2.0,
        "pitch": free / turns, "turns": turns})
    coil = p.state()["features"][-1]["id"]
    # SOLID wire (no wall_thickness): a spring is round bar coiled up, not
    # tubing — and a bore is a second swept surface for a drawing's
    # hidden-line pass to classify against.
    p.app.add_feature(feature={
        "type": "Pipe", "name": "Spring", "path_id": coil,
        "outer_diameter": SPRING_WIRE, "mode": "add"})
    p.material("steel")
    p.frame("seat", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def spring_retainer(app):
    p = app.part("spring retainer")
    p.revolve_profile([(STEM_D / 2.0 + 0.5, 0), (RETAINER_D / 2.0, 0),
                       (RETAINER_D / 2.0, 4.0), (STEM_D / 2.0 + 4.0, 10.0),
                       (STEM_D / 2.0 + 0.5, 10.0)],
                      plane=Plane.xz(), axis="z", name="Retainer")
    p.material("steel")
    p.frame("seat", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def collet(app):
    """Half a split cone. Two of them lock the retainer into the stem's
    groove, which is the whole reason the groove is there."""
    p = app.part("valve collet")
    # A RIB at the bottom that sits in the stem's groove, and a wider bore
    # above it. Bored to the groove's root all the way up, a collet is
    # inside the stem everywhere the groove is not.
    root = STEM_D / 2.0 - 1.1
    p.revolve_profile([(root, 0), (STEM_D / 2.0 + 3.0, 0),
                       (STEM_D / 2.0 + 5.5, 7.0), (STEM_D / 2.0 + 0.2, 7.0),
                       (STEM_D / 2.0 + 0.2, 3.5), (root, 3.5)],
                      plane=Plane.xz(), axis="z", angle=170.0, name="Collet")
    p.material("steel")
    p.frame("seat", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


# ---------------------------------------------------- the pre-chamber and co
def prechamber(app):
    """The pre-combustion chamber: a steel insert with a swirl chamber above
    the fire face and a throat down into the cylinder. This is the part that
    makes it an OM615 and not a direct-injection four."""
    p = app.part("pre-chamber")
    h = PRE_Z[1] - PRE_Z[0]
    p.revolve_profile([(PRE_THROAT_D / 2.0, 0), (PRE_D / 2.0 - FIT / 2.0, 0),
                       (PRE_D / 2.0 - FIT / 2.0, h), (0, h), (0, h - 7.0),
                       (PRE_D / 2.0 - 5.0, h - 7.0),
                       (PRE_D / 2.0 - 5.0, 8.0), (PRE_THROAT_D / 2.0, 8.0)],
                      plane=Plane.xz(), axis="z", name="Pre-chamber")
    p.material("stainless_steel")
    p.frame("seat", origin=(0, 0, h), z=(0, 0, -1), x=(1, 0, 0))
    p.check()
    return p


def _plug(app, name, d, reach, head_len, head_d, material="steel"):
    """A glow plug or an injector: a body that goes IN and a head that stays
    out, drawn along +z from the face it screws into."""
    p = app.part(name)
    p.revolve_profile([(0, -head_len), (head_d / 2.0, -head_len),
                       (head_d / 2.0, -3.0), (d / 2.0, -3.0),
                       (d / 2.0, reach - 6.0), (d / 2.0 - 2.0, reach),
                       (0, reach)],
                      plane=Plane.xz(), axis="z", name=name)
    p.material(material)
    p.frame("face", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def glow_plug(app):
    reach = (V(GLOW_TARGET) - V(GLOW_FACE)).length() + 4.0
    return _plug(app, "glow plug", GLOW_D - FIT, reach, 22.0, 17.0)


def injector(app):
    reach = (V(INJ_TARGET) - V(INJ_FACE)).length() + 4.0
    return _plug(app, "injector", INJ_D - FIT, reach, 46.0, 22.0)


# ---------------------------------------------------------- the valve gear
def camshaft(app):
    """One shaft, five journals, eight lobes — and the lobes are the SAME
    function the cam relations use, so the picture and the kinematics cannot
    drift apart. A lobe drawn at polar angle psi from straight down gives the
    lift at camshaft angle psi, because that is the point that comes round to
    the follower when the shaft has turned psi."""
    p = app.part("camshaft")
    first = ["new_body"]

    def section(y0, y1, d, name, profile=None):
        sk = p.sketch(Plane.xz(y1), name=name)
        if profile is None:
            sk.circle((0, CAM_Z), d=d)
        else:
            sk.polyline(profile)
        mode, first[0] = first[0], "add"
        p.extrude(sk.add(), y1 - y0, mode=mode, name=name)

    section(CAM_NOSE_Y, CAM_BEARING_Y[0] - CAM_BEARING_W / 2.0, 30.0, "Nose")
    for i, y in enumerate(CAM_BEARING_Y):
        section(y - CAM_BEARING_W / 2.0, y + CAM_BEARING_W / 2.0,
                CAM_JOURNAL_D, f"Journal {i + 1}")
        if i < len(CAM_BEARING_Y) - 1:
            section(y + CAM_BEARING_W / 2.0,
                    CAM_BEARING_Y[i + 1] - CAM_BEARING_W / 2.0, CAM_SHAFT_D,
                    f"Shaft {i + 1}")
    if CAM_TAIL_Y > CAM_BEARING_Y[-1] + CAM_BEARING_W / 2.0:
        section(CAM_BEARING_Y[-1] + CAM_BEARING_W / 2.0, CAM_TAIL_Y,
                CAM_SHAFT_D, "Tail")
    for k, cy in enumerate(CYL_Y):
        for kind, (_, dy) in VALVE.items():
            y = cy + dy
            section(y - CAM_LOBE_W / 2.0, y + CAM_LOBE_W / 2.0, 0,
                    f"Lobe {k + 1} {kind}", profile=lobe_profile(k, kind))
    p.material("steel")
    p.frame("axle", origin=(0, 0, CAM_Z), z=(0, 1, 0), x=(1, 0, 0))
    p.frame("drive", origin=(0, DRIVE_Y, CAM_Z), z=(0, 1, 0), x=(1, 0, 0))
    p.check()
    return p


CAM_NOSE_Y = DRIVE_Y - CHAIN_W / 2.0 - 4.0
CAM_TAIL_Y = BLOCK_Y1 - 22.0      # short of the cam cover's own rear rail
LOBE_STEPS = 180


def lobe_start(k, kind):
    """The camshaft angle at which this cylinder's lobe starts to lift."""
    base = INLET_OPEN_CAM if kind == "intake" else EXHAUST_OPEN_CAM
    return (base + cam_phase(k)) % 360.0


def lobe_profile(k, kind):
    """The drawn lobe, as (x, z) points about the cam axis."""
    start = lobe_start(k, kind)
    pts = []
    for i in range(LOBE_STEPS):
        psi = 360.0 * i / LOBE_STEPS
        r = CAM_BASE_R + lobe_lift(psi, start, rise=CAM_LIFT)
        a = math.radians(psi)
        pts.append((r * math.sin(a), CAM_Z - r * math.cos(a)))
    return pts


def cam_cap(app):
    """A bearing cap for the camshaft, on the head's own top face."""
    p = app.part("cam bearing cap")
    sk = p.sketch(Plane.xz(CAM_BEARING_W / 2.0), name="Cap")
    sk.rect((-CAM_PEDESTAL_X, 0.0), (CAM_PEDESTAL_X, 22.0))
    p.extrude(sk.add(), CAM_BEARING_W, mode="new_body", name="Cap")
    bore = p.sketch(Plane.xz(CAM_BEARING_W), name="Bore")
    bore.circle((0, 0), d=CAM_JOURNAL_D + FIT)
    p.extrude(bore.add(), 2 * CAM_BEARING_W, mode="cut", name="Bore")
    bolts = p.sketch(Plane.xy(-1.0), name="Bolt holes")
    for sx in (-1, 1):
        bolts.circle((sx * CAM_CAP_BOLT_X, 0), d=CAM_CAP_BOLT_D + 0.5)
    p.extrude(bolts.add(), 24.0, mode="cut", name="Bolt holes")
    p.material("aluminum")
    p.frame("seat", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def rocker(app):
    """A rocker arm: pivot at its origin, valve pad at ARM_VALVE hanging
    under the arm, cam pad at ARM_CAM standing PROUD of it.

    Which side of the arm each pad is on is the whole shape of the part. The
    cam is above and presses down, so its pad is on top; the valve is below
    and is pressed down, so its pad is underneath. And the cam pad stands 8
    mm up, so the lobe's flanks pass over the arm instead of through it.
    """
    p = app.part("rocker arm")
    w = 14.0
    boss = p.sketch(Plane.xz(w / 2.0), name="Pivot boss")
    boss.circle((0, 0), r=16.0)
    p.extrude(boss.add(), w, mode="new_body", name="Pivot boss")
    tip = ARM_CAM + 6.0
    arm = p.sketch(Plane.xz(w / 2.0), name="Arm")
    arm.polyline([(0, arm_top(0)), (tip, arm_top(tip)),
                  (tip, arm_top(tip) - ARM_T), (0, arm_top(0) - ARM_T)])
    p.extrude(arm.add(), w, mode="add", name="Arm")
    pad = p.sketch(Plane.xz(w / 2.0), name="Cam pad")
    pad.rect((ARM_CAM - 5.0, arm_top(ARM_CAM) - 2.0),
             (ARM_CAM + 5.0, PAD_Z - ROCKER_Z))
    p.extrude(pad.add(), w, mode="add", name="Cam pad")
    vp = p.sketch(Plane.xz(w / 2.0), name="Valve pad")
    # Narrow: the pad TILTS as the rocker turns, and a wide one catches its
    # own outer corner on the spring retainer under it.
    vp.rect((ARM_VALVE - 2.5, VALVE_PAD_Z - ROCKER_Z),
            (ARM_VALVE + 2.5, arm_top(ARM_VALVE) - ARM_T + 2.0))
    p.extrude(vp.add(), w, mode="add", name="Valve pad")
    bore = p.sketch(Plane.xz(w), name="Pivot bore")
    bore.circle((0, 0), d=ROCKER_SHAFT_D + FIT)
    p.extrude(bore.add(), 2 * w, mode="cut", name="Pivot bore")
    p.material("steel")
    p.frame("pivot", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.check()
    return p


def rocker_shaft(app):
    p = B.ring(app, "rocker shaft", ROCKER_SHAFT_D, 8.0,
               BLOCK_Y1 - BLOCK_Y0 - 24.0, material="steel")
    return p


def cam_cover(app):
    """The cam cover, drawn as a SECTION and extruded along the engine: a
    bolting rail, a step, and a raised roof over the camshaft. It seals on
    the head's top face, which is also the camshaft's own centreline."""
    p = app.part("cam cover")
    hx = HEAD_X

    def section(sk, o):
        # OPEN at the bottom when inset: the cover sits ON the head's rail,
        # so an inset floor is 6 mm of aluminium across the whole cover at
        # exactly the height of the camshaft and every rocker's pivot boss.
        floor = CAM_Z - 1.0 if o else CAM_Z
        sk.polyline([(-hx + o, floor), (hx - o, floor),
                     (hx - o, CAM_Z + 22.0), (hx - 16.0, CAM_Z + 32.0),
                     (hx - 16.0, CAM_COVER_TOP - o),
                     (-hx + 16.0, CAM_COVER_TOP - o),
                     (-hx + 16.0, CAM_Z + 32.0), (-hx + o, CAM_Z + 22.0)])

    outer = p.sketch(Plane.xz(BLOCK_Y1), name="Cover")
    section(outer, 0.0)
    p.extrude(outer.add(), BLOCK_Y1 - BLOCK_Y0, mode="new_body", name="Cover")
    inner = p.sketch(Plane.xz(BLOCK_Y1 - 6.0), name="Inside")
    section(inner, 6.0)
    p.extrude(inner.add(), BLOCK_Y1 - BLOCK_Y0 - 12.0, mode="cut",
              name="Inside")
    # The camshaft comes out of the front of the cover, so the cover has a
    # notch for it — and its gasket does too.
    B.bore(p, (0, BLOCK_Y0 - 1.0, CAM_Z), (0, BLOCK_Y0 + 20.0, CAM_Z),
           CAM_JOURNAL_D + 10.0, "Cam nose notch", over=0.0)
    filler = p.sketch(Plane.xy(CAM_COVER_TOP - 1.0), name="Filler")
    filler.circle((0, BLOCK_Y1 - 40.0), d=44.0)
    p.extrude(filler.add(), 8.0, mode="cut", name="Filler")
    holes = p.sketch(Plane.xy(CAM_Z - 1.0), name="Bolt holes")
    for y in _cover_bolt_y():
        for sx in (-1, 1):
            holes.circle((sx * (HEAD_X - 4.0), y), d=8.0)
    p.extrude(holes.add(), 10.0, mode="cut", name="Bolt holes")
    p.material("aluminum")
    p.frame("rail", origin=(0, 0, CAM_Z), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("filler", origin=(0, BLOCK_Y1 - 40.0, CAM_COVER_TOP - 1.0),
            z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def _cover_bolt_y():
    n = 6
    y0, y1 = BLOCK_Y0 + 20.0, BLOCK_Y1 - 20.0
    return [y0 + (y1 - y0) * i / (n - 1) for i in range(n)]


def cam_cover_gasket(app):
    p = app.part("cam cover gasket")
    sk = p.sketch(Plane.xy(CAM_Z), name="Gasket")
    sk.rect((-HEAD_X, BLOCK_Y0), (HEAD_X, BLOCK_Y1))
    # The opening has to clear the ROCKER BOSSES, which reach 76 mm out —
    # a wide gasket rim is a 3 mm shelf right where they turn.
    sk.rect((-HEAD_X + 8.0, BLOCK_Y0 + 8.0), (HEAD_X - 8.0, BLOCK_Y1 - 8.0))
    p.extrude(sk.add(), CAM_GASKET_T, mode="new_body", name="Gasket")
    B.bore(p, (0, BLOCK_Y0 - 1.0, CAM_Z), (0, BLOCK_Y0 + 20.0, CAM_Z),
           CAM_JOURNAL_D + 10.0, "Cam nose notch", over=0.0)
    p.material("rubber")
    p.frame("rail", origin=(0, 0, CAM_Z), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def oil_filler_cap(app):
    p = app.part("oil filler cap")
    p.revolve_profile([(0, 0), (21.0, 0), (21.0, 6.0), (26.0, 6.0),
                       (26.0, 14.0), (0, 14.0)], plane=Plane.xz(), axis="z",
                      name="Filler cap")
    p.material("plastic_black")
    p.frame("seat", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


# ------------------------------------------------------------ the manifolds
def _manifold(app, name, kind, log, runner_d, material="steel"):
    """A log manifold: a flange pad per port, a runner from each into a log
    that runs along the engine, and a flange on the end of the log.

    Drawn in engine coordinates like the head it bolts to, so its frame only
    translates. The runners CONVERGE — each ends at a fraction of its own
    cylinder's y — which is what makes it look like a manifold rather than
    four pipes into a rail.
    """
    p = app.part(name)
    sx = -1 if kind == "intake" else 1
    lx, lz, od, bore_d = log
    # The flange starts a gasket's thickness off the head, so the manifold
    # and its gasket mate to the SAME frame and neither is placed by a
    # number the other has to agree with.
    face = sx * (HEAD_X + MANIFOLD_GASKET_T)
    pad = sx * (HEAD_X + MANIFOLD_GASKET_T + MANIFOLD_PAD)
    first = ["new_body"]
    for k, cy in enumerate(CYL_Y):
        vx, dy = VALVE[kind]
        y = cy + dy
        sk = p.sketch(Plane.xz(y + 34.0), name=f"Flange {k + 1}")
        sk.rect((min(face, pad), PORT_Z - 30.0), (max(face, pad),
                                                  PORT_Z + 30.0))
        mode, first[0] = first[0], "add"
        p.extrude(sk.add(), 68.0, mode=mode, name=f"Flange {k + 1}")
        B.axis_solid(p, (pad, y, PORT_Z), (lx, cy * MANIFOLD_CONVERGE, lz),
                     runner_d, f"Runner {k + 1}")
    # The log itself, and the flange on its end.
    y0 = CYL_Y[0] * MANIFOLD_CONVERGE - 40.0
    y1 = CYL_Y[-1] * MANIFOLD_CONVERGE + 40.0
    lg = p.sketch(Plane.xz(y1), name="Log")
    lg.circle((lx, lz), d=od)
    p.extrude(lg.add(), y1 - y0, mode="add", name="Log")
    out_y = y0 if kind == "intake" else y1
    fl = p.sketch(Plane.xz(out_y + (0.0 if kind == "intake" else 14.0)),
                  name="Outlet flange")
    fl.circle((lx, lz), d=od + 28.0)
    p.extrude(fl.add(), 14.0, mode="add", name="Outlet flange")
    # ...and only now the inside, so every runner opens into the log.
    for k, cy in enumerate(CYL_Y):
        y = cy + VALVE[kind][1]
        B.bore(p, (face, y, PORT_Z), (lx, cy * MANIFOLD_CONVERGE, lz),
               PORT_D[kind], f"Runner bore {k + 1}", over=0.0)
    hollow = p.sketch(Plane.xz(y1 + 16.0), name="Log bore")
    hollow.circle((lx, lz), d=bore_d)
    p.extrude(hollow.add(), y1 - y0 + 32.0, mode="cut", name="Log bore")
    for cy in CYL_Y:
        for s2 in (-1, 1):
            y = cy + VALVE[kind][1] + s2 * MANIFOLD_BOLT_DY
            B.bore(p, (pad, y, PORT_Z), (face, y, PORT_Z),
                   MANIFOLD_BOLT_D, f"Stud hole {cy}{s2}", over=1.0)
    p.material(material)
    p.frame("face", origin=(sx * HEAD_X, 0, PORT_Z), z=(0, 0, 1),
            x=(1, 0, 0))
    p.check()
    return p


def exhaust_manifold(app):
    return _manifold(app, "exhaust manifold", "exhaust", EXH_LOG,
                     EXH_RUNNER_D, material="steel")


def intake_manifold(app):
    return _manifold(app, "intake manifold", "intake", INT_LOG,
                     INT_RUNNER_D, material="aluminum")


def manifold_gasket(app, kind):
    """One gasket per side: the four ports and the eight studs, on the same
    lists the head drills and the manifold does."""
    sx = -1 if kind == "intake" else 1
    p = app.part(f"{kind} manifold gasket")
    face = sx * HEAD_X
    t = MANIFOLD_GASKET_T
    first = ["new_body"]
    for k, cy in enumerate(CYL_Y):
        y = cy + VALVE[kind][1]
        sk = p.sketch(Plane.xz(y + 34.0), name=f"Gasket {k + 1}")
        sk.rect((min(face, face + sx * t), PORT_Z - 30.0),
                (max(face, face + sx * t), PORT_Z + 30.0))
        mode, first[0] = first[0], "add"
        p.extrude(sk.add(), 68.0, mode=mode, name=f"Gasket {k + 1}")
        B.bore(p, (face + sx * 3.0, y, PORT_Z), (face - sx * 3.0, y, PORT_Z),
               PORT_D[kind] + 2.0, f"Port {k + 1}", over=0.0)
        for s2 in (-1, 1):
            yy = y + s2 * MANIFOLD_BOLT_DY
            B.bore(p, (face + sx * 3.0, yy, PORT_Z),
                   (face - sx * 3.0, yy, PORT_Z), MANIFOLD_BOLT_D + 1.0,
                   f"Stud {k + 1}{s2}", over=0.0)
    p.material("copper")
    p.frame("face", origin=(face, 0, PORT_Z), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p
