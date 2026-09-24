#!/usr/bin/env python3
"""A four-cylinder OM615-shaped diesel: 1988 cc, five main bearings, a single
overhead camshaft on a duplex chain, rocker arms, and a pre-chamber per
cylinder — built complete, down to the bearing shells and the seals.

    python3 generators/om615.py --port <port> [--out DIR] [--build-only]

`om615_spec` holds the engine specification; `om615_bottom`, `om615_top`
and `om615_accessories` build its parts. This module assembles and checks them.
The mechanism is four slider-crank loops off one crankshaft, a
chain relation halving crank speed to the camshaft, and per valve a cam
relation for its lift and a rack relation that makes its rocker follow.

Findings go in generators/om615/README.md.
"""
import argparse
import hashlib
from pathlib import Path
import atexit
import json
import math
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "tools"))
from designer_components import claim, plan
from designer_validation import validate
from designer_sdk import SdkError, V, connect  # noqa: E402
# This generator's own directory, and generators/ above it for _lib.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from om615_spec import *  # noqa: E402,F401,F403
import om615_bottom as B  # noqa: E402
import om615_top as T  # noqa: E402
import om615_accessories as A
import om615_finish as F  # noqa: E402


def _solved(what, rig):
    """Solve and say the three numbers that matter, not the mate list."""
    r = rig.solve()
    # The loop keys are only there when the assembly has loops.
    print(f"  {what}: {len(r['mates'])} mates, dof {r.get('dof', '-')}, "
          f"{r.get('loop_mates', 0)} in loops, "
          f"closed={r.get('loops_closed', True)}, "
          f"residual {r.get('residual_mm', 0.0):.2e} mm, "
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
                        tooth_tip(OIL_PCD / 2.0, OIL_PUMP_TEETH),
                        tooth_tip(OIL_CRANK_PCD / 2.0, OIL_CRANK_TEETH),
                        OIL_CHAIN_W, thickness=OIL_CHAIN_T)
    pump, pump_cover = B.oil_pump(app), B.oil_pump_cover(app)
    g1 = B.oil_pump_gear(app, "oil pump gear")
    g2 = B.oil_pump_gear(app, "oil pump idler gear",
                         180.0 / B.OIL_GEAR_TEETH)
    pickup = B.oil_pickup(app)
    wp, wps = B.water_pump(app), B.water_pump_shaft(app)
    imp = B.impeller(app)
    wpp = B.vee_pulley(app, "water pump pulley", WP_PULLEY_D, B.WP_SHAFT_D)
    belt = A.belt(app)

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
                     ratio=A.PULLEYS[0][2] / A.PULLEYS[2][2],
                     name="fan belt")
    accessories = A.install(app, rig, bo, sho, wp_mate, main)
    accessories["vacuum"] = A.install_vacuum(app, rig, cv, main)
    # The mounts it hangs on.
    for side, nm in (("left", "mount_left"), ("right", "mount_right")):
        mb = B.engine_mount(app, side)
        rig.activate()
        mo = rig.place(mb)
        rig.fasten((bo, nm), (mo, "pad"), flipped=False)
    _solved("front and rear", rig)
    return {"crank_sprocket": cso, "cover": cv, "flywheel": fo,
            "sump": pn, "crank": co, "water pump": wpo,
            "accessories": accessories}


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
    heat_shield = T.injector_heat_shield(app)
    cam, ccap = T.camshaft(app), T.cam_cap(app)
    rock, rshaft = T.rocker(app), T.rocker_shaft(app)
    cover, cgask, filler = (T.cam_cover(app), T.cam_cover_gasket(app),
                            T.oil_filler_cap(app))
    exm, inm = T.exhaust_manifold(app), T.intake_manifold(app)
    tstat = T.thermostat_housing(app)
    gaskets = {k: T.manifold_gasket(app, k) for k in VALVE}
    ms = B.sprocket(app, "cam sprocket", CAM_PCD / 2.0, 30.0, CAM_TEETH,
                    CHAIN_W, hub_d=60.0)
    tips = (tooth_tip(CRANK_PCD / 2.0, CRANK_TEETH),
            tooth_tip(CAM_PCD / 2.0, CAM_TEETH))
    tchain = B.chain(app, "timing chain", CAM_Z, *tips, CHAIN_W)
    guide_rail = B.chain_rail(app, "chain guide rail", 40.0, CAM_Z - 90.0,
                              -1, CAM_Z, *tips, bow=10.0, width=CHAIN_W)
    tension_rail = B.chain_rail(app, "chain tensioner rail", 40.0,
                                CAM_Z - 90.0, 1, CAM_Z, *tips, bow=14.0,
                                width=CHAIN_W)

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

    for sx, nm in ((1, "exhaust"),):
        sh = rig.place(rshaft, at=(sx * ROCKER_PIVOT_X, 0, ROCKER_Z))
        rig.fasten((ho, f"rocker_shaft_{nm}"), (sh, "seat"), flipped=False)

    lifts = []
    for k, cy in enumerate(CYL_Y):
        po = rig.place(pre)
        rig.fasten((ho, f"pre_{k}"), (po, "seat"), flipped=False)
        rig.fasten((po, "nozzle_seat"), (rig.place(heat_shield), "seat"), flipped=False)
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
    # The thermostat housing sits on the head's front ear, manifold side.
    tso = rig.place(tstat)
    rig.fasten((ho, "thermostat"), (tso, "seat"), flipped=False)

    gk = rig.place(cgask)
    rig.fasten((ho, "top"), (gk, "rail"), flipped=False)
    cvo = rig.place(cover)
    rig.fasten((ho, "cover_rail"), (cvo, "rail"), flipped=False)
    rig.fasten((cvo, "filler"), (rig.place(filler), "seat"), flipped=False)
    _solved("top end", rig)
    return {"head": ho, "camshaft": cam_occ, "valves": lifts,
            "cam cover": cvo, "thermostat": tso}, cam_mate


# Two dozen tabs in one strip is a list you scroll rather than read, so the
# parts go into folders by what they belong to. Whatever nothing claims is a
# fastener — they are DERIVED from the holes they go in, so naming them here
# would mean keeping a list in step with a table in the app.
FOLDERS = [
    ("Block", ["block", "cylinder head", "head gasket", "main cap",
               "centre main cap", "sump", "sump gasket", "drain plug",
               "rear seal carrier", "timing cover", "timing cover gasket",
               "cam cover", "cam cover gasket", "oil filler cap",
               "oil filter housing", "dipstick tube", "dipstick"]),
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
    ("Combustion", ["pre-chamber", "glow plug", "injector", "injector heat shield",
                    "fuel filter", "fuel filter bracket"]),
    ("Manifolds", ["intake manifold", "exhaust manifold",
                   "intake manifold gasket", "exhaust manifold gasket"]),
    ("Timing drive", ["crank sprocket", "cam sprocket", "timing chain",
                      "chain guide rail", "chain tensioner rail"]),
    ("Oil pump", ["oil pump", "oil pump cover", "oil pump gear",
                  "oil pump idler gear", "oil pickup", "oil drive sprocket",
                  "oil pump sprocket", "oil pump chain"]),
    ("Cooling", ["water pump", "water pump shaft", "impeller",
                 "water pump pulley", "fan belt", "cooling fan", "fan hub spacer",
                 "thermostat housing", "upper coolant hose",
                 "lower coolant hose", "heater pipe"]),
    ("Alternator", ["alternator front casting", "alternator rear casting",
                    "alternator stator", "alternator rotor and shaft",
                    "alternator pulley", "alternator cooling fan",
                    "alternator mounting bracket", "alternator adjustment bracket",
                    "alternator winding pack"]),
    ("Vacuum pump", ["vacuum pump casting", "vacuum pump valve cover",
                     "vacuum pump diaphragm rim", "vacuum pump diaphragm centre",
                     "vacuum pump rocker yoke", "vacuum pump follower roller",
                     "vacuum pump face cam", "vacuum timer shaft bush"]),
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
    existing = app.list_motion_studies()["studies"]
    study = existing[0] if existing else rig.motion(main, dof="rotation", **{"from": 0}, to=720,
                       samples=samples, clash_mode=mode,
                       name="two revolutions")
    r = app.run_motion_study(study=study["id"], include_poses=True,
                             _timeout=3600)
    return study, r


def pose(sample, occurrence, axis=2):
    return next(p["position"][axis] for p in sample["poses"]
                if p["occurrence"] == occurrence.id)


def reference_dimensions(app):
    """Measure finished B-Reps, so a changed constant is not its own proof."""
    measured = {}
    for name, axes in (
            ("piston", {2: PISTON_HEIGHT}),
            ("gudgeon pin", {0: GUDGEON_D, 2: GUDGEON_L}),
            ("intake valve", {0: VALVE_HEAD_D["intake"]}),
            ("exhaust valve", {0: VALVE_HEAD_D["exhaust"]}),
            ("top ring", {2: RING_T[0]}),
            ("second ring", {2: RING_T[1]}),
            ("oil ring", {2: RING_T[2]})):
        part = app.part(name)
        lo, hi = part.bbox()
        volume = part.volume()
        if volume <= 0:
            raise SdkError(f"{name} has no solid volume")
        extents = [hi[i] - lo[i] for i in range(3)]
        for axis, expected in axes.items():
            if abs(extents[axis] - expected) > 0.01:
                raise SdkError(f"{name} measured extent {extents[axis]:.3f} "
                               f"differs from reference {expected:.3f} mm")
        measured[name] = dict(extents_mm=extents, volume_mm3=volume)
        if name == "piston":
            # The part's pin axis is z=0; its crown is the highest face.
            if abs(hi[2] - PIN_H) > 0.01:
                raise SdkError("piston compression height differs from reference")
            measured[name]["compression_height_mm"] = hi[2]
        if name.endswith(" valve"):
            rims = part.body().edges(circular=True, radius=STEM_D / 2.0,
                                     tol=0.001)
            if not rims:
                raise SdkError(f"{name}: no stem edge at the reference diameter")
            measured[name]["stem_diameter_mm"] = 2.0 * rims[0].radius

    head = app.part("cylinder head")
    faces = head.body().faces(kind="plane")
    lower = [f for f in faces if f.normal.z < -0.99
             and abs(f.centroid.z-FIRE_FACE) < 0.01 and f.area > 100]
    upper = [f for f in faces if f.normal.z > 0.99
             and abs(f.centroid.z-HEAD_TOP) < 0.01 and f.area > 100]
    if not lower or not upper:
        raise SdkError("head has no measurable fire-face / cover-rail datums")
    height = max(upper, key=lambda f:f.area).centroid.z - max(
        lower, key=lambda f:f.area).centroid.z
    if not 84.8-0.001 <= height <= 85.0+0.001:
        raise SdkError(f"head machined height {height:.3f} is outside 84.8–85.0 mm")
    measured["cylinder head"] = dict(machined_height_mm=height,
        source="Workshop 05.4-118/1 (PDF page 220)", limits_mm=[84.8,85.0])
    injector = app.part("injector")
    flats = injector.body().faces(kind="plane", where=lambda f:
        abs(f.normal.x)>0.99 and -35.01<f.centroid.z<-25.99)
    flat_x = [f.centroid.x for f in flats]
    if len(flat_x)<2 or abs(max(flat_x)-min(flat_x)-27)>0.01:
        raise SdkError("injector has no 27 mm wrench flats")
    measured["injector"] = dict(wrench_af_mm=max(flat_x)-min(flat_x),
        source="Workshop 07.1.4-230/1 (PDF page 424)",
        envelope_status="photo-derived; not manufacturing dimensions")
    for kind in ("intake", "exhaust"):
        part = app.part(f"{kind} manifold")
        lo,hi = part.bbox()
        if lo.x < HEAD_X+MANIFOLD_GASKET_T-0.01:
            raise SdkError(f"{kind} crosses the shared manifold-side gasket face")
        measured[f"{kind} manifold"] = dict(side="+X", external_ports=len(port_centers(kind)),
                                             bounds_mm=[list(lo),list(hi)])
    block = app.part("block")
    registers = block.body().edges(circular=True, radius=PUMP_REGISTER_D/2,
        where=lambda e: (e.center-V(PUMP_MOUNT)).length()<0.02)
    if not registers:
        raise SdkError("injection-pump mounting register is missing")
    measured["pump mounting register"] = dict(diameter_mm=2*registers[0].radius,
        status="estimated interface; pump and drive not fitted")

    print("  measured piston height, pin size and valve head diameters match "
          "the reference dimensions; head height, injector flats and pump register checked")
    return measured


def profile_calls(app, out):
    """Measure the actual generator's RPCs, including failed operations."""
    original = app.call
    measurements = {}

    def measured(method, *args, **kwargs):
        started = time.perf_counter()
        try:
            return original(method, *args, **kwargs)
        finally:
            row = measurements.setdefault(method, dict(calls=0, seconds=0.0))
            row["calls"] += 1
            row["seconds"] += time.perf_counter() - started

    def save():
        os.makedirs(out, exist_ok=True)
        with open(os.path.join(out, "rpc-timings.json"), "w") as f:
            json.dump(dict(sorted(measurements.items(),
                                  key=lambda kv: -kv[1]["seconds"])), f, indent=2)

    app.call = measured
    atexit.register(save)


def _finished(builder):
    """A component builder whose part comes out with its finish, so a part
    rebuilt on its own (--update-component) looks as it does in the engine."""
    return lambda app: F.finish(builder(app))


COMPONENTS = {
    "belt": ("fan belt", A.belt), "fan": ("cooling fan", A.fan),
    "vacuum-pump": ("vacuum pump casting", A.pump_housing),
    "block": ("block", B.block), "head": ("cylinder head", T.head),
    "sump": ("sump", B.sump), "timing-cover": ("timing cover", B.timing_cover),
    "damper": ("crank damper", B.damper),
    "engine-mount-left": ("engine mount left",
                          lambda app: B.engine_mount(app, "left")),
    "engine-mount-right": ("engine mount right",
                           lambda app: B.engine_mount(app, "right")),
    "cam-cover": ("cam cover", T.cam_cover),
    "intake": ("intake manifold", T.intake_manifold),
    "exhaust": ("exhaust manifold", T.exhaust_manifold),
    "injector": ("injector", T.injector), "prechamber": ("pre-chamber", T.prechamber),
    "thermostat": ("thermostat housing", T.thermostat_housing),
    "oil-filter": ("oil filter housing", A.oil_filter_housing),
    "fuel-filter": ("fuel filter", A.fuel_filter),
    "fuel-filter-bracket": ("fuel filter bracket", A.fuel_filter_bracket),
    "upper-hose": ("upper coolant hose", A.upper_hose),
    "lower-hose": ("lower coolant hose", A.lower_hose),
    "heater-pipe": ("heater pipe", A.heater_pipe),
    "dipstick-tube": ("dipstick tube", A.dipstick_tube),
    "dipstick": ("dipstick", A.dipstick),
}
COMPONENTS = {k: (name, _finished(b)) for k, (name, b) in COMPONENTS.items()}


def component_fingerprint(key):
    # Whole builder modules are conservative dependencies. Shared specification
    # changes require a complete rebuild: they also affect unowned mechanisms.
    root = Path(__file__).resolve().parent
    spec = hashlib.sha256((root / 'om615_spec.py').read_bytes() +
                          json.dumps(A.assembly_inputs(), sort_keys=True).encode()).hexdigest()
    inputs = [root / 'om615_top.py', root / 'om615_bottom.py',
              root / 'om615_accessories.py', root / 'om615_finish.py',
              root.parent.parent / 'tools/designer_sdk.py',
              root.parent.parent / 'tools/designer_components.py']
    digest = hashlib.sha256(key.encode())
    for path in inputs:
        digest.update(path.read_bytes())
    return spec + ':' + digest.hexdigest()


def write_report(out, name, report):
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, name), 'w') as f:
        json.dump(report, f, indent=2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage", choices=("preview", "local", "full"),
                    help="default preview; local adds standing checks, full adds motion")
    ap.add_argument("--project", help="open an existing project for update/validation")
    ap.add_argument("--update-component", choices=tuple(COMPONENTS))
    ap.add_argument("--validate-only", action="store_true")
    ap.add_argument("--port", type=int)
    ap.add_argument("--out", default="/tmp/om615")
    ap.add_argument("--samples", type=int, default=49,
                    help="motion study samples for a fresh build over two crank revolutions")
    ap.add_argument("--mesh", action="store_true",
                    help="check clashes with the collision mesh instead of "
                         "exact booleans. Much faster and NOT the gate: the "
                         "mesh's deflection on an engine this size is 1.17 "
                         "mm, so every running fit in it reads as contact")
    ap.add_argument("--no-study", action="store_true")
    ap.add_argument("--build-only", action="store_true",
                    help="save an unchecked draft for geometry iteration")
    ap.add_argument("--no-export", action="store_true",
                    help="run all checks but skip animated GLB export")
    ap.add_argument("--component", choices=tuple(COMPONENTS),
                    help="build one component for fast shape iteration")
    ap.add_argument("--profile", action="store_true",
                    help="write RPC counts and elapsed time, including failed runs")
    a = ap.parse_args()
    if a.build_only and a.no_study:
        ap.error('--build-only and --no-study are alternative legacy stages')
    legacy = 'preview' if a.build_only else 'local' if a.no_study else None
    if a.stage and legacy and a.stage != legacy:
        ap.error('legacy validation flag conflicts with --stage')
    a.stage = a.stage or legacy or 'preview'
    if a.mesh:
        ap.error('staged validation uses exact checks; use preview to skip collision work')
    if a.component and (a.update_component or a.validate_only or a.project):
        ap.error('--component creates an isolated component; use --update-component for an existing one')
    if a.project and not (a.update_component or a.validate_only):
        ap.error('--project requires --update-component or --validate-only')
    app = connect(a.port)
    if a.profile:
        profile_calls(app, a.out)
    t0 = time.perf_counter()
    if a.update_component or a.validate_only:
        if a.project:
            app.open_project(path=os.path.abspath(a.project))
        if a.update_component:
            key = a.update_component
            fp = component_fingerprint(key)
            owned = app.component_status(namespace='om615', key=key)
            old = owned.get('generator', {}).get('fingerprint', '')
            if old and old.split(':')[0] != fp.split(':')[0]:
                raise SdkError('shared engine specification changed; rebuild the complete engine')
            with plan(app, namespace='om615', key=key, fingerprint=fp,
                      builder=COMPONENTS[key][1]) as change:
                print('  component plan:', change.report)
                change.apply()
                write_report(a.out, 'component-update.json', change.report)
        rig = app.assembly('engine', create=False)
        write_report(a.out, 'reference-dimensions.json', reference_dimensions(app))
        write_report(a.out, 'accessory-structure-report.json', A.check_mounting_structure(app, rig))
        if a.stage != 'preview':
            write_report(a.out, 'accessory-standing-report.json', A.check_subassemblies(app))
            write_report(a.out, 'accessory-mounting-report.json', A.check_mountings(app, rig))
        result = validate(app, stage=a.stage, assembly=rig)
        report = result.current(app)
        write_report(a.out, 'validation-report.json', report)
        path = os.path.join(a.out, 'om615.dsgn' if report['accepted'] else 'om615-unchecked.dsgn')
        rig.activate()
        app.save_project(path=path)
        write_report(a.out, 'timings.json', dict(total_s=time.perf_counter()-t0))
        print(f"  {a.stage}: {report['status']}; accepted={report['accepted']} -> {path}")
        return 0 if report['status'] == 'passed' else 1
    app.new()
    if a.component:
        part = COMPONENTS[a.component][1](app)
        claim(part, namespace="om615", key=a.component,
              fingerprint=component_fingerprint(a.component))
        part.activate()
        os.makedirs(a.out, exist_ok=True)
        path = os.path.join(a.out, f"{a.component}-unchecked.dsgn")
        app.save_project(path=path)
        print(f"  component: {part.name}, {part.volume():.1f} mm3, "
              f"{time.perf_counter() - t0:.1f}s -> {path}")
        print("  component only: assembly and motion checks have NOT run")
        return 0
    timings = {}
    rig = app.assembly("engine")
    stage = time.perf_counter()
    occ, main_mate = bottom_end(app, rig)
    timings["bottom_end_s"] = time.perf_counter() - stage
    stage = time.perf_counter()
    front = front_and_rear(app, rig, occ, main_mate)
    timings["front_and_rear_s"] = time.perf_counter() - stage
    stage = time.perf_counter()
    top, cam_mate = top_end(app, rig, occ, main_mate)
    timings["top_end_s"] = time.perf_counter() - stage
    stage = time.perf_counter()
    top["hanging"] = A.install_hanging(app, rig, {
        "block": occ["block"], "head": top["head"],
        "thermostat": top["thermostat"], "water pump": front["water pump"]})
    _solved("what hangs on it", rig)
    timings["hanging_s"] = time.perf_counter() - stage
    stage = time.perf_counter()
    fasteners(app, rig, occ, top, front)
    timings["fasteners_s"] = time.perf_counter() - stage
    stage = time.perf_counter()
    F.apply_all(app)
    timings["finish_s"] = time.perf_counter() - stage
    for part in app.list_parts()["parts"]:
        if part["name"] == "Part 1":
            app.delete_part(part=part["name"])
    for key, (name, _) in COMPONENTS.items():
        claim(app.part(name, create=False), namespace='om615', key=key,
              fingerprint=component_fingerprint(key))
    rig.activate()
    rig.motion(main_mate, dof='rotation', **{'from': 0}, to=720,
               samples=a.samples, clash_mode='exact', name='two revolutions')
    organise(app)
    timings["build_s"] = time.perf_counter() - t0
    print(f"  built {len(rig.occurrences())} occurrences from "
          f"{len(app.parts())} parts in {timings['build_s']:.1f}s")
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "reference-dimensions.json"), "w") as f:
        json.dump(reference_dimensions(app), f, indent=2)
    rig.activate()
    # Save before expensive validation: a failing gate must leave geometry
    # available for inspection. The filename explicitly says it is a draft.
    draft = os.path.join(a.out, "om615-unchecked.dsgn")
    app.save_project(path=draft)
    print(f"  unchecked build: {draft}")
    with open(os.path.join(a.out, "timings.json"), "w") as f:
        json.dump(timings, f, indent=2)
    write_report(a.out, 'accessory-structure-report.json', A.check_mounting_structure(app, rig))
    initial = validate(app, stage='preview' if a.stage == 'full' else a.stage,
                       assembly=rig)
    validation = initial.current(app)
    write_report(a.out, 'validation-report.json', validation)
    if validation['status'] != 'passed':
        raise SdkError('validation failed; see validation-report.json')
    if a.stage != 'preview':
        write_report(a.out, 'accessory-standing-report.json', A.check_subassemblies(app))
        write_report(a.out, 'accessory-mounting-report.json', A.check_mountings(app, rig))
    if a.stage != 'full':
        print(f"  {a.stage} passed; motion NOT checked; draft: {draft}")
        timings['total_s'] = time.perf_counter() - t0
        write_report(a.out, 'timings.json', timings)
        return 0
    rig.activate()

    # ---- the standing check: nothing is inside anything else -------------
    # Running fits are intentional: ask only for interference, without the
    # costly distances that contact_report needs for connectivity/near misses.
    #
    # The BUDGET is the point. Both sweep APIs are bounded — closest pairs
    # first, then it stops and says so — and at its defaults it reached 145
    # of this engine's 790 candidate pairs, found nothing, and read as a
    # clean bill of health. It is also the only check that looks at parts
    # RIGID to each other, which a motion study skips by design: between
    # them, the two of them had never examined 72 real overlaps.
    report = app.interference(mode="exact", max_pairs=20000,
                              deadline_s=900, _timeout=1800)
    with open(os.path.join(a.out, "standing-report.json"), "w") as f:
        json.dump(report, f, indent=2)
    if report.get("truncated") or report["clash"]:
        raise SdkError("standing sweep failed; see standing-report.json")
    timings["standing_s"] = report["elapsed_s"]
    # In interference, `pairs` contains only clashes, not every candidate.
    print(f"  assembled: no part is inside another — "
          f"{report['pairs_checked']} candidate pairs checked exactly, "
          f"{report['pairs_box_cleared']} cleared by bounds, "
          f"in {report['elapsed_s']:.1f}s")

    if a.stage == "full":
        study, r = run_engine(app, rig, main_mate, top, a.samples,
                              mode="mesh" if a.mesh else "exact")
        with open(os.path.join(a.out, "motion-report.json"), "w") as f:
            json.dump(r, f, indent=2)
        print(r["table"])
        write_report(a.out, "accessory-motion-report.json",
                     A.motion_dimensions(r, front["accessories"]))
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
        timings["motion_s"] = r["seconds"]

        os.makedirs(a.out, exist_ok=True)
        glb = os.path.join(a.out, "om615.glb")
        if not a.no_export:
            started = time.perf_counter()
            ex = app.export_motion(study=study["id"], path=glb, _timeout=1800)
            timings["export_s"] = time.perf_counter() - started
            print(f"  animation: {ex['nodes']} nodes x {ex['frames']} keyframes "
                  f"-> {glb} ({os.path.getsize(glb) // 1024} kB)")

    validation.update(stage='full', status='passed', accepted=True,
                      revision=app.validation_revision(),
                      standing=dict(status='passed', report=report),
                      motion=dict(status='passed', report=r, scope='defined_samples'))
    write_report(a.out, 'validation-report.json', validation)
    os.makedirs(a.out, exist_ok=True)
    project = os.path.join(a.out, "om615.dsgn")
    app.save_project(path=project)
    timings["total_s"] = time.perf_counter() - t0
    with open(os.path.join(a.out, "timings.json"), "w") as f:
        json.dump(timings, f, indent=2)
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
             size, n, what, centres=None, rim_key=None, far_key=None,
             centre_key=None, *, snapshots):
    """Fit `n` real fasteners, each derived from the hole it goes in.

    A blind hole needs its FAR END said outright — the probe follows the
    axis on past the joint and finds material three parts away otherwise —
    so every set here names the floor of the hole it threads into, matched
    to its own rim by position rather than by order.
    """
    # The fastener stage owns these explicit snapshots. It creates hardware,
    # but does not regenerate the parts being measured. Reuse their frozen
    # centers across bolt groups, and check their revisions before finishing.
    def measured_edges(owner, radius):
        if owner.id not in snapshots:
            snapshots[owner.id] = owner.body().snapshot(
                circular=True, max_elements=10000)
        return [e for e in snapshots[owner.id].edges
                if e.radius is not None and abs(e.radius - radius) < 0.05]

    rims = [e for e in measured_edges(part, rim_r) if abs(rim_at(e)) < 0.6]
    floors = [e for e in measured_edges(far_part, far_r)
              if abs(far_at(e)) < 0.6]
    if len(rims) < n or len(floors) < n:
        raise SdkError(f"{what}: {len(rims)} rims and {len(floors)} hole "
                       f"floors, expected at least {n} of each")
    # Pick by POSITION, not by count. A boss's top face is an annulus and a
    # blind hole's floor is a disc, and a model this size has other edges of
    # the same radius at the same height — matching the two lists by order
    # is how a bolt ends up in the wrong hole.
    if centres:
        # `centres` are in the rim's own (x, y) unless `centre_key` says
        # otherwise: a hole drilled along x has its centres in (y, z).
        ck = centre_key or (lambda e: (e.center.x, e.center.y))
        near = []
        for c in centres:
            # A rim can come back SPLIT into two arcs (the element name then
            # carries a `.n`), and half a circle is not a rim the app will
            # take: "a cylindrical face has an axis but no direction". Prefer
            # the whole one at that centre.
            at = [e for e in rims
                  if (ck(e)[0] - c[0]) ** 2 + (ck(e)[1] - c[1]) ** 2 < 1.0]
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
    snapshots = {}

    def bolt_up(*args, **kwargs):
        return _bolt_up(*args, **kwargs, snapshots=snapshots)

    blk, hd = app.part("block"), app.part("cylinder head")
    pan, fw = app.part("sump"), app.part("flywheel")
    crk, cap = app.part("crankshaft"), app.part("main cap")
    rig.activate()
    bolt_up(rig, top["head"], hd, (HEAD_BOLT_D + 1.0) / 2.0,
             lambda e: e.center.z - T.HEAD_BOLT_SEAT,
             occ["block"], blk, (HEAD_BOLT_D + TAP_FIT) / 2.0,
             lambda e: e.center.z - (DECK - 60.0), "M12", len(HEAD_BOLT),
             "head bolts", centres=HEAD_BOLT)
    bolt_up(rig, front["sump"], pan, 4.5,
             lambda e: e.center.z - (PAN_RAIL - 6.0),
             occ["block"], blk, (PAN_BOLT_D + TAP_FIT) / 2.0,
             lambda e: e.center.z - (PAN_RAIL + 20.0), "M8",
             2 * len(B._pan_bolt_y()), "sump bolts",
             centres=[(sx * PAN_BOLT_X, y) for y in B._pan_bolt_y()
                      for sx in (-1, 1)])
    bolt_up(rig, front["flywheel"], fw, 5.5,
             lambda e: e.center.z - B.FLYWHEEL_T,
             front["crank"], crk, (FLYWHEEL_BOLT_D + TAP_FIT) / 2.0,
             lambda e: e.center.y - (FLANGE_Y + FLANGE_T + 9.0 - 40.0),
             "M10", FLYWHEEL_BOLTS, "flywheel bolts",
             rim_key=lambda e: (e.center.x, -e.center.y),
             far_key=lambda e: (e.center.x, e.center.z))
    bolt_up(rig, top["cam cover"], app.part("cam cover"), 3.3,
             lambda e: e.center.z - (COVER_BASE + CAM_COVER_BOSS_H),
             top["head"], hd, (6.0 + TAP_FIT) / 2.0,
             lambda e: e.center.z - (COVER_BASE - 11.0), "M6",
             2 * len(T._cover_bolt_y()), "cam cover bolts",
             centres=[(cover_bolt_x(sx), y) for y in T._cover_bolt_y()
                      for sx in (-1, 1)])
    bolt_up(rig, top["thermostat"], app.part("thermostat housing"),
             (TSTAT_BOLT_D + 1.0) / 2.0,
             lambda e: e.center.z - (HEAD_TOP + TSTAT_FLANGE_T),
             top["head"], hd, (TSTAT_BOLT_D + TAP_FIT) / 2.0,
             lambda e: e.center.z - (HEAD_TOP + 1.0 - TSTAT_TAP_DEPTH), "M8", 2,
             "thermostat housing bolts",
             centres=[(TSTAT_X, TSTAT_Y + s * TSTAT_BOLT_DY) for s in (-1, 1)])
    # What hangs on it: drilled along x, so every centre is a (y, z).
    yz = lambda e: (e.center.y, e.center.z)  # noqa: E731
    hang = top["hanging"]
    fx, fy, fz = OIL_FILTER_BOSS
    r = OIL_FILTER_BOLT_PCD / 2.0 / math.sqrt(2.0)
    bolt_up(rig, hang["oil filter housing"], app.part("oil filter housing"),
             (OIL_FILTER_BOLT_D + 1.0) / 2.0,
             lambda e: e.center.x - (fx - OIL_FILTER_FLANGE_T),
             occ["block"], blk, (OIL_FILTER_BOLT_D + TAP_FIT) / 2.0,
             lambda e: e.center.x - (fx + OIL_FILTER_TAP_DEPTH), "M8", 4,
             "oil filter housing bolts",
             centres=[(fy + sy * r, fz + sz * r) for sy in (-1, 1)
                      for sz in (-1, 1)],
             rim_key=yz, far_key=yz, centre_key=yz)
    bx, by, bz = FUEL_FILTER_BOSS
    bolt_up(rig, hang["fuel filter bracket"], app.part("fuel filter bracket"),
             (FUEL_FILTER_BOLT_D + 1.0) / 2.0,
             lambda e: e.center.x - (bx - A.FUEL_BRACKET_T),
             top["head"], hd, (FUEL_FILTER_BOLT_D + TAP_FIT) / 2.0,
             lambda e: e.center.x - (bx + FUEL_FILTER_TAP_DEPTH), "M8", 2,
             "fuel filter bracket bolts",
             centres=[(by, bz + s * FUEL_FILTER_BOLT_DZ) for s in (-1, 1)],
             rim_key=yz, far_key=yz, centre_key=yz)
    # A cap is drawn about ITS OWN origin and placed at its main's y, so its
    # rims are at y = 0 in part space and at MAIN_Y[i] in the world the
    # block's hole floors are measured in. Both have to be said.
    for i, c in enumerate(occ["caps"]):
        bolt_up(rig, c, app.part("centre main cap" if i == 2 else "main cap"),
                 B.CAP_CLEARANCE_D / 2.0,
                 lambda e: e.center.z + CAP_DEPTH,
                 occ["block"], blk, (CAP_BOLT_D + TAP_FIT) / 2.0,
                 lambda e: e.center.z - 46.0, "M12", 2,
                 f"main cap {i + 1} bolts",
                 centres=[(-CAP_BOLT_X, 0.0), (CAP_BOLT_X, 0.0)],
                 rim_key=lambda e, y=MAIN_Y[i]: (e.center.x, e.center.y + y))

    for snapshot in snapshots.values():
        snapshot.validate()


if __name__ == "__main__":
    sys.exit(main())
