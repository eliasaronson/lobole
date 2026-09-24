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
                                "..", "..", "tools"))
from designer_sdk import Plane, SdkError, V  # noqa: E402
# This generator's own directory, and generators/ above it for _lib.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from om615_spec import *  # noqa: E402,F401,F403
import om615_bottom as B  # noqa: E402  (ring(), shared by both ends)

# The rocker chamber's floor. It used to BE the spring seat; the springs now
# sit in pockets sunk below it, and tying the two together made every pocket
# zero deep.
CHAMBER_FLOOR = FIRE_FACE + 62.0
CHAMBER_X = 78.0
# The island the spring pocket's annulus leaves standing around each guide.
# It is not a BOSS: nothing is added here, because the ladder jacket never
# comes near the guides.
GUIDE_BOSS_D = 24.0
HEAD_BOLT_SEAT = HEAD_TOP - 15.0
CAM_PEDESTAL_X = 30.5
# Both plugs reach PAST the point they are aimed at, into the pre-chamber —
# which is the point of them, and is why the pre-chamber is drilled for both.
GLOW_HEAD_D = 17.0
INJ_HEAD_D = 22.0
PLUG_REACH_PAST = 4.0
PLUG_WELL_DEPTH = 5.0
PLUG_MOUTH_BACK = 8.0                # how far outside the face each bore
                                     # starts, so the shank's proud 3 mm has
                                     # somewhere to be
CAM_CAP_BOLT_X = 25.0
CAM_CAP_H = 22.0                     # the bearing cap, above the cam axis
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
HEAD_GALLERY_X = -75.0
HEAD_GALLERY_Z = CHAMBER_FLOOR + 6.0
OUTLET_Y = 150.0
OUTLET_D = 28.0
PEDESTAL_TOP = ROCKER_Z + 16.0
# A rocker post stands over a head bolt: the shaft is at x = 60 and the bolt
# at 58, so they cannot both have the same 20 mm of chamber. So the post is
# a CORBEL — a rib on the chamber wall for its full height, reaching in to
# the shaft only above the bolt's head. Drawn as one pillar it swallowed the
# bolt head whole, ten times, and nothing outside the head showed it.
HEAD_BOLT_HEAD_H = HEAD_BOLT_D          # ISO 4762: head height = the size
POST_ROOT_X = HEAD_BOLT_X + HEAD_BOLT_BOSS_D / 2.0 + 2.0
POST_CORBEL_Z = HEAD_BOLT_SEAT + HEAD_BOLT_HEAD_H + 2.0


def head(app):
    """An 85 mm head casting with separate raised cam/rocker supports.

    Six external ports serve eight valves on +X. Four inclined prechambers
    enter from -X. Coolant and oil routing are modeled, not factory core data.
    """
    p = app.part("cylinder head")
    p.var("bore", BORE).var("valve_lift", VALVE_LIFT)
    # Drawn as a SECTION with its lower outer edges chamfered, not as a box.
    # A casting has draft and a radius everywhere a box has a corner, and the
    # difference is most of what makes one look like the other.
    sec = p.sketch(Plane.xz(BLOCK_Y1), name="Head")
    sec.polyline([(-HEAD_X + HEAD_CHAMFER, FIRE_FACE),
                  (HEAD_X - HEAD_CHAMFER, FIRE_FACE),
                  (HEAD_X-2.5, FIRE_FACE + HEAD_CHAMFER), (HEAD_X, HEAD_TOP),
                  (-HEAD_X, HEAD_TOP), (-HEAD_X+2.5, FIRE_FACE + HEAD_CHAMFER)])
    p.extrude(sec.add(), BLOCK_Y1 - BLOCK_Y0, mode="new_body", name="Head")

    # Drafted walls end in local machined pads. Port and gasket datums are
    # on these faces; the surrounding casting has release taper.
    for kind, (_, dy) in VALVE.items():
        sx = PORT_SIDE
        for k, py in enumerate(port_centers(kind)):
            pad = p.sketch(B.axis_plane((sx*(HEAD_X-5), py, PORT_Z),
                                        (sx, 0, 0)),
                           name=f"{kind} machined pad {k+1}")
            B.rounded_polygon(pad, [(-MANIFOLD_PAD_HY,-MANIFOLD_PAD_HZ),
                                    (MANIFOLD_PAD_HY,-MANIFOLD_PAD_HZ),
                                    (MANIFOLD_PAD_HY,MANIFOLD_PAD_HZ),
                                    (-MANIFOLD_PAD_HY,MANIFOLD_PAD_HZ)], 8)
            p.extrude(pad.add(), 5, mode="add", name=f"{kind} port pad {k+1}")

    # The manifold side is not a flat wall: a rib runs its length under the
    # cover rail, clear above the manifold flanges, drafted on its underside.
    proud, tall = HEAD_RIB
    rib = p.sketch(Plane.xz(BLOCK_Y1), name="Side rib")
    rib.polyline([(HEAD_X - 1.0, HEAD_TOP - tall - proud),
                  (HEAD_X + proud, HEAD_TOP - tall),
                  (HEAD_X + proud, HEAD_TOP), (HEAD_X - 1.0, HEAD_TOP)])
    p.extrude(rib.add(), BLOCK_Y1 - BLOCK_Y0, mode="add", name="Side rib")
    # ...and at its front end, the ear the thermostat housing bolts to: out
    # over the first exhaust flange, and forward past the head's front face
    # only where the timing cover is not (outboard of it).
    ear_x0 = HEAD_X - 1.0
    ear_x1 = TSTAT_X + TSTAT_BODY_D / 2.0
    ear_y0 = TSTAT_Y - TSTAT_BOLT_DY - TSTAT_BOLT_D - 2.0
    ear_y1 = TSTAT_Y + TSTAT_BOLT_DY + TSTAT_BOLT_D + 2.0
    ear_front_x = HEAD_X + COVER_LIP + 1.0
    ear = p.sketch(Plane.xy(TSTAT_EAR_Z), name="Thermostat ear")
    B.rounded_polygon(ear, [(ear_x0, BLOCK_Y0), (ear_front_x, BLOCK_Y0),
                            (ear_front_x, ear_y0), (ear_x1, ear_y0),
                            (ear_x1, ear_y1), (ear_x0, ear_y1)],
                      [0, 0, 6, 10, 10, 0])
    p.extrude(ear.add(), HEAD_TOP - TSTAT_EAR_Z, mode="add",
              name="Thermostat ear")
    # A web under its front edge, down the head's side ahead of the flange.
    web = p.sketch(Plane.xz(BLOCK_Y0 + TSTAT_WEB_T), name="Thermostat web")
    # (Rooted INSIDE the drafted wall, which is 2.5 mm in at the chamfer.)
    web.polyline([(HEAD_X - 4.0, TSTAT_EAR_Z - 2.0 * TSTAT_BOLT_DY),
                  (ear_x1 - TSTAT_BODY_D / 2.0, TSTAT_EAR_Z + 1.0),
                  (HEAD_X - 4.0, TSTAT_EAR_Z + 1.0)])
    p.extrude(web.add(), TSTAT_WEB_T, mode="add", name="Thermostat web")

    # The chamber the springs, rockers and camshaft live in.
    ch = p.sketch(Plane.xy(CHAMBER_FLOOR), name="Rocker chamber")
    ch.rect((-28.0, BLOCK_Y0 + 8.0), (CHAMBER_X, BLOCK_Y1 - 8.0))
    p.extrude(ch.add(), HEAD_H, mode="cut", name="Rocker chamber")
    for i, y in enumerate(CAM_BEARING_Y):
        ped = p.sketch(Plane.xy(CHAMBER_FLOOR), name=f"Cam saddle {i + 1}")
        ped.rect((-CAM_PEDESTAL_X, y - CAM_BEARING_W / 2.0),
                 (CAM_PEDESTAL_X, y + CAM_BEARING_W / 2.0))
        p.extrude(ped.add(), CAM_Z - CHAMBER_FLOOR, mode="add",
                  name=f"Cam saddle {i + 1}")
        for sx in (1,):
            # Out to the chamber wall, not past it: the posts stand above
            # the head's own top face, and 4 mm proud of the wall is 4 mm
            # into the cam cover. And in only as far as the head bolt under
            # it allows, until the bolt's head is behind us.
            for j, (x0, z0) in enumerate(
                    ((POST_ROOT_X, CHAMBER_FLOOR),
                     (ROCKER_PIVOT_X - 12.0, POST_CORBEL_Z))):
                r = p.sketch(Plane.xy(z0), name=f"Rocker post {i}{sx}{j}")
                r.rect((sx * x0, y - CAM_BEARING_W / 2.0),
                       (sx * CHAMBER_X, y + CAM_BEARING_W / 2.0))
                p.extrude(r.add(), PEDESTAL_TOP - z0, mode="add",
                          name=f"Rocker post {i}{sx}{j}")

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
    # Head bolt bosses stand through it all — they are at x = 58 and the
    # jacket's side channel is 48..70, so without them a head bolt is drilled
    # into water. There are NO guide bosses: this jacket is a ladder, and it
    # never reaches the guides at x = +-18, so eight of them added nothing
    # and said so ("add merged no new material") on every single run.
    for x, y in HEAD_BOLT:
        B.disc(p, (x, y), HEAD_BOLT_BOSS_D, FIRE_FACE, HEAD_BOLT_SEAT,
               "Head bolt boss")

    # Restore a dry wall around each insert wherever it traverses coolant.
    for k,cy in enumerate(CYL_Y):
        origin=V((INJ_FACE[0],cy+PRE_DY,INJ_FACE[2]))
        axis=injection_axis()
        for start,stop,diameter in [(22*math.hypot(axis.x,axis.y)/(-axis.z)+1,40,44)]:
            sk=p.sketch(B.axis_plane(tuple(origin+axis*start),tuple(axis)),
                        name="Prechamber dry sleeve")
            sk.circle((0,0),d=diameter)
            p.extrude(sk.add(),stop-start,mode="add",name="Prechamber dry sleeve")

    # The spring pockets: an ANNULUS, so the cut leaves each guide's own
    # boss standing in the middle of it.
    for k, cy in enumerate(CYL_Y):
        for kind, (vx, dy) in VALVE.items():
            sp = p.sketch(Plane.xy(SPRING_SEAT_Z), name=f"Spring pocket {k}")
            sp.circle((vx, cy + valve_dy(k,kind)), d=SPRING_POCKET_D)
            sp.circle((vx, cy + valve_dy(k,kind)), d=GUIDE_BOSS_D)
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
    for sx in (1,):
        rb = p.sketch(Plane.xz(BLOCK_Y1 + 1.0), name=f"Rocker bore {sx}")
        rb.circle((sx * ROCKER_PIVOT_X, ROCKER_Z), d=ROCKER_SHAFT_D + FIT)
        p.extrude(rb.add(), BLOCK_Y1 - BLOCK_Y0 + 2.0, mode="cut",
                  name=f"Rocker bore {sx}")

    for k, cy in enumerate(CYL_Y):
        for kind, (vx, dy) in VALVE.items():
            y = cy + valve_dy(k, kind)
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
            sx = PORT_SIDE
            B.bore(p, (sx * HEAD_X, port_y(k,kind), PORT_Z), (vx, y, GUIDE_Z - 4.0),
                   PORT_D[kind], f"{kind} port {k + 1}")
        axis=injection_axis()
        origin=V((INJ_FACE[0],cy+PRE_DY,INJ_FACE[2]))
        length=(INJ_FACE[2]-FIRE_FACE)/(-axis.z)
        for start,stop,diameter in [(-20,40.5,33),(39.5,70.5,25),(69.5,length+1,13)]:
            B.bore(p,tuple(origin+axis*start),tuple(origin+axis*stop),diameter,
                   f"Prechamber register {k+1}/{start}",over=0)
        for nm, face, target, d, hd in (
                ("glow plug", GLOW_FACE, GLOW_TARGET, GLOW_D, GLOW_HEAD_D),):
            start = V((face[0], cy + face[1], face[2]))
            end = V((target[0], cy + target[1], target[2]))
            axis = (end - start).unit()
            # Both bores start OUTSIDE the face, not on it. The plug's shank
            # stands 3 mm proud and its shoulder is a disc square to its own
            # INCLINED axis, while the face is a flat wall — so a hole that
            # begins exactly on the face leaves a sliver of each plug inside
            # the head, one for every plug in the engine.
            mouth = start - axis * PLUG_MOUTH_BACK
            B.bore(p, tuple(mouth), tuple(end), d, f"{nm} bore {k + 1}")
            # ...and a WELL round the mouth, for the shoulder to land in.
            B.bore(p, tuple(mouth),
                   tuple(mouth + axis * (PLUG_MOUTH_BACK
                                         + PLUG_WELL_DEPTH)),
                   hd + FIT, f"{nm} well {k + 1}", over=0.0)

    for y in port_centers("intake"):
        B.bore(p,(HEAD_X+1,y,PORT_Z),(HEAD_X-12,y,PORT_Z),44,
               "Paired intake mouth",over=0)

    # Studs belong to the six external ports, not to eight valve seats.
    for kind in VALVE:
        for y in port_centers(kind):
            for sign in (-1,1):
                z=PORT_Z+sign*MANIFOLD_BOLT_DZ
                B.bore(p,(HEAD_X,y,z),(HEAD_X-MANIFOLD_STUD_DEPTH,y,z),
                       MANIFOLD_BOLT_D-1.5,f"{kind} stud {y}/{sign}",over=0)

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
    for x, z in COVER_BOLTS_HEAD:
        fr.circle((x, z), d=7.5)
    p.extrude(fr.add(), 22.0, mode="cut", name="Cover bolt holes")
    # Recess the head screw heads below the cover gasket datum.
    recess = p.sketch(Plane.xy(HEAD_TOP + 1), name="Head screw counterbores")
    for x, y in HEAD_BOLT:
        recess.circle((x, y), d=HEAD_BOLT_BOSS_D + 1)
    p.extrude(recess.add(), HEAD_BOLT_SEAT - HEAD_TOP - 1,
              mode="cut", name="Head screw counterbores")
    # Integral bosses provide a closed blind thread below each cover seat.
    for y in _cover_bolt_y():
        for sx in (-1, 1):
            _cover_mount_pad(p, sx, y, COVER_BASE - 14.0, COVER_BASE, "Cover thread boss")
    threads = p.sketch(Plane.xy(COVER_BASE + 1.0), name="Cover threads")
    for y in _cover_bolt_y():
        for sx in (-1, 1):
            threads.circle((cover_bolt_x(sx), y), d=6.0 + TAP_FIT)
    p.extrude(threads.add(), -12.0, mode="cut", name="Cover threads")
    # The thermostat housing's two tapped holes, blind in its ear.
    tt = p.sketch(Plane.xy(HEAD_TOP + 1.0), name="Thermostat threads")
    for s in (-1, 1):
        tt.circle((TSTAT_X, TSTAT_Y + s * TSTAT_BOLT_DY),
                  d=TSTAT_BOLT_D + TAP_FIT)
    p.extrude(tt.add(), -TSTAT_TAP_DEPTH, mode="cut",
              name="Thermostat threads")

    B.casting_finish(p, "steel", (0.36, 0.38, 0.39))
    p.frame("deck", origin=(0, 0, FIRE_FACE), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("top", origin=(0, 0, COVER_BASE), z=(0, 0, 1), x=(1, 0, 0))
    # The cover sits a gasket ABOVE the head's face, not on it — and the
    # gasket sits on the face, not under it. Mated to the same frame, the
    # gasket was 3 mm below the head's top face, which is inside the head,
    # the camshaft and every rocker boss.
    p.frame("cover_rail", origin=(0, 0, COVER_BASE + CAM_GASKET_T), z=(0, 0, 1),
            x=(1, 0, 0))
    p.frame("front_face", origin=(0, BLOCK_Y0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("thermostat", origin=(TSTAT_X, TSTAT_Y, HEAD_TOP), z=(0, 0, 1),
            x=(1, 0, 0))
    p.frame("cam_axis", origin=(0, 0, CAM_Z), z=(0, 1, 0), x=(1, 0, 0))
    for sx, nm in ((PORT_SIDE, "intake_face"), (PORT_SIDE, "exhaust_face")):
        p.frame(nm, origin=(sx * HEAD_X, 0, PORT_Z), z=(0, 0, 1),
                x=(1, 0, 0))
    p.frame("outlet", origin=(-HEAD_X, BLOCK_Y0 + OUTLET_Y,
                              sum(JACKET_Z) / 2.0), z=(-1, 0, 0),
            x=(0, 1, 0))
    for i, y in enumerate(CAM_BEARING_Y):
        p.frame(f"cam_cap_{i}", origin=(0, y, CAM_Z), z=(0, 0, 1),
                x=(1, 0, 0))
    for sx, nm in ((1, "rocker_shaft_intake"), (1, "rocker_shaft_exhaust")):
        p.frame(nm, origin=(sx * ROCKER_PIVOT_X, 0, ROCKER_Z), z=(0, 1, 0),
                x=(1, 0, 0))
    for k, cy in enumerate(CYL_Y):
        for kind, (vx, dy) in VALVE.items():
            y = cy + valve_dy(k, kind)
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
            sx = 1
            p.frame(f"rocker_{k}_{kind}",
                    origin=(sx * ROCKER_PIVOT_X, y, ROCKER_Z),
                    z=(0, -sx, 0), x=(-sx, 0, 0))
        p.frame(f"pre_{k}", origin=(INJ_FACE[0], cy + PRE_DY, INJ_FACE[2]),
                z=tuple(injection_axis()), x=(0, 1, 0))
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
                       (STEM_D / 2.0, 11.0),
                       (STEM_D / 2.0, VALVE_LEN - STEM_TIP_CHAMFER),
                       (STEM_D / 2.0 - STEM_TIP_CHAMFER, VALVE_LEN),
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
    # The helix is the WIRE'S CENTRELINE, so it starts half a wire above the
    # seat and ends half a wire below the top — laid on z = 0 the bottom
    # coil's underside was 2 mm inside the head's own spring pocket, and
    # `free` is the height of the spring, not of its path.
    pitch = (free - SPRING_WIRE) / turns
    if pitch <= SPRING_WIRE:
        raise SdkError(f"the valve spring is {free:.1f} long over {turns} "
                       f"turns: its coils are closer than the {SPRING_WIRE} "
                       f"mm wire is thick")
    p.app.add_feature(feature={
        "type": "Curve3D", "name": "Spring path", "kind": "helix",
        "origin": [0, 0, SPRING_WIRE / 2.0], "axis": [0, 0, 1],
        "radius": SPRING_D / 2.0, "pitch": pitch, "turns": turns})
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
    """A dished washer whose BORE is the collet's cone, which is what locks
    the pair onto the stem. Its top is flush with the collet's, because a
    retainer taller than the cone has nothing to wedge against."""
    p = app.part("spring retainer")
    h = COLLET_H + COLLET_DROP
    p.revolve_profile([(collet_r(0.0) + COLLET_FIT, 0),
                       (RETAINER_D / 2.0, 0), (RETAINER_D / 2.0, 3.0),
                       (collet_r(h) + COLLET_FIT + 4.0, h),
                       (collet_r(h) + COLLET_FIT, h)],
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
    p.revolve_profile([(root, 0), (COLLET_R0, 0),
                       (COLLET_R1, COLLET_H), (STEM_D / 2.0 + 0.2, COLLET_H),
                       (STEM_D / 2.0 + 0.2, 3.5), (root, 3.5)],
                      plane=Plane.xz(), axis="z", angle_deg=170.0, name="Collet")
    p.material("steel")
    p.frame("seat", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


# ---------------------------------------------------- the pre-chamber and co
def injection_axis():
    return (V(INJ_TARGET)-V(INJ_FACE)).unit()


def prechamber(app):
    """Inclined insert: injector socket, swirl cavity and a narrow firing tip.

    Architecture: workshop 07.1-230/1. Diameters and intermediate shoulders
    remain modeled envelopes; the end follows the fire face geometrically.
    """
    p=app.part("pre-chamber")
    axis=injection_axis()
    length=(INJ_FACE[2]-FIRE_FACE)/(-axis.z)
    p.revolve_profile([(0,0),(16,0),(16,40),(12,40),(12,70),
                       (6,70),(6,length),(0,length)],
                      plane=Plane.xz(),axis="z",name="Stepped prechamber")
    B.bore(p,(0,0,-1),(0,0,INJ_REACH+2),INJ_D,"Injector socket",over=0)
    B.bore(p,(0,0,INJ_REACH),(0,0,68),16,"Swirl chamber",over=0)
    B.bore(p,(0,0,65),(0,0,length+1),4,"Firing throat",over=0)
    # Inverse of the head frame: local x is longitudinal, z follows the bore.
    ex=V((0,1,0)); ey=axis.cross(ex)
    def local(point):
        d=V(point)-V(INJ_FACE)
        return (d.dot(ex),d.dot(ey),d.dot(axis))
    B.bore(p,local(GLOW_FACE),local(GLOW_TARGET),GLOW_D+FIT,"Glow plug bore")
    p.material("stainless_steel")
    p.frame("seat",origin=(0,0,0),z=(0,0,1),x=(1,0,0))
    p.frame("nozzle_seat",origin=(0,0,INJ_REACH),z=(0,0,1),x=(1,0,0))
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
    reach = (V(GLOW_TARGET) - V(GLOW_FACE)).length() + PLUG_REACH_PAST
    return _plug(app, "glow plug", GLOW_D - FIT, reach, 22.0, GLOW_HEAD_D)


def injector(app):
    """Turned nozzle holder with two wrench hexes, inlet and leak-off nipples.

    Envelope proportions follow the supplied injector photograph. Threads are
    represented by their major-diameter lands, not cosmetic helices.
    Local +Z enters the prechamber; negative Z is outside the mounting seat.
    """
    p = app.part("injector")
    p.var("body_diameter", INJ_BODY_D, "Photo-derived envelope, not a factory dimension")
    p.revolve_profile([(0,-INJ_HEIGHT),(6,-INJ_HEIGHT),(6,-58),(10,-58),
                       (10,-51),(13,-51),(13,-34),(15,-29),(15,-15),
                       (13,-7),(11.5,-7),(11.5,8),(9,8),(9,INJ_REACH),
                       (0,INJ_REACH)], plane=Plane.xz(), axis="z",
                      name="Turned nozzle holder")
    for z,h,af in [(-35,9,INJ_HEX_AF),(-56,7,22.0)]:
        sk=p.sketch(Plane.xy(z),name="Wrench flats")
        radius=af/math.sqrt(3)
        sk.polyline([(radius*math.cos(math.radians(30+i*60)),
                      radius*math.sin(math.radians(30+i*60))) for i in range(6)])
        p.extrude(sk.add(),h,mode="add",name="Wrench hex")
    for side in (-1,1):
        root=V((side*9,0,-47)); direction=V((side*0.2,0,-0.98)).unit()
        sk=p.sketch(B.axis_plane(tuple(root),tuple(direction)),name="Leak-off spigot")
        sk.circle((0,0),d=4.5)
        p.extrude(sk.add(),17,mode="add",name="Leak-off spigot")
        B.bore(p,tuple(root+direction*18),tuple(root),2,"Leak-off bore",over=0)
    B.bore(p,(0,0,-INJ_HEIGHT-1),(0,0,-INJ_HEIGHT+10),3,"Fuel inlet",over=0)
    B.casting_finish(p,"steel",(0.40,0.41,0.42))
    p.frame("face",origin=(0,0,0),z=(0,0,1),x=(1,0,0))
    p.frame("fuel_inlet",origin=(0,0,-INJ_HEIGHT),z=(0,0,-1),x=(1,0,0))
    p.check()
    return p


def injector_heat_shield(app):
    p=app.part("injector heat shield")
    p.revolve_profile([(2.5,0),(11.8,0),(11.8,1.5),(2.5,1.5)],
                      plane=Plane.xz(),axis="z",name="Nozzle sealing plate")
    p.material("steel")
    p.frame("seat",origin=(0,0,0),z=(0,0,1),x=(1,0,0))
    p.check()
    return p


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
            y = cy + valve_dy(k, kind)
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
    """A bearing cap on a raised camshaft support."""
    p = app.part("cam bearing cap")
    sk = p.sketch(Plane.xz(CAM_BEARING_W / 2.0), name="Cap")
    sk.rect((-CAM_PEDESTAL_X, 0.0), (CAM_PEDESTAL_X, CAM_CAP_H))
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
    """As long as the ROCKER CHAMBER allows, not the head: the chamber is
    not centred (the head runs further back than forward), and a shaft cut
    to the head's length and then centred came 1 mm out through the front of
    the cam cover's gasket."""
    half = min(BLOCK_Y1, -BLOCK_Y0) - 8.0 - 2.0
    p = B.ring(app, "rocker shaft", ROCKER_SHAFT_D, 8.0, 2 * half,
               material="steel")
    return p


def _crown(left, top):
    """The cover's roof across one section: two shoulder arcs and, between
    them, a shallow CROWN arc tangent to both — a cast lid is domed, and a
    flat roof between two quarter-rounds is what made it read as a box.

    Returns (shoulder centres cl, cr, their height cz, shoulder radius, the
    crown's centre x and z, its radius, and the angle where they meet)."""
    right = HEAD_X + COVER_LIP
    shoulder = 80 if left > 80 else 32
    cr, cl, cz = right - shoulder, -left + shoulder, top - shoulder
    h, xm = (cr - cl) / 2.0, (cr + cl) / 2.0
    # Internally tangent: the crown's centre is (crown radius - shoulder)
    # from each shoulder's, and its top is CAM_COVER_CROWN above theirs.
    rc = shoulder + (h * h + CAM_COVER_CROWN ** 2) / (2.0 * CAM_COVER_CROWN)
    zc = cz + shoulder + CAM_COVER_CROWN - rc
    theta = math.degrees(math.atan2(cz - zc, cr - xm))
    return cl, cr, cz, shoulder, xm, zc, rc, theta


def _cover_section(sk, left, top, inset=0):
    """Rounded shoulders under a crowned roof, with an open-bottom inner
    profile. Inset, every radius shrinks by the same amount about the same
    centres, so the wall is one thickness all round."""
    right = HEAD_X + COVER_LIP
    floor = COVER_BASE - 1 if inset else COVER_BASE
    cl, cr, cz, shoulder, xm, zc, rc, theta = _crown(left, top)
    r = shoulder - inset
    sk.line((-left + inset, floor), (right - inset, floor))
    sk.line((right - inset, floor), (right - inset, cz))
    sk.arc((cr, cz), r, 0, theta)
    sk.arc((xm, zc), rc - inset, theta, 180 - theta)
    sk.arc((cl, cz), r, 180 - theta, 180)
    sk.line((-left + inset, cz), (-left + inset, floor))


def cover_top_z(x, left=COVER_LEFT, top=CAM_COVER_TOP):
    """The rocker section's outside height at x, in the cover's own frame."""
    cl, cr, cz, shoulder, xm, zc, rc, theta = _crown(left, top)
    return zc + math.sqrt(rc * rc - (x - xm) ** 2)


def _section_clearance(px, pz, left=COVER_LEFT, top=CAM_COVER_TOP):
    """How far a point inside the rocker section is from the CAVITY's wall,
    both in the cover's own frame: from whichever arc of the roof its angle
    falls on, or from a side wall below the shoulders."""
    cl, cr, cz, shoulder, xm, zc, rc, theta = _crown(left, top)
    w = CAM_COVER_WALL
    out = [px - (-left + w), HEAD_X + COVER_LIP - w - px]
    for cx, lo, hi in ((cr, 0.0, theta), (cl, 180.0 - theta, 180.0)):
        a = math.degrees(math.atan2(pz - cz, px - cx))
        if lo <= a <= hi:
            out.append(shoulder - w - math.hypot(px - cx, pz - cz))
    a = math.degrees(math.atan2(pz - zc, px - xm))
    if theta <= a <= 180.0 - theta:
        out.append(rc - w - math.hypot(px - xm, pz - zc))
    return min(out)


def cover_end_radius():
    """How round the cover's REAR end can be: the largest radius its cavity
    can take and still clear what stands in its corners. The rounding runs
    round the whole end — the side walls in plan AND the shoulders and crown
    in elevation — so both are asked: the last rocker post (out to the
    chamber wall) and the last cam cap, each against the side wall below
    them and against the roof over them. A radius taken from the plan alone
    put 1.2 mm3 of the cover into the rear cam cap, under its shoulder."""
    def most(a, b, margin=1.0):
        # A corner of radius R, a point a and b in from its two walls: the
        # point is inside the rounded corner while R is below this root.
        s = a + b - margin
        return s + math.sqrt(s * s - (a * a + b * b - margin * margin))
    back = BLOCK_Y1 - CAM_COVER_WALL - (CAM_BEARING_Y[-1] + CAM_BEARING_W / 2)
    # The cover sits a gasket above the head; its frame is the gasket's
    # underside, so a head-side height is lower by the gasket in it.
    corners = [(CHAMBER_X, PEDESTAL_TOP - CAM_GASKET_T),
               (-CAM_PEDESTAL_X, CAM_Z + CAM_CAP_H - CAM_GASKET_T),
               (CAM_PEDESTAL_X, CAM_Z + CAM_CAP_H - CAM_GASKET_T)]
    return math.floor(min(most(_section_clearance(x, z), back)
                          for x, z in corners))


def _end_edges(p, owner, y, above):
    """The edges round one END face of a body — the profile it was extruded
    from, less its floor line."""
    body = next(b for b in p.bodies(reference=False) if b.created_by == owner)
    edges = body.edges(where=lambda e: e.midpoint is not None
                       and abs(e.midpoint.y - y) < 0.01
                       and e.midpoint.z > above)
    if not edges:
        raise SdkError(f"{p.name}: no edges round the end face at y = {y}")
    return edges


def _offset_polygon(points, d):
    """A counter-clockwise polygon grown outward by d, corner for corner."""
    lines = []
    for i, a in enumerate(points):
        b = points[(i + 1) % len(points)]
        dx, dy = b[0] - a[0], b[1] - a[1]
        n = math.hypot(dx, dy)
        ox, oy = dy / n * d, -dx / n * d
        lines.append(((a[0] + ox, a[1] + oy), (dx, dy)))
    out = []
    for i in range(len(lines)):
        (p0, d0), (p1, d1) = lines[i - 1], lines[i]
        den = d0[0] * d1[1] - d0[1] * d1[0]
        t = ((p1[0] - p0[0]) * d1[1] - (p1[1] - p0[1]) * d1[0]) / den
        out.append((p0[0] + d0[0] * t, p0[1] + d0[1] * t))
    return out


def _cover_plan():
    """The cover's footprint on the head and the chain case: the gasket's
    outline, counter-clockwise from the front left."""
    right = HEAD_X + COVER_LIP
    return [(-right, COVER_Y0), (right, COVER_Y0), (right, BLOCK_Y1),
            (-COVER_LEFT, BLOCK_Y1), (-COVER_LEFT, COVER_TRANSITION_Y),
            (-right, COVER_DOME_END_Y)]


def _plan_radii(extra):
    """Corner radii for `_cover_plan`, grown by `extra` — so a rim, a gasket
    and the cover's own rounded ends all turn about the same centres."""
    rear = cover_end_radius() + CAM_COVER_WALL
    return [CAM_COVER_FRONT_R + extra, CAM_COVER_FRONT_R + extra,
            rear + extra, rear + extra, 0, 0]


def _cover_shell(p, inset, name):
    """The rocker enclosure, the loft into the chain dome, and the dome —
    one body, drawn at `inset` from the outside."""
    rear = BLOCK_Y1 - inset
    front = COVER_Y0 + inset
    sk = p.sketch(Plane.xz(rear), name=name + " rocker section")
    _cover_section(sk, COVER_LEFT, CAM_COVER_TOP, inset)
    body = p.extrude(sk.add(), rear - COVER_TRANSITION_Y, mode="new_body",
                     name=name + " rocker enclosure")
    profiles = []
    for y, left, top in [(COVER_TRANSITION_Y, COVER_LEFT, CAM_COVER_TOP),
                         (COVER_DOME_END_Y, HEAD_X + COVER_LIP, COVER_TOP)]:
        sk = p.sketch(Plane.xz(y), name=name + " transition")
        _cover_section(sk, left, top, inset)
        profiles.append(sk.add().id)
    p._add({"type": "Loft", "name": name + " chain transition",
            "sketch_ids": profiles, "mode": "add"})
    sk = p.sketch(Plane.xz(COVER_DOME_END_Y), name=name + " chain dome")
    _cover_section(sk, HEAD_X + COVER_LIP, COVER_TOP, inset)
    p.extrude(sk.add(), COVER_DOME_END_Y - front, mode="add",
              name=name + " chain enclosure")
    return body.created_by


def cam_cover(app):
    """One continuous cover over valve gear AND the upper timing chain.

    The head's 85 mm machined height and the cam axis are distinct datums.
    A broad front dome tapers into the narrower rocker enclosure. Outside,
    it is the cast lid of the photographs: a crowned roof, a rear end
    rounded in plan and in elevation, a rim at the gasket, the star, the
    filler near the front and a breather stub on the manifold side.

    Built as two bodies — the outside, and the cavity drawn at the wall's
    inset and rounded to match — and the one subtracted from the other, so
    a rounded outside keeps its wall instead of a square cavity corner
    breaking through it.
    """
    p = app.part("cam cover")
    base = COVER_BASE + 0.5
    outer = _cover_shell(p, 0, "Outer")
    p.fillet(_end_edges(p, outer, BLOCK_Y1, base),
             cover_end_radius() + CAM_COVER_WALL, name="Rounded rear end")
    p.fillet(_end_edges(p, outer, COVER_Y0, base), CAM_COVER_FRONT_R,
             name="Rounded dome front")
    proud, tall = CAM_COVER_RIM
    rim = p.sketch(Plane.xy(COVER_BASE), name="Rim")
    B.rounded_polygon(rim, _offset_polygon(_cover_plan(), proud),
                      _plan_radii(proud))
    p.extrude(rim.add(), tall, mode="add", name="Rim")
    inner = _cover_shell(p, CAM_COVER_WALL, "Cavity")
    p.fillet(_end_edges(p, inner, BLOCK_Y1 - CAM_COVER_WALL, base),
             cover_end_radius(), name="Cavity rear end")
    p._add({"type": "Boolean", "name": "Hollow", "target_id": outer,
            "tool_id": inner, "op": "subtract"})

    for y in _cover_bolt_y():
        for side in (-1, 1):
            _cover_mount_pad(p, side, y, COVER_BASE,
                             COVER_BASE + CAM_COVER_BOSS_H, "Cover bolt boss")
    holes = p.sketch(Plane.xy(COVER_BASE - 1), name="Bolt holes")
    for y in _cover_bolt_y():
        for side in (-1, 1):
            holes.circle((cover_bolt_x(side), y), d=6.6)
    p.extrude(holes.add(), CAM_COVER_BOSS_H + 2, mode="cut", name="Bolt holes")

    # The filler neck: a boss standing through the crown, bored into the
    # cavity. Its floor is inside the cover, above the lobes' reach.
    xm = _crown(COVER_LEFT, CAM_COVER_TOP)[4]
    seat = cover_top_z(xm) + CAM_COVER_WALL / 2.0
    neck_floor = CAM_COVER_TOP - CAM_COVER_WALL + 1.0
    B.disc(p, (xm, FILLER_Y), FILLER_BOSS_D, neck_floor, seat, "Filler neck")
    B.disc(p, (xm, FILLER_Y), FILLER_BOSS_D - 12.0, neck_floor - 1.0,
           seat + 1.0, "Filler bore", mode="cut")

    # The three-pointed star, raised on a medallion on the crown.
    star_y = CYL_Y[1] + BORE_PITCH / 2.0
    disc_top = cover_top_z(xm) + STAR_RELIEF / 2.0
    low = cover_top_z(xm + STAR_D / 2.0) - 1.0
    B.disc(p, (xm, star_y), STAR_D, low, disc_top, "Star medallion")
    ring = p.sketch(Plane.xy(disc_top - 0.5), name="Star ring")
    ring.circle((xm, star_y), d=STAR_D - 4.0)
    ring.circle((xm, star_y), d=STAR_D - 10.0)
    p.extrude(ring.add(), STAR_RELIEF + 0.5, mode="add", name="Star ring")
    arm = (STAR_D - 10.0) / 2.0
    pts = []
    for i in range(6):
        # Tips at 180 (toward the injection side, as on the engine), 300, 60.
        a = math.radians(180.0 + 60.0 * i)
        r = arm + 0.5 if i % 2 == 0 else arm * 0.16
        pts.append((xm + r * math.cos(a), star_y + r * math.sin(a)))
    star = p.sketch(Plane.xy(disc_top - 0.5), name="Star")
    star.polyline(pts)
    p.extrude(star.add(), STAR_RELIEF + 0.5, mode="add", name="Star")

    # The breather: a stub out of the manifold-side shoulder, bored through.
    cl, cr, cz, shoulder = _crown(COVER_LEFT, CAM_COVER_TOP)[:4]
    axis = V((1.0, 0.0, 1.0)).unit()
    centre = V((cr, BREATHER_Y, cz))
    B.axis_solid(p, tuple(centre + axis * (shoulder - CAM_COVER_WALL + 1.0)),
                 tuple(centre + axis * (shoulder + 22.0)), BREATHER[0],
                 "Breather stub")
    B.bore(p, tuple(centre + axis * (shoulder + 23.0)),
           tuple(centre + axis * (shoulder - CAM_COVER_WALL - 4.0)),
           BREATHER[1], "Breather bore", over=0.0)

    B.casting_finish(p)
    p.frame("rail", origin=(0, 0, COVER_BASE), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("filler", origin=(xm, FILLER_Y, seat), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("breather", origin=tuple(centre + axis * (shoulder + 22.0)),
            z=tuple(axis), x=(0, 1, 0))
    p.check()
    return p


def _cover_mount_pad(p, side, y, bottom, top, name):
    # Low outboard ears leave vertical socket access beside the cover wall.
    x = cover_bolt_x(side)
    inner = -COVER_LEFT + 6 if side < 0 else HEAD_X - 6
    lo, hi = min(x-8, inner), max(x+8, inner)
    sk = p.sketch(Plane.xy(bottom), name=name)
    B.rounded_polygon(sk, [(lo,y-8),(hi,y-8),(hi,y+8),(lo,y+8)], 6)
    p.extrude(sk.add(), top-bottom, mode="add", name=name)


def _cover_bolt_y():
    return [-121.25,24.25,121.25]


def cam_cover_gasket(app):
    """The cover's own footprint, rounded where the cover is, so no corner of
    it stands out past the rim."""
    p=app.part("cam cover gasket")
    sk=p.sketch(Plane.xy(COVER_BASE),name="Continuous head and chain seal")
    B.rounded_polygon(sk, _cover_plan(), _plan_radii(0.0))
    B.rounded_polygon(sk, _offset_polygon(_cover_plan(), -CAM_COVER_WALL),
                      [max(r - CAM_COVER_WALL, 0.0) for r in _plan_radii(0.0)])
    p.extrude(sk.add(),CAM_GASKET_T,mode="new_body",name="Continuous seal")
    for y in _cover_bolt_y():
        for side in (-1,1):
            _cover_mount_pad(p, side, y, COVER_BASE, COVER_BASE+CAM_GASKET_T, "Gasket bolt pad")
    holes=p.sketch(Plane.xy(COVER_BASE-1),name="Cover bolt clearance")
    for y in _cover_bolt_y():
        for side in (-1,1): holes.circle((cover_bolt_x(side),y),d=6.6)
    p.extrude(holes.add(),CAM_GASKET_T+2,mode="cut",name="Cover bolt clearance")
    p.material("rubber")
    p.frame("rail",origin=(0,0,COVER_BASE),z=(0,0,1),x=(1,0,0))
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


def thermostat_housing(app):
    """The thermostat housing: a flange on the head's front ear, the round
    chamber the thermostat sits in, its cover, and the upper-hose outlet
    pointing forward and up toward the radiator.

    Static and external: the model drills no feed from the jacket to it —
    the ear stands off the head's side wall above the ports, and the rocker
    chamber is on the other side of that wall."""
    p = app.part("thermostat housing")
    x, y, z0 = TSTAT_X, TSTAT_Y, HEAD_TOP
    top = z0 + TSTAT_FLANGE_T
    fl = p.sketch(Plane.xy(z0), name="Flange")
    hx = TSTAT_BODY_D / 2.0          # as wide as the ear it sits on
    hy = TSTAT_BOLT_DY + TSTAT_BOLT_D + 2.0
    B.rounded_polygon(fl, [(x - hx, y - hy), (x + hx, y - hy),
                           (x + hx, y + hy), (x - hx, y + hy)], hx - 1.0)
    p.extrude(fl.add(), TSTAT_FLANGE_T, mode="new_body", name="Flange")
    chamber = TSTAT_BODY_D
    height = chamber
    B.disc(p, (x, y), chamber, top - 1.0, top + height, "Chamber")
    B.disc(p, (x, y), chamber + 8.0, top + height, top + height + 8.0,
           "Thermostat cover")
    # The outlet leaves the cover forward and up.
    axis = V((0.0, -0.6, 0.8)).unit()
    root = V((x, y, top + height + 4.0))
    reach = chamber / 2.0 + 24.0
    B.axis_solid(p, tuple(root), tuple(root + axis * reach), TSTAT_OUTLET[0],
                 "Hose outlet")
    B.axis_solid(p, tuple(root + axis * (reach - 8.0)),
                 tuple(root + axis * (reach - 4.0)), TSTAT_OUTLET[0] + 4.0,
                 "Hose bead")
    B.bore(p, tuple(root + axis * (reach + 1.0)), tuple(root), TSTAT_OUTLET[1],
           "Outlet bore", over=0.0)
    B.disc(p, (x, y), chamber - 10.0, z0 + 4.0, top + height + 4.0,
           "Chamber bore", mode="cut")
    holes = p.sketch(Plane.xy(z0 - 1.0), name="Bolt holes")
    for s in (-1, 1):
        # ISO 273 medium clearance for M8.
        holes.circle((x, y + s * TSTAT_BOLT_DY), d=TSTAT_BOLT_D + 1.0)
    p.extrude(holes.add(), TSTAT_FLANGE_T + 2.0, mode="cut", name="Bolt holes")
    B.casting_finish(p)
    p.frame("seat", origin=(x, y, z0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("hose", origin=tuple(root + axis * reach), z=tuple(axis),
            x=(1, 0, 0))
    p.check()
    return p


# ------------------------------------------------------------ the manifolds
# Both are built SOLID and bored afterwards, for the reason a real manifold
# is cast that way: two hollow tubes unioned leave each one's wall across the
# other's throat. And every step is WEIGHED — an add that loses material, or
# a cut that removes more than its own tool, is a boolean the kernel could
# not do, and what it leaves touches nothing, so no clash check says a word.
def _grows(p, what, step):
    before = p.volume()
    step()
    if p.volume() < before - 0.01:
        raise SdkError(f"{p.name}: {what} lost solid volume "
                       f"({before:.1f} -> {p.volume():.1f} mm3)")


def _removes_at_most(p, what, room, step):
    before = p.volume()
    step()
    gone = before - p.volume()
    if gone > room * 1.001:
        raise SdkError(f"{p.name}: {what} removed {gone:.0f} mm3, and the "
                       f"tool itself is at most {room:.0f}")
    return gone


def _port_flanges(p, kind):
    """A machined flange per external port, on the head's gasket face."""
    face = PORT_SIDE * (HEAD_X + MANIFOLD_GASKET_T)
    for k, y in enumerate(port_centers(kind)):
        def flange(k=k, y=y):
            sk = p.sketch(B.axis_plane((face, y, PORT_Z), (1, 0, 0)),
                          name=f"Flange {k + 1}")
            B.rounded_polygon(sk, [(-MANIFOLD_PAD_HY, -MANIFOLD_PAD_HZ),
                                   (MANIFOLD_PAD_HY, -MANIFOLD_PAD_HZ),
                                   (MANIFOLD_PAD_HY, MANIFOLD_PAD_HZ),
                                   (-MANIFOLD_PAD_HY, MANIFOLD_PAD_HZ)], 8)
            p.extrude(sk.add(), MANIFOLD_PAD, mode="add",
                      name=f"Flange {k + 1}")
        _grows(p, f"flange {k + 1}", flange)


def _stud_holes(p, kind):
    face = PORT_SIDE * (HEAD_X + MANIFOLD_GASKET_T)
    pad = face + PORT_SIDE * MANIFOLD_PAD
    drill = math.pi * (MANIFOLD_BOLT_D / 2) ** 2 * (MANIFOLD_PAD + 1)
    for k, y in enumerate(port_centers(kind)):
        for s2 in (-1, 1):
            z = PORT_Z + s2 * MANIFOLD_BOLT_DZ
            gone = _removes_at_most(
                p, f"stud {k + 1}/{s2}", drill,
                lambda: B.bore(p, (pad, y, z), (face, y, z), MANIFOLD_BOLT_D,
                               f"Stud hole {k + 1}{s2}", over=1.0))
            if gone < 1:
                raise SdkError(f"{p.name}: stud {k + 1}/{s2} drilled nothing")


def _bore_runner(p, path, bore_d, what):
    """A pipe swept round a bend tighter than its own radius crosses itself;
    OCC cuts with it and reports nothing, and one such bore once took 91% of
    a manifold with it. A cut cannot remove more than the tool it cuts with."""
    span = sum((V(b) - V(a)).length() for a, b in zip(path, path[1:]))
    room = math.pi * (bore_d / 2.0) ** 2 * span * 1.4
    try:
        _removes_at_most(p, what, room, lambda: _runner_pipe(
            p, path, bore_d, what, mode="cut"))
    except SdkError as e:
        raise SdkError(f"{e} — the swept pipe crosses itself. Give the "
                       f"runner more length to turn in (its bend has to be "
                       f"wider than its own {bore_d / 2:.0f} mm radius).")


def _stays_off_the_head(p):
    """Nothing on a manifold may cross the face it bolts to. Everything on it
    is placed relative to that plane, so one measurement covers every runner,
    pad and flange at once — and it is the one that caught the tilted end
    caps."""
    reach = p.bbox()[0][0]
    if reach < HEAD_X + MANIFOLD_GASKET_T - 1e-6:
        raise SdkError(
            f"{p.name} reaches {abs(reach):.2f} mm from the engine's centre "
            f"and the head's gasket face is at "
            f"{HEAD_X + MANIFOLD_GASKET_T:.2f} — "
            f"{HEAD_X + MANIFOLD_GASKET_T - reach:.2f} mm of it is inside "
            f"the cylinder head")


def _round_ends(p, owner, y0, y1, r, what):
    """Round a cast box's or a log's two end faces."""
    before = p.volume()
    body = next(b for b in p.bodies(reference=False) if b.created_by == owner)
    edges = body.edges(where=lambda e: e.midpoint is not None and min(
        abs(e.midpoint.y - y0), abs(e.midpoint.y - y1)) < 0.01)
    p.fillet(edges, r, name=what)
    if p.volume() >= before:
        raise SdkError(f"{p.name}: rounding {what} took nothing off")


def _root_fillets(p, near, r, what):
    """A cast fillet where the runners meet the collector: the junction
    curves are the only edges there that are neither lines nor circles."""
    body = p.bodies(reference=False)[0]
    edges = body.edges(where=lambda e: e.midpoint is not None
                       and e.kind not in ("line", "circle", "arc")
                       and near(e.midpoint))
    if not edges:
        raise SdkError(f"{p.name}: no runner roots found to round")
    _grows(p, what, lambda: p.fillet(edges, r, name=what))


def exhaust_manifold(app):
    """Cast iron: four short runners dropping from the ports into a log
    under them, the log's ends rounded where the end runners turn into it,
    and ONE outlet in the middle, straight down, on a three-stud flange.

    Four runners, four ports: the exhaust ports are the head's own, one per
    cylinder (the intakes are paired)."""
    p = app.part("exhaust manifold")
    lx, lz, od, bore_d = EXH_LOG
    r_od, r_bore = EXH_RUNNER
    y0, y1 = EXH_LOG_Y
    lg = p.sketch(Plane.xz(y1), name="Log")
    lg.circle((lx, lz), d=od)
    log = p.extrude(lg.add(), y1 - y0, mode="new_body", name="Log").created_by
    _round_ends(p, log, y0, y1, r_od / 2.0, "Log ends")
    for k, y in enumerate(port_centers("exhaust")):
        _grows(p, f"runner {k + 1}", lambda y=y, k=k: _runner_pipe(
            p, _runner_path("exhaust", y, EXH_LOG), r_od, f"Runner {k + 1}",
            mode="add"))
    _port_flanges(p, "exhaust")

    # The outlet: down from the log's middle, a flange with three ears.
    stud_d, stud_r, stud_l = EXH_OUTLET_STUD
    drop = lz - od / 2.0 - EXH_OUTLET_DROP
    down = (0, 0, -1)
    # Narrower than the log: a tee of two equal tubes meets in a saddle that
    # reads as a spike on the log's top.
    out_od, out_bore = od - 8.0, bore_d - 6.0
    _grows(p, "outlet", lambda: B.axis_solid(
        p, (lx, 0.0, lz), (lx, 0.0, drop), out_od, "Outlet"))
    ears = [math.radians(a) for a in (0.0, 120.0, 240.0)]

    def flange():
        # The plane's u is world y and its v world x, so an ear at angle 0
        # points straight OUT from the engine, away from the block.
        plane = B.axis_plane((lx, 0.0, drop), down)
        p.extrude_shapes(plane, [lambda s: s.circle((0, 0), d=out_od + 16.0)] + [
            (lambda s, a=a: s.circle((stud_r * math.sin(a),
                                      stud_r * math.cos(a)),
                                     d=2.75 * stud_d)) for a in ears],
            EXH_OUTLET_T, name="Outlet flange", mode="add")
    _grows(p, "outlet flange", flange)
    for i, a in enumerate(ears):
        c = (lx + stud_r * math.cos(a), stud_r * math.sin(a))
        _grows(p, f"outlet stud {i + 1}", lambda c=c, i=i: B.axis_solid(
            p, (c[0], c[1], drop - EXH_OUTLET_T + 1.0),
            (c[0], c[1], drop - EXH_OUTLET_T - stud_l), stud_d,
            f"Outlet stud {i + 1}"))

    # Rounded LAST of the additions: a fuse onto a filleted body is where the
    # kernel lost a whole log to the first flange.
    _root_fillets(p, lambda m: m.z > lz - 5.0 and abs(
        math.hypot(m.x - lx, m.z - lz) - od / 2.0) < 3.0,
        (r_od - r_bore) / 2.0 + 1.0, "Runner roots")
    for k, y in enumerate(port_centers("exhaust")):
        _bore_runner(p, _runner_path("exhaust", y, EXH_LOG), r_bore,
                     f"Runner bore {k + 1}")
    # The collector runs between the end runners and no further: a log open
    # at both ends is a pipe.
    ys = port_centers("exhaust")
    hollow = p.sketch(Plane.xz(ys[-1]), name="Collector bore")
    hollow.circle((lx, lz), d=bore_d)
    _removes_at_most(p, "the collector", math.pi * (bore_d / 2) ** 2
                     * (ys[-1] - ys[0]), lambda: p.extrude(
                         hollow.add(), ys[-1] - ys[0], mode="cut",
                         name="Collector bore"))
    B.bore(p, (lx, 0.0, drop - EXH_OUTLET_T - 1.0), (lx, 0.0, lz), out_bore,
           "Outlet bore", over=0)
    _stud_holes(p, "exhaust")
    B.casting_finish(p, "steel")
    p.frame("face", origin=(PORT_SIDE * HEAD_X, 0, PORT_Z), z=(0, 0, 1),
            x=(1, 0, 0))
    p.frame("outlet", origin=(lx, 0.0, drop - EXH_OUTLET_T), z=down,
            x=(1, 0, 0))
    p.check()
    _stays_off_the_head(p)
    return p


def _plenum_section(sk, lx, lz, inset=0.0):
    w, h, r = PLENUM
    B.rounded_polygon(sk, [(lx - w / 2 + inset, lz - h / 2 + inset),
                           (lx + w / 2 - inset, lz - h / 2 + inset),
                           (lx + w / 2 - inset, lz + h / 2 - inset),
                           (lx - w / 2 + inset, lz + h / 2 - inset)],
                      r - inset)


def intake_manifold(app):
    """Cast aluminium: a long rounded box — the plenum — high on the
    manifold side, fed through a throttle body on a short neck in the middle
    of its outer face, and FOUR curved runners dropping from its underside
    to the head, two into each of the paired intake ports.

    The throttle body carries its butterfly shaft in two bosses, the shaft
    across the bore, and the lever on its rear end."""
    p = app.part("intake manifold")
    lx, lz = INT_LOG[:2]
    w, h, r = PLENUM
    wall = CAM_COVER_WALL
    y0, y1 = -PLENUM_HALF_L, PLENUM_HALF_L
    face = PORT_SIDE * (HEAD_X + MANIFOLD_GASKET_T)
    pad = face + PORT_SIDE * MANIFOLD_PAD
    b_od, b_bore = INT_BRANCH

    sk = p.sketch(Plane.xz(y1), name="Plenum")
    _plenum_section(sk, lx, lz)
    box = p.extrude(sk.add(), y1 - y0, mode="new_body",
                    name="Plenum").created_by
    _round_ends(p, box, y0, y1, r - 3.0, "Plenum ends")

    def branches():
        for y in port_centers("intake"):
            for dy in (-INT_BRANCH_DY, INT_BRANCH_DY):
                yield y + dy, _elbow((pad, y + dy, PORT_Z),
                                     (lx, y + dy, lz), b_od)
    for k, (y, path) in enumerate(branches()):
        _grows(p, f"runner {k + 1}", lambda path=path, k=k: _runner_pipe(
            p, path, b_od, f"Runner {k + 1}", mode="add"))
    _port_flanges(p, "intake")

    # The throttle body, on its neck, in the middle of the outer face.
    outer = lx + w / 2.0
    tb_x = outer + THROTTLE_NECK
    tb_end = tb_x + THROTTLE_L
    neck_hy, neck_hz = THROTTLE_OD / 2.0 + 2.0, h / 2.0 - 4.0

    def neck():
        sk = p.sketch(B.axis_plane((lx, 0.0, lz), (1, 0, 0)), name="Neck")
        B.rounded_polygon(sk, [(-neck_hy, -neck_hz), (neck_hy, -neck_hz),
                               (neck_hy, neck_hz), (-neck_hy, neck_hz)], 10)
        p.extrude(sk.add(), tb_x - lx + 2.0, mode="add", name="Neck")
    _grows(p, "the throttle neck", neck)
    _grows(p, "the throttle body", lambda: B.axis_solid(
        p, (tb_x, 0.0, lz), (tb_end, 0.0, lz), THROTTLE_OD, "Throttle body"))
    _grows(p, "the inlet lip", lambda: B.axis_solid(
        p, (tb_end - 4.0, 0.0, lz), (tb_end, 0.0, lz), THROTTLE_OD + 6.0,
        "Inlet lip"))
    shaft_x = (tb_x + tb_end) / 2.0
    boss_y = THROTTLE_OD / 2.0 + 8.0
    _grows(p, "the shaft bosses", lambda: B.axis_solid(
        p, (shaft_x, boss_y, lz), (shaft_x, -boss_y, lz), 16.0,
        "Shaft bosses"))

    _root_fillets(p, lambda m: abs(m.z - (lz - h / 2.0)) < 4.0,
                  (b_od - b_bore) / 2.0 + 1.0, "Runner roots")
    for k, (y, path) in enumerate(branches()):
        _bore_runner(p, path, b_bore, f"Runner bore {k + 1}")
    cap = wall
    hollow = p.sketch(Plane.xz(y1 - r), name="Plenum bore")
    _plenum_section(hollow, lx, lz, wall)
    _removes_at_most(p, "the plenum bore", (w - 2 * cap) * (h - 2 * cap)
                     * (y1 - y0 - 2 * r), lambda: p.extrude(
                         hollow.add(), y1 - y0 - 2 * r, mode="cut",
                         name="Plenum bore"))
    # Each paired port opens into BOTH of its branches: a slot through the
    # flange, the branches' bores at its two ends.
    for k, y in enumerate(port_centers("intake")):
        sk = p.sketch(B.axis_plane((pad + 1.0, y, PORT_Z), (-1, 0, 0)),
                      name=f"Port {k + 1}")
        sk.slot((-INT_BRANCH_DY, 0), (INT_BRANCH_DY, 0), b_bore)
        p.extrude(sk.add(), MANIFOLD_PAD + 2.0, mode="cut",
                  name=f"Port {k + 1}")
    B.bore(p, (tb_end + 1.0, 0.0, lz), (outer - wall - 2.0, 0.0, lz),
           THROTTLE_BORE, "Throttle bore", over=0)
    # ...and the butterfly shaft ACROSS the bore, which is what the bosses
    # are for, with the lever on its rear end.
    _grows(p, "the butterfly shaft", lambda: B.axis_solid(
        p, (shaft_x, boss_y + 6.0, lz), (shaft_x, -boss_y - 6.0, lz), 6.0,
        "Butterfly shaft"))
    lever_y = boss_y + 2.0

    def lever():
        sk = p.sketch(Plane.xz(lever_y + 4.0), name="Throttle lever")
        sk.slot((shaft_x, lz), (shaft_x + 16.0, lz + 20.0), 14.0)
        p.extrude(sk.add(), 4.0, mode="add", name="Throttle lever")
    _grows(p, "the throttle lever", lever)
    _grows(p, "the lever pin", lambda: B.axis_solid(
        p, (shaft_x + 16.0, lever_y + 2.0, lz + 20.0),
        (shaft_x + 16.0, lever_y + 12.0, lz + 20.0), 6.0, "Lever pin"))
    _stud_holes(p, "intake")
    B.casting_finish(p, "aluminum")
    p.frame("face", origin=(PORT_SIDE * HEAD_X, 0, PORT_Z), z=(0, 0, 1),
            x=(1, 0, 0))
    p.frame("throttle", origin=(tb_end, 0.0, lz), z=(1, 0, 0), x=(0, 1, 0))
    p.check()
    _stays_off_the_head(p)
    return p


def _elbow(start, end, tube_od):
    """A circular elbow, horizontal where it leaves `start` and turning in
    the XZ plane to `end`. The bend radius follows from the two points;
    unlike a fitted spline it has bounded curvature and cannot fold a tube
    back through itself."""
    (x0, y, z0), (x1, _, z1) = start, end
    dx, dz = abs(x1 - x0), z1 - z0
    radius = (dx * dx + dz * dz) / (2 * abs(dz))
    if radius <= tube_od / 2.0 + 2:
        raise SdkError("manifold bend radius must exceed the tube radius")
    angle = math.atan2(dx, radius - abs(dz))
    sign = 1 if dz > 0 else -1
    sx = 1 if x1 > x0 else -1
    mid = (x0 + sx * radius * math.sin(angle / 2), y,
           z0 + sign * radius * (1 - math.cos(angle / 2)))
    return [(x0, y, z0), mid, (x1, y, z1)]


def _runner_path(kind, y, log):
    """An exhaust runner: an elbow from the port, at the gasket face, to the
    log it drops into."""
    x0 = PORT_SIDE * (HEAD_X + MANIFOLD_GASKET_T)
    return _elbow((x0, y, PORT_Z), (log[0], y, log[1]),
                  (INT_RUNNER if kind == "intake" else EXH_RUNNER)[0])


def _runner_pipe(p, path, diameter, name, mode):
    sk = p.sketch(Plane.xz(path[0][1]), name=f"{name} arc")
    sk.arc_through(*[(q[0], q[2]) for q in path])
    path_id = sk.add().id
    p.app.add_feature(feature={"type": "Pipe", "name": name,
                               "path_id": path_id, "outer_diameter": diameter,
                               "mode": mode})


def manifold_gasket(app, kind):
    """Port gasket pads, using the same six-port layout as the head."""
    sx = PORT_SIDE
    p = app.part(f"{kind} manifold gasket")
    face = sx * HEAD_X
    t = MANIFOLD_GASKET_T
    first = ["new_body"]
    for k, y in enumerate(port_centers(kind)):
        sk = p.sketch(Plane.xz(y + MANIFOLD_PAD_HY), name=f"Gasket {k + 1}")
        sk.rect((min(face, face + sx * t), PORT_Z - MANIFOLD_PAD_HZ),
                (max(face, face + sx * t), PORT_Z + MANIFOLD_PAD_HZ))
        mode, first[0] = first[0], "add"
        p.extrude(sk.add(), 2 * MANIFOLD_PAD_HY, mode=mode,
                  name=f"Gasket {k + 1}")
        B.bore(p, (face + sx * 3.0, y, PORT_Z), (face - sx * 3.0, y, PORT_Z),
               (44.0 if kind=="intake" else PORT_D[kind]) + 2.0, f"Port {k + 1}", over=0.0)
        for s2 in (-1, 1):
            z = PORT_Z + s2 * MANIFOLD_BOLT_DZ
            B.bore(p, (face + sx * 3.0, y, z), (face - sx * 3.0, y, z),
                   MANIFOLD_BOLT_D + 1.0, f"Stud {k + 1}{s2}", over=0.0)
    p.material("copper")
    p.frame("face", origin=(face, 0, PORT_Z), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p
