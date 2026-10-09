#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""An automatic mechanical watch — time only, with small seconds.

    python3 generators/watch/watch.py [--port N] [--out DIR]
                                      [--stage parts|assembly|all]
                                      [--recheck]

A 21,600 vph self-winding movement in a 40 mm case: barrel, centre, third,
fourth and escape wheels, a Swiss lever escapement with a hairspring, motion
works under the dial, a rotor winding the barrel through a reduction wheel,
and a dial, hands and case to put it in (generators/watch/README.md).

Every dimension is a document variable (watch_spec.declare). The going train
and the motion works are coupled by gear relations, so one motion study turns
every hand at its true rate; the clearances that decide whether it runs are
DECLARED on the assembly and re-checked there. The run refuses to save what
it has not checked, and it gates on each check's receipt, not on its `ok`.
"""
import argparse
import subprocess
import json
import hashlib
import math
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import watch_parts as W  # noqa: E402
import watch_spec as spec  # noqa: E402
from designer_sdk import SdkError, connect  # noqa: E402
from watch_spec import *  # noqa: F401,F403,E402 — rebound by declare()

PHASES = []


class phase:
    def __init__(self, name):
        self.name = name

    def __enter__(self):
        self.t = time.perf_counter()

    def __exit__(self, *exc):
        PHASES.append((self.name, time.perf_counter() - self.t))


def report_phases():
    total = sum(t for _, t in PHASES)
    print(f"time: {total:.1f} s")
    for name, t in PHASES:
        print(f"  {name:<22} {t:6.1f} s  {100 * t / max(total, 1e-9):4.0f}%")


# --- parts --------------------------------------------------------------------------

def build_parts(app):
    P = {}
    P["mainplate"] = W.mainplate(app)
    P["barrel"] = W.barrel(app)
    P["barrel_arbor"] = W.barrel_arbor(app)
    P.update(W.train(app))                       # center, third, fourth, escape
    P["pallet"] = W.pallet_fork(app)
    P["balance"] = W.balance(app)
    P["hairspring"] = W.hairspring(app)
    for k, v in W.bridges(app).items():
        P[f"{k}_bridge"] = v
    P["cannon"] = W.cannon_pinion(app)
    P["minute"] = W.minute_wheel(app)
    P["hour"] = W.hour_wheel(app)
    P["dial"] = W.dial(app)
    P["indices"] = W.indices(app)
    for k, v in W.hands(app).items():
        P[f"{k}_hand"] = v
    P["rotor"] = W.rotor(app)
    P["reduction"] = W.reduction_wheel(app)
    P["reduction_pinion"] = W.reduction_pinion(app)
    P["reverser"] = W.reverser_wheel(app)
    P["reverser_pinion"] = W.reverser_pinion(app)
    P["click"] = W.click_pawl(app)
    P["crown_wheel"] = W.crown_wheel(app)
    P["stem"] = W.stem(app)
    P["clutch"] = W.clutch(app)
    P["winding_pinion"] = W.winding_pinion(app)
    P["setting"] = W.setting_wheel(app)
    P["setting_lever"] = W.lever(app, "Setting lever",
                                 float(SETTING_LEVER_ARM),
                                 float(STEM_Z) + 0.24)   # into the stem groove
    P["yoke"] = W.lever(app, "Yoke", float(YOKE_ARM),
                        float(STEM_Z) + 0.56)            # into the clutch's
    P["keyless_bridge"] = W.keyless_bridge(app)
    P.update(W.case(app))
    return P


def check_parts(app, P):
    """Every part is a valid solid that built — on its receipt, not its `ok`.
    An empty part's `valid` is vacuously true and its receipt says so."""
    bad = []
    for key, part in P.items():
        part.activate()
        g = app.check_geometry()
        standing = g["receipt"]["standing"]
        if standing != "verified":
            bad.append(f"{part.name}: {standing} — {g['receipt']['summary']}"
                       + (f" {g['feature_errors']}" if g.get("feature_errors")
                          else ""))
    return bad


# --- the assembly -------------------------------------------------------------------

def mesh_phase(pa, pb, za, zb):
    """The relation phase that puts a GAP of b where a has a tooth, on the
    line between them.

    Every arbor is built with tooth 0 along +X and mated so that dof 0 keeps
    it there. With a's teeth at alpha + 2*pi*k/za, b's gaps at
    beta + 2*pi*(j + 1/2)/zb and beta = -(za/zb) * alpha + phase, a tooth of a
    on the centre line (angle phi) meeting a gap of b (at phi + pi) gives

        phase = phi * (1 + za/zb) + pi - pi/zb      (mod 2*pi/zb)

    gear_train.py gets away with pi/24 because its centre line IS +X; a watch
    train turns corners.
    """
    phi = math.atan2(pb[1] - pa[1], pb[0] - pa[0])
    return (phi * (1 + za / zb) + math.pi - math.pi / zb) % (2 * math.pi / zb)


def build_assembly(app, P):
    asm = app.assembly("Watch")
    occ = {}
    occ["mainplate"] = asm.place(P["mainplate"].name, fixed=True,
                                 name="Mainplate")
    for k, p in P.items():
        if k != "mainplate":
            occ[k] = asm.place(p.name, name=p.name)
    mp = occ["mainplate"]

    def rev(key, frame):
        return asm.revolute((mp, frame), (occ[key], "axis"),
                            flipped=False)["id"]

    def fasten(a, fa, b, fb="axis"):
        return asm.fasten((occ[a] if isinstance(a, str) else a, fa),
                          (occ[b], fb), flipped=False)["id"]

    m = {}
    for key, frame in (("barrel", "pivot barrel"),
                       ("barrel_arbor", "pivot barrel"),
                       ("center", "pivot center"), ("third", "pivot third"),
                       ("fourth", "pivot fourth"), ("escape", "pivot escape"),
                       ("pallet", "pivot pallet"),
                       ("balance", "pivot balance"),
                       ("minute", "pivot minute"), ("hour", "pivot hour"),
                       ("reduction", "pivot reduction"),
                       # The wheel rides loose on this staff (one pivot, two
                       # mates): the one-way clutch between them is a
                       # freewheel relation (winding_relations).
                       ("reduction_pinion", "pivot reduction"),
                       ("reverser", "pivot reverser"),
                       ("reverser_pinion", "pivot reverser"),
                       ("click", "pivot click"),
                       ("crown_wheel", "pivot crown_wheel"),
                       ("rotor", "pivot center"),
                       # The cannon pinion turns on the centre arbor: its
                       # friction fit is a RELATION (keyless_relations), so
                       # setting the hands can make it slip.
                       ("cannon", "pivot center"),
                       ("setting", "pivot setting"),
                       ("setting_lever", "pivot setting_lever"),
                       ("yoke", "pivot yoke")):
        m[key] = rev(key, frame)
    # On the stem's axis: the stem and the clutch slide as well as turn —
    # the clutch turned by the stem and slid by the yoke, two relations into
    # one cylindrical mate. The winding pinion only turns.
    for key in ("stem", "clutch"):
        m[key] = asm.cylindrical((mp, "stem"), (occ[key], "axis"),
                                 flipped=False)["id"]
    m["winding_pinion"] = rev("winding_pinion", "stem")
    fasten("balance", "axis", "hairspring")
    for key in ("train_bridge", "barrel_bridge", "cock_bridge",
                "pallet_bridge", "dial", "indices", "case", "bezel",
                "crystal", "caseback", "window", "keyless_bridge"):
        fasten(mp, "seat", key, "seat")

    pos = POS
    rel = {}
    for a, b, za, zb, pa, pb in (
            ("barrel", "center", Z_BARREL, Z_CENTER_PIN, "barrel", "center"),
            ("center", "third", Z_CENTER, Z_THIRD_PIN, "center", "third"),
            ("third", "fourth", Z_THIRD, Z_FOURTH_PIN, "third", "fourth"),
            ("fourth", "escape", Z_FOURTH, Z_ESCAPE_PIN, "fourth", "escape"),
            ("cannon", "minute", Z_CANNON, Z_MINUTE_W, "center", "minute"),
            ("minute", "setting", Z_MINUTE_W, Z_SETTING_W, "minute",
             "setting"),
            ("minute", "hour", Z_MINUTE_P, Z_HOUR, "minute", "center"),
            ("rotor", "reduction", Z_ROTOR_PIN, Z_REDUCTION_W, "center",
             "reduction"),
            ("reduction_pinion", "barrel_arbor", Z_REDUCTION_P, Z_RATCHET,
             "reduction", "barrel"),
            # The reverser: the second wheel takes its turn from the first,
            # wheel to wheel, and its pinion drives the ratchet beside the
            # first one's (generators/watch/README.md).
            ("reduction", "reverser", Z_REDUCTION_W, Z_REDUCTION_W,
             "reduction", "reverser"),
            ("reverser_pinion", "barrel_arbor", Z_REDUCTION_P, Z_RATCHET,
             "reverser", "barrel"),
            ("barrel_arbor", "crown_wheel", Z_RATCHET, Z_CROWN_WHEEL,
             "barrel", "crown_wheel")):
        r = asm.gear(m[a], m[b], za, zb,
                     phase=mesh_phase(pos[pa], pos[pb], za, zb),
                     name=f"{a} {za} -> {b} {zb}")
        if r.get("error"):
            raise SdkError(f"relation {a}->{b}: {r['error']}")
        rel[f"{a}->{b}"] = r
    keyless_relations(app, m, rel)
    escapement_relations(app, m, rel)
    winding_relations(app, m, rel)

    # The hands. Meshing phases turn every driven arbor by up to a tooth
    # pitch — 22 minutes of hour hand, on a 32-tooth hour wheel — so each hand
    # is fastened at the angle that cancels its arbor's resting dof: the
    # watch reads 12:00:00 with the barrel at zero.
    app.call("set_mate", id=m["barrel"], rotation=0.0)
    app.solve_assembly()
    dof = {x["id"]: x.get("rotation", 0.0) for x in app.list_mates()["mates"]}
    for hand, arbor in (("hour_hand", "hour"), ("minute_hand", "cannon"),
                        ("seconds_hand", "fourth")):
        base = m[arbor]
        asm.app.add_mate(type="fastened",
                         a={"occurrence": occ[arbor].id, "frame": "axis"},
                         b={"occurrence": occ[hand].id, "frame": "axis"},
                         flipped=False, rotation=-dof.get(base, 0.0))
    keyless_positions(app, m, rel)
    solved = app.solve_assembly()
    return asm, occ, m, rel, solved


# --- the keyless works -----------------------------------------------------------

# A contrate ring meshing a flat wheel turns it the other way about the
# wheel's own axis: at the contact, the ring's tooth and the wheel's rim move
# the same way at r_ring * w = R_wheel * W.
WIND_RATIO = -Z_CONTRATE / Z_WINDING_W
SET_RATIO = -Z_CONTRATE / Z_SETTING_W


def contrate_phase(ratio, z_wheel, tooth_at, gap_at):
    """The relation phase that puts a GAP of the wheel where the contrate
    ring has a tooth: the ring's tooth 0 at frame angle `tooth_at` (the
    contact, about the stem) meets the wheel's gap centred at `gap_at`
    (about the wheel) — mesh_phase for axes at right angles."""
    return (gap_at - math.pi / z_wheel - ratio * tooth_at) \
        % (2 * math.pi / z_wheel)


def relate(app, m, rel, key, kind, a, b, **kw):
    r = app.call("add_relation", type=kind, a=m[a], b=m[b], name=key, **kw)
    if r.get("error"):
        raise SdkError(f"relation {key}: {r['error']}")
    rel[key] = r
    return r


def escapement_relations(app, m, rel):
    """The escapement animates from the train: a cam from the escape wheel
    swings the balance, harmonically and once per tooth pitch, and another
    points the pallet fork at the impulse pin while the pin is in its notch
    (watch_spec.escapement_tables). Approximate in one way, said in
    generators/watch/README.md: the wheel turns continuously, not in steps."""
    from watch_spec import escapement_tables
    bal, fork = escapement_tables()
    relate(app, m, rel, "balance swing", "cam", "escape", "balance", lift=bal)
    relate(app, m, rel, "fork swing", "cam", "escape", "pallet", lift=fork)


def winding_relations(app, m, rel):
    """The one-way reduction wheel (generators/watch/README.md): the wheel the
    rotor drives turns the pinion only in the WINDING direction. Crown
    winding turns the ratchet positive, which through its mesh turns the
    pinion negative — so the freewheel drives negative, and the crown's
    winding runs the pinion on ahead of a still rotor."""
    relate(app, m, rel, "one-way wheel", "freewheel", "reduction",
           "reduction_pinion", ratio=1.0, drives="negative")
    # The second wheel turns the other way, and its clutch drives the same
    # sense — both pinions mesh the ratchet — so one engages and the other
    # slips whichever way the rotor swings.
    relate(app, m, rel, "one-way wheel 2", "freewheel", "reverser",
           "reverser_pinion", ratio=1.0, drives="negative")
    # The click: the ratchet may turn the winding way and is HELD the other,
    # and the pawl rides over each of its 48 teeth as it goes.
    relate(app, m, rel, "click", "click", "barrel_arbor", "barrel_arbor",
           drives="positive")
    # What the pawl DOES as the wheel turns under it is a cam, from the
    # ratchet's own flank: it drops into each gap and rides out over each
    # tooth, touching neither (watch_spec.click_table).
    relate(app, m, rel, "click ride", "cam", "barrel_arbor", "click",
           lift=spec.click_table())


def check_rotor(app, m):
    """The reverser: the rotor winds the ratchet the SAME way whichever way
    it swings, by the train's ratio either time. Undone afterwards, so the
    watch is left as it was — and undo must bring both freewheels' history
    back with the pose."""
    want = 30.0 * Z_ROTOR_PIN / Z_REDUCTION_W * Z_REDUCTION_P / Z_RATCHET
    before = _mates(app)
    r0 = math.degrees(before[m["rotor"]]["rotation"])
    app.call("set_mate", id=m["rotor"], rotation_deg=r0 + 30.0)
    app.solve_assembly()
    fwd = _mates(app)
    app.call("set_mate", id=m["rotor"], rotation_deg=r0 - 30.0)  # 60 back
    app.solve_assembly()
    back = _mates(app)
    app.call("undo")
    app.call("undo")
    app.solve_assembly()
    after = _mates(app)
    got = {"forward": _turn(before, fwd, m["barrel_arbor"]),
           "back": _turn(fwd, back, m["barrel_arbor"]),
           "crown": _turn(before, fwd, m["stem"]),
           "restored": max(abs(_turn(before, after, m[k]))
                           for k in ("rotor", "reduction", "reduction_pinion",
                                     "reverser", "reverser_pinion",
                                     "barrel_arbor", "stem"))}
    bad = []
    if abs(got["forward"] - want) > 1e-3 * max(1.0, abs(want)):
        bad.append(f"the rotor turned 30 deg forward wound the ratchet "
                   f"{got['forward']:+.4f} deg, want {want:+.4f}")
    if abs(got["back"] - 2 * want) > 1e-3 * max(1.0, abs(want)):
        bad.append(f"the rotor turned 60 deg back wound the ratchet "
                   f"{got['back']:+.4f} deg, want {2 * want:+.4f} — the "
                   f"reverser winds both ways")
    if abs(got["crown"]) > 1e-6:
        bad.append(f"the rotor winding turned the crown {got['crown']:+.4f} "
                   f"deg; the dogs should ride over it")
    if got["restored"] > 1e-6:
        bad.append(f"undo left the winding train {got['restored']:.6f} deg "
                   f"off where it was")
    return got, bad


def check_dogs(app, m):
    """The crown turned BACK rides over the dogs: the ratchet does not move,
    the clutch lifts along the stem, and undo puts both back. Half a dog's
    pitch is half the lift."""
    before = _mates(app)
    s0 = math.degrees(before[m["stem"]].get("rotation", 0.0))
    app.call("set_mate", id=m["stem"], rotation_deg=s0 - 360.0 / DOG_N / 2)
    app.solve_assembly()
    after = _mates(app)
    app.call("undo")
    app.solve_assembly()
    back = _mates(app)
    # The lift is the WHOLE of it whenever the dogs are off a notch (solid
    # dogs cannot stand part way over each other), so it is where the clutch
    # STANDS that says it rode out, not how far it moved: the rotor's
    # winding may have left it out already.
    want = float(DOG_H) + 0.02
    got = {"ratchet": _turn(before, after, m["barrel_arbor"]),
           "lift": after[m["clutch"]].get("offset", 0.0),
           "restored": max(abs(_turn(before, back, m[k]))
                           for k in ("barrel_arbor", "stem", "clutch"))}
    bad = []
    if abs(got["ratchet"]) > 1e-4:
        bad.append(f"the crown turned back moved the ratchet "
                   f"{got['ratchet']:+.4f} deg; the dogs should slip")
    if abs(got["lift"] + want) > 0.01:
        bad.append(f"turned back, the clutch stands {got['lift']:+.4f} mm "
                   f"along the stem, want {-want:+.4f} — riding the dogs")
    if got["restored"] > 1e-6:
        bad.append(f"undo left the keyless works {got['restored']:.6f} off")
    return got, bad


def seat_dogs(app, m, rel):
    """Drop the dogs back into a notch, by turning the crown on to the next
    one. The rotor's winding runs the winding pinion past the clutch and
    leaves it riding out; a real one is pushed back in by its spring as soon
    as the wheel stops, and the watch should not sit with its clutch held
    out of mesh."""
    r = next(x for x in app.call("list_relations")["relations"]
             if x["name"] == "dogs")
    pitch = 2 * math.pi / DOG_N
    # Turning the crown FORWARD closes the gap: the lead is (pinion less
    # clutch), so the clutch has to make up what it has already ridden.
    ahead = r["lead"] - math.floor(r["lead"] / pitch) * pitch
    if ahead > 1e-9:
        now = math.degrees(_mates(app)[m["stem"]].get("rotation", 0.0))
        app.call("set_mate", id=m["stem"],
                 rotation_deg=now + math.degrees(ahead))
        app.solve_assembly()


def check_escapement(app, m):
    """Drive the escape wheel through a passage of the pin and read the
    balance and the fork back against their tables, then put it back."""
    from watch_spec import balance_deg, fork_deg, fork_bank_deg, Z_ESCAPE

    def rot(ms, key):
        return math.degrees(ms[m[key]]["rotation"])

    ms = {x["id"]: x for x in app.call("list_mates")["mates"]}
    e_rest = rot(ms, "escape")
    c = round(e_rest / (180.0 / Z_ESCAPE)) * (180.0 / Z_ESCAPE)
    bad, worst, swing = [], 0.0, []
    for de in (-3.0, -0.6, -0.3, 0.0, 0.3, 0.6, 3.0):
        app.call("set_mate", id=m["escape"], rotation_deg=c + de)
        app.solve_assembly()
        ms = {x["id"]: x for x in app.call("list_mates")["mates"]}
        e = rot(ms, "escape")
        wb, wf = balance_deg(e), fork_deg(balance_deg(e))
        gb, gf = rot(ms, "balance"), rot(ms, "pallet")
        db = (gb - wb + 180.0) % 360.0 - 180.0
        worst = max(worst, abs(db), abs(gf - wf))
        swing.append(gf)
    app.call("set_mate", id=m["escape"], rotation_deg=e_rest)
    app.solve_assembly()
    if worst > 0.5:
        bad.append(f"the balance and fork are {worst:.3f} deg off their "
                   f"tables")
    if abs(max(swing) - min(swing) - 2 * fork_bank_deg()) > 0.2:
        bad.append(f"the fork swings {max(swing) - min(swing):.2f} deg, not "
                   f"{2 * fork_bank_deg():.2f}")
    return {"worst": worst, "swing": max(swing) - min(swing),
            "centre": c}, bad


def keyless_relations(app, m, rel):
    """Crown in — the winding position, which the document holds."""
    # The cannon pinion's friction fit on the centre arbor.
    relate(app, m, rel, "friction", "gear", "center", "cannon", ratio=1.0)
    relate(app, m, rel, "square", "gear", "stem", "clutch", ratio=1.0)
    # Winding pinion's contrate teeth under the stem (-90 deg about it) meet
    # the winding wheel on its -X side (180 deg about the crown wheel arbor).
    relate(app, m, rel, "winding", "gear", "winding_pinion", "crown_wheel",
           ratio=WIND_RATIO, phase=contrate_phase(WIND_RATIO, Z_WINDING_W,
                                                  -math.pi / 2, math.pi))
    # Pulling the stem turns the setting lever (its pin rides the stem's
    # groove), the lever turns the yoke, and the yoke — its pin in the
    # clutch's groove — slides the clutch in.
    relate(app, m, rel, "setting lever", "rack", "setting_lever", "stem",
           pitch_radius=float(SETTING_LEVER_ARM))
    relate(app, m, rel, "lever -> yoke", "gear", "setting_lever", "yoke",
           ratio=-(float(CLUTCH_TRAVEL) / float(YOKE_ARM))
           / (float(STEM_PULL) / float(SETTING_LEVER_ARM)))
    relate(app, m, rel, "yoke pin", "rack", "yoke", "clutch",
           pitch_radius=float(YOKE_ARM))
    # The dogs LAST, and Breguet: sawteeth that drive one way and ride over
    # the other, which they can only because the clutch LIFTS along the stem
    # as they do (generators/watch/README.md#keyless-works-and-winding). The crown turned back
    # then slips here instead of unwinding the barrel, and the rotor winding
    # runs the winding pinion on ahead of a still clutch.
    #
    # Last because its phase is where the two stand AS DRAWN — the clutch's
    # dogs in the winding pinion's gaps, seated rather than half way up a
    # ramp — and the winding relation above is what puts the pinion there.
    now = _mates(app)
    seated = (now[m["winding_pinion"]].get("rotation", 0.0)
              - now[m["clutch"]].get("rotation", 0.0))
    relate(app, m, rel, "dogs", "freewheel", "clutch", "winding_pinion",
           ratio=1.0, drives="positive", teeth=DOG_N, phase=seated,
           ride_mate=m["clutch"], ride_dof="offset",
           ride_mm=-(float(DOG_H) + 0.02))


# The crown's two positions, as CONFIGURATIONS of the assembly (v101).
CONFIGS = {}


def keyless_positions(app, m, rel):
    """"Crown in" and "Crown out" as configurations, and the relation only
    crown-out uses. Crown in takes the clutch's relation to the setting wheel
    out of the solve; crown out takes out the clutch's dogs and the cannon
    pinion's friction fit and HOLDS the stem pulled — the yoke's relations
    then slide the clutch onto the setting wheel by themselves. The base,
    with every relation in, over-constrains the keyless works (the setting
    wheel driven by the clutch AND the minute wheel) and says so if anyone
    activates it; the document is saved crown in.

    The setting relation's phase is the one of its sixteen that meshes where
    the setting wheel stands now, so pulling the crown moves the hands by
    less than half a setting-wheel tooth."""
    now = _mates(app)
    pitch = 2 * math.pi / Z_SETTING_W
    base = contrate_phase(SET_RATIO, Z_SETTING_W, math.pi / 2, 0.0)
    want = (now[m["setting"]].get("rotation", 0.0)
            - SET_RATIO * now[m["clutch"]].get("rotation", 0.0))
    setting = relate(app, m, rel, "setting", "gear", "clutch", "setting",
                     ratio=SET_RATIO,
                     phase=base + pitch * round((want - base) / pitch))
    # Each position HOLDS the stem where it is — pushed in, or pulled out.
    # Held, the stem is where the lever chain starts; without a hold a plain
    # solve roots that chain at its first relation's driving side, which is
    # the setting LEVER (a rack runs from the turning part to the sliding
    # one), and crown in kept the lever's last crown-out angle: the stem
    # pulled back out and the clutch left on the setting wheel.
    cin = app.call("add_configuration", name="Crown in")["id"]
    app.call("set_configuration", id=cin,
             suppressed_relations=[setting["id"]],
             mate_overrides=[{"mate": m["stem"], "offset": 0.0}])
    cout = app.call("add_configuration", name="Crown out")["id"]
    app.call("set_configuration", id=cout,
             suppressed_relations=[rel["dogs"]["id"], rel["friction"]["id"]],
             mate_overrides=[{"mate": m["stem"], "offset": float(STEM_PULL)}])
    app.call("set_active_configuration", id=cin)
    CONFIGS.update({"in": cin, "out": cout})


def to_setting(app, m, rel):
    """Crown out: activate the configuration, which re-solves."""
    app.call("set_active_configuration", id=CONFIGS["out"])
    return app.solve_assembly()


def to_winding(app, m, rel):
    app.call("set_active_configuration", id=CONFIGS["in"])
    return app.solve_assembly()


def _mates(app):
    return {x["id"]: x for x in app.list_mates()["mates"]}


def _turn(before, after, mate):
    return math.degrees(after[mate].get("rotation", 0.0)
                        - before[mate].get("rotation", 0.0))


def check_winding(app, m):
    """Crown in, a quarter turn of the crown winds the ratchet
    (10/16)*(70/48) of a quarter turn, and neither the going train nor the
    rotor moves."""
    # TWICE: the dogs have a dead band — the rotor's winding runs the
    # winding pinion on ahead of the clutch, and the crown takes that up
    # before it bites — so the first quarter turn is the take-up and the
    # second is the one that must be exact.
    s0 = math.degrees(_mates(app)[m["stem"]].get("rotation", 0.0))
    app.call("set_mate", id=m["stem"], rotation_deg=s0 + 90.0)
    app.solve_assembly()
    before = _mates(app)
    app.call("set_mate", id=m["stem"], rotation_deg=s0 + 180.0)
    app.solve_assembly()
    after = _mates(app)
    # Undone, not turned back: turning the crown back would UNWIND the
    # ratchet (there are no Breguet teeth to slip), and the one-way wheel
    # locks that way, so it would drag the rotor round with it.
    app.call("undo")
    app.call("undo")
    app.solve_assembly()
    got = {k: _turn(before, after, m[k])
           for k in ("barrel_arbor", "center", "winding_pinion", "rotor",
                     "reverser")}
    # The rotor stays: the one-way reduction wheel lets the ratchet run on.
    want = {"barrel_arbor": 90.0 * Z_CONTRATE / Z_WINDING_W * Z_CROWN_WHEEL
            / Z_RATCHET, "center": 0.0, "winding_pinion": 90.0, "rotor": 0.0,
            "reverser": 0.0}
    bad = [f"crown in, a quarter turn of the crown turned the {k} "
           f"{got[k]:+.4f} deg, want {want[k]:+.4f}"
           for k in want if abs(got[k] - want[k]) > 1e-6 * max(1, want[k])]
    return got, bad


# The meshes that only exist crown-out, as MESHES has them.
SETTING_MESHES = [
    ("clutch", "setting", "stem", Z_CONTRATE, 1.0),
    ("setting", "minute", "stem", Z_SETTING_W, Z_CONTRATE / Z_SETTING_W),
]


def check_setting(app, occ, m, rel, sweep=True):
    """Crown out, a quarter turn of the crown turns the cannon pinion — the
    minute hand — a quarter turn back, the centre wheel stays where it was,
    the clutch has slid CLUTCH_TRAVEL in, and (sweep) the setting pose and
    its meshes are clash-free. Leaves the watch crown-in."""
    bad, info = [], {}
    solved = to_setting(app, m, rel)
    try:
        if solved.get("errored_mates"):
            bad.append(f"crown out, {solved['errored_mates']} mates errored")
        before = _mates(app)
        # A quarter turn from where the stem STANDS: engaging the clutch
        # applies the whole crown-out chain from its root, so the stem need
        # not be at zero (it was, only while a solve left chains unapplied).
        stem0 = before[m["stem"]].get("rotation", 0.0)
        info["clutch offset"] = before[m["clutch"]].get("offset", 0.0)
        if abs(info["clutch offset"] + float(CLUTCH_TRAVEL)) > 1e-6:
            bad.append(f"crown out, the clutch moved "
                       f"{info['clutch offset']:+.4f}, want "
                       f"{-float(CLUTCH_TRAVEL):+.4f}")
        app.call("set_mate", id=m["stem"], rotation=stem0 + math.pi / 2)
        app.solve_assembly()
        after = _mates(app)
        app.call("set_mate", id=m["stem"], rotation=stem0)
        app.solve_assembly()
        for k in ("cannon", "center", "winding_pinion"):
            info[k] = _turn(before, after, m[k])
        want = {"cannon": -90.0, "center": 0.0, "winding_pinion": 0.0}
        bad += [f"crown out, a quarter turn of the crown turned the {k} "
                f"{info[k]:+.4f} deg, want {want[k]:+.4f}"
                for k in want if abs(info[k] - want[k]) > 1e-6 * 90]
        if sweep:
            info["meshes"] = check_meshes(app, occ, m, meshes=SETTING_MESHES)
            bad += [f"crown out, the {pair} mesh interferes: {v:.7f} mm3"
                    for pair, v in info["meshes"].items() if v > 0.0]
            r = app.interference(mode="exact", deadline_s=300,
                                 max_pairs=20000, _timeout=600)
            info["sweep"] = (len(r.get("pairs", [])),
                             r["receipt"]["standing"])
            if r.get("pairs") or r["receipt"]["standing"] != "verified":
                bad.append(f"crown out, the exact sweep: {info['sweep']} "
                           + "; ".join(f"{p.get('a')}/{p.get('b')} "
                                       f"{p.get('volume', 0):.6f}"
                                       for p in r.get("pairs", [])[:6]))
    finally:
        to_winding(app, m, rel)
    return info, bad


# --- declared clearances (v100) ------------------------------------------------------

# The gaps that decide whether it runs, DECLARED on the assembly: they survive
# the save, list_assertions shows them beside the model, and moving a part
# marks them stale without this script running. (key, key, min mm, mode, why)
CLEARANCES = [
    ("seconds hand to hour hand", "seconds_hand", "hour_hand", 0.04, "exact",
     "the hour hand sweeps over the register at six"),
    ("hour hand to minute hand", "hour_hand", "minute_hand", 0.12, "exact",
     "two blued hands, one stacked over the other"),
    ("minute hand to indices", "minute_hand", "indices", 0.20, "exact",
     "the minute hand passes over every applied baton"),
    ("minute hand to crystal", "minute_hand", "crystal", 0.30, "exact",
     "a crystal pressed by a thumb must not stop the watch"),
    ("escape wheel to third wheel", "escape", "third", 0.10, "exact",
     "they overlap in plan, one level apart"),
    ("fourth wheel to centre wheel", "fourth", "center", 0.20, "exact",
     "they overlap in plan, one level apart"),
    ("balance to balance cock", "balance", "cock_bridge", 0.05, "exact",
     "the balance swings under the cock that holds its pivot"),
    ("rotor to ratchet wheel", "rotor", "barrel_arbor", 0.15, "exact",
     "the rotor passes over the ratchet every turn"),
    ("rotor to caseback", "rotor", "caseback", 0.30, "mesh",
     "a flexed caseback must not brake the rotor"),
    ("crown to case", "stem", "case", 0.05, "exact",
     "the crown turns against the case tube"),
    ("clutch clear of the setting wheel", "clutch", "setting", 0.04, "exact",
     "crown in, the clutch's contrate teeth stand off the setting wheel"),
    ("setting wheel over the stem", "setting", "stem", 0.05, "exact",
     "the setting wheel runs over the stem's inner end"),
    ("winding wheel under the stem", "crown_wheel", "stem", 0.05, "exact",
     "the winding wheel runs under the stem, which turns over it"),
    ("keyless bridge to dial", "keyless_bridge", "dial", 0.10, "exact",
     "the keyless works live under the dial"),
]


def declare_clearances(app, occ):
    for name, a, b, mm, mode, why in CLEARANCES:
        app.add_assertion(name=name, a=occ[a].id, b=occ[b].id, min_mm=mm,
                          mode=mode, why=why)
    return len(CLEARANCES)


# --- verification ---------------------------------------------------------------------

def check_ratios(app, m):
    """Turn the barrel through one minute of the going train and read every
    arbor's dof back. The ratios are the watch; they must be exact."""
    minute_of_barrel = 360.0 / (Z_BARREL / Z_CENTER_PIN) / 60.0    # degrees
    app.call("set_mate", id=m["barrel"], rotation_deg=minute_of_barrel)
    app.solve_assembly()
    got = {x["id"]: x.get("rotation", 0.0) for x in app.list_mates()["mates"]}
    app.call("set_mate", id=m["barrel"], rotation_deg=0.0)
    app.solve_assembly()
    base = {x["id"]: x.get("rotation", 0.0) for x in app.list_mates()["mates"]}
    turn = {k: math.degrees(got[m[k]] - base[m[k]]) for k in
            ("center", "fourth", "escape", "hour")}
    want = {"center": -6.0, "fourth": -360.0, "escape": 12 * 360.0,
            "hour": -0.5}
    bad = [f"{k} turned {turn[k]:+.4f} deg in one minute, want {want[k]:+.1f}"
           for k in want if abs(turn[k] - want[k]) > 1e-6 * max(1, abs(want[k]))]
    return turn, bad


# Every mesh, as (driver key, driven key, the arbor whose mate drives the
# chain, teeth of the smaller gear in the pair, that gear's turns per turn of
# the chain's driver).
MESHES = [
    ("barrel", "center", "barrel", Z_CENTER_PIN, Z_BARREL / Z_CENTER_PIN),
    ("center", "third", "barrel", Z_THIRD_PIN,
     Z_BARREL / Z_CENTER_PIN * Z_CENTER / Z_THIRD_PIN),
    ("third", "fourth", "barrel", Z_FOURTH_PIN,
     Z_BARREL / Z_CENTER_PIN * 60),
    ("fourth", "escape", "barrel", Z_ESCAPE_PIN,
     Z_BARREL / Z_CENTER_PIN * 60 * Z_FOURTH / Z_ESCAPE_PIN),
    ("cannon", "minute", "barrel", Z_CANNON, Z_BARREL / Z_CENTER_PIN),
    ("minute", "hour", "barrel", Z_MINUTE_P,
     Z_BARREL / Z_CENTER_PIN * Z_CANNON / Z_MINUTE_W),
    ("rotor", "reduction", "rotor", Z_ROTOR_PIN, 1.0),
    ("reduction_pinion", "barrel_arbor", "rotor", Z_REDUCTION_P,
     Z_ROTOR_PIN / Z_REDUCTION_W),
    ("reduction", "reverser", "rotor", Z_REDUCTION_W,
     Z_ROTOR_PIN / Z_REDUCTION_W),
    ("reverser_pinion", "barrel_arbor", "rotor", Z_REDUCTION_P,
     Z_ROTOR_PIN / Z_REDUCTION_W),
    # The pawl over one ratchet tooth: it drops into the gap and rides out
    # over the tooth, and must touch neither.
    ("click", "barrel_arbor", "rotor", Z_RATCHET,
     Z_ROTOR_PIN / Z_REDUCTION_W * Z_REDUCTION_P / Z_RATCHET),
    ("barrel_arbor", "crown_wheel", "rotor", Z_RATCHET,
     Z_ROTOR_PIN / Z_REDUCTION_W * Z_REDUCTION_P / Z_RATCHET),
    ("minute", "setting", "barrel", Z_SETTING_W,
     Z_BARREL / Z_CENTER_PIN * Z_CANNON / Z_SETTING_W),
    # The keyless works, crown in: the stem drives them.
    ("stem", "clutch", "stem", Z_CONTRATE, 1.0),
    ("clutch", "winding_pinion", "stem", Z_CONTRATE, 1.0),
    ("winding_pinion", "crown_wheel", "stem", Z_CONTRATE, 1.0),
]


def check_meshes(app, occ, m, samples=9, meshes=None):
    """Every mesh, EXACT, through one pitch of its smaller gear.

    The whole-assembly motion study cannot do this: in one pitch of the
    escape pinion the barrel's mesh moves a ten-thousandth of a pitch, so the
    slow meshes are sampled at one phase and their interference is never
    seen. And its mesh-mode check reads the 3 um that backlash leaves between
    flanks as contact. So each pair is driven through ITS OWN pitch and asked
    with a boolean — the worst volume over the pitch is the answer.
    """
    worst = {}
    for a, b, driver, z_small, turns in (meshes or MESHES):
        pitch_deg = 360.0 / z_small / turns
        # Forward from where the driver stands, and LEFT there: turning one
        # back would drive the winding train backwards, which the reverser's
        # clutches hold against rather than unwind (generators/watch/README.md).
        start = math.degrees(_mates(app)[m[driver]].get("rotation", 0.0))
        v_max = 0.0
        for i in range(samples):
            app.call("set_mate", id=m[driver],
                     rotation_deg=start + pitch_deg * i / (samples - 1))
            app.solve_assembly()
            r = app.interference(a=occ[a].id, b=occ[b].id, mode="exact")
            v_max = max(v_max, r.get("volume", 0.0) or 0.0)
        worst[f"{occ[a].name} / {occ[b].name}"] = v_max
    app.solve_assembly()
    return worst


def mesh_cache_key(app):
    """What a mesh's result depends on: the tooth geometry and the phases
    (this generator's four files) and the kernel that swept them (the app's
    source, by git revision and uncommitted diff under src/). None when the
    key cannot vouch for the app — its binary was rebuilt under the running
    instance, or there is no git to ask."""
    if app.call("ping").get("binary_replaced"):
        return None
    h = hashlib.sha256()
    for f in ("watch_spec.py", "watch_gearing.py", "watch_parts.py",
              "watch.py"):
        with open(os.path.join(HERE, f), "rb") as fh:
            h.update(fh.read())
    root = os.path.join(HERE, "..", "..")
    try:
        for cmd in (["rev-parse", "HEAD"], ["diff", "HEAD", "--", "src"]):
            h.update(subprocess.run(["git", "-C", root] + cmd,
                                    capture_output=True, check=True).stdout)
    except (OSError, subprocess.CalledProcessError):
        return None
    return h.hexdigest()


def cached_meshes(app, occ, m, path):
    """check_meshes, unless nothing it depends on has changed since a CLEAN
    result was kept (80% of a full run is these sweeps, and the interference
    RPC cannot be narrowed to one body — its mesh mode reads the gears'
    backlash as contact). Only an all-zero result is ever kept. Returns the
    result and when it was checked, or None for a fresh sweep."""
    key = mesh_cache_key(app) if path else None
    if key and os.path.exists(path):
        try:
            with open(path) as fh:
                kept = json.load(fh)
        except (OSError, ValueError):
            kept = {}
        worst = kept.get("worst") or {}
        if kept.get("key") == key and worst and \
                all(v == 0.0 for v in worst.values()):
            return worst, kept.get("checked")
    worst = check_meshes(app, occ, m)
    if key and worst and all(v == 0.0 for v in worst.values()):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as fh:
            json.dump({"key": key, "checked": time.strftime("%Y-%m-%d %H:%M"),
                       "worst": worst}, fh, indent=1)
    return worst, None


def verify(app, occ, m, rel, cache=None):
    print("checks:")
    bad = []
    with phase("ratios"):
        turn, why = check_ratios(app, m)
    bad += why
    with phase("meshes"):
        worst, since = cached_meshes(app, occ, m, cache)
    if since:
        print(f"  meshes: nothing they depend on changed since {since} — "
              f"that clean sweep stands (--recheck to sweep again)")
    for pair, v in worst.items():
        print(f"  mesh {pair:<36} worst {v:.7f} mm3 over a pitch")
        if v > 0.0:
            bad.append(f"the {pair} mesh interferes: {v:.7f} mm3")
    print("  one minute: " + ", ".join(f"{k} {v:+.3f} deg"
                                        for k, v in turn.items()))

    with phase("clash sweep"):
        r = app.interference(mode="exact", deadline_s=300, max_pairs=20000,
                             _timeout=600)
    standing = r["receipt"]["standing"]
    print(f"  interference: {len(r.get('pairs', []))} clash pair(s), "
          f"receipt {standing}")
    if standing != "verified":
        bad.append(f"the clash sweep is {standing}: {r['receipt']['summary']}")
    for p in r.get("pairs", [])[:12]:
        bad.append(f"clash {p['a']} <-> {p['b']} {p.get('volume', 0):.4f} mm3")

    with phase("keyless"):
        wind, why = check_winding(app, m)
        bad += why
        rot, why = check_rotor(app, m)
        bad += why
        dog, why = check_dogs(app, m)
        bad += why
        sett, why = check_setting(app, occ, m, rel)
        bad += why
        # Last, so nothing above leaves the watch sitting with its clutch
        # held out of mesh by the dogs it rode over.
        seat_dogs(app, m, rel)
    print(f"  crown in, a quarter turn winds the ratchet "
          f"{wind['barrel_arbor']:+.3f} deg; centre {wind['center']:+.4f}, "
          f"rotor {wind['rotor']:+.4f}")
    print(f"  the rotor 30 deg forward winds the ratchet "
          f"{rot['forward']:+.4f} deg, 60 deg back {rot['back']:+.4f}; "
          f"the crown stays {rot['crown']:+.4f}")
    print(f"  the crown turned back rides the dogs over: the ratchet moves "
          f"{dog['ratchet']:+.4f} deg and the clutch lifts "
          f"{dog['lift']:+.4f} mm")
    for pair, v in sett.get("meshes", {}).items():
        print(f"  mesh {pair:<36} worst {v:.7f} mm3 over a pitch (crown out)")
    print(f"  crown out, the clutch slid {sett.get('clutch offset', 0):+.3f}; "
          f"a quarter turn turns the minute hand {sett.get('cannon', 0):+.3f} "
          f"deg and the centre wheel {sett.get('center', 0):+.4f}; sweep "
          f"{sett.get('sweep')}")

    with phase("clearances"):
        c = app.verify_assertions(_timeout=900)
    for row in c["assertions"]:
        print(f"  {row['name']:<30} {row.get('measured_mm', 0):7.3f} mm "
              f"(min {row['min_mm']}) {row['standing']}")
        if row["standing"] != "holds":
            bad.append(f"{row['name']}: {row['standing']} "
                       f"{row.get('note', '')}".rstrip())
    print(f"  clearances: {c['receipt']['standing'].upper()}")

    with phase("motion study"):
        # One pitch of the escape pinion, the fastest mesh in the train: the
        # escape wheel turns 60 deg while the barrel turns 60/5760 of that.
        ratio = (Z_BARREL / Z_CENTER_PIN) * 60 * (Z_FOURTH / Z_ESCAPE_PIN)
        study = app.add_motion_study(driver=m["barrel"], dof="rotation",
                                     from_=0.0, to=360.0 / Z_ESCAPE_PIN / ratio,
                                     samples=13, check_clash=True,
                                     clash_mode="mesh", name="One escape pitch")
        run = app.run_motion_study(study=study["id"], _timeout=1800)
    appeared = run.get("appeared", [])
    unsolved = sum(1 for s in run.get("samples", []) if not s.get("solved"))
    print(f"  motion: {len(run.get('samples', []))} samples, "
          f"{run.get('seated_contacts', 0)} seated contacts, "
          f"{len(appeared)} new, {unsolved} unsolved, receipt "
          f"{run['receipt']['standing']}")
    for pair in appeared:
        bad.append(f"turning the train brings {pair['a_name']} into "
                   f"{pair['b_name']}")
    if unsolved:
        bad.append(f"{unsolved} motion samples did not solve")

    with phase("escapement"):
        esc, why = check_escapement(app, m)
        bad += why
        # The pin's passage through the notch, where the fork turns 13
        # degrees in about 1.5 of the wheel: a tenth of a degree a sample.
        c = esc["centre"]
        study = app.add_motion_study(driver=m["escape"], dof="rotation",
                                     from_=c - 1.0, to=c + 1.0, samples=21,
                                     check_clash=True, clash_mode="mesh",
                                     name="Impulse")
        run = app.run_motion_study(study=study["id"], _timeout=1800)
    print(f"  escapement: the fork swings +/-{esc['swing'] / 2:.2f} deg, balance "
          f"and fork within {esc['worst']:.3f} deg of their tables; impulse "
          f"{len(run.get('samples', []))} samples, "
          f"{len(run.get('appeared', []))} new contacts, receipt "
          f"{run['receipt']['standing']}")
    for pair in run.get("appeared", []):
        bad.append(f"the pin's passage brings {pair['a_name']} into "
                   f"{pair['b_name']}")
    if any(not smp.get("solved") for smp in run.get("samples", [])):
        bad.append("impulse samples did not solve")
    if bad:
        raise SdkError("VERIFICATION FAILED:\n  " + "\n  ".join(bad))


def set_time(app, m, h, mi, s):
    """Pose the train at a time of day (the barrel at zero reads 12:00:00)."""
    minutes = (h % 12) * 60 + mi + s / 60.0
    # Forward time turns the centre wheel clockwise seen from the dial (-Z),
    # so the barrel, which meshes it, turns positive.
    app.call("set_mate", id=m["barrel"],
             rotation_deg=minutes / 60.0 * 360.0 / (Z_BARREL / Z_CENTER_PIN))
    app.solve_assembly()


def renders(app, occ, out):
    """The stills, each naming what it hides. Glass is see-through in a still
    (docs/rendering/materials.md#glass), so the crystal and the caseback window
    stay in; the movement shot takes the case, the caseback and the rotor
    off, as a watchmaker would, and the keyless shot the dial and hands."""
    os.makedirs(out, exist_ok=True)
    shots = (("dial", dict(eye=[10.0, -44.0, 70.0], target=[0.0, -1.0, 0.0]),
              ()),
             ("case", dict(eye=[62.0, -48.0, 24.0], target=[0.0, 0.0, -3.5]),
              ()),
             ("caseback", dict(eye=[-16.0, -40.0, -66.0],
                               target=[0.0, 0.0, -6.0]), ()),
             ("movement", dict(eye=[-9.0, -23.0, -38.0],
                               target=[0.0, 0.0, -4.0]),
              ("caseback", "window", "rotor", "case", "bezel")),
             # Under the dial: motion works and keyless works, crown in.
             ("keyless", dict(eye=[20.0, -24.0, 26.0], target=[6.0, 0.0, 0.0]),
              ("crystal", "bezel", "case", "dial", "indices", "hour_hand",
               "minute_hand", "seconds_hand", "keyless_bridge")))
    for name, cam, hidden in shots:
        for k in hidden:
            app.call("set_occurrence_visible", id=occ[k].id, visible=False)
        app.set_camera(up=[0.0, 0.0, 1.0], **cam)
        app.render_still(path=os.path.join(out, f"{name}.png"), width=1600,
                         height=1200, samples=256, _timeout=900)
        for k in hidden:
            app.call("set_occurrence_visible", id=occ[k].id, visible=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int)
    ap.add_argument("--out", default=os.path.join(HERE, "..", "..", "scratch",
                                                  "watch"))
    ap.add_argument("--stage", choices=("parts", "assembly", "all"),
                    default="all")
    ap.add_argument("--recheck", action="store_true",
                    help="sweep every mesh even if a clean result is kept")
    args = ap.parse_args()
    app = connect(args.port)
    app.call("new_project")
    dims = spec.declare(app)
    globals().update(dims)
    W.__dict__.update(dims)

    with phase("parts"):
        P = build_parts(app)
    bad = check_parts(app, P)
    print(f"parts: {len(P)} built, {len(bad)} not verified")
    for b in bad:
        print("  " + b)
    if bad:
        raise SdkError("parts did not build clean")
    if args.stage == "parts":
        report_phases()
        return

    with phase("assembly"):
        asm, occ, m, rel, solved = build_assembly(app, P)
    print(f"assembly: {len(occ)} occurrences, {len(m)} revolute mates, "
          f"{len(rel)} gear relations; {solved.get('placed_by_mates')} of "
          f"{solved.get('occurrences')} placed by mates, "
          f"{solved.get('placed_by_transform')} by transform, "
          f"errored mates {solved.get('errored_mates')}")
    print(f"  {declare_clearances(app, occ)} clearances declared")
    verify(app, occ, m, rel, cache=None if args.recheck else
           os.path.join(args.out, "mesh-cache.json"))
    set_time(app, m, 10, 8, 37)
    os.makedirs(args.out, exist_ok=True)
    app.save_project(path=os.path.join(args.out, "watch.dsgn"))
    if args.stage == "all":
        with phase("renders"):
            renders(app, occ, args.out)
    print("saved")
    report_phases()


if __name__ == "__main__":
    main()
