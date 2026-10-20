#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A printed weight-driven pendulum clock: a Graham deadbeat escapement, a
two-stage cycloidal going train to the centre wheel (one turn an hour),
12:1 motion works, a cord drum with a printed click, two plates on four
pillars closed by printed nuts, a dial ring and hands, and a pendulum with a
rating nut on a printed thread. Everything printed but the cord.

    tools/headless-preview.sh --start <port>
    python3 generators/clock/build.py --port <port> --profile "K5 PLA on P1S"
    python3 generators/clock/build.py --port <port> --period 0.8

Every size derives from `--period` (the pendulum's, there and back) and the
print profile's allowances; the pendulum's length from the period and the
modelled masses, as a compound pendulum. What it builds, checks and
predicts: generators/clock/README.md.

Output (generated, untracked): scratch/clock/ (or $LOBOLE_SCRATCH/clock,
or --out) — clock.dsgn, clock.json (the receipt), clock_card.md (the test
card), and stage1/ and stage2/, one STL per print plate; with --video,
stage1_sequence.mp4, stage 1's assembly sequence filmed.
"""
import argparse
import importlib.util
import json
import math
import os
import sys
import time
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import clock_parts as P  # noqa: E402
import clock_spec as S  # noqa: E402
import cycloidal as C  # noqa: E402
from _lib import (PRINT_ITEMS, bed_zones, check_print,  # noqa: E402
                  clear_x, export_print_job, fresh_project, job_receipt,
                  pin_strength, print_item_line, print_receipt)
from lobole.sdk import SdkError, connect  # noqa: E402

ROOT = Path(HERE).resolve().parent.parent
OUT = Path(os.environ.get("LOBOLE_SCRATCH", ROOT / "scratch")) / "clock"
BED_MM = 256.0        # the P1S's bed, square; the profile's bed_mm when
                      # it states one (main)
BED_MARGIN = 4.0      # kept clear round its edge
BED_ZONES = []        # the profile's keep-out zones (bed_exclude_mm)
PHASES = []


class phase:
    def __init__(self, name):
        self.name = name

    def __enter__(self):
        self.t = time.perf_counter()
        print(f"[{self.name}]")

    def __exit__(self, *exc):
        PHASES.append((self.name, time.perf_counter() - self.t))


class Allow:
    """print_allowance's diametral allowance by kind and size, asked once."""

    def __init__(self, d, proc):
        self.d, self.proc, self.cache, self.confirm = d, proc, {}, None

    def __call__(self, kind, size):
        k = (kind, round(size, 4))
        if k not in self.cache:
            r = self.d.call("print_allowance", kind=kind, size_mm=size,
                            **self.proc)
            self.cache[k] = r["allowance_mm"]
            self.confirm = r.get("confirm")
        return self.cache[k]


def jar_weight(allow):
    """The weight is generators/jar/'s jar at its own defaults, full of
    sand: its outer diameter (which sets how far forward the cord must run
    to pass the bob) from the jar's own solve(), with this profile's
    allowances."""
    spec = importlib.util.spec_from_file_location(
        "jar_build", str(ROOT / "generators" / "jar" / "build.py"))
    jar = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(jar)
    inner, ml = 60.0, 200.0   # jar/build.py's --inner-diameter and --volume
    probe = jar.solve(inner, ml, {"external_thread": 0, "internal_thread": 0,
                                  "hole": 0, "shaft": 0}, probe=True)
    sizes = {"external_thread": probe["basic_major"],
             "internal_thread": probe["basic_major"],
             "hole": probe["gasket_out_d"], "shaft": probe["gasket_in_d"]}
    js = jar.solve(inner, ml, {k: allow(k, v) for k, v in sizes.items()})
    return {"outer_d": js["outer_d"], "ml": ml,
            "sand_g": ml * S.SAND_G_ML}


# --- parts ----------------------------------------------------------------------

FIRST_ESCAPE_ARBOR = "escape arbor (stage 1, first print)"


def part_names(d):
    return {p["name"] for p in d.call("list_parts")["parts"]
            if p["type"] == "part"}


def build_parts(d, s, proc):
    """Every part but the pendulum's (find_length builds those)."""
    e, z = s["escape"], s["z"]
    H, t, es = s["H"], s["t_plate"], s["end_shake"]
    P.build_beat(d)
    P.build_escape_wheel(d, s, proc)
    P.build_anchor(d, s, proc)
    P.build_anchor(d, s, proc, name="anchor (pallets too deep)",
                   deeper_deg=2.0 * e["lock_deg"])
    P.build_drum(d, s, proc, name="drum (stage 1)", stage1=True)
    zd1 = s["drum1"]["z"]
    rz0, rz1 = P.rod_z(s)
    S.arbor_plan(s, (rz0, rz1), zd1)
    for name in s["arbors"]:
        P.build_arbor(d, s, proc, name)
    # The insertion check's negative control: the stage-1 escape arbor as
    # it was first printed, its round back pivot in the way of the wheel's
    # press D (docs/inspect/README.md#insertion).
    P.build_arbor_first(d, s, proc, FIRST_ESCAPE_ARBOR, -t, zd1[1],
                        [(0.0, H), zd1])
    P.build_third(d, s, proc)
    P.build_spacer(d, s, proc, "third spacer", es, z["third"][0])
    P.build_centre(d, s, proc)
    # On the centre arbor's slide D, which the centre wheel passes.
    P.build_spacer(d, s, proc, "centre spacer", es, z["centre"][0],
                   bore_d=2.0 * (s["slide_r"] + s["g_small"]))
    P.build_drum(d, s, proc)
    for stage, fr in (("", s["frame2"]), (" (stage 1)", s["frame1"])):
        P.build_plate(d, s, proc, f"back plate{stage}", fr, back=True)
        P.build_plate(d, s, proc, f"front plate{stage}",
                      fr if stage == "" else dict(fr, minute_wheel=None),
                      back=False)
    P.build_pillar(d, s, proc)
    P.build_nut(d, s, proc)
    P.build_cannon(d, s, proc)
    P.build_minute_wheel(d, s, proc)
    P.build_hour_wheel(d, s, proc)
    P.build_hand(d, s, proc, "minute hand", s["minute_hand_r"],
                 "minute_hand", s["cannon_pipe_d"], d_flat=True)
    P.build_hand(d, s, proc, "hour hand", s["hour_hand_r"], "hour_hand",
                 s["hour_pipe_d"], d_flat=False)
    P.build_dial_ring(d, s, proc)
    P.build_dial_spacer(d, s, proc)


# What swings is weighed as it PRINTS (mass_properties' printed estimate,
# docs/tolerances/README.md#printed-mass), so the period is the period of
# the plastic on the bed. SOLID is the other reading, kept to say what the
# infill moves.
WEIGH = {"as": "printed"}


def body_props(d, part):
    """Each body's (mass_g, centre of mass, I_zz about it g mm^2), as it
    prints: the printed estimate at the part's slicer settings. A part
    printed solid (100 %) with no estimate (no profile states a layer and a
    line width) is its solid self; any other part without one is refused --
    its period would be the solid's, which it does not print as."""
    r = d.call("mass_properties", part=part)
    out = []
    for b in r["bodies"]:
        pr = b.get("printed")
        solid = WEIGH["as"] == "solid"
        if not solid and (not pr or pr.get("mass_g") is None):
            st = d.call("get_print_settings", part=part)["bodies"][0]
            if st["settings"].get("infill_pct") != 100:
                raise SystemExit(
                    f"clock: {part} has no printed mass ("
                    f"{(pr or {}).get('why', 'not printed')}), and at "
                    f"{st['described']} it is not its solid self: the "
                    "period is computed for what prints, or refused")
            solid = True
        if solid:
            rho = b["mass_g"] / b["volume_mm3"]
            out.append((b["mass_g"], b["center_of_mass_mm"],
                        b["inertia"][2][2] * rho))
        else:
            rho = pr["mass_g"] / pr["volume_mm3"]
            out.append((pr["mass_g"], pr["center_of_mass_mm"],
                        pr["inertia"][2][2] * rho))
    return out


PENDULUM = ("pendulum rod", "bob", "rating nut")
# The pendulum prints SOLID, rectilinear (Bambu refuses grid at 100 %):
# its period was designed for its mass, and a 15 % bob is a lighter,
# shorter-swinging one. Bob and nut stand on end, the rod's axis (+Y) up;
# the rod lies on its back (+Z up).
PENDULUM_PRINT = {"walls": 3, "top_layers": 5, "bottom_layers": 5,
                  "infill_pct": 100, "infill_pattern": "rectilinear"}
PENDULUM_UP = {"pendulum rod": [0, 0, 1], "bob": [0, 1, 0],
               "rating nut": [0, 1, 0]}


def build_pendulum(d, s, proc, length):
    have = part_names(d)
    for name in PENDULUM:
        if name in have:
            d.call("delete_part", part=name)
    P.build_rod(d, s, proc, length)
    y_n = S.nut_y(s, length)
    P.build_rating_nut(d, s, proc, y_n)
    P.build_bob(d, s, proc, y_n + s["nut"]["h"])
    if proc is not None:
        for name in PENDULUM:
            d.call("set_print_settings", part=name, direction=PENDULUM_UP[name],
                   **PENDULUM_PRINT)
    s["pend"]["length"] = length
    s["pend"]["nut_y"] = y_n


def swinging(d):
    """What swings, each body as compound_period wants it: the pendulum and
    the anchor with its arbor."""
    bodies = []
    for part in PENDULUM + ("anchor", "anchor arbor"):
        bodies += body_props(d, part)
    return bodies


def find_length(d, s, proc, target):
    """The rod's length for which the modelled pendulum, rod, bob, nut,
    anchor and arbor together, swings at `target` seconds at the design
    amplitude: T scales as the root of the length, so a few steps of
    L <- L (target / T)^2 on the real parts converge."""
    e = s["escape"]
    l_eq = S.simple_length_mm(target, e["amplitude_deg"])
    pend = S.pendulum_sizes(s, l_eq)
    th = s["rating_thread"]
    y_c = -l_eq
    y_n0 = y_c - pend["bob_h"] / 2.0 - s["nut"]["h"]
    length = -(y_n0 - th["pitch"] / 2.0 - pend["travel"])
    steps = []
    for _ in range(8):
        build_pendulum(d, s, proc, length)
        cp = S.compound_period(swinging(d), e["amplitude_deg"])
        steps.append((length, cp["period_s"]))
        print(f"  rod {length:.3f} mm: {cp['period_s']:.6f} s "
              f"(equivalent length {cp['equivalent_length_mm']:.1f} mm)")
        if abs(cp["period_s"] - target) < 1e-6 * target:
            break
        length *= (target / cp["period_s"]) ** 2
    else:
        raise SystemExit(f"clock: the pendulum's length did not converge: "
                         f"{steps}")
    s["pend"]["steps"] = steps
    return cp


def rating_per_turn(d, s, cp0):
    """The rate change for one turn of the rating nut that LOWERS the bob
    a pitch (the clock loses), s/day: the same bodies, the bob and nut moved
    a pitch down."""
    p = s["rating_thread"]["pitch"]
    bodies = []
    for part in PENDULUM + ("anchor", "anchor arbor"):
        for m, c, izz in body_props(d, part):
            if part in ("bob", "rating nut"):
                c = [c[0], c[1] - p, c[2]]
            bodies.append((m, c, izz))
    cp = S.compound_period(bodies, s["escape"]["amplitude_deg"])
    return -86400.0 * (cp["period_s"] - cp0["period_s"]) / cp0["period_s"]


def verify_parts(d):
    bad = []
    n = 0
    for name in sorted(part_names(d)):
        d.call("set_active_part", part=name)
        if not d.call("list_elements")["bodies"]:
            continue            # new_project's empty first part
        n += 1
        geo = d.call("check_geometry")
        if geo["receipt"]["standing"] != "verified":
            bad.append(f"{name}: {geo['receipt'].get('summary')}")
    if bad:
        raise SystemExit("clock: parts not verified: " + "; ".join(bad))
    return n


# --- assemblies ---------------------------------------------------------------------

class Asm:
    def __init__(self, d, name):
        self.d = d
        self.name = name
        d.call("add_assembly", name=name)
        self.occ, self.mates, self.rel = {}, {}, {}

    def activate(self):
        self.d.call("set_active_part", part=self.name)

    def fixed(self, part, name, pos=(0.0, 0.0, 0.0), deg=0.0):
        a = math.radians(deg) / 2.0
        self.occ[name] = self.d.call(
            "add_occurrence", part=part, name=name, fixed=True,
            transform={"position": list(pos),
                       "orientation": [0.0, 0.0, math.sin(a),
                                       math.cos(a)]})["id"]
        return self.occ[name]

    def place(self, part, name):
        self.occ[name] = self.d.call("add_occurrence", part=part,
                                     name=name)["id"]
        return self.occ[name]

    def mate(self, kind, name, a, b):
        r = self.d.call("add_mate", type=kind, name=name, a=a, b=b,
                        flipped=False)
        if r.get("errored_mates"):
            raise SystemExit(f"clock: mate {name} errored: {r}")
        self.mates[name] = r["id"]
        return r["id"]

    def turn(self, plate, axis, part, name):
        """A turning part on a revolute about an axis frame of `plate`."""
        self.place(part, name)
        return self.mate("revolute", f"{name} axis",
                         {"occurrence": self.occ[plate], "frame": axis},
                         {"occurrence": self.occ[name], "origin": True})

    def on(self, carrier, part, name, kind="fastened"):
        self.place(part, name)
        return self.mate(kind, f"{name} on {carrier}",
                         {"occurrence": self.occ[carrier], "origin": True},
                         {"occurrence": self.occ[name], "origin": True})

    def relate(self, name, **kw):
        r = self.d.call("add_relation", name=name, **kw)
        if r.get("error"):
            raise SystemExit(f"clock: relation {name}: {r['error']}")
        self.rel[name] = r["id"]
        return r

    def interference(self, a, b, mode="exact"):
        """The exact overlap's volume; with mode "mesh", 1 for a clash (the
        mesh check knows only that), ten times faster on a thread."""
        r = self.d.call("interference", a=self.occ[a], b=self.occ[b],
                        mode=mode)
        if mode == "mesh":
            return 1.0 if r.get("clash") else 0.0
        return r.get("volume_mm3", 0.0) if r.get("clash") else 0.0

    def set(self, mate, deg):
        self.d.call("set_mate", id=self.mates[mate], rotation_deg=deg)
        self.d.call("solve_assembly")

    def rotations(self):
        return {m["name"]: m.get("rotation_deg", 0.0)
                for m in self.d.call("list_mates")["mates"]}


def escapement_relations(a, s):
    """The escapement as the watch animates its own (watch.py
    escapement_relations): not simulated. A hidden beat turns once a
    period; it steps the escape wheel half a tooth a beat through an
    intermittent relation whose table law follows the pallets
    (clock_spec.window()), and swings the anchor (and the pendulum fastened
    to it) through a cam whose table is A cos(beat)."""
    win, e = s["window"], s["escape"]
    a.relate("tick", type="intermittent", a=a.mates["beat axis"],
             b=a.mates["escape wheel axis"], motion="table",
             table=win["table"], start_deg=win["start_deg"],
             step_deg=win["step_deg"], duration_deg=win["duration_deg"],
             period_deg=win["period_deg"], phase=0.0)
    a.relate("swing", type="cam", a=a.mates["beat axis"],
             b=a.mates["anchor axis"], lift=S.cam_table(e))


def phase_search(a, part_a, part_b, place, step_deg=15.0):
    """The turn at which `part_b`'s thread clears `part_a`'s: the jar's
    find_phase (generators/jar/build.py), by exact interference. `place`
    puts b at a turn."""
    def vol(th):
        place(th)
        return a.interference(part_a, part_b, mode="mesh")
    n = int(round(360.0 / step_deg))
    samples = [(i * step_deg, vol(i * step_deg)) for i in range(n)]
    clean = [th for th, v in samples if v <= 0.0]
    if not clean or len(clean) == n:
        raise SystemExit(f"clock: no clean turn for {part_b} on {part_a}: "
                         f"{samples}")
    i0 = next(i for i in range(n)
              if samples[i][1] <= 0.0 and samples[i - 1][1] > 0.0)
    run, i = [], i0
    while samples[i % n][1] <= 0.0:
        run.append(i)
        i += 1
    lo_in, hi_in = run[0] * step_deg, run[-1] * step_deg

    def edge(inside, outside):
        for _ in range(6):
            mid = (inside + outside) / 2.0
            if vol(mid) <= 0.0:
                inside = mid
            else:
                outside = mid
        return inside
    lo, hi = edge(lo_in, lo_in - step_deg), edge(hi_in, hi_in + step_deg)
    best = (lo + hi) / 2.0
    place(best)
    if a.interference(part_a, part_b) > 0.0:
        raise SystemExit(f"clock: {part_b} at {best:.1f} deg clears "
                         f"{part_a} by the mesh check and not exactly")
    return best, hi - lo


def place_nuts(a, s, fr, nut_zs):
    """The pillars and their nuts, every nut turned to where its thread
    clears its pillar's: one search per thread (rear, front), the rest
    from the pitch (a right-hand thread advances a turn per pitch), each
    checked."""
    pt, th = s["pillar"], s["pillar_thread"]
    for k, (x, y) in enumerate(fr["pillars"]):
        a.fixed("pillar", f"pillar {k + 1}", (x, y, 0.0))
    found = {}
    windows = {}
    x0, y0 = fr["pillars"][0]
    for k, (x, y) in enumerate(fr["pillars"]):
        for which, zn in nut_zs(k):
            name = f"nut {k + 1} {which}"
            a.fixed("pillar nut", name, (x, y, zn))
            nid = a.occ[name]

            def place(deg, nid=nid, x=x, y=y, zn=zn):
                h = math.radians(deg) / 2.0
                a.d.call("set_occurrence_transform", occurrence=nid,
                         transform={"position": [x, y, zn], "orientation":
                                    [0.0, 0.0, math.sin(h), math.cos(h)]})
            if which not in found:
                ph, win = phase_search(a, f"pillar {k + 1}", name, place)
                found[which] = (ph, zn)
                windows[which] = win
                continue
            ph0, z0 = found[which]
            for sgn in (1.0, -1.0):
                ph = ph0 + sgn * 360.0 * (zn - z0) / th["pitch"]
                place(ph)
                if a.interference(f"pillar {k + 1}", name, "mesh") <= 0.0 \
                        and a.interference(f"pillar {k + 1}", name) <= 0.0:
                    break
            else:
                ph, _ = phase_search(a, f"pillar {k + 1}", name, place)
    del pt, x0, y0
    return windows


def build_stage1(d, s):
    a = Asm(d, "stage 1")
    fr = s["frame1"]
    a.fixed("back plate (stage 1)", "back plate")
    a.fixed("front plate (stage 1)", "front plate")
    pt = s["pillar"]
    nut_win = place_nuts(a, s, fr, lambda k: [
        ("rear", -s["t_plate"] - pt["nut_h"]), ("front", s["F"])])
    a.turn("back plate", "beat", "beat", "beat")
    d.call("set_occurrence_visible", occurrence=a.occ["beat"], visible=False)
    a.turn("back plate", "escape", "escape wheel", "escape wheel")
    a.on("escape wheel", "escape arbor (stage 1)", "escape arbor")
    a.on("escape wheel", "drum (stage 1)", "drum")
    a.turn("back plate", "anchor", "anchor", "anchor")
    a.on("anchor", "anchor arbor", "anchor arbor")
    rating_nut_on(a, s)
    escapement_relations(a, s)
    return a, nut_win


def rating_nut_on(a, s):
    """The rod and bob fastened to the anchor; the rating nut on a revolute
    about the rod's thread, turned until its thread clears the rod's."""
    a.on("anchor", "pendulum rod", "pendulum rod")
    a.on("anchor", "bob", "bob")
    a.place("rating nut", "rating nut")
    m = a.mate("revolute", "rating nut on its thread",
               {"occurrence": a.occ["pendulum rod"], "frame": "thread"},
               {"occurrence": a.occ["rating nut"], "frame": "thread"})
    del m

    def place(deg):
        a.set("rating nut on its thread", deg)
    ph, win = phase_search(a, "pendulum rod", "rating nut", place)
    s["pend"]["nut_phase_deg"] = ph
    s["pend"]["nut_window_deg"] = win


def build_stage2(d, s, control=False):
    name = "stage 2" if not control else "stage 2 (control)"
    a = Asm(d, name)
    fr = s["frame2"]
    pt = s["pillar"]
    a.fixed("back plate", "back plate")
    a.fixed("front plate", "front plate")
    lower = {0, 1}
    nut_win = place_nuts(a, s, fr, lambda k: [
        ("rear", -s["t_plate"] - pt["nut_h"]),
        ("front" if k not in lower else "dial", s["F"] if k not in lower
         else s["z"]["ring"][1])])
    for k, (x, y) in enumerate(s["ring"]["pillars"]):
        a.fixed("dial spacer", f"dial spacer {k + 1}", (x, y, 0.0))
    a.fixed("dial ring", "dial ring")
    a.turn("back plate", "beat", "beat", "beat")
    d.call("set_occurrence_visible", occurrence=a.occ["beat"], visible=False)
    a.turn("back plate", "escape", "escape wheel", "escape wheel")
    a.on("escape wheel", "escape arbor", "escape arbor")
    a.turn("back plate", "anchor", "anchor", "anchor")
    a.on("anchor", "anchor arbor", "anchor arbor")
    rating_nut_on(a, s)
    a.turn("back plate", "third", "third wheel", "third wheel")
    a.on("third wheel", "third arbor", "third arbor")
    a.on("third wheel", "third spacer", "third spacer")
    a.turn("back plate", "centre", "centre wheel", "centre wheel")
    a.on("centre wheel", "centre arbor", "centre arbor")
    a.on("centre wheel", "centre spacer", "centre spacer")
    a.on("centre wheel", "cannon pinion", "cannon pinion")
    a.on("cannon pinion", "minute hand", "minute hand")
    a.on("centre wheel", "drum", "drum", kind="revolute")
    a.turn("front plate", "minute wheel", "minute wheel", "minute wheel")
    a.turn("back plate", "centre", "hour wheel", "hour wheel")
    a.on("hour wheel", "hour hand", "hour hand")
    escapement_relations(a, s)
    c, pos = s["counts"], s["pos"]
    mw = s["minute_wheel_xy"]
    for rname, ma, mb, za, zb, pa, pb in (
            ("escape pinion drives the third wheel", "escape wheel axis",
             "third wheel axis", c["escape_pinion"], c["third"],
             pos["escape"], pos["third"]),
            ("third pinion drives the centre wheel", "third wheel axis",
             "centre wheel axis", c["third_pinion"], c["centre"],
             pos["third"], pos["centre"]),
            ("cannon drives the minute wheel", "centre wheel axis",
             "minute wheel axis", S.Z_CANNON, S.Z_MINUTE_W, pos["centre"],
             mw),
            ("minute pinion drives the hour wheel", "minute wheel axis",
             "hour wheel axis", S.Z_MINUTE_P, S.Z_HOUR, mw, pos["centre"])):
        a.relate(rname, type="gear", a=a.mates[ma], b=a.mates[mb],
                 teeth_a=za, teeth_b=zb, phase=C.mesh_phase(pa, pb, za, zb))
    # The click: the drum may turn the winding way (anticlockwise, +) on
    # the centre wheel and is held the going way, which drives the wheel.
    a.relate("click", type="click", a=a.mates["drum on centre wheel"],
             b=a.mates["drum on centre wheel"], drives="positive")
    return a, nut_win


# --- the assembly sequence --------------------------------------------------------

# How each stage goes together, in order (docs/assemblies/README.md
# #assembly-sequences): each step the occurrence and the way it moves going
# home. The movement's front is +Z: from the front is FRONT, -Z. Every part
# goes on from clock_spec.ENTRY's end relative to what it goes on; one put
# on its arbor "from the back" (a spacer, the centre wheel) is placed first
# and its arbor slides through it from the front, which is the same slide.
# Each step is checked against everything placed before it.
FRONT, BACK = [0.0, 0.0, -1.0], [0.0, 0.0, 1.0]
ROD = "up the rod"  # the pendulum rod's own axis, read off its placement


def step(occ, direction=None, **kw):
    """One step: an occurrence, or a list of them -- a group that goes home
    as one, along one path."""
    out = ({"occurrences": list(occ)} if isinstance(occ, (list, tuple))
           else {"occurrence": occ})
    if direction is not None:
        out["direction"] = direction
    out.update(kw)
    return out


def step_name(x):
    """A step's name as the sequence check answers it."""
    return " + ".join(x["occurrences"]) if "occurrences" in x else \
        x["occurrence"]


SEQUENCE = {
    "stage 1": [
        step("back plate", note="the base"),
        *[step(f"pillar {i}", FRONT) for i in (1, 2, 3, 4)],
        *[step(f"nut {i} rear", BACK, thread=True) for i in (1, 2, 3, 4)],
        step("escape arbor", FRONT), step("escape wheel", FRONT),
        step("anchor arbor", FRONT), step("anchor", FRONT),
        step("pendulum rod", BACK),
        step("front plate", FRONT),
        # The drum rides the escape arbor in front of the front plate.
        step("drum", FRONT),
        *[step(f"nut {i} front", FRONT, thread=True) for i in (1, 2, 3, 4)],
        step("bob", ROD), step("rating nut", ROD, thread=True)],
    "stage 2": [
        step("back plate", note="the base"),
        *[step(f"pillar {i}", FRONT) for i in (1, 2, 3, 4)],
        *[step(f"nut {i} rear", BACK, thread=True) for i in (1, 2, 3, 4)],
        # The back of the train first. The third and escape arbors go into
        # the back plate together, their wheels on them and meshed: the
        # escape wheel, printed with its pinion, has matter behind the
        # third wheel's teeth, and the third wheel's rim runs under the
        # escape wheel's, so neither has a straight way home past the
        # other alone.
        step("third spacer", FRONT),
        step(["third arbor", "third wheel", "escape arbor", "escape wheel"],
             FRONT, note="both arbors with their wheels, meshed"),
        step("centre spacer", FRONT), step("centre wheel", FRONT),
        step("centre arbor", FRONT), step("drum", FRONT),
        step("anchor arbor", FRONT), step("anchor", FRONT),
        step("pendulum rod", BACK),
        step("front plate", FRONT), step("cannon pinion", FRONT),
        step("nut 3 front", FRONT, thread=True),
        step("nut 4 front", FRONT, thread=True),
        step("dial spacer 1", FRONT), step("dial spacer 2", FRONT),
        step("minute wheel", FRONT), step("hour wheel", FRONT),
        step("dial ring", FRONT),
        step("nut 1 dial", FRONT, thread=True),
        step("nut 2 dial", FRONT, thread=True),
        step("hour hand", FRONT), step("minute hand", FRONT),
        step("bob", ROD), step("rating nut", ROD, thread=True)],
}

def asm_id(d, name):
    return next(p["id"] for p in d.call("list_parts")["parts"]
                if p["type"] == "assembly" and p["name"] == name)


def rod_up(d):
    """The pendulum rod's own +Y in the assembly: the way up it, as the
    pendulum hangs where the stage placed it."""
    rod = next(o for o in d.call("list_occurrences")["occurrences"]
               if o["name"] == "pendulum rod")
    qx, qy, qz, qw = rod["transform"]["orientation"]
    return [2 * (qx * qy - qz * qw), 1 - 2 * (qx * qx + qz * qz),
            2 * (qy * qz + qx * qw)]


def sequence(a, s):
    """The stage's assembly sequence, stated on its assembly and checked:
    every step goes home against everything placed before it, at the
    print's closing, and the stage states it as an `assembly_sequence`
    requirement too; any step that fails stops the run, naming it."""
    d = a.d
    a.activate()
    up = rod_up(d)
    steps = [dict(x, direction=up) if x.get("direction") == ROD else x
             for x in SEQUENCE[a.name]]
    d.call("set_assembly_sequence", steps=steps,
           allowance_mm=s["print_closing"])
    t0 = time.time()
    r = d.call("check_assembly_sequence", _timeout=3600)
    secs = time.time() - t0
    out, bad = {}, []
    for row in r["steps"]:
        name = row["name"]
        out[name] = {k: row.get(k) for k in (
            "pass", "press_mm", "interfering_mm", "excess_mm", "deeper_mm",
            "worst_depth_mm", "worst_against", "threads", "seconds")}
        out[name]["moves"] = (row.get("path") or {}).get("moves")
        if row["pass"]:
            continue
        bad.append(f"step {row['step']} {name}: "
                   + (row.get("error") or
                      f"forced {row.get('interfering_mm', 0):.2f} mm against "
                      f"a press of {row.get('press_mm', 0):.2f}, worst "
                      f"against {row.get('worst_against')}"))
    if r["missing"]:
        bad.append(f"no step places {r['missing']}")
    print(f"  {a.name} assembly sequence: {len(r['steps'])} steps, "
          f"checked against everything before each in {secs:.0f} s"
          + ("" if bad else ", every one goes home"))
    if bad:
        raise SystemExit(f"clock: {a.name} cannot be put together: {bad}")
    doc = asm_id(d, a.name)
    rid = d.call("add_requirement", name=f"{a.name} goes together",
                 measure={"kind": "assembly_sequence", "document": doc},
                 bound={"kind": "pass"},
                 source="the order it is built in "
                        "(docs/assemblies/README.md#assembly-sequences)"
                 )["id"]
    res = d.call("check_requirements", ids=[rid], _timeout=3600)
    st = res["requirements"][0]["state"]
    if st != "pass":
        raise SystemExit(f"clock: {a.name}'s assembly_sequence "
                         f"requirement is {st}")
    return {"seconds": secs, "steps": out}


def first_arbor_control(d, s):
    """The negative control: the stage-1 escape arbor as first printed,
    with the escape wheel, must FAIL the insertion check from either end:
    from the back its round pivot is forced through the wheel's D bore,
    far deeper than the wheel is pressed at its seat."""
    a = Asm(d, "stage 1 escape (first print)")
    a.fixed("escape wheel", "escape wheel")
    a.on("escape wheel", FIRST_ESCAPE_ARBOR, "escape arbor")
    r = d.call("check_insertion", a=a.occ["escape wheel"],
               b=a.occ["escape arbor"], entry="either",
               allowance_mm=s["print_closing"], _timeout=600)
    print(f"  negative control, the stage-1 escape arbor as first printed: "
          f"{'passes' if r['pass'] else 'fails'}, forced "
          f"{r['interfering_mm']:.2f} mm from the {r['entry']} side against "
          f"a press of {r['press_mm']:.2f}, {r['deeper_mm']:.2f} mm of it "
          f"{r['worst_depth_mm']:.2f} mm deep (its seat is pressed "
          f"{s['print_closing'] - (s['press_d'] - s['arbor_d']) / 2.0:.2f})")
    if r["pass"]:
        raise SystemExit("clock: the first stage-1 escape arbor goes on: "
                         "the insertion check cannot see a round pivot in "
                         f"a D bore: {r}")
    return {k: r[k] for k in ("pass", "entry", "press_mm", "interfering_mm",
                              "excess_mm", "deeper_mm", "from_mm",
                              "worst_depth_mm")}


# --- motion -----------------------------------------------------------------------

def run_motion(a, name, driver, from_deg, to_deg, samples, clash,
               check_clash=True):
    d = a.d
    a.activate()
    st = d.call("add_motion_study", driver=a.mates[driver], dof="rotation",
                **{"from": from_deg}, to=to_deg, samples=samples,
                clash_mode="mesh", clash_deflection_mm=clash, name=name,
                check_clash=check_clash)
    if st.get("why_not"):
        raise SystemExit(f"clock: motion study {name}: {st['why_not']}")
    sid = st.get("id", st.get("study"))
    r = d.call("run_motion_study", study=sid, include_poses=True,
               _timeout=1800)
    return r.get("result", r)


def clashing(result):
    return sorted({tuple(sorted((c["a_name"], c["b_name"])))
                   for smp in result.get("samples", [])
                   for c in smp.get("clashes", [])})


def beat_ends(result, s):
    """At every sample on a beat's end (the swing's turn), how far the
    escape wheel stands off its locked step, degrees."""
    worst, n = 0.0, 0
    for smp in result.get("samples", []):
        b = smp["value"]
        if abs(b / 180.0 - round(b / 180.0)) > 1e-6:
            continue
        mates = {m["name"]: m["rotation_deg"] for m in smp["mates"]}
        want = s["window"]["step_deg"] * round(b / 180.0)
        worst = max(worst, abs(mates["escape wheel axis"] - want))
        n += 1
    return worst, n


def check_ratios(result, s):
    """The train's turns over the study, first sample to last, against the
    tooth counts' (watch.py check_ratios): one escape-wheel turn moves the
    minute hand 360/(escape to centre) and the hour hand a twelfth of
    that."""
    first = {m["name"]: m["rotation_deg"] for m in result["samples"][0]
             ["mates"]}
    last = {m["name"]: m["rotation_deg"] for m in result["samples"][-1]
            ["mates"]}
    c = s["counts"]
    turned = {k: last[k] - first[k] for k in (
        "escape wheel axis", "third wheel axis", "centre wheel axis",
        "minute wheel axis", "hour wheel axis")}
    want = {"escape wheel axis": -360.0,
            "third wheel axis": 360.0 * c["escape_pinion"] / c["third"],
            "centre wheel axis": -360.0 / c["escape_to_centre"],
            "minute wheel axis": 360.0 / c["escape_to_centre"]
            * S.Z_CANNON / S.Z_MINUTE_W,
            "hour wheel axis": -360.0 / c["escape_to_centre"] / 12.0}
    bad = [f"{k} turned {turned[k]:+.6f} deg, the counts say "
           f"{want[k]:+.6f}" for k in want
           if abs(turned[k] - want[k]) > 1e-6 * max(1.0, abs(want[k]))]
    return turned, want, bad


def mesh_sweeps(a, s, samples=9):
    """Every mesh through one pitch of its smaller gear, exact (watch.py
    check_meshes): the beat driven far enough to turn that gear a pitch,
    sampled at beat ends."""
    c = s["counts"]
    e2c = c["escape_to_centre"]
    out = {}
    # (pair, smaller gear's teeth, its turns per escape-wheel turn)
    meshes = (("escape wheel", "third wheel", c["escape_pinion"], 1.0),
              ("third wheel", "centre wheel", c["third_pinion"],
               c["escape_pinion"] / c["third"]),
              ("cannon pinion", "minute wheel", S.Z_CANNON, 1.0 / e2c),
              ("minute wheel", "hour wheel", S.Z_MINUTE_P,
               S.Z_CANNON / S.Z_MINUTE_W / e2c))
    for pa, pb, z, per_escape in meshes:
        escape_deg = 360.0 / z / per_escape
        beats = escape_deg / abs(s["window"]["step_deg"])
        worst = 0.0
        for i in range(samples):
            b = 180.0 * round(beats * i / (samples - 1))
            a.set("beat axis", b)
            worst = max(worst, a.interference(pa, pb))
        out[f"{pa} / {pb}"] = {"pitch_escape_deg": escape_deg,
                               "worst_mm3": worst}
    a.set("beat axis", 0.0)
    return out


def click_check(a):
    """Wound the winding way the drum turns and the hands stay; pushed the
    going way on its own, the click holds it at its mark."""
    a.set("beat axis", 0.0)
    r0 = a.rotations()
    a.set("drum on centre wheel", r0["drum on centre wheel"] + 30.0)
    r1 = a.rotations()
    wound = r1["drum on centre wheel"] - r0["drum on centre wheel"]
    hands = abs(r1["centre wheel axis"] - r0["centre wheel axis"]) + \
        abs(r1["hour wheel axis"] - r0["hour wheel axis"])
    a.set("drum on centre wheel", r1["drum on centre wheel"] - 30.0)
    r2 = a.rotations()
    held = r2["drum on centre wheel"] - r1["drum on centre wheel"]
    bad = []
    if abs(wound - 30.0) > 1e-6 or hands > 1e-9:
        bad.append(f"winding 30 deg turned the drum {wound:.3f} and the "
                   f"hands {hands:.6f}")
    if abs(held) > 1e-6:
        bad.append(f"pushed the going way the drum moved {held:.3f} deg")
    return {"wound_deg": wound, "hands_moved_deg": hands,
            "held_deg": held}, bad


def lock_distance(a, s):
    """The resting tooth's distance from its locking face, at the end of
    each swing: the rest gap, both pallets."""
    out = {}
    for b, side in ((0.0, "left"), (180.0, "right")):
        a.set("beat axis", b)
        r = a.d.call("measure", a={"occurrence": a.occ["escape wheel"]},
                     b={"occurrence": a.occ["anchor"]})
        out[side] = r.get("distance_mm", r.get("distance"))
    a.set("beat axis", 0.0)
    return out


# --- FEM ----------------------------------------------------------------------------

def run_study(d, sid, timeout_s):
    run = d.call("run_study", study=sid, **{"async": True})
    t0 = time.monotonic()
    while d.call("study_status", study=sid).get("running"):
        if time.monotonic() - t0 > timeout_s:
            d.call("cancel_study", study=sid)
            raise SystemExit(f"clock: a study ran past {timeout_s:.0f} s")
        time.sleep(0.5)
    st = d.call("study_status", study=sid)
    if st.get("error") or not st.get("has_results"):
        raise SystemExit(f"clock: the study did not solve: {st}")
    r = d.call("study_results", study=sid)
    r["reused"] = bool(run.get("reused"))
    return r


def faces(d, part):
    d.call("set_active_part", part=part)
    return d.call("list_elements")["bodies"][0]["faces"]


# A printed pin's factor against its interlayer strength must be at least
# this, or the run stops: the hook's design factor (generators/hook).
PIN_NEED = 2.0


def fem(d, s, weight_g, timeout_s):
    """Two static studies, each solved once: the pendulum rod holding the
    bob and nut by its D (the weight hung from its lower end, the D's faces
    held), and the centre arbor in its two pivots with the cord's pull, the
    weight's, on the drum's seat."""
    out = {}
    rod = s["pend"]
    mass_bn = sum(m for p in ("bob", "rating nut")
                  for m, _, _ in body_props(d, p))
    fs = faces(d, "pendulum rod")
    # The D it is pressed on by: the bore's round and its flat.
    held = [f["name"] for f in fs
            if (f["kind"] == "cylinder" and
                abs(f.get("radius", 0) - s["press_d"] / 2.0) < 1e-3) or
            (f["kind"] == "plane" and math.hypot(*f["centroid"][:2]) <
             s["press_d"] / 2.0 and abs(f["centroid"][1]) < s["press_d"])]
    end = [f["name"] for f in fs if f["kind"] == "plane" and
           abs(f["centroid"][1] + rod["length"]) < 1e-3]
    if not held or len(end) != 1:
        raise SystemExit(f"clock: the rod's study faces: {held}, {end}")
    st = d.call("add_study", type="static", name="rod under its bob")
    sid = st.get("id", st.get("study"))
    for f in held:
        d.call("add_load", study=sid, type="fixed", face=f)
    n_bob = mass_bn / 1000.0 * S.G
    d.call("add_load", study=sid, type="force", face=end[0],
           magnitude=n_bob, direction=[0.0, -1.0, 0.0])
    t0 = time.perf_counter()
    r = run_study(d, sid, timeout_s)
    out["rod"] = {"load_n": n_bob, "held_faces": len(held),
                  "max_von_mises_mpa": r.get("max_von_mises_mpa"),
                  "max_displacement_mm": r.get("max_displacement_mm"),
                  "max_principal_mpa": r.get("max_principal_mpa"),
                  "factor_of_safety": r.get("factor_of_safety"),
                  "nodes": r.get("nodes"), "reused": r["reused"],
                  "seconds": round(time.perf_counter() - t0, 1)}
    # The arbor: the cord pulls the drum, and the drum the arbor, straight
    # down, on the band the drum turns on; the pivots hold it, each the
    # band of the round in its plate. One round face carries all three, so
    # each is a REGION of it
    # (docs/simulation/README.md#loads-on-part-of-a-face): no step on the arbor makes the seat a face of its own. It is a
    # printed pin lying on its flat, so its layers run along it, up +X:
    # the interlayer factor is what the receipt states (_lib.pin_strength).
    z = s["z"]
    H, t = s["H"], s["t_plate"]
    fs = faces(d, "centre arbor")
    # Flatted on two sides (its D and the flat it lies on), a round is its
    # two arcs, each a face: the seat and the pivots are bands of them, the
    # pull shared by their areas. The back pivot is the slide D the centre
    # wheel passes (clock_spec.arbor_plan), a round of its own radius.
    secs = s["arbors"]["centre arbor"][2]
    radius = {"round": s["arbor_d"] / 2.0, "press": s["arbor_d"] / 2.0,
              "slide": s["slide_r"]}

    def rounds_at(z0, z1):
        ks = {k for a, b, k in secs if a < z1 - 1e-9 and b > z0 + 1e-9}
        rs = {radius[k] for k in ks}
        if len(rs) != 1:
            raise SystemExit(f"clock: the centre arbor over {z0:g}..{z1:g} "
                             f"is not one round: {sorted(ks)}")
        r = rs.pop()
        out = [f for f in fs if f["kind"] == "cylinder" and
               abs(f.get("radius", 0) - r) < 1e-6]
        if not 1 <= len(out) <= 2:
            raise SystemExit(f"clock: the arbor's round of radius {r:g} is "
                             f"{[f['name'] for f in out]}, not its arcs")
        return out
    rounds = rounds_at(*z["drum"])
    areas = [f.get("area_mm2") or f.get("area") or 1.0 for f in rounds]
    shares = [(f["name"], a / sum(areas)) for f, a in zip(rounds, areas)]
    band = lambda z0, z1: {"normal": [0.0, 0.0, 1.0], "from_mm": z0,  # noqa
                           "to_mm": z1}
    pull = weight_g / 1000.0 * S.G
    t0 = time.perf_counter()
    pin = pin_strength(
        d, "centre arbor", shares, pull, [0.0, -1.0, 0.0],
        seat=[{"face": f["name"], "region": band(z0, z1)}
              for z0, z1 in ((-t, 0.0), (H, H + t))
              for f in rounds_at(z0, z1)],
        layer_normal=P.LIE_UP, region=band(*z["drum"]),
        name="centre arbor under the weight", need=PIN_NEED,
        timeout_s=timeout_s)
    out["centre arbor"] = {
        "load_n": pull, "weight_g": weight_g,
        "seat_band_mm": list(z["drum"]),
        "pivot_bands_mm": [[-t, 0.0], [H, H + t]],
        "max_von_mises_mpa": pin["max_von_mises_mpa"],
        "max_displacement_mm": pin["max_displacement_mm"],
        "max_principal_mpa": pin["max_principal_mpa"],
        "factor_of_safety": pin["factor_of_safety"],
        "pin": pin, "nodes": pin["nodes"], "reused": pin["reused"],
        "seconds": round(time.perf_counter() - t0, 1)}
    return out


# --- print plates ---------------------------------------------------------------------

def bbox(d, part):
    r = d.call("mass_properties", part=part)
    lo = [min(b["bbox_min"][i] for b in r["bodies"]) for i in range(3)]
    hi = [max(b["bbox_max"][i] for b in r["bodies"]) for i in range(3)]
    return lo, hi


def rot_quat(kind):
    """The print orientation: as modelled; stood on end (the rod's axis,
    Y, up); or lying (a shaft's axis, Z, along the bed's -X and its flat,
    at -X, down: a quarter turn back about Y)."""
    if kind == "lying":
        h = math.radians(-90.0) / 2.0
        return [0.0, math.sin(h), 0.0, math.cos(h)]
    if kind == "up":
        h = math.radians(90.0) / 2.0
        return [math.sin(h), 0.0, 0.0, math.cos(h)]
    return [0.0, 0.0, 0.0, 1.0]


def rotated_box(lo, hi, kind):
    if kind == "lying":    # (x, y, z) -> (-z, y, x)
        return ([-hi[2], lo[1], lo[0]], [-lo[2], hi[1], hi[0]])
    if kind == "up":       # (x, y, z) -> (x, -z, y)
        return ([lo[0], -hi[2], lo[1]], [hi[0], -lo[2], hi[1]])
    return lo, hi


def pack(d, pieces, spacing):
    """Shelf packing into beds of BED_MM: each piece (part, label, kind)
    by its footprint in print orientation, tallest rows first. Returns
    beds, each a list of (part, label, kind, (x, y) of its footprint's low
    corner, its rotated box)."""
    boxes = []
    for part, label, kind in pieces:
        lo, hi = rotated_box(*bbox(d, part), kind)
        boxes.append((part, label, kind, lo, hi))
    boxes.sort(key=lambda b: -(b[4][1] - b[3][1]))
    usable = BED_MM - 2.0 * BED_MARGIN
    # The bed's keep-out zones, in the packing's frame (the margin off).
    zones = [[z[0] - BED_MARGIN, z[1] - BED_MARGIN, z[2] - BED_MARGIN,
              z[3] - BED_MARGIN] for z in BED_ZONES]

    def free(x, y, w, h):
        return clear_x(x, y, w, h, zones, spacing)
    beds = []
    for box in boxes:
        w, h = box[4][0] - box[3][0], box[4][1] - box[3][1]
        if w > usable or h > usable:
            raise SystemExit(f"clock: {box[1]} ({w:.0f} x {h:.0f}) does not "
                             f"fit the {BED_MM:g} mm bed")
        placed = False
        for bed in beds:
            for row in bed["rows"]:
                x = free(row["x"], row["y"], w, h)
                if x + w <= usable + 1e-9 and h <= row["h"] + 1e-9:
                    bed["items"].append((box, x, row["y"]))
                    row["x"] = x + w + spacing
                    placed = True
                    break
            if placed:
                break
            x = free(0.0, bed["y"], w, h)
            if bed["y"] + h <= usable + 1e-9 and x + w <= usable + 1e-9:
                bed["rows"].append({"x": x + w + spacing, "y": bed["y"],
                                    "h": h})
                bed["items"].append((box, x, bed["y"]))
                bed["y"] += h + spacing
                placed = True
                break
        if not placed:
            x = free(0.0, 0.0, w, h)
            beds.append({"rows": [{"x": x + w + spacing, "y": 0.0, "h": h}],
                         "items": [(box, x, 0.0)], "y": h + spacing})
    return [[(b[0], b[1], b[2], (BED_MARGIN + x, BED_MARGIN + y), b[3])
             for b, x, y in bed["items"]] for bed in beds]


def pendulum_bed(d, shafts=(), spacing=0.0):
    """The rod corner to corner (it is longer than the bed), from the
    front right to the back left so it stays clear of the keep-out zone in
    the front-left corner, and the bob and the rating nut in the two
    corners it leaves: the bob at the back right, the nut at the front
    left, past the zone. `shafts` (part, label, kind) lie in rows in the
    front-left half, above the nut and clear of the rod by `spacing`."""
    lo, hi = bbox(d, "pendulum rod")
    out = [("pendulum rod", "pendulum rod", "diag", None, (lo, hi))]
    # The rod's centre line is the diagonal x + y = BED_MM; a point is
    # clear of it by its distance to that line less the rod's half-width.
    half_w = (hi[0] - lo[0]) / 2.0
    rl, rh = rotated_box(*bbox(d, "rating nut"), "up")
    y = BED_MARGIN + (rh[1] - rl[1]) + spacing
    boxes = sorted(((part, label, kind) + rotated_box(*bbox(d, part), kind)
                    for part, label, kind in shafts),
                   key=lambda b: -(b[4][1] - b[3][1]))
    for part, label, kind, blo, bhi in boxes:
        w, h = bhi[0] - blo[0], bhi[1] - blo[1]
        x = clear_x(BED_MARGIN, y, w, h, BED_ZONES, spacing)
        if (BED_MM - (x + w) - (y + h)) / math.sqrt(2.0) < half_w + spacing:
            raise SystemExit(f"clock: {label} does not fit beside the "
                             "pendulum rod")
        out.append((part, label, kind, (x, y), blo))
        y += h + spacing
    for part, corner in (("bob", "top-right"), ("rating nut", "bottom-left")):
        rl, rh = rotated_box(*bbox(d, part), "up")
        w, h = rh[0] - rl[0], rh[1] - rl[1]
        if corner == "top-right":
            x, y = BED_MM - BED_MARGIN - w, BED_MM - BED_MARGIN - h
        else:
            y = BED_MARGIN
            x = clear_x(BED_MARGIN, y, w, h, BED_ZONES, S.lines(10))
        out.append((part, part, "up", (x, y), rl))
    return out


def make_bed(d, proc, name, items):
    """A print plate: a Derived copy of every piece where it prints."""
    ids = {p["name"]: p["id"] for p in d.call("list_parts")["parts"]
           if p["type"] == "part"}
    d.call("add_part", name=name)
    d.call("set_print_process", **proc)
    rows = []
    for part, label, kind, at, lo in items:
        if kind == "diag":
            blo, bhi = lo
            # The rod's Y along the bed's diagonal: turned 45 degrees about
            # Z (its top, +Y, towards the back left corner, away from the
            # front-left keep-out zone), centred on the bed.
            h = math.radians(45.0) / 2.0
            q = [0.0, 0.0, math.sin(h), math.cos(h)]
            cx, cy = (blo[0] + bhi[0]) / 2.0, (blo[1] + bhi[1]) / 2.0
            c, sn = math.cos(math.radians(45.0)), math.sin(math.radians(
                45.0))
            rx, ry = c * cx - sn * cy, sn * cx + c * cy
            pos = [BED_MM / 2.0 - rx, BED_MM / 2.0 - ry, -blo[2]]
            orient = "flat on its back face, diagonal"
        else:
            q = rot_quat(kind)
            pos = [at[0] - lo[0], at[1] - lo[1], -lo[2]]
            orient = {"up": "stood on its lower end, the rod's axis up",
                      "lying": "lying on its flat, its axis along the bed",
                      "flat": "as modelled, its back face down"}[kind]
        r = d.call("add_feature", feature={
            "type": "Derived", "name": label, "source_part": ids[part],
            "reference": False,
            "placement": {"position": pos, "orientation": q}})
        if not r.get("regen", {}).get("ok", True):
            raise SystemExit(f"clock: {label} on {name}: {r['regen']}")
        rows.append({"piece": label, "part": part, "orientation": orient,
                     "at_mm": [round(v, 2) for v in pos[:2]]})
    return rows


# The pieces that moved off stage 1's plate 1 onto its plate 2, lying: the
# card names them.
MOVED = ("pillar 1", "pillar 2", "pillar 3", "pillar 4", "anchor arbor",
         "escape arbor (stage 1)")
# Stage 1's pieces whose shape changed when its arbors were made assemblable
# (2026-10-18, docs/inspect/README.md#insertion), and what changed: each
# reprinted on stage1_arbors/ replaces the piece of that name.
REPRINT1 = {
    "escape arbor (stage 1)": "its press D only under the escape wheel's "
    "teeth and pinion; a slide D from there out through the front, front "
    "pivot and drum band included, so the wheel and the drum go on from the "
    "front; round at the back pivot",
    "anchor arbor": "its press D only under the anchor's hub; a slide D from "
    "there out through the front pivot, so the anchor goes on from the "
    "front; the pendulum rod's D at the back unchanged",
    "drum (stage 1)": "its bore pressed on the escape arbor's slide D, the "
    "light press it had on the D",
    "front plate (stage 1)": "its escape and anchor pivot holes the gap "
    "over the slide-D pivots, smaller",
}


def old_plate1(d, proc, pieces, sp):
    """The negative control: stage 1's plate 1 as it was printed, the
    pillars and arbors stood on end among the rest. check_print must fail
    it on slender, on exactly those six, and pass every other item; the
    plate is then dropped."""
    beds = pack(d, pieces, sp)
    if len(beds) != 1:
        raise SystemExit(f"clock: the old plate 1 packs onto {len(beds)} "
                         "plates")
    name = "stage1 plate 1 (as printed, shafts standing)"
    make_bed(d, proc, name, beds[0])
    others = tuple(k for k in PRINT_ITEMS if k != "slender")
    pr = check_print(d, f"  {name}", need=others, quiet=True)
    sl = pr["slender"]
    # A plate's bodies carry their sources' names; each is the Derived
    # feature that placed it, by its label.
    feats = {f["id"]: f["name"] for f in d.call("get_state")["features"]}
    bodies = d.call("list_elements")["bodies"]

    def label(p):
        return feats[bodies[p["where"]["body"]]["created_by"]]
    over = sorted(label(p) for p in sl["pieces"] if p["over"])
    if sl["status"] != "fail" or over != sorted(MOVED):
        raise SystemExit(f"clock: the old plate 1 should fail slender on "
                         f"{sorted(MOVED)}, not {over}: {sl}")
    worst = ", ".join(f"{label(p)} {p['worst_ratio']:.1f}:1"
                      for p in sl["pieces"] if p["over"])
    print(f"  negative control, {name}: slender fails on the six shafts "
          f"({worst}), as it must; every other item passes")
    d.call("delete_part", part=name)
    return pr


def print_plates(d, s, proc, out):
    """Every piece on a bed, stage 1's in stage1/ and what stage 2 adds in
    stage2/; every plate's check_print passes every item, then its STL."""
    sp = S.lines(10)
    stage1 = ([("back plate (stage 1)", "back plate (stage 1)", "flat"),
               ("front plate (stage 1)", "front plate (stage 1)", "flat")] +
              [("pillar nut", f"pillar nut {k + 1}", "flat")
               for k in range(8)] +
              [(p, p, "flat") for p in ("escape wheel", "anchor",
                                        "drum (stage 1)")])
    # The shafts print lying, on the pendulum's plate: stood on end, on
    # plate 1 with the rest, they came off the bed. Plate 1 keeps every
    # other piece where it was, so a stage 1 printed from the old plate 1
    # is finished from plate 2 alone.
    shafts1 = ([("pillar", f"pillar {k + 1}", "lying") for k in range(4)] +
               [(p, p, "lying") for p in ("anchor arbor",
                                          "escape arbor (stage 1)")])
    old_plate1(d, proc, stage1 + [(p, lab, "flat")
                                  for p, lab, _ in shafts1], sp)
    stage2 = ([(p, p, "flat") for p in (
        "back plate", "front plate", "third wheel", "third spacer",
        "centre wheel", "centre spacer", "drum", "cannon pinion",
        "minute wheel", "hour wheel", "minute hand", "hour hand",
        "dial ring")] +
        [("dial spacer", f"dial spacer {k + 1}", "flat") for k in range(2)] +
        [(p, p, "lying") for p in ("escape arbor", "third arbor",
                                   "centre arbor")])
    plates, jobs = {}, {}
    # What stage 1 reprints: every stage-1 piece whose shape changed when
    # the arbors were made assemblable (REPRINT1), on a plate of its own.
    reprint = [next(x for x in stage1 + shafts1 if x[1] == p)
               for p in REPRINT1]
    for stage, pieces in (("stage1", stage1), ("stage2", stage2),
                          ("stage1_arbors", reprint)):
        beds = pack(d, pieces, sp)
        if stage == "stage1_arbors" and len(beds) != 1:
            raise SystemExit(f"clock: stage 1's reprint takes {len(beds)} "
                             "plates, not one")
        if stage == "stage1":
            if len(beds) != 1:
                raise SystemExit(f"clock: stage 1's frame takes {len(beds)} "
                                 "plates, not the one it was printed on")
            beds.append(pendulum_bed(d, shafts1, sp))
        (out / stage).mkdir(parents=True, exist_ok=True)
        names = []
        for k, items in enumerate(beds):
            name = f"{stage} plate {k + 1}"
            rows = make_bed(d, proc, name, items)
            geo = d.call("check_geometry")
            if geo["receipt"]["standing"] != "verified":
                raise SystemExit(f"clock: {name} is not verified: {geo}")
            pr = check_print(d, f"  {name}", need=PRINT_ITEMS, quiet=True)
            path = out / stage / f"plate{k + 1}.stl"
            exp = d.call("export_stl", path=str(path))
            total = d.call("mass_properties")["total"]
            plates[name] = {"stl": str(path), "pieces": rows,
                            "shells": exp.get("shells"),
                            "deflection_mm": exp.get("deflection_mm"),
                            "solid_g": total.get("mass_g"),
                            "printed_g": (total.get("printed") or {}).get(
                                "mass_g"),
                            "print": print_receipt(pr)}
            names.append(name)
            print(f"  {name}: {len(rows)} pieces, every check_print item "
                  f"passes ({print_item_line('stacking', pr['stacking'])})"
                  f" -> {path.name}")
        # One job a stage, every piece its own object with its settings: the
        # pendulum solid, the rest at the frame's.
        jobs[stage] = (job_receipt(export_print_job(
            d, [(n, n) for n in names],
            out / stage / f"clock_{stage}_job.3mf", "pla"))
            if "profile" in proc else
            {"skipped": "printed on the built-in table: no printer"})
    return plates, jobs


# --- the test card ---------------------------------------------------------------------

def assembly_lines(s, checks):
    """The card's assembly steps: each stage's sequence, the way each part
    goes on and how far it is pressed, as the sequence check read it."""
    out = []
    for stage in ("stage 1", "stage 2"):
        rows = (checks.get(f"{stage} sequence") or {}).get("steps") or {}
        out.append(f"{chr(10) if out else ''}**{stage.capitalize()}:**"
                   "\n")
        for x in SEQUENCE[stage]:
            name = step_name(x)
            r = rows.get(name) or {}
            way = {str(FRONT): "from the front", str(BACK): "from the back",
                   ROD: "up the rod"}.get(str(x.get("direction")),
                                          "from the front")
            if x.get("direction") is None:
                out.append(f"- {name}: the base")
                continue
            if "occurrences" in x:
                how = "together, as one"
            elif x.get("thread"):
                how = "screwed on"
            else:
                press = r.get("interfering_mm") or 0.0
                how = (f"pressed home the last {press:.1f} mm" if press > 0.0
                       else "slides on free")
            out.append(f"- {name}, {way}: {how}")
    return "\n".join(out)


def card(s, pred, plates, profile, checks):
    c, e = s["counts"], s["escape"]
    pend = s["pend"]
    rows = []
    for name, p in plates.items():
        for r in p["pieces"]:
            rows.append(f"| {Path(p['stl']).parent.name}/"
                        f"{Path(p['stl']).name} | {r['piece']} | "
                        f"{r['orientation']} |")
    return f"""# Clock test card

Period {s['period_s']:g} s (there and back; a beat every {s['period_s'] / 2:g} s),
profile {profile}. Predicted before printing, by build.py.

## Prediction

| what | predicted |
|---|---|
| period at the design swing (±{e['amplitude_deg']:.2f}°), printed as set below | {pred['period_s']:.5f} s |
| the same parts read solid | {pred['period_read_solid_s']:.5f} s |
| rate, as built | {pred['rate_s_day']:+.1f} s/day |
| a turn of the rating nut that lowers the bob | {pred['per_turn_s_day']:+.0f} s/day ({pred['per_notch_s_day']:+.1f} a notch of {s['nut']['notches']}) |
| a degree more swing either way | {pred['per_degree_s_day']:+.1f} s/day (circular error) |
| run per metre of drop (the first layer of cord) | {pred['hours_per_m']:.1f} h |
| torque at the escape wheel per 100 g, stage 2 | {pred['escape_nmm_per_100g']:.4f} N·mm |
| torque at the escape wheel per 100 g, stage 1 (drum on the escape arbor) | {pred['stage1_nmm_per_100g']:.3f} N·mm |

The pendulum: rod {pend['length']:.1f} mm from the anchor arbor to its lower end, bob
{pend['bob_w']:g} × {pend['bob_h']:g} × {s['bob_depth']:g} mm on the rating nut;
equivalent length {pred['equivalent_length_mm']:.1f} mm, {pred['pendulum_g']:.0f} g swinging
(rod, bob, nut, anchor and arbor, weighed as they print: the pendulum at
{pred['period_at']['pendulum']}, the anchor at {pred['period_at']['anchor']}). Escape wheel {e['Z']} teeth, train
{c['escape_pinion']}/{c['third']} and {c['third_pinion']}/{c['centre']} (escape to centre {c['escape_to_centre']:g}:1), motion
works {S.Z_CANNON}/{S.Z_MINUTE_W} and {S.Z_MINUTE_P}/{S.Z_HOUR}, module {s['module']:g}.

Friction is not modelled, so the weight is found on the bench (below). Turning
the rating nut clockwise seen from below raises the bob (a right-hand thread):
the clock gains.

## Print

The pendulum (rod, bob and rating nut) SOLID, 100 % rectilinear, as its
period is computed; every other part at Bambu Studio's 0.20 mm Standard (2
walls, 5/3 shells, 15 % grid). Each stage's print job (`clock_stage1_job.3mf`,
`clock_stage2_job.3mf`) carries those per object; printed from bare STLs, set
them by hand. 0.2 mm layers, no supports, no brim.

| plate | piece | orientation |
|---|---|---|
{chr(10).join(rows)}

**Moved since the first print (2026-10):** {", ".join(MOVED)} left stage 1's
plate 1 for its plate 2, where they print lying on a flat along their length
beside the pendulum (stood on end they came off the bed: {s['pillar_height']:.0f} mm on a
Ø{2 * s['pillar_foot_r']:.2f} end). Plate 1 keeps every other piece it had, so a stage 1 whose
plate 1 printed but for its shafts is finished from plate 2 alone. Each pillar thread
keeps {100 * s['pillar']['thread_engaged']:.0f} % of its turn above the flat; stage 2's arbors lie too.

**Reprint for stage 1 (2026-10-18):** a press-D bore could reach its D
only forced over a round pivot (the escape wheel bound halfway through its
hub). `stage1_arbors/` (`clock_stage1_arbors_job.3mf`) holds every stage-1
piece that changed; each replaces the piece of its name, and nothing else
of stage 1 changes:

{chr(10).join(f"- **{p}**: {why}" for p, why in REPRINT1.items())}

## Assembly

Every pressed or slid pair is checked to go on from the end named, slid
from clear to its seat (docs/inspect/README.md#insertion): each D a part is
pressed on runs only under that part, and from there out through the end
it goes on from the arbor is a D the running gap under the bore. Put each
part on its arbor before the arbor goes into the plates; turn the part
until its D meets the arbor's (it goes on no other way). Each arbor goes
into the back plate from the front, and the front plate on over the
arbors' front pivots.

{assembly_lines(s, checks)}

## Test

1. **Stage 1** (stage1/): the escape wheel, anchor and pendulum in the short
   frame, a weight on a cord round the drum on the escape arbor. Hang it
   level on a screw through the keyhole, set it in beat (tilt until the
   ticks are even), and find the least weight that keeps it ticking for five
   minutes.
2. **Stage 2** (stage2/, with stage 1's pillars, nuts, escape wheel, anchor
   and pendulum): the same escape torque needs about
   {c['escape_to_centre']:g} × the stage-1 weight before the train's friction.
   The jar (generators/jar/) holds about {pred['sand_g']:.0f} g of sand; more
   if filled with coins.
3. Run it an hour against a phone; then a day, and read the rate.

| stage | weight (g) | ran for | rate (s/day) | swing (°) | notes |
|---|---|---|---|---|---|
| 1 | | | | | |
| 2 | | | | | |
"""


# --- main ------------------------------------------------------------------------------

def main():
    global BED_MM, BED_ZONES
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int)
    ap.add_argument("--period", type=float, default=1.0,
                    help="the pendulum's period, there and back, s")
    ap.add_argument("--process", default="fdm")
    ap.add_argument("--material", default="pla")
    ap.add_argument("--profile", default=None,
                    help="one of your print profiles (list_print_profiles); "
                         "without it the built-in --process/--material table")
    ap.add_argument("--cord-mm", type=float, default=1.5,
                    help="the cord's diameter, mm: a measured input (the "
                         "drawer's cord), not a size of the clock")
    ap.add_argument("--drop-mm", type=float, default=None,
                    help="how far the weight can fall, mm: the run per "
                         "winding is reported for it")
    ap.add_argument("--fresh", action="store_true",
                    help="start a new project first (an untitled scratch one is "
                         "discarded; a saved one with unsaved work refused)")
    ap.add_argument("--fem-timeout", type=float, default=300.0,
                    help="the most one FEM study may take, s")
    ap.add_argument("--out", default=None)
    ap.add_argument("--no-render", action="store_true",
                    help="skip the stills (a --headless instance has no "
                         "renderer)")
    ap.add_argument("--video", action="store_true",
                    help="film stage 1's assembly sequence going together "
                         "to stage1_sequence.mp4 beside the card "
                         "(export_assembly_video; needs a renderer and "
                         "ffmpeg)")
    args = ap.parse_args()
    if args.video and args.no_render:
        raise SystemExit("clock: --video films with the renderer, which "
                         "--no-render says this instance has not")
    out = Path(args.out) if args.out else OUT
    out.mkdir(parents=True, exist_ok=True)
    t_start = time.perf_counter()

    d = connect(args.port)
    if args.fresh:
        fresh_project(d)
    proc = ({"profile": args.profile} if args.profile else
            {"process": args.process, "material": args.material})
    prof = {}
    if args.profile:
        prof = next((p for p in d.call("list_print_profiles")["profiles"]
                     if p["name"] == args.profile), None)
        if prof is None:
            raise SystemExit(f"clock: no print profile '{args.profile}'; "
                             "list_print_profiles names yours")
    if prof.get("bed_mm"):
        if prof["bed_mm"][0] != prof["bed_mm"][1]:
            raise SystemExit("clock: the packer fills a square bed; "
                             f"'{args.profile}' is {prof['bed_mm']}")
        BED_MM = float(prof["bed_mm"][0])
        BED_ZONES = bed_zones(prof)
    if prof.get("material", args.material) != "pla":
        raise SystemExit("clock: the parts are PLA (its density is in the "
                         "pendulum's sums)")
    layer = prof.get("layer_height_mm") or 0.2
    allow = Allow(d, proc)
    jar = jar_weight(allow)

    with phase("sizes"):
        s = S.solve(args.period, allow, layer, prof.get("z_gap_mm"),
                    jar["outer_d"], args.cord_mm)
        e = s["escape"]
        s["window"] = win = e["window"]
        c = s["counts"]
        print(f"clock: {args.period:g} s, escape wheel {e['Z']} teeth "
              f"R{e['R']:g}, train {c['escape_pinion']}/{c['third']} "
              f"{c['third_pinion']}/{c['centre']} (escape to centre "
              f"{c['escape_to_centre']:g}:1), module {s['module']:g}, "
              f"plates {s['H']:g} apart, gap {s['g']:g}/{s['g_small']:g} "
              f"a side ({args.profile or args.material})")
        print(f"  escapement: lift {e['lift_deg']:.2f} deg, lock "
              f"{e['lock_deg']:.2f}, swing ±{e['amplitude_deg']:.2f}, "
              f"pallet {e['pallet_w']:.2f} mm, drop {e['drop_mm']:.2f}, "
              f"teeth {e['tooth_h']:.1f} deep; the wheel steps over "
              f"{win['start_deg']:.1f}-{win['start_deg'] + win['duration_deg']:.1f}"
              f" deg of each beat")
        print(f"  the swing: the impulse and the lock and "
              f"{e['supplementary_locks']:g} lock(s) past them, the wheel "
              f"following the pallets by a table of {len(win['table'])} "
              f"points ({'; '.join(e['swing_tried']) or 'the first tried'})")
        sweep = e["sweep"]
        rest = [round(dm, 4) for _, dm, _ in sweep["ends"]]
        print(f"  the escapement's own sweep: {sweep['min_mm']:.3f} mm least "
              f"at beat {sweep['at_beat_deg']:.0f} deg, locked "
              f"{rest} mm off the face at each beat's end")
        if sweep["min_mm"] < 2.0 * s["g"] / 5.0 or any(
                abs(r - e["rest_gap_mm"]) > 0.02 for r in rest):
            raise SystemExit("clock: the escapement's polygons do not clear "
                             "or do not lock")
        deep = S.escapement_sweep(e, win, 45, 2,
                                  deeper_deg=2.0 * e["lock_deg"])
        print(f"  ... with the pallets set {2 * e['lock_deg']:.2f} deg too "
              f"deep: {deep['min_mm']:.3f} mm (must be 0, overlapping)")
        if deep["min_mm"] > 0.0:
            raise SystemExit("clock: a pallet set too deep clears in the "
                             "sweep")

    with phase("parts"):
        build_parts(d, s, proc)
        cp = find_length(d, s, proc, args.period)
        n_parts = verify_parts(d)
        print(f"  {n_parts} parts, every one verified")
        d.call("save_project", path=str(out / "clock.dsgn"))


    clash = s["g"] / 5.0
    checks = {"clash_deflection_mm": clash, "escapement_sweep": {
        "min_mm": sweep["min_mm"], "at_beat_deg": sweep["at_beat_deg"],
        "locked_mm": rest, "too_deep_min_mm": deep["min_mm"]}}
    with phase("stage 1"):
        a1, w1 = build_stage1(d, s)
        checks["stage 1 nut windows deg"] = w1
        dist = lock_distance(a1, s)
        print(f"  locked: {dist} mm off the locking faces")
        checks["stage 1 locked mm"] = dist
        r = run_motion(a1, "one period, 2 deg a sample", "beat axis", 0.0,
                       360.0, 181, clash)
        bad = clashing(r)
        print(f"  one period at 2 deg: {r['collision_status']} {bad}")
        checks["stage 1 period"] = {"status": r["collision_status"],
                                    "clashing": bad}
        if not r["clean"]:
            raise SystemExit(f"clock: stage 1 clashes over a period: {bad}")
        n = 60
        r = run_motion(a1, "one escape-wheel turn, 4 samples a beat",
                       "beat axis", 0.0, 180.0 * n, 4 * n + 1, clash)
        lock, ends = beat_ends(r, s)
        print(f"  one escape-wheel turn ({n} beats, 4 a beat): "
              f"{r['collision_status']} {clashing(r)}; at {ends} beat ends "
              f"the wheel stands {lock:.2e} deg off its step")
        checks["stage 1 turn"] = {"status": r["collision_status"],
                                  "clashing": clashing(r),
                                  "beat_ends": ends, "off_step_deg": lock}
        if not r["clean"] or lock > 1e-6 or ends != n + 1:
            raise SystemExit("clock: stage 1 over a turn")
        # Negative control: the same frame with the pallets too deep.
        ad = Asm(d, "stage 1 (pallets too deep)")
        ad.fixed("back plate (stage 1)", "back plate")
        ad.turn("back plate", "beat", "beat", "beat")
        d.call("set_occurrence_visible", occurrence=ad.occ["beat"],
               visible=False)
        ad.turn("back plate", "escape", "escape wheel", "escape wheel")
        ad.turn("back plate", "anchor", "anchor (pallets too deep)",
                "anchor")
        escapement_relations(ad, s)
        r = run_motion(ad, "control: pallets too deep", "beat axis", 0.0,
                       360.0, 91, clash)
        bad = clashing(r)
        print(f"  negative control, pallets {2 * e['lock_deg']:.2f} deg too "
              f"deep: {r['collision_status']} {bad}")
        checks["control: pallets too deep"] = {"status": r["collision_status"],
                                               "clashing": bad}
        if r["clean"] or ("anchor", "escape wheel") not in bad:
            raise SystemExit("clock: a pallet set too deep does not clash: "
                             "the study cannot see the escapement")
        checks["stage 1 sequence"] = sequence(a1, s)
        checks["control: first stage-1 escape arbor"] = \
            first_arbor_control(d, s)

    with phase("stage 2"):
        a2, w2 = build_stage2(d, s)
        checks["stage 2 nut windows deg"] = w2
        n = 60
        r = run_motion(a2, "one escape-wheel turn, 4 samples a beat",
                       "beat axis", 0.0, 180.0 * n, 4 * n + 1, clash)
        lock, ends = beat_ends(r, s)
        turned, want, bad = check_ratios(r, s)
        print(f"  one escape-wheel turn: {r['collision_status']} "
              f"{clashing(r)}; locked at {ends} beat ends "
              f"({lock:.2e} deg off)")
        for k in turned:
            print(f"    {k}: {turned[k]:+.6f} deg (counts: {want[k]:+.6f})")
        checks["stage 2 turn"] = {"status": r["collision_status"],
                                  "clashing": clashing(r),
                                  "beat_ends": ends, "off_step_deg": lock,
                                  "turned_deg": turned, "want_deg": want}
        if not r["clean"] or lock > 1e-6 or bad:
            raise SystemExit(f"clock: stage 2 over a turn: {bad} "
                             f"{clashing(r)}")
        meshes = mesh_sweeps(a2, s)
        for k, v in meshes.items():
            print(f"  {k}, a pitch exact: {v['worst_mm3']:.4f} mm3")
        checks["meshes"] = meshes
        if any(v["worst_mm3"] > 0.0 for v in meshes.values()):
            raise SystemExit(f"clock: a mesh interferes: {meshes}")
        ck, bad = click_check(a2)
        print(f"  click: wound {ck['wound_deg']:.1f} deg, the hands moved "
              f"{ck['hands_moved_deg']:.2e}; pushed the going way it moved "
              f"{ck['held_deg']:.2e}")
        checks["click"] = ck
        if bad:
            raise SystemExit(f"clock: the click: {bad}")
        checks["stage 2 sequence"] = sequence(a2, s)
        # Negative control: one tooth wrong on the third wheel.
        rid = a2.rel["escape pinion drives the third wheel"]
        d.call("set_relation", id=rid, teeth_a=c["escape_pinion"],
               teeth_b=c["third"] + 1)
        r = run_motion(a2, "control: a third wheel of one tooth more",
                       "beat axis", 0.0, 180.0 * n, 2, clash,
                       check_clash=False)
        d.call("set_relation", id=rid, teeth_a=c["escape_pinion"],
               teeth_b=c["third"])
        d.call("solve_assembly")
        _, _, bad = check_ratios(r, s)
        print(f"  negative control, {c['third'] + 1} teeth on the third "
              f"wheel: {'fails, as it must' if bad else 'PASSES (bad)'}: "
              f"{bad[:1]}")
        checks["control: one tooth wrong"] = bad
        if not bad:
            raise SystemExit("clock: a wrong tooth count passes the ratio "
                             "check")

    with phase("predictions"):
        # The period is the printed pendulum's; the same parts read solid
        # say what the infill moves.
        WEIGH["as"] = "solid"
        cp_solid = S.compound_period(swinging(d), e["amplitude_deg"])
        WEIGH["as"] = "printed"
        pend_print = d.call("get_print_settings", part="bob")["bodies"][0]
        print(f"  the period at the pendulum's {pend_print['described']}: "
              f"{cp['period_s']:.6f} s (read solid, "
              f"{cp_solid['period_s']:.6f} s)")
        p_turn = rating_per_turn(d, s, cp)
        amp = e["amplitude_deg"]
        cp1 = S.compound_period(swinging(d), amp + 1.0)
        r_cord = s["drum"]["r_cord"]
        pred = {
            "period_s": cp["period_s"], "small_swing_s": cp["small_swing_s"],
            "period_at": {"pendulum": pend_print["described"],
                          "anchor": d.call("get_print_settings",
                                           part="anchor")["bodies"][0][
                                               "described"],
                          "weighed": "printed (mass_properties' estimate)"},
            "period_read_solid_s": cp_solid["period_s"],
            "rate_s_day": -86400.0 * (cp["period_s"] - args.period)
            / args.period,
            "per_turn_s_day": p_turn,
            "per_notch_s_day": p_turn / s["nut"]["notches"],
            "per_degree_s_day": -86400.0 * (cp1["period_s"] - cp["period_s"])
            / cp["period_s"],
            "equivalent_length_mm": cp["equivalent_length_mm"],
            "pendulum_g": cp["mass_g"],
            "hours_per_m": 1000.0 / (2.0 * math.pi * r_cord),
            "drop_mm": args.drop_mm,
            "run_h": (args.drop_mm / (2.0 * math.pi * r_cord)
                      if args.drop_mm else None),
            "cord_one_layer_mm": s["drum"]["core"] / args.cord_mm * 2.0
            * math.pi * r_cord,
            "escape_nmm_per_100g": 0.1 * S.G * r_cord
            / c["escape_to_centre"],
            "stage1_nmm_per_100g": 0.1 * S.G * r_cord,
            "sand_g": jar["sand_g"], "jar_od_mm": jar["outer_d"],
            "weight_air_mm": s["weight_air_mm"]}
        for k, v in pred.items():
            print(f"  {k}: {v}")

    with phase("FEM"):
        fem_r = fem(d, s, jar["sand_g"], args.fem_timeout)
        for k, v in fem_r.items():
            print(f"  {k}: {v['max_von_mises_mpa']} MPa von Mises, "
                  f"{v['max_displacement_mm']} mm, FoS "
                  f"{v['factor_of_safety']} ({v['seconds']} s"
                  f"{', reused' if v['reused'] else ''})")

    with phase("print plates"):
        plates, jobs = print_plates(d, s, proc, out)
        solid = sum(p["solid_g"] or 0.0 for p in plates.values())
        printed = [p["printed_g"] for p in plates.values()]
        weight = {"solid_g": solid,
                  "printed_g": sum(printed) if all(printed) else None}
        if weight["printed_g"]:
            weight["printed_share"] = weight["printed_g"] / solid
            print(f"  the clock prints {weight['printed_g']:.0f} g against "
                  f"{solid:.0f} g solid "
                  f"({100 * weight['printed_share']:.0f} %)")

    if not args.no_render:
        with phase("renders"):
            # Seen from the front and a little to the right and above, the
            # movement frame's Y up (the app's world is Z-up; the clock is
            # modelled in its own frame, clock_spec's docstring).
            y_mid = (s["pos"]["anchor"][1] - s["pend"]["length"]) / 2.0
            span = s["pos"]["anchor"][1] + s["pend"]["length"]
            for a, fn in ((a1, "stage1.png"), (a2, "stage2.png")):
                a.activate()
                d.call("set_camera", target=[0.0, y_mid, s["H"] / 2.0],
                       eye=[0.9 * span, y_mid + 0.6 * span, 3.2 * span],
                       up=[0.0, 1.0, 0.0])
                d.call("render_still", path=str(out / fn), width=1200,
                       height=1600)
                if args.video and a is a1:
                    # From the same side, fitted to the film's own size:
                    # each part appears at its step and goes home along the
                    # path the check took (docs/assemblies/README.md
                    # #playing-and-filming-a-sequence). Stage 2 waits on a
                    # step that places two parts together.
                    d.call("fit_view", width=720, height=960)
                    v = d.call("export_assembly_video",
                               path=str(out / "stage1_sequence.mp4"),
                               width=720, height=960, fps=30,
                               _timeout=3600)
                    print(f"  stage 1 sequence film: {v['frames']} frames, "
                          f"{v['bytes'] / 1e6:.1f} MB in {v['seconds']:.0f} "
                          f"s -> stage1_sequence.mp4")

    a2.activate()
    d.call("save_project", path=str(out / "clock.dsgn"))
    (out / "clock_card.md").write_text(card(s, pred, plates,
                                            args.profile or args.material,
                                            checks))
    spec = {k: v for k, v in s.items() if k not in ("gears",)}
    spec["gears"] = {k: {kk: vv for kk, vv in g.items()}
                     for k, g in s["gears"].items()}
    receipt = {"period_s": args.period, "process": proc,
               "allowances": {f"{k}@{sz}": v for (k, sz), v in
                              allow.cache.items()},
               "jar": jar, "sizes": spec, "predictions": pred,
               "checks": checks, "fem": fem_r, "plates": plates,
               "jobs": jobs, "weight": weight,
               "confirm": allow.confirm,
               "seconds": round(time.perf_counter() - t_start, 1),
               "phases": {n: round(t, 1) for n, t in PHASES}}
    (out / "clock.json").write_text(json.dumps(receipt, indent=1,
                                               default=str))
    total = sum(t for _, t in PHASES)
    print(f"time: {total:.0f} s; " + ", ".join(f"{n} {t:.0f}"
                                               for n, t in PHASES))
    print(f"clock: done, {len(plates)} plates -> {out}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SdkError as exc:
        raise SystemExit(f"clock: {exc}")
