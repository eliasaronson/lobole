#!/usr/bin/env python3
"""A four-cylinder OM615-shaped diesel: 1988 cc, five main bearings, a single
overhead camshaft on a duplex chain, rocker arms, and a pre-chamber per
cylinder — built complete, down to the bearing shells and the seals.

    python3 generators/om615.py --port <port> [--out DIR] [--stage bottom]

Three modules: `om615_spec` is every shared number, `om615_bottom` and
`om615_top` build the parts, and this one puts them together, drives them and
checks them. The mechanism is four slider-crank loops off one crankshaft, a
chain relation halving crank speed to the camshaft, and per valve a cam
relation for its lift and a rack relation that makes its rocker follow.

Findings go in docs/modeling/om615-dogfood.md.
"""
import argparse
import math
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from designer_sdk import SdkError, connect  # noqa: E402
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from om615_spec import *  # noqa: E402,F401,F403
import om615_bottom as B  # noqa: E402
import om615_top as T  # noqa: E402


def _solved(what, rig):
    """Solve and say the three numbers that matter, not the mate list."""
    r = rig.solve()
    print(f"  {what}: {len(r['mates'])} mates, dof {r['dof']}, "
          f"{r['loop_mates']} in loops, closed={r['loops_closed']}, "
          f"residual {r['residual_mm']:.2e} mm, "
          f"{r['errored_mates']} errored")
    if r["errored_mates"]:
        raise SdkError(f"{r['errored_mates']} mates could not be solved")
    return r


def bottom_end(app, rig):
    """Block, crank, four rods and pistons, and everything bolted or pressed
    to them. Returns the occurrences the top end and the study need."""
    blk = B.block(app)
    crk = B.crankshaft(app)
    rod, cap = B.con_rod(app), B.rod_cap(app)
    pis, pin = B.piston(app), B.gudgeon_pin(app)
    clip, bush = B.circlip(app), B.small_end_bush(app)
    ring_parts = B.rings(app)
    mcap = B.main_cap(app)
    tcap = B.main_cap(app, "centre main cap", thrust=True)
    mshell, rshell = B.main_shell(app), B.rod_shell(app)
    thrust = B.thrust_washer(app)

    rig.activate()
    bo = rig.place(blk, fixed=True)
    co = rig.place(crk)
    main = rig.revolute((bo, "crank_axis"), (co, "main"), flipped=False)

    # The five mains: a cap under each web, a shell in each half, and the
    # thrust washers either side of the centre one.
    caps = []
    for i in range(len(MAIN_Y)):
        centre = (i == len(MAIN_Y) // 2)
        c = rig.place(tcap if centre else mcap, at=(0, MAIN_Y[i], 0))
        rig.fasten((bo, f"cap_{i}"), (c, "seat"), flipped=False)
        caps.append(c)
        for holder, frame in ((bo, f"main_{i}"), (c, "shell")):
            sh = rig.place(mshell, at=(0, MAIN_Y[i], 0))
            rig.fasten((holder, frame), (sh, "seat"), flipped=False)
        if centre:
            for holder, nm in ((bo, "thrust_f"), (bo, "thrust_r"),
                               (c, "thrust_f"), (c, "thrust_r")):
                w = rig.place(thrust, at=(0, MAIN_Y[i], 0))
                rig.fasten((holder, nm), (w, "seat"), flipped=False)

    pistons, rods = [], []
    for k, y in enumerate(CYL_Y):
        z = piston_z(0.0, THROW_DEG[k])
        sgn = 1.0 if THROW_DEG[k] == 0.0 else -1.0
        ro = rig.place(rod, at=(0, y, sgn * CRANK_R))
        po = rig.place(pis, at=(0, y, z))
        rig.revolute((co, f"pin_{k}"), (ro, "big_end"), flipped=False)
        rig.revolute((ro, "small_end"), (po, "wrist_pin"), flipped=False)
        rig.slider((bo, f"bore_{k}"), (po, "slide"), flipped=False)
        cp = rig.place(cap, at=(0, y, sgn * CRANK_R))
        rig.fasten((ro, "cap"), (cp, "cap"), flipped=False)
        for holder, frame in ((ro, "shell"), (cp, "shell")):
            sh = rig.place(rshell, at=(0, y, sgn * CRANK_R))
            rig.fasten((holder, frame), (sh, "seat"), flipped=False)
        bu = rig.place(bush, at=(0, y, z))
        rig.fasten((ro, "small_end"), (bu, "seat"), flipped=False)
        gp = rig.place(pin, at=(0, y, z))
        rig.fasten((po, "wrist_pin"), (gp, "seat"), flipped=False)
        for nm in ("clip_f", "clip_r"):
            cl = rig.place(clip, at=(0, y, z))
            rig.fasten((po, nm), (cl, "seat"), flipped=False)
        for i, rp in enumerate(ring_parts):
            rr = rig.place(rp, at=(0, y, z))
            rig.fasten((po, f"ring_{i}"), (rr, "seat"), flipped=False)
        pistons.append(po)
        rods.append(ro)

    _solved("bottom end", rig)
    return {"block": bo, "crank": co, "caps": caps, "pistons": pistons,
            "rods": rods}, main


def front_and_rear(app, rig, occ, main):
    """Flywheel and clutch ring, damper, both crank seals and their housings,
    the sump, the chain case, the oil pump and the water pump — everything
    that hangs off the ends of the crank."""
    bo, co = occ["block"], occ["crank"]
    fw, rg = B.flywheel(app), B.ring_gear(app)
    dmp = B.damper(app)
    carrier = B.rear_seal_carrier(app)
    rear_seal = B.lip_seal(app, "rear crank seal", REAR_SEAL_D, 100.0, 12.0)
    front_seal = B.lip_seal(app, "front crank seal", FRONT_SEAL_D, 70.0, 10.0)
    pan, pan_gasket, plug = B.sump(app), B.sump_gasket(app), B.drain_plug(app)
    cover, cover_gasket = B.timing_cover(app), B.timing_cover_gasket(app)
    cs = B.sprocket(app, "crank sprocket", CRANK_PCD / 2.0, NOSE_D,
                    CRANK_TEETH, CHAIN_W)
    ocs = B.sprocket(app, "oil drive sprocket", OIL_CRANK_PCD / 2.0, NOSE_D,
                     OIL_CRANK_TEETH, OIL_CHAIN_W)
    ops = B.sprocket(app, "oil pump sprocket", OIL_PCD / 2.0, 16.0,
                     OIL_PUMP_TEETH, OIL_CHAIN_W)
    # The oil chain is drawn from the PUMP end so its span is positive: the
    # builder puts sprocket A at its own origin and B at (0, span), and the
    # pump is below the crank.
    oil_chain = B.chain(app, "oil pump chain", -OIL_PUMP_Z,
                        B.tooth_tip(OIL_PCD / 2.0, OIL_PUMP_TEETH),
                        B.tooth_tip(OIL_CRANK_PCD / 2.0, OIL_CRANK_TEETH),
                        OIL_CHAIN_W, thickness=5.0)
    pump, pump_cover = B.oil_pump(app), B.oil_pump_cover(app)
    g1 = B.oil_pump_gear(app, "oil pump gear")
    g2 = B.oil_pump_gear(app, "oil pump idler gear",
                         180.0 / B.OIL_GEAR_TEETH)
    pickup = B.oil_pickup(app)
    wp, wps = B.water_pump(app), B.water_pump_shaft(app)
    imp = B.impeller(app)
    wpp = B.vee_pulley(app, "water pump pulley", WP_PULLEY_D, B.WP_SHAFT_D)
    belt = B.chain(app, "fan belt", WATER_PUMP_Z,
                   PULLEY_D / 2.0 - B.BELT_GROOVE_D + 4.0,
                   WP_PULLEY_D / 2.0 - B.BELT_GROOVE_D + 4.0, B.BELT_W,
                   thickness=5.0)

    rig.activate()
    fo = rig.place(fw, at=(0, FLANGE_Y + FLANGE_T, 0))
    rig.fasten((co, "flywheel"), (fo, "flange"), flipped=False)
    ro = rig.place(rg, at=(0, FLANGE_Y + FLANGE_T + 1.0, 0))
    rig.fasten((fo, "ring_gear"), (ro, "seat"), flipped=False)
    do = rig.place(dmp, at=(0, DAMPER_Y, 0))
    rig.fasten((co, "damper"), (do, "hub"), flipped=False)

    ca = rig.place(carrier, at=(0, BLOCK_Y1, 0))
    rig.fasten((bo, "rear_face"), (ca, "face"), flipped=False)
    rs = rig.place(rear_seal, at=(0, REAR_SEAL_Y, 0))
    rig.fasten((ca, "seal"), (rs, "seat"), flipped=False)

    pg = rig.place(pan_gasket, at=(0, 0, PAN_RAIL))
    rig.fasten((bo, "pan_rail"), (pg, "rail"), flipped=False)
    pn = rig.place(pan)
    rig.fasten((bo, "pan_face"), (pn, "rail"), flipped=False)
    pl = rig.place(plug)
    rig.fasten((pn, "drain"), (pl, "head"), flipped=False)

    cg = rig.place(cover_gasket)
    rig.fasten((bo, "front_face"), (cg, "face"), flipped=False)
    cv = rig.place(cover)
    rig.fasten((bo, "cover_face"), (cv, "face"), flipped=False)
    fs = rig.place(front_seal, at=(0, FRONT_SEAL_Y, 0))
    rig.fasten((cv, "front_seal"), (fs, "seat"), flipped=False)

    # Both crank sprockets are FASTENED, not on revolutes: they are keyed to
    # the nose, so a revolute would give each a degree of freedom nothing
    # drives and the solver would be right to leave them where they lay.
    cso = rig.place(cs, at=(0, DRIVE_Y, 0))
    rig.fasten((co, "timing_drive"), (cso, "hub"), flipped=False)
    oco = rig.place(ocs, at=(0, OIL_DRIVE_Y, 0))
    rig.fasten((co, "oil_drive"), (oco, "hub"), flipped=False)

    po = rig.place(pump)
    rig.fasten((bo, "oil_pump"), (po, "mount"), flipped=False)
    rig.fasten((po, "cover"), (rig.place(pump_cover), "face"), flipped=False)
    rig.fasten((po, "pickup"), (rig.place(pickup), "flange"), flipped=False)
    opo = rig.place(ops, at=(0, OIL_DRIVE_Y, OIL_PUMP_Z))
    pump_mate = rig.revolute((po, "drive"), (opo, "hub"), flipped=False)
    g1o = rig.place(g1, at=(0, sum(B.OIL_BODY_Y) / 2.0, OIL_PUMP_Z))
    driver = rig.revolute((po, "driver"), (g1o, "hub"), flipped=False)
    g2o = rig.place(g2, at=(B.OIL_GEAR_X[1], sum(B.OIL_BODY_Y) / 2.0,
                            OIL_PUMP_Z))
    idler = rig.revolute((po, "idler"), (g2o, "hub"), flipped=False)
    ocho = rig.place(oil_chain)
    rig.fasten((bo, "oil_chain"), (ocho, "crank_end"), flipped=False)

    wpo = rig.place(wp)
    rig.fasten((cv, "water_pump"), (wpo, "face"), flipped=False)
    sho = rig.place(wps, at=(0, sum(B.WP_SHAFT_Y) / 2.0, WATER_PUMP_Z))
    wp_mate = rig.revolute((wpo, "shaft"), (sho, "seat"), flipped=False)
    imo = rig.place(imp, at=(0, B.IMPELLER_Y, WATER_PUMP_Z))
    rig.fasten((sho, "impeller"), (imo, "hub"), flipped=False)
    # By its BELT frame, not its hub: the pulley has to straddle the belt's
    # plane, and hung by its back face it stands 8 mm further aft — inside
    # the pump housing.
    puo = rig.place(wpp, at=(0, BELT_Y, WATER_PUMP_Z))
    rig.fasten((sho, "pulley"), (puo, "belt"), flipped=False)
    bo_belt = rig.place(belt)
    rig.fasten((bo, "belt"), (bo_belt, "crank_end"), flipped=False)

    # The drives, as relations. A chain turns both sprockets the same way.
    rig.chain(main, pump_mate, teeth_a=OIL_CRANK_TEETH,
              teeth_b=OIL_PUMP_TEETH, name="oil pump chain")
    rig.chain(pump_mate, driver, teeth_a=1, teeth_b=1, name="pump drive")
    rig.gear(driver, idler, teeth_a=B.OIL_GEAR_TEETH,
             teeth_b=B.OIL_GEAR_TEETH, phase=math.pi / B.OIL_GEAR_TEETH,
             name="pump gears")
    # A belt, so a chain relation — and its ratio is the ratio of the two
    # groove radii, said outright rather than through invented tooth counts.
    rig.activate()
    app.add_relation(type="chain", a=main["id"], b=wp_mate["id"],
                     ratio=(PULLEY_D - 16.0) / (WP_PULLEY_D - 16.0),
                     name="fan belt")
    # The mounts it hangs on.
    for side, nm in (("left", "mount_left"), ("right", "mount_right")):
        mb = B.engine_mount(app, side)
        rig.activate()
        mo = rig.place(mb)
        rig.fasten((bo, nm), (mo, "pad"), flipped=False)
    _solved("front and rear", rig)
    return {"crank_sprocket": cso, "cover": cv, "flywheel": fo,
            "sump": pn, "crank": co}


def top_end(app, rig, occ, main):
    """Head, gasket, valve gear, pre-chambers and the camshaft drive.

    The kinematics are two relations per valve: a CAM relation off the
    camshaft gives the valve its lift, and a RACK relation makes its rocker
    follow — backwards, which is the only way round it can go, because a cam
    is the one relation that does not invert.
    """
    hd, hg = T.head(app), T.head_gasket(app)
    valves = {k: T.valve(app, k) for k in VALVE}
    seats = {k: T.valve_seat(app, k) for k in VALVE}
    guide, seal = T.valve_guide(app), T.stem_seal(app)
    spring, retainer, collet = (T.valve_spring(app), T.spring_retainer(app),
                                T.collet(app))
    pre, glow, inj = T.prechamber(app), T.glow_plug(app), T.injector(app)
    cam, ccap = T.camshaft(app), T.cam_cap(app)
    rock, rshaft = T.rocker(app), T.rocker_shaft(app)
    cover, cgask, filler = (T.cam_cover(app), T.cam_cover_gasket(app),
                            T.oil_filler_cap(app))
    exm, inm = T.exhaust_manifold(app), T.intake_manifold(app)
    gaskets = {k: T.manifold_gasket(app, k) for k in VALVE}
    ms = B.sprocket(app, "cam sprocket", CAM_PCD / 2.0, 30.0, CAM_TEETH,
                    CHAIN_W, hub_d=60.0)
    tchain = B.chain(app, "timing chain", CAM_Z,
                     B.tooth_tip(CRANK_PCD / 2.0, CRANK_TEETH),
                     B.tooth_tip(CAM_PCD / 2.0, CAM_TEETH), CHAIN_W)
    guide_rail = B.chain_rail(app, "chain guide rail", 40.0, CAM_Z - 90.0,
                              -46.0, -14.0, CHAIN_W)
    tension_rail = B.chain_rail(app, "chain tensioner rail", 40.0,
                                CAM_Z - 90.0, 37.0, 14.0, CHAIN_W)

    rig.activate()
    bo = occ["block"]
    go = rig.place(hg)
    rig.fasten((bo, "deck"), (go, "deck"), flipped=False)
    ho = rig.place(hd)
    rig.fasten((bo, "head_face"), (ho, "deck"), flipped=False)

    cam_occ = rig.place(cam)
    cam_mate = rig.revolute((ho, "cam_axis"), (cam_occ, "axle"), flipped=False)
    for i, y in enumerate(CAM_BEARING_Y):
        cp = rig.place(ccap, at=(0, y, CAM_Z))
        rig.fasten((ho, f"cam_cap_{i}"), (cp, "seat"), flipped=False)
    mso = rig.place(ms, at=(0, DRIVE_Y, CAM_Z))
    rig.fasten((cam_occ, "drive"), (mso, "hub"), flipped=False)
    tco = rig.place(tchain)
    rig.fasten((bo, "timing_chain"), (tco, "crank_end"), flipped=False)
    for rail in (guide_rail, tension_rail):
        r = rig.place(rail)
        rig.fasten((bo, "timing_chain"), (r, "plane"), flipped=False)
    # Half crank speed, same direction: a chain, not a gear.
    rig.chain(main, cam_mate, teeth_a=CRANK_TEETH, teeth_b=CAM_TEETH,
              name="timing chain")

    for sx, nm in ((-1, "intake"), (1, "exhaust")):
        sh = rig.place(rshaft, at=(sx * ROCKER_PIVOT_X, 0, ROCKER_Z))
        rig.fasten((ho, f"rocker_shaft_{nm}"), (sh, "seat"), flipped=False)

    lifts = []
    for k, cy in enumerate(CYL_Y):
        po = rig.place(pre)
        rig.fasten((ho, f"pre_{k}"), (po, "seat"), flipped=False)
        for part, frame in ((glow, f"glow_plug_{k}"), (inj, f"injector_{k}")):
            o = rig.place(part)
            rig.fasten((ho, frame), (o, "face"), flipped=False)
        for kind in VALVE:
            so = rig.place(seats[kind])
            rig.fasten((ho, f"seat_{k}_{kind}"), (so, "bottom"), flipped=False)
            gu = rig.place(guide)
            rig.fasten((ho, f"guide_seat_{k}_{kind}"), (gu, "bottom"),
                       flipped=False)
            sl = rig.place(seal)
            rig.fasten((gu, "top"), (sl, "bottom"), flipped=False)
            vo = rig.place(valves[kind])
            slide = rig.slider((ho, f"guide_{k}_{kind}"), (vo, "seat"),
                               flipped=False)
            sp = rig.place(spring)
            rig.fasten((ho, f"spring_{k}_{kind}"), (sp, "seat"), flipped=False)
            re = rig.place(retainer)
            rig.fasten((vo, "retainer"), (re, "seat"), flipped=False)
            for nm in ("collet_a", "collet_b"):
                rig.fasten((vo, nm), (rig.place(collet), "seat"),
                           flipped=False)
            ro = rig.place(rock)
            rocker_mate = rig.revolute((ho, f"rocker_{k}_{kind}"),
                                       (ro, "pivot"), flipped=False)
            # The lift, off the camshaft — the SAME lobe the camshaft is
            # drawn from, said once in om615_spec and used by both.
            rig.cam(cam_mate, slide, rise=VALVE_LIFT,
                    duration_deg=DURATION_CAM,
                    start_deg=T.lobe_start(k, kind),
                    name=f"lobe {k + 1} {kind}")
            # ...and the rocker follows the valve. A rack couples a rotation
            # to a slide, and it is the RIGHT way round: the cam drives the
            # valve, the rack runs backwards from the valve to the rocker,
            # because a cam relation is the one that cannot be inverted.
            app.add_relation(type="rack", a=rocker_mate["id"], b=slide["id"],
                             ratio=ARM_VALVE,
                             name=f"rocker {k + 1} {kind}")
            lifts.append((k, kind, vo, slide))

    # Both manifolds and both gaskets hang off the head's own port faces.
    for kind, part in (("intake", inm), ("exhaust", exm)):
        gk_ = rig.place(gaskets[kind])
        rig.fasten((ho, f"{kind}_face"), (gk_, "face"), flipped=False)
        mo = rig.place(part)
        rig.fasten((ho, f"{kind}_face"), (mo, "face"), flipped=False)

    gk = rig.place(cgask)
    rig.fasten((ho, "top"), (gk, "rail"), flipped=False)
    cvo = rig.place(cover)
    rig.fasten((ho, "cover_rail"), (cvo, "rail"), flipped=False)
    rig.fasten((cvo, "filler"), (rig.place(filler), "seat"), flipped=False)
    _solved("top end", rig)
    return {"head": ho, "camshaft": cam_occ, "valves": lifts}, cam_mate


# Two dozen tabs in one strip is a list you scroll rather than read, so the
# parts go into folders by what they belong to. Whatever nothing claims is a
# fastener — they are DERIVED from the holes they go in, so naming them here
# would mean keeping a list in step with a table in the app.
FOLDERS = [
    ("Block", ["block", "cylinder head", "head gasket", "main cap",
               "centre main cap", "sump", "sump gasket", "drain plug",
               "rear seal carrier", "timing cover", "timing cover gasket",
               "cam cover", "cam cover gasket", "oil filler cap"]),
    ("Crank train", ["crankshaft", "con rod", "rod cap", "piston",
                     "gudgeon pin", "gudgeon circlip", "top ring",
                     "second ring", "oil ring", "flywheel", "ring gear",
                     "crank damper"]),
    ("Bearings and seals", ["main bearing shell", "rod bearing shell",
                            "thrust washer", "small end bush", "valve guide",
                            "intake valve seat", "exhaust valve seat",
                            "valve stem seal", "front crank seal",
                            "rear crank seal"]),
    ("Valve gear", ["camshaft", "cam bearing cap", "rocker arm",
                    "rocker shaft", "intake valve", "exhaust valve",
                    "valve spring", "spring retainer", "valve collet"]),
    ("Combustion", ["pre-chamber", "glow plug", "injector"]),
    ("Manifolds", ["intake manifold", "exhaust manifold",
                   "intake manifold gasket", "exhaust manifold gasket"]),
    ("Timing drive", ["crank sprocket", "cam sprocket", "timing chain",
                      "chain guide rail", "chain tensioner rail"]),
    ("Oil pump", ["oil pump", "oil pump cover", "oil pump gear",
                  "oil pump idler gear", "oil pickup", "oil drive sprocket",
                  "oil pump sprocket", "oil pump chain"]),
    ("Cooling", ["water pump", "water pump shaft", "impeller",
                 "water pump pulley", "fan belt"]),
    ("Mounts", ["engine mount left", "engine mount right"]),
]


def organise(app):
    named = set()
    for folder, parts in FOLDERS:
        known = {x["name"] for x in app.parts()}
        have = [n for n in parts if n in known]
        if not have:
            continue
        app.create_folder(folder=folder)
        app.move_to_folder(folder=folder, part=have)
        named.update(have)
    # ...but only the PARTS. `list_parts` lists every TAB, assemblies
    # included, and a first sweep of this kind once filed the engine itself
    # under Fasteners.
    rest = [x["name"] for x in app.parts() if x["name"] not in named]
    if rest:
        app.create_folder(folder="Fasteners")
        app.move_to_folder(folder="Fasteners", part=rest)
    print(f"  tabs: {len(FOLDERS) + bool(rest)} folders, "
          f"{len(named) + len(rest)} parts")


def run_engine(app, rig, main, top, samples, mode="exact"):
    """Two turns of the crank, checked at every sample: the pistons follow
    the slider-crank exactly, each valve lifts on its own lobe, and nothing
    touches anything it should not."""
    rig.activate()
    study = rig.motion(main, dof="rotation", **{"from": 0}, to=720,
                       samples=samples, clash_mode=mode,
                       name="two revolutions")
    r = app.run_motion_study(study=study["id"], include_poses=True,
                             _timeout=3600)
    return study, r


def pose(sample, occurrence, axis=2):
    return next(p["position"][axis] for p in sample["poses"]
                if p["occurrence"] == occurrence.id)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    ap.add_argument("--out", default="/tmp/om615")
    ap.add_argument("--samples", type=int, default=49,
                    help="motion study samples over two crank revolutions")
    ap.add_argument("--mesh", action="store_true",
                    help="check clashes with the collision mesh instead of "
                         "exact booleans. Much faster and NOT the gate: the "
                         "mesh's deflection on an engine this size is 1.17 "
                         "mm, so every running fit in it reads as contact")
    ap.add_argument("--no-study", action="store_true")
    a = ap.parse_args()
    app = connect(a.port)
    app.new()
    t0 = time.time()
    rig = app.assembly("engine")
    occ, main_mate = bottom_end(app, rig)
    front = front_and_rear(app, rig, occ, main_mate)
    top, cam_mate = top_end(app, rig, occ, main_mate)
    fasteners(app, rig, occ, top, front)
    for part in app.list_parts()["parts"]:
        if part["name"] == "Part 1":
            app.delete_part(part=part["name"])
    print(f"  built {len(rig.occurrences())} occurrences from "
          f"{len(app.parts())} parts in {time.time() - t0:.1f}s")

    # ---- the standing check: nothing is inside anything else -------------
    # `connected=False` because the pistons, valves and rockers are held by
    # mates rather than by touching, and `clearances=False` because every
    # running fit in this engine is a deliberate half-millimetre.
    rig.check(connected=False, clearances=False)
    print("  assembled: no part is inside another")

    if not a.no_study:
        study, r = run_engine(app, rig, main_mate, top, a.samples,
                              mode="mesh" if a.mesh else "exact")
        print(r["table"])
        for k, o in enumerate(occ["pistons"]):
            # The occurrence's origin is the GUDGEON PIN, so the crown is
            # PIN_H above it — reporting the pin as the crown made the squish
            # read 50 mm on an engine whose piston nearly touches the head.
            zs = [pose(s, o) + PIN_H for s in r["samples"]]
            stroke = max(zs) - min(zs)
            if abs(stroke - STROKE) > 0.05:
                raise SdkError(f"cylinder {k + 1} stroke {stroke:.3f} is not "
                               f"the design stroke {STROKE}")
            print(f"  cylinder {k + 1}: stroke {stroke:.3f} mm, crown at TDC "
                  f"{max(zs):.2f} (deck {DECK}, squish {DECK - max(zs):.2f})")
        for k, kind, vo, _ in top["valves"]:
            lift = [pose(s, vo) for s in r["samples"]]
            got = max(lift) - min(lift)
            if abs(got - VALVE_LIFT) > 0.3:
                raise SdkError(f"cylinder {k + 1} {kind} lifts {got:.2f}, "
                               f"not {VALVE_LIFT}")
        print(f"  all eight valves lift {VALVE_LIFT} mm on their own lobes; "
              f"the camshaft turns once per two crank turns")
        # The firing order is not a property of the CRANK — cylinders 1 and 4
        # share a throw and reach TDC together, so the pistons cannot tell
        # you which of them is firing. It is a property of the CAMSHAFT, and
        # this reads it off the exhaust events: a cylinder's exhaust lobe
        # peaks a fixed 260 crank degrees after its own firing TDC.
        order = _firing_order(r, top["valves"])
        print(f"  firing order, off the exhaust lobes: {order} "
              f"(design {'-'.join(str(n) for n in FIRING_ORDER)})")
        if order != "-".join(str(n) for n in FIRING_ORDER):
            raise SdkError("the camshaft does not fire in the design order")
        print(f"  clean={r['clean']} ({'mesh' if a.mesh else 'exact'}), "
              f"{r['pairs_checked']} pair checks "
              f"({r.get('pairs_rigid', 0)} skipped as rigid to each other) "
              f"in {r['seconds']:.1f}s")
        if not r["clean"]:
            raise SdkError("the mechanism collides — see the table above")

        os.makedirs(a.out, exist_ok=True)
        glb = os.path.join(a.out, "om615.glb")
        ex = app.export_motion(study=study["id"], path=glb, _timeout=1800)
        print(f"  animation: {ex['nodes']} nodes x {ex['frames']} keyframes "
              f"-> {glb} ({os.path.getsize(glb) // 1024} kB)")

    organise(app)
    os.makedirs(a.out, exist_ok=True)
    project = os.path.join(a.out, "om615.dsgn")
    app.save_project(path=project)
    print(f"  project: {project}")
    return 0


def _firing_order(r, valves):
    """The firing order, measured: each cylinder's exhaust lobe peaks
    2 * (EXHAUST_OPEN_CAM + DURATION_CAM / 2) crank degrees after that
    cylinder's own firing TDC, so the peaks in order ARE the order."""
    lag = 2.0 * (EXHAUST_OPEN_CAM + DURATION_CAM / 2.0)
    tdc = []
    for k, kind, vo, _ in valves:
        if kind != "exhaust":
            continue
        # MIN, not max: a valve opens DOWNWARD, so its lowest z is its
        # greatest lift. Taking the max read a shut valve and returned the
        # right cycle started at the wrong cylinder.
        peak = min(((pose(s, vo), s["value"]) for s in r["samples"]))[1]
        tdc.append(((peak - lag) % 720.0, k + 1))
    order = [k for _, k in sorted(tdc)]
    # A firing order is a CYCLE: 4-2-1-3 and 1-3-4-2 are the same engine.
    # Say it starting at cylinder one, which is how anyone quotes it.
    i = order.index(1)
    return "-".join(str(k) for k in order[i:] + order[:i])



def _bolt_up(rig, occ, part, rim_r, rim_at, far_occ, far_part, far_r, far_at,
             size, n, what, centres=None, rim_key=None, far_key=None):
    """Fit `n` real fasteners, each derived from the hole it goes in.

    A blind hole needs its FAR END said outright — the probe follows the
    axis on past the joint and finds material three parts away otherwise —
    so every set here names the floor of the hole it threads into, matched
    to its own rim by position rather than by order.
    """
    rims = part.body().edges(circular=True, radius=rim_r,
                             where=lambda e: abs(rim_at(e)) < 0.6)
    floors = far_part.body().edges(circular=True, radius=far_r,
                                   where=lambda e: abs(far_at(e)) < 0.6)
    if len(rims) < n or len(floors) < n:
        raise SdkError(f"{what}: {len(rims)} rims and {len(floors)} hole "
                       f"floors, expected at least {n} of each")
    # Pick by POSITION, not by count. A boss's top face is an annulus and a
    # blind hole's floor is a disc, and a model this size has other edges of
    # the same radius at the same height — matching the two lists by order
    # is how a bolt ends up in the wrong hole.
    if centres:
        near = []
        for c in centres:
            # A rim can come back SPLIT into two arcs (the element name then
            # carries a `.n`), and half a circle is not a rim the app will
            # take: "a cylindrical face has an axis but no direction". Prefer
            # the whole one at that centre.
            at = [e for e in rims
                  if (e.center.x - c[0]) ** 2 + (e.center.y - c[1]) ** 2 < 1.0]
            if not at:
                raise SdkError(f"{what}: no rim at {c}")
            # Prefer an element the toponamer did not have to disambiguate:
            # where a hole's seat face is CROSSED by something else its rim
            # comes back as two arcs, and half a circle is not a rim a
            # fastener can be fitted to.
            whole = [e for e in at if "." not in e.ref["element"]]
            hit = (whole or at)[0]
            near.append(hit)
        rims = near
    # Matching a rim to its own hole floor is a match in WORLD space, and the
    # two parts need not be drawn on the same axis: the flywheel's bolt
    # circle is in its local XY and the crank's is in its local XZ, so
    # comparing (x, y) to (x, y) put every bolt 64 mm off its own axis.
    rk = rim_key or (lambda e: (e.center.x, e.center.y))
    fk = far_key or (lambda e: (e.center.x, e.center.y))
    fitted = []
    for rim in rims[:n]:
        a0, a1 = rk(rim)
        floor = min(floors, key=lambda e: (fk(e)[0] - a0) ** 2
                    + (fk(e)[1] - a1) ** 2)
        # A generous timeout: fitting probes the whole assembly along the
        # bolt's axis, and by the fortieth fastener there is a lot of it.
        fitted.append(rig.bolt(rim, occ, standard="iso4762", size=size,
                               far_end={**floor.ref,
                                        "occurrence": far_occ.id},
                               _timeout=600))
    print(f"  {what}: {n} x {fitted[0]['size']} x "
          f"{fitted[0]['length_mm']} mm")
    return fitted


def fasteners(app, rig, occ, top, front):
    """The bolts that actually hold it together — head, mains, sump and
    flywheel. Each is DERIVED from its hole: nothing here says a length."""
    blk, hd = app.part("block"), app.part("cylinder head")
    pan, fw = app.part("sump"), app.part("flywheel")
    crk, cap = app.part("crankshaft"), app.part("main cap")
    rig.activate()
    _bolt_up(rig, top["head"], hd, (HEAD_BOLT_D + 1.0) / 2.0,
             lambda e: e.center.z - T.HEAD_BOLT_SEAT,
             occ["block"], blk, HEAD_BOLT_D / 2.0,
             lambda e: e.center.z - (DECK - 60.0), "M12", len(HEAD_BOLT),
             "head bolts", centres=HEAD_BOLT)
    _bolt_up(rig, front["sump"], pan, 4.5,
             lambda e: e.center.z - (PAN_RAIL - 6.0),
             occ["block"], blk, 4.0,
             lambda e: e.center.z - (PAN_RAIL + 20.0), "M8",
             2 * len(B._pan_bolt_y()), "sump bolts",
             centres=[(sx * PAN_BOLT_X, y) for y in B._pan_bolt_y()
                      for sx in (-1, 1)])
    _bolt_up(rig, front["flywheel"], fw, 5.5,
             lambda e: e.center.z - B.FLYWHEEL_T,
             front["crank"], crk, 5.0,
             lambda e: e.center.y - (FLANGE_Y + FLANGE_T + 9.0 - 40.0),
             "M10", FLYWHEEL_BOLTS, "flywheel bolts",
             rim_key=lambda e: (e.center.x, -e.center.y),
             far_key=lambda e: (e.center.x, e.center.z))
    # A cap is drawn about ITS OWN origin and placed at its main's y, so its
    # rims are at y = 0 in part space and at MAIN_Y[i] in the world the
    # block's hole floors are measured in. Both have to be said.
    for i, c in enumerate(occ["caps"]):
        _bolt_up(rig, c, app.part("centre main cap" if i == 2 else "main cap"),
                 B.CAP_CLEARANCE_D / 2.0,
                 lambda e: e.center.z + CAP_DEPTH,
                 occ["block"], blk, CAP_BOLT_D / 2.0,
                 lambda e: e.center.z - 46.0, "M12", 2,
                 f"main cap {i + 1} bolts",
                 centres=[(-CAP_BOLT_X, 0.0), (CAP_BOLT_X, 0.0)],
                 rim_key=lambda e, y=MAIN_Y[i]: (e.center.x, e.center.y + y))


if __name__ == "__main__":
    sys.exit(main())
