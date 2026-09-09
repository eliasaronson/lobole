#!/usr/bin/env python3
"""A 5-inch FPV freestyle quadcopter, modelled to be built.

    python3 generators/fpv_drone.py [--port N] [--stage parts|assembly|all]
                                    [--out scratch/fpv-drone]

Five carbon plates and an arm are the parts you have cut; everything else
is either hardware from a standard (the fasteners come from the vendored
ISO tables) or a bought component modelled at its catalogue dimensions so
that the clearances mean something.

What the run asserts before it claims anything (see `verify`):
  - every part builds as one solid, and the B-Rep is valid;
  - the propeller discs clear each other, the top plate and the battery;
  - nothing in the assembly interferes, exactly, not by mesh;
  - the props still clear everything through a full turn of the motors;
  - every fastener reaches the thread it is supposed to reach.
"""
import argparse
import datetime
import json
import math
import os
import shutil
import sys
import time
from pathlib import Path

TODAY = datetime.date.today().isoformat()

# Where a run's time goes, printed at the end. A generator that takes a
# minute gets run a hundred times while it is being written, so WHICH minute
# it is decides what is worth making faster.
PHASES = []


class phase:
    def __init__(self, name):
        self.name = name

    def __enter__(self):
        self.at = time.perf_counter()
        return self

    def __exit__(self, *exc):
        PHASES.append((self.name, time.perf_counter() - self.at))


def report_phases():
    total = sum(d for _, d in PHASES) or 1.0
    print(f"time: {total:.1f} s")
    for name, seconds in PHASES:
        print(f"  {name:<18} {seconds:6.1f} s {100 * seconds / total:4.0f}%")

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))

from designer_sdk import SdkError, connect  # noqa: E402

import fpv_drone_parts as P  # noqa: E402
from fpv_drone_spec import *  # noqa: F401,F403,E402


def quat_z(deg):
    a = math.radians(deg) / 2.0
    return [0.0, 0.0, math.sin(a), math.cos(a)]


def quat_y(deg):
    a = math.radians(deg) / 2.0
    return [0.0, math.sin(a), 0.0, math.cos(a)]


def quat_x(deg):
    a = math.radians(deg) / 2.0
    return [math.sin(a), 0.0, 0.0, math.cos(a)]


def place(asm, part, position=(0, 0, 0), orientation=None, name=None,
          fixed=False):
    """add_occurrence with an orientation — the SDK's place() takes only a
    position, and every arm here is a rotation about Z."""
    asm.activate()
    kw = {"part": part.name if hasattr(part, "name") else part,
          "fixed": fixed,
          "transform": {"position": [float(v) for v in position],
                        "orientation": orientation or [0, 0, 0, 1]}}
    if name:
        kw["name"] = name
    r = asm.app.add_occurrence(**kw)
    from designer_sdk import Occurrence
    return Occurrence(asm, dict(r, name=r.get("name", name or part.name)))


# --- the parts --------------------------------------------------------------

def build_parts(app):
    print("parts:")
    parts = {}

    def add(key, fn, *a):
        part = fn(app, *a)
        parts[key] = part
        lo, hi = part.bbox()
        print(f"  {part.name:<28} {hi[0]-lo[0]:6.1f} x {hi[1]-lo[1]:6.1f} x "
              f"{hi[2]-lo[2]:6.1f} mm")
        return part

    add("bottom", P.bottom_plate)
    add("mid", P.mid_plate)
    add("top", P.top_plate)
    add("arm", P.arm)
    add("camplate", P.camera_plate)
    add("standoff", P.standoff)
    parts["spacer_lo"] = P.spacer(app, f"Nylon spacer M3x{STACK_LOWER_SPACER:.0f}",
                                  STACK_SPACER_D, STACK_LOWER_SPACER, M3_CLEAR)
    parts["spacer_hi"] = P.spacer(app, f"Nylon spacer M3x{STACK_UPPER_SPACER:.0f}",
                                  STACK_SPACER_D, STACK_UPPER_SPACER, M3_CLEAR)
    parts["spacer_vtx"] = P.spacer(app, "Nylon spacer M2x5", 4.0, 5.0, M2_CLEAR)
    parts["cage_spacer"] = P.tapped_spacer(
        app, "Cage spacer", CAGE_SPACER_D, CAM_MOUNT_GAP, CAGE_SCREW, 1.6,
        CAGE_TAP_DEPTH)
    add("antmount", P.antenna_mount)
    add("battpad", P.battery_pad)
    add("fc", P.flight_controller)
    add("esc", P.esc)
    add("vtx", P.vtx)
    add("camera", P.camera)
    add("rx", P.receiver)
    add("battery", P.battery)
    add("cap", P.capacitor)
    add("buzzer", P.buzzer)
    add("antenna", P.antenna)
    add("motor", P.motor)
    add("propnut", P.prop_nut)
    add("prop_cw", P.propeller, "CW")
    add("prop_ccw", P.propeller, "CCW")
    return parts


# --- the assembly -----------------------------------------------------------

def build_assembly(app, parts):
    print("assembly:")
    asm = app.assembly("Quadcopter")
    occ = {}

    occ["bottom"] = place(asm, parts["bottom"], (0, 0, Z_BOTTOM), fixed=True,
                          name="Bottom plate")
    occ["mid"] = place(asm, parts["mid"], (0, 0, Z_MID), name="Mid plate")
    occ["top"] = place(asm, parts["top"], (0, 0, Z_TOP), name="Top plate")

    for k in range(4):
        occ[f"arm{k}"] = place(asm, parts["arm"], (0, 0, Z_ARM),
                               quat_z(ARM_DIRS[k]), name=ARM_NAMES[k])
        x, y = diag(MOTOR_R, k)
        # Turned with its arm: the motor's 16 x 16 bolt square has to line
        # up with the four holes in the arm, and the arm is at 45 degrees.
        occ[f"motor{k}"] = place(asm, parts["motor"], (x, y, Z_MOTOR),
                                 quat_z(ARM_DIRS[k]), name=MOTOR_NAMES[k])
        prop = parts["prop_cw" if PROP_HAND[k] == "CW" else "prop_ccw"]
        occ[f"prop{k}"] = place(asm, prop, (x, y, Z_PROP),
                                name=f"Propeller {ARM_NAMES[k][-2:]}")
        occ[f"nut{k}"] = place(asm, parts["propnut"],
                               (x, y, Z_PROP + PROP_HUB_H),
                               name=f"Prop nut {ARM_NAMES[k][-2:]}")
        occ[f"standoff{k}"] = place(asm, parts["standoff"],
                                    diag(ARM_BOLT_OUT, k) + (Z_MID_TOP,),
                                    name=f"Standoff {ARM_NAMES[k][-2:]}")

    # Camera cage: two plates of the same part, 19.5 mm apart.
    inner = CAM_MOUNT_GAP / 2.0
    occ["camplate_l"] = place(asm, parts["camplate"], (0, inner + T_SIDE, 0),
                              name="Camera plate L")
    occ["camplate_r"] = place(asm, parts["camplate"], (0, -inner, 0),
                              name="Camera plate R")
    occ["cage_spacer"] = place(asm, parts["cage_spacer"],
                               (CAGE_LOCK[0], -inner, CAGE_LOCK[1]),
                               quat_x(-90), name="Cage spacer")
    occ["camera"] = place(asm, parts["camera"],
                          (CAM_PIVOT[0], 0, CAM_PIVOT[1]),
                          quat_y(-CAM_TILT_DEG), name="FPV camera")

    # The stack, standing through the mid plate's opening.
    for i, (x, y) in enumerate(P.stack_points()):
        occ[f"sp_lo{i}"] = place(asm, parts["spacer_lo"], (x, y, Z_ARM),
                                 name=f"Lower spacer {i + 1}")
        occ[f"sp_hi{i}"] = place(asm, parts["spacer_hi"],
                                 (x, y, Z_ESC + ESC_PCB_T),
                                 name=f"Upper spacer {i + 1}")
    occ["esc"] = place(asm, parts["esc"], (0, 0, Z_ESC), name="ESC")
    occ["fc"] = place(asm, parts["fc"], (0, 0, Z_FC), name="Flight controller")

    # Video and radio, on the mid plate behind the stack.
    for i, (sx, sy) in enumerate(((1, 1), (1, -1), (-1, 1), (-1, -1))):
        occ[f"sp_vtx{i}"] = place(
            asm, parts["spacer_vtx"],
            (VTX_AT[0] + sx * VTX_PATTERN / 2, VTX_AT[1] + sy * VTX_PATTERN / 2,
             Z_MID_TOP), name=f"VTX spacer {i + 1}")
    occ["vtx"] = place(asm, parts["vtx"], (VTX_AT[0], VTX_AT[1], Z_MID_TOP + 5.0),
                       name="VTX")
    occ["rx"] = place(asm, parts["rx"], (RX_AT[0], RX_AT[1], Z_MID_TOP),
                      name="Receiver")
    occ["cap"] = place(asm, parts["cap"], (CAP_AT[0], CAP_AT[1],
                                           Z_MID_TOP + CAP_D / 2),
                       name="Capacitor")
    occ["buzzer"] = place(asm, parts["buzzer"], (BUZZER_AT[0], BUZZER_AT[1],
                                                 Z_MID_TOP), name="Buzzer")

    # Battery deck.
    occ["battpad"] = place(asm, parts["battpad"], (BATT_X, 0, Z_TOP_TOP),
                           name="Battery pad")
    occ["battery"] = place(asm, parts["battery"], (BATT_X, 0, Z_TOP_TOP + 3.0),
                           name="Battery")
    occ["antmount"] = place(asm, parts["antmount"], (ANT_MOUNT_X, 0, Z_MID_TOP),
                            name="Antenna mount")
    sma = ant_sma_origin()
    occ["antenna"] = place(
        asm, parts["antenna"],
        (ANT_MOUNT_X + sma[0], sma[1], Z_MID_TOP + sma[2]),
        quat_y(-ANT_MOUNT_ANGLE), name="Antenna")
    print(f"  {len(occ)} occurrences placed")
    return asm, occ


# --- the fasteners ----------------------------------------------------------

def rim_at(part, point, radius, tol=0.35):
    """The circular edge of `radius` whose centre is at `point`, in the
    part's own space. A pick, made by measuring rather than by naming an
    index that the next feature would renumber."""
    # Cheap pre-filter, so only the plausible rims cost a frame query: a
    # point ON the circle is exactly `radius` from its centre, whichever way
    # the hole is drilled. (Comparing z only works for holes drilled DOWN,
    # and the camera plate's are drilled along Y.)
    near = [e for e in part.edges(circular=True, radius=radius, tol=0.06)
            if e.midpoint is None
            or abs(math.dist(tuple(e.midpoint), tuple(point)) - radius) < tol]
    hits = [e for e in near
            if all(abs(e.center[i] - point[i]) < tol for i in range(3))]
    if len(hits) != 1:
        raise SdkError(f"{part.name}: {len(hits)} rims of r{radius} at "
                       f"{point} — expected exactly one")
    return hits[0]


def far(part, point, radius, occurrence):
    """A `far_end` pick: the rim of the same bore where the joint stops.

    It has to be ON the screw's axis — a plate's whole top face is not,
    its centroid being wherever the plate's material happens to balance,
    so the pick is the far rim of the very hole the screw is in.

    Without one, a screw is sized to everything its axis crosses on the
    way out of the aircraft: the stack screws would be sized through the
    top plate, 25 mm above the board they actually clamp.
    """
    return dict(rim_at(part, point, radius).ref, occurrence=_oid(occurrence))


def _oid(o):
    return o.id if hasattr(o, "id") else o


def add_fasteners(asm, parts, occ):
    """Every screw in the aircraft, derived from the hole it goes in.

    `fit_fastener` reads the size out of the bore and the length out of the
    stack the axis crosses, and mates what the screw clamps — so the frame
    is held together by its own hardware rather than by mates that agree
    with the hardware.
    """
    print("fasteners:")
    made = []

    def bolt(rim, where, **kw):
        r = asm.bolt(rim, where, **kw)
        made.append((r["size"], r["length_mm"], r["summary"]))
        return r

    # Arm bolts: up from under the bottom plate, through arm and mid plate.
    # The outer one runs on into the standoff's thread; the inner one takes
    # a nut, because there is nothing above it to thread into.
    for k in range(4):
        for radius in (ARM_BOLT_IN, ARM_BOLT_OUT):
            x, y = diag(radius, k)
            rim = rim_at(parts["bottom"], (x, y, 0.0), M3_CLEAR / 2)
            if radius == ARM_BOLT_OUT:
                # ... on into the standoff's lower thread, and no further:
                # the pick is the floor of that tapped hole.
                bolt(rim, occ["bottom"],
                     far_end=far(parts["standoff"],
                                 (0.0, 0.0, STANDOFF_THREAD_DEPTH), 1.25,
                                 occ[f"standoff{k}"]))
            else:
                bolt(rim, occ["bottom"], nut="iso4032",
                     far_end=far(parts["mid"], (x, y, T_MID), M3_CLEAR / 2,
                                 occ["mid"]))

    # Top plate down into the same standoffs.
    for k in range(4):
        x, y = diag(ARM_BOLT_OUT, k)
        bolt(rim_at(parts["top"], (x, y, T_TOP), M3_CLEAR / 2), occ["top"],
             far_end=far(parts["standoff"],
                         (0.0, 0.0, STANDOFF_LEN - STANDOFF_THREAD_DEPTH),
                         1.25, occ[f"standoff{k}"]))

    # Motors: up through the arm into the motor's own tapped base.
    for k in range(4):
        for sx in (1, -1):
            for sy in (1, -1):
                pt = (MOTOR_R + sx * MOTOR_BOLT_SQ / 2,
                      sy * MOTOR_BOLT_SQ / 2, 0.0)
                bolt(rim_at(parts["arm"], pt, M3_CLEAR / 2), occ[f"arm{k}"],
                     far_end=far(parts["motor"],
                                 (sx * MOTOR_BOLT_SQ / 2,
                                  sy * MOTOR_BOLT_SQ / 2, MOTOR_BOLT_DEPTH),
                                 1.25, occ[f"motor{k}"]))

    # The stack, on its own screws through both boards.
    for x, y in P.stack_points():
        bolt(rim_at(parts["bottom"], (x, y, 0.0), M3_CLEAR / 2), occ["bottom"],
             nut="iso4032",
             far_end=far(parts["fc"], (x, y, FC_PCB_T), STACK_HOLE_D / 2,
                         occ["fc"]))

    # The camera cage: one transverse screw locks both plates to the spacer,
    # and the camera hangs on its M2 tilt screw.
    x, z = CAGE_LOCK
    for local_y, side, tap_z in ((-T_SIDE, "camplate_r", CAGE_TAP_DEPTH),
                                 (0.0, "camplate_l",
                                  CAM_MOUNT_GAP - CAGE_TAP_DEPTH)):
        bolt(rim_at(parts["camplate"], (x, local_y, z), M2_CLEAR / 2),
             occ[side],
             far_end=far(parts["cage_spacer"], (0.0, 0.0, tap_z), 0.8,
                         occ["cage_spacer"]))
    # The camera's own two M2s, one in from each plate.
    x, z = CAM_PIVOT
    for sy, side, local_y in ((-1, "camplate_r", -T_SIDE), (1, "camplate_l", 0.0)):
        bolt(rim_at(parts["camplate"], (x, local_y, z), M2_CLEAR / 2),
             occ[side],
             far_end=far(parts["camera"],
                         (0.0, sy * (CAM_BODY / 2 - 4.0), 0.0), 0.8,
                         occ["camera"]))

    # VTX, down through its spacers into a nut under the mid plate.
    for sx in (1, -1):
        for sy in (1, -1):
            pt = (sx * VTX_PATTERN / 2, sy * VTX_PATTERN / 2,
                  VTX_PCB_T + VTX_TOP_H)
            try:
                rim = rim_at(parts["vtx"], (pt[0], pt[1], VTX_PCB_T),
                             VTX_HOLE_D / 2)
            except SdkError:
                rim = rim_at(parts["vtx"], (pt[0], pt[1], 0.0), VTX_HOLE_D / 2)
            bolt(rim, occ["vtx"], nut="iso4032",
                 far_end=far(parts["mid"],
                             (VTX_AT[0] + sx * VTX_PATTERN / 2,
                              VTX_AT[1] + sy * VTX_PATTERN / 2, 0.0),
                             M2_CLEAR / 2, occ["mid"]))

    # The antenna mount, bolted down to the mid plate's tail.
    for sy in (1, -1):
        y = sy * ANT_MOUNT_PITCH / 2
        bolt(rim_at(parts["antmount"], (0.0, y, ANT_BASE_T), 3.4 / 2),
             occ["antmount"], nut="iso4032",
             far_end=far(parts["mid"], (ANT_MOUNT_X, y, 0.0), M3_CLEAR / 2,
                         occ["mid"]))

    for size, length, summary in made:
        print(f"  {size}x{length:<5.0f} {summary}")
    return made


# --- what the bolts do not hold --------------------------------------------

def add_mates(asm, occ):
    """The propellers TURN, so they are revolute; everything a screw does
    not already hold is fastened to what it sits on.

    The four propellers are coupled by relations rather than driven one at a
    time: diagonal pairs turn together, adjacent ones opposite, which is
    what makes one motion study a check on the whole disc.
    """
    print("mates:")
    props = []
    for k in range(4):
        # flipped=False throughout: `add_mate` defaults to flipping, which
        # is right for two FACES meeting (their normals oppose) and wrong
        # for a seat frame, which is the same frame on both parts. Left at
        # the default every seated part turned over and sank into what it
        # was sitting on — the battery a full 42 mm into the top plate.
        props.append(asm.revolute((occ[f"prop{k}"], "Seat"),
                                  (occ[f"motor{k}"], "Prop seat"),
                                  flipped=False))
        asm.fasten((occ[f"nut{k}"], "Seat"), (occ[f"prop{k}"], "Nut seat"),
                   flipped=False)
    for k in (1, 2, 3):
        same = PROP_HAND[k] == PROP_HAND[0]
        # A chain keeps the sign (same way round), a gear reverses it.
        (asm.chain if same else asm.gear)(props[0], props[k], 1, 1)

    for a, b in ((("battpad", "Origin"), ("top", "Battery seat")),
                 (("battery", "Origin"), ("battpad", "Pack seat")),
                 (("rx", "Origin"), ("mid", "RX seat")),
                 (("cap", "Origin"), ("mid", "Cap seat")),
                 (("buzzer", "Origin"), ("mid", "Buzzer seat")),
                 (("antenna", "Face"), ("antmount", "SMA"))):
        asm.fasten((occ[a[0]], a[1]), (occ[b[0]], b[1]), flipped=False)

    solved = asm.solve()
    print(f"  {len(asm.app.list_mates()['mates'])} mates, "
          f"dof {solved.get('dof')}, loops {solved.get('loops_closed')}")
    return props


# --- what the model is checked against --------------------------------------

RESIDUE_MM3 = 0.01   # coincident thread surfaces: a cube 0.2 mm on a side


def _clearances(gap):
    """The clearances that decide whether it flies or eats itself."""
    # The DIAGONAL pair is not measured. It cannot touch — its motors are
    # 220 mm apart against 155.6 for the adjacent pair, and the adjacent one
    # is measured — and asking cost 55 s of a 100 s run: an exact distance
    # between two lofted propellers that far apart prunes to nothing
    # (docs/modeling/fpv-drone.md). The motion study still checks every pair.
    return {
        "prop to prop (adjacent)": gap("Propeller FL", "Propeller FR"),
        "prop to top plate": gap("Propeller FL", "Top plate"),
        "prop to battery": gap("Propeller RL", "Battery"),
        "prop to antenna": gap("Propeller RL", "Antenna"),
        "prop to camera plate": gap("Propeller FL", "Camera plate L"),
        "battery to camera": gap("Battery", "FPV camera"),
    }


def verify(app, asm, parts, occ, props):
    """Every claim this model makes, asked of the model."""
    print("checks:")
    bad = []

    with phase("clash sweep"):
        r = app.interference(mode="exact", deadline_s=240, max_pairs=8000,
                             _timeout=400)
    if r.get("truncated"):
        bad.append(f"the interference sweep was truncated: {r['truncation']}")
    over = [p for p in r.get("pairs", []) if p.get("volume", 0) > RESIDUE_MM3]
    print(f"  interference: {len(r.get('pairs', []))} contacts, "
          f"{len(over)} above {RESIDUE_MM3} mm3 of thread-surface residue")
    for p in over:
        bad.append(f"clash {p['a']} <-> {p['b']} {p['volume']:.2f} mm3")

    with phase("fastener check"):
        findings = app.check_fasteners().get("findings", [])
    print(f"  fasteners: {len(findings)} findings")
    for f in findings:
        bad.append(f"fastener {f.get('part')}: {f.get('why')}")

    # The clearances that decide whether it flies or eats itself.
    def gap(a, b):
        started = time.perf_counter()
        # A minute is not enough: the exact minimum distance between two
        # LOFTED propellers is a spline-to-spline extrema, and it runs for
        # tens of seconds (occasionally past the client's default timeout)
        # where the same call between two plates is instant.
        d = app.measure(a={"occurrence": a}, b={"occurrence": b},
                        distance_kind="minimum", _timeout=600)["distance"]
        PHASES.append((f"  gap {a[:9]}/{b[:9]}", time.perf_counter() - started))
        return d

    with phase("clearances"):
        clear = _clearances(gap)
    for what, mm in clear.items():
        print(f"  {what:<26} {mm:7.2f} mm")
        if mm < 2.0:
            bad.append(f"{what} is only {mm:.2f} mm")
    # ... and the same question asked THROUGH a turn of the motors, which is
    # the only way a three-blade propeller's clearance is actually decided.
    study = app.add_motion_study(
        driver=props[0]["id"], dof="rotation", from_=0.0,
        to=360.0 / PROP_BLADES, samples=24, check_clash=True,
        clash_mode="mesh", name="Propellers turning")
    with phase("motion study"):
        run = app.run_motion_study(study=study["id"], _timeout=900)
    samples = run.get("samples") or []
    # `appeared` is what TURNING changed — the app subtracts the contacts
    # that were already there at rest, which on a working machine is most of
    # them (a propeller touches its motor, its nut and four screws).
    unsolved = sum(1 for sample in samples if not sample.get("solved"))
    appeared = run.get("appeared", [])
    print(f"  motion: {len(samples)} samples over "
          f"{360.0 / PROP_BLADES:.0f} deg, "
          f"{run.get('seated_contacts', 0)} seated contacts, "
          f"{len(appeared)} new, {unsolved} unsolved")
    for pair in appeared:
        bad.append(f"turning the propellers brings {pair['a_name']} "
                   f"into {pair['b_name']}")
    if unsolved:
        bad.append(f"{unsolved} motion samples did not solve")
    if bad:
        raise SdkError("VERIFICATION FAILED:\n  " + "\n  ".join(bad))
    return clear


# --- what you actually build from -------------------------------------------

# (part, view, sheet, quantity, material, thickness) — the five carbon parts
# and the two printed ones. Everything else is bought.
SHEETS = [
    ("Bottom plate", "top", "a3", 1, "3K carbon plate", T_BOTTOM),
    ("Mid plate", "top", "a3", 1, "3K carbon plate", T_MID),
    ("Top plate", "top", "a3", 1, "3K carbon plate", T_TOP),
    ("Arm", "top", "a3", 4, "3K carbon plate", T_ARM),
    ("Camera plate", "front", "a4", 2, "3K carbon plate", T_SIDE),
]


SHEET_MM = {"a4": (297.0, 210.0), "a3": (420.0, 297.0), "a2": (594.0, 420.0)}
GA_SCALE = 0.5    # a PREFERRED scale (1:2). A drawing scale is one of a
                  # short list; "1:1.33" is not on it, whatever fits best.
GA_GAP = 18.0     # between the top view and the front view under it
GA_SHEET = "a3"   # ... on the smallest sheet that holds it at that scale


def _extremes(geometry, axis):
    """The outermost two things to dimension between along `axis`.

    Straight edges square to the axis where there are any — and a circle's
    centre where there are not, because an arm has no square edges at all:
    its sides taper into a round motor pad, and the dimension that matters
    there is where the motor SITS.
    """
    at = []
    for g in geometry:
        if not g.get("visible"):
            continue
        if g["type"] == "line" and abs(g["p1"][1 - axis]
                                       - g["p2"][1 - axis]) < 1e-6:
            at.append((g["p1"][1 - axis], g))
        elif (g["type"] == "circle" and g.get("closed")
              and (g.get("radius") or 0) <= 5.0):
            at.append((g["center"][1 - axis], g))
    at.sort(key=lambda p: p[0])
    if len(at) < 2:
        raise SdkError("nothing to dimension between on this view")
    return at[0][1], at[-1][1]


def note(app, at, lines):
    """A block of text as ONE note.

    It used to be one note per line: a newline was neither measured (the
    validator boxed the whole string as a single line, so any multi-line
    note read as running off the sheet) nor rendered (the SVG wrote one
    <text> element, and SVG ignores newlines). Both fixed, so this is a
    note again.
    """
    app.add_drawing_note(position=[at[0], at[1]],
                         text="\n".join(line for line in lines if line))


def hole_table(part, axes=(0, 1)):
    """Every bore in the part, by size and centre, from the model.

    A drawing is what a shop cuts from; this is what it types in. Read off
    the geometry rather than off the spec, so a hole that moved and a table
    that did not cannot disagree.

    `axes` are the two the sheet's view shows. Keying on those two and not
    on all three is what makes a through hole ONE row: it has a rim at each
    end, and keeping both listed every hole in the frame twice.
    """
    bores = {}
    # CIRCLES, not arcs: a filleted outside corner is an arc of exactly the
    # radius a small bore would be, and counting those listed a camera
    # plate's nine corner radii as holes to drill.
    for edge in part.edges(kind="circle"):
        r = round(edge.radius or 0, 2)
        if r > 5.0:
            continue  # a corner radius or a lightening opening, not a hole
        centre = edge.center
        key = tuple(round(centre[a], 2) for a in axes)
        bores.setdefault(2 * r, set()).add(key)
    lines = []
    for diameter, centres in sorted(bores.items()):
        lines.append(f"{len(centres)} x \u00f8{diameter:g}")
        for u, v in sorted(centres):
            lines.append(f"   {u:8.2f} {v:8.2f}")
    return lines


def _exports(app, out):
    """STEP for the assembly and each carbon part, STL for the printed."""
    app.export_step(path=os.path.join(out, "fpv-drone.step"))
    for kind, names in EXPORTED.items():
        for part in names:
            app.part(part, create=False).activate()
            path = os.path.join(out, part.lower().replace(" ", "-")
                                + "." + kind)
            if kind == "stl":
                app.export_stl(path=path, deflection=0.02)
            else:
                app.export_step(path=path)


def part_sheets(app, out):
    """One dimensioned sheet per part you have to have made."""
    print("drawings:")
    made = []
    # Read every hole table BEFORE the first sheet exists: asking for a part
    # by name opens its tab, and a note lands on whatever tab is active.
    tables = {name: hole_table(app.part(name, create=False),
                               (0, 1) if orientation == "top" else (0, 2))
              for name, orientation, *_ in SHEETS}
    for name, orientation, size, qty, material, thickness in SHEETS:
        # Not just `name`: a drawing is a TAB, and the part already has
        # that name.
        drawing = app.create_drawing(name=f"{name} sheet", sheet={
            "size": size, "landscape": True,
            "title_block": {"project": "5-inch FPV freestyle quadcopter",
                            "document": name, "author": "generators/fpv_drone.py",
                            "date": TODAY}})
        sheet_w, sheet_h = SHEET_MM[size]
        view = app.add_drawing_view(drawing=drawing["id"], source=name,
                                    orientation=orientation, scale=1.0,
                                    position=[0.30 * sheet_w, 0.52 * sheet_h],
                                    quality="exact")
        app.update_drawing()
        geometry = app.list_drawing_geometry(view=view["id"])["geometry"]

        # The label goes OUTSIDE the view, so the offset is measured from
        # the view's own extent rather than picked: at 1:1 a 108 mm plate
        # needs 66, and a guessed 16 puts the number on top of the part.
        for axis in (0, 1):
            a, b = _extremes(geometry, axis)
            # No `offset`: the app places a dimension clear of the views,
            # the title block AND the labels already on the sheet. Working
            # one out by hand — anchor, extent, which side — was thirty
            # lines that did the same job worse.
            app.add_drawing_dim(
                view=view["id"], kind="distance_x" if axis else "distance_y",
                refs=[{"edge": a["ref"]}, {"edge": b["ref"]}])

        # One callout per distinct bore, which is where the recorded fit
        # earns its keep: the note says what the hole is FOR.
        holes = {}
        for g in geometry:
            # CLOSED: an opening's corner is an arc of the same radius as a
            # bore could be, and a callout on one resolves to nothing.
            if (g["type"] == "circle" and g.get("visible") and g.get("closed")
                    and 0 < (g.get("radius") or 0) <= 5.0):
                holes.setdefault(round(g["radius"], 2), g)
        for radius, circle in sorted(holes.items()):
            app.add_drawing_dim(view=view["id"], kind="hole_callout",
                                refs=[{"edge": circle["ref"]}])

        note(app, (0.72 * sheet_w, 0.46 * sheet_h), [
            f"MATERIAL {material}",
            f"THICKNESS {thickness:g} mm",
            f"QUANTITY {qty}",
            "CUT CNC router or waterjet",
            "1 mm cutter, corners as drawn",
            "EDGES deburr, tab roots square",
            "FINISH none"])
        note(app, (0.72 * sheet_w, 0.92 * sheet_h),
             ["HOLE CENTRES from the part origin"] + tables[name])

        dims = app.list_drawing_dims()["dims"]
        dangling = [d for d in dims if not d.get("resolved")]
        if dangling:
            raise SdkError(f"{name}: {len(dangling)} of {len(dims)} dimensions "
                           f"do not resolve: {dangling[:2]}")
        path = os.path.join(out, f"{name.lower().replace(' ', '-')}.svg")
        app.export_drawing(path=path)
        app.export_drawing(path=path.replace(".svg", ".pdf"))
        issues = app.check_drawing(drawing=drawing["id"], update=True)
        if not issues.get("ok"):
            raise SdkError(f"{name} sheet: {issues['issues']}")
        print(f"  {name:<14} {size.upper()} 1:1, {len(dims)} dimensions, "
              f"{len(holes)} bore sizes")
        made.append((name, path, issues))
    return made


def assembly_sheet(app, out, name="Quadcopter"):
    """Top over front over on the left, iso on the right.

    Laid out from the views' MEASURED extents, not from three positions
    that looked about right: at 1:1 this quad's plan is 263 x 273 mm, which
    is most of an A2, and the front view ended up inside the top one. The
    scale comes from the sheet too, so a bigger or smaller aircraft still
    lands on it.
    """
    sheet_w, sheet_h = SHEET_MM[GA_SHEET]
    drawing = app.create_drawing(name="General arrangement", sheet={
        "size": GA_SHEET, "landscape": True,
        "title_block": {"project": "5-inch FPV freestyle quadcopter",
                        "document": "General arrangement",
                        "author": "generators/fpv_drone.py", "date": TODAY}})
    views = {}
    for orientation in ("top", "front", "iso"):
        # `fast` on purpose: twelve lofted propeller blades are what exact
        # HLR costs most on, and this sheet is a LOOK — the dimensioned
        # sheets are the ones anything is made from.
        views[orientation] = app.add_drawing_view(
            drawing=drawing["id"], source=name, orientation=orientation,
            scale=GA_SCALE, position=[0.25 * sheet_w, 0.5 * sheet_h],
            quality="fast")
    app.update_drawing(budget_s=180, _timeout=300)

    # A view's `position` is where its model ORIGIN lands, not where its
    # drawing sits: this quad's side view runs from z 0 upward, so its box
    # is 18 mm above the point that positions it. Move each view by the
    # difference between where its box IS and where it should be, and the
    # distinction stops mattering.
    box = {}
    for row in app.list_drawing_views()["views"]:
        (x0, y0), (x1, y1) = row["bbox"]
        box[row["id"]] = {"w": x1 - x0, "h": y1 - y0,
                          "centre": ((x0 + x1) / 2, (y0 + y1) / 2),
                          "position": row["position"]}

    top, front, iso = (box[views[o]["id"]] for o in ("top", "front", "iso"))
    stack = top["h"] + GA_GAP + front["h"]
    middle = sheet_h / 2
    targets = {
        views["top"]["id"]: (0.28 * sheet_w,
                             middle + stack / 2 - top["h"] / 2),
        views["front"]["id"]: (0.28 * sheet_w,
                               middle - stack / 2 + front["h"] / 2),
        views["iso"]["id"]: (0.72 * sheet_w, middle + 0.08 * sheet_h),
    }
    for view_id, (want_x, want_y) in targets.items():
        at, centre = box[view_id]["position"], box[view_id]["centre"]
        app.set_drawing_view(id=view_id,
                             position=[at[0] + want_x - centre[0],
                                       at[1] + want_y - centre[1]])
    issues = app.check_drawing(drawing=drawing["id"], update=True,
                               _timeout=300)
    if not issues.get("ok"):
        raise SdkError(f"General arrangement: {issues['issues']}")
    path = os.path.join(out, "general-arrangement.svg")
    app.export_drawing(path=path)
    app.export_drawing(path=path.replace(".svg", ".pdf"))
    print(f"  General arrangement {GA_SHEET.upper()} 1:{1 / GA_SCALE:g}, "
          f"3 views, no issues")
    return path


# What each row of the BOM actually weighs. The model's own mass is right
# for everything made of a real material — the carbon plates included, now
# that the table has carbon — and wrong for one kind of row: a bought
# component modelled as an envelope, whose mass is a catalogue figure and
# not a volume at all.
CATALOGUE_G = {
    "Motor 2207 1750KV": MOTOR_MASS_G,
    "Propeller 5.1x4.3x3 CW": PROP_MASS_G,
    "Propeller 5.1x4.3x3 CCW": PROP_MASS_G,
    "Flight controller F722": FC_MASS_G,
    "ESC 4-in-1 55A": ESC_MASS_G,
    "VTX 5.8G 800mW": VTX_MASS_G,
    "FPV camera 19mm": CAM_MASS_G,
    "Receiver ELRS 2.4G": RX_MASS_G,
    f"Battery {CELLS}S 1300mAh": BATT_MASS_G,
    "Capacitor 35V 1000uF": CAP_MASS_G,
    "Buzzer 5V": BUZZER_MASS_G,
    "Antenna RHCP SMA": ANT_MASS_G,
}
THRUST_PER_MOTOR_G = 1500.0   # 2207 1750KV on 6S, 5.1x4.3x3, manufacturer


def mass_budget(rows):
    """What it will actually weigh, row by row, and why that differs from
    what the document says."""
    out = []
    for row in rows:
        each = row["unit_mass_g"]
        why = "modelled"
        if row["part"] in CATALOGUE_G:
            each, why = CATALOGUE_G[row["part"]], "catalogue"
        out.append((row["part"], row["quantity"], each,
                    each * row["quantity"], why))
    return out


# Which parts leave the document as a file, and how.
EXPORTED = {"stl": ("Antenna mount", "Battery pad"),
            "step": ("Bottom plate", "Mid plate", "Top plate", "Arm",
                     "Camera plate")}


def centre_of_mass(app, budget):
    """Where the aircraft balances, with the corrected masses.

    The document's own centre of mass is the wrong answer here for the same
    reason its total is: the two heaviest things on a quadcopter are a
    battery and four motors, and both are envelopes. This re-weights each
    occurrence by its catalogue mass, placed at its own world box centre —
    an approximation, and a good one for a pack, a motor or a plate.

    It matters as much as the clearances do: a quad whose mass is not over
    the centre of its four propellers trims out crooked and eats its
    battery on hard landings.
    """
    per_part = {row[0]: row[2] for row in budget}
    by_id = {p["id"]: p["name"] for p in app.list_parts()["parts"]}
    total = 0.0
    moment = [0.0, 0.0, 0.0]
    for occ in app.list_occurrences(bounds=True)["occurrences"]:
        mass = per_part.get(by_id.get(occ.get("part_id")), 0.0)
        if not mass or "bbox_min" not in occ:
            continue
        total += mass
        for i in range(3):
            moment[i] += mass * (occ["bbox_min"][i] + occ["bbox_max"][i]) / 2
    return [m / total for m in moment], total


def outputs(app, out, name="Quadcopter"):
    """The files somebody builds from.

    By NAME, not by the handles the build happened to leave lying about, so
    this stage can run against a project that was opened rather than built
    — which is what makes `--stage drawings` possible, and it is worth
    having: this generator was run 90 times while it was being written and
    40 of those were a full rebuild-and-verify to look at a drawing.
    """
    os.makedirs(out, exist_ok=True)
    asm = app.assembly(name, create=False)
    asm.activate()

    mass = app.mass_properties()["total"]
    rows = app.bom()["rows"]
    app.export_bom(path=os.path.join(out, "bom.csv"))
    budget = mass_budget(rows)
    battery = sum(t for p, q, e, t, w in budget if p.startswith("Battery"))
    auw = sum(t for _, _, _, t, _ in budget)
    with open(os.path.join(out, "mass-budget.csv"), "w") as f:
        f.write("part,quantity,each_g,total_g,source\n")
        for part, qty, each, total, why in sorted(budget,
                                                  key=lambda r: -r[3]):
            f.write(f"{part},{qty},{each:.2f},{total:.1f},{why}\n")
        f.write(f"DRY (no pack),,,{auw - battery:.1f},\n")
        f.write(f"ALL-UP,,,{auw:.1f},\n")
    print(f"bom: {len(rows)} rows; model {mass['mass_g']:.0f} g, "
          f"corrected dry {auw - battery:.0f} g, all-up {auw:.0f} g, "
          f"thrust/weight {4 * THRUST_PER_MOTOR_G / auw:.1f}:1")

    cg, weighed = centre_of_mass(app, budget)
    print(f"  centre of mass {cg[0]:+.1f}, {cg[1]:+.1f}, {cg[2]:+.1f} mm "
          f"({weighed:.0f} g accounted)")
    if abs(cg[0]) > 15.0 or abs(cg[1]) > 5.0:
        raise SdkError(f"the centre of mass is {cg[0]:+.1f}, {cg[1]:+.1f} mm "
                       "from the middle of the propeller square")

    with phase("exports"):
        _exports(app, out)

    # The build guide travels with the files somebody builds from.
    guide = HERE.parent / "docs" / "modeling" / "fpv-drone-build.md"
    if guide.exists():
        shutil.copy(guide, os.path.join(out, "BUILD.md"))

    # Re-running the sheets against a project that already has them would
    # collide on the tab name, so clear ours first. (A drawing IS a tab:
    # `delete_part` takes one by name.)
    wanted = {f"{name} sheet" for name, *_ in SHEETS} | {"General arrangement"}
    for existing in app.list_drawings()["drawings"]:
        if existing["name"] in wanted:
            app.delete_part(part=existing["name"])

    with phase("part sheets"):
        sheets = part_sheets(app, out)
    asm.activate()
    with phase("GA sheet"):
        assembly_sheet(app, out)

    asm.activate()
    with phase("renders"):
        app.render_sheet(path=os.path.join(out, "views.png"))
        app.set_camera(eye=[330.0, -260.0, 210.0], target=[0.0, 0.0, 25.0])
        app.render_still(path=os.path.join(out, "render.png"), width=1920,
                         height=1080, samples=256, _timeout=600)
    return mass, rows, sheets


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    ap.add_argument("--stage", default="all",
                    choices=("parts", "assembly", "all", "drawings"))
    ap.add_argument("--out", default="scratch/fpv-drone")
    args = ap.parse_args()

    app = connect(args.port)
    project = os.path.join(args.out, "fpv-drone.dsgn")
    if args.stage == "drawings":
        # The sheets and exports against the project as SAVED. The model is
        # not rebuilt and nothing is re-checked, which is the point: it is
        # the loop for laying a sheet out, and it is four times faster.
        app.open_project(path=project)
        outputs(app, args.out)
        app.save_project(path=project, force=True)
        print("saved")
        return

    app.new()
    with phase("parts"):
        parts = build_parts(app)
    if args.stage == "parts":
        report_phases()
        return
    with phase("assembly"):
        asm, occ = build_assembly(app, parts)
    with phase("fasteners"):
        add_fasteners(asm, parts, occ)
    with phase("mates"):
        props = add_mates(asm, occ)
    verify(app, asm, parts, occ, props)
    os.makedirs(args.out, exist_ok=True)
    app.save_project(path=project)
    if args.stage == "all":
        outputs(app, args.out)
        app.save_project(path=project, force=True)
    print("saved")
    report_phases()


if __name__ == "__main__":
    main()
