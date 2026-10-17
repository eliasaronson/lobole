#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A printed inline four: a flat-plane crank in a split block, rods with
lapped caps on pressed pins, pistons on pressed wrist pins, a single
overhead cam of pressed lobes on bucket followers, eight valves closed by
TPU bellows springs, a printed TPU toothed belt at 2:1, a flywheel with
TDC marks and a hand crank. Everything printed, nothing bought.

    tools/headless-preview.sh --start <port>
    python3 generators/engine/build.py --port <port> --profile "K5 PLA on P1S"
    python3 generators/engine/build.py --port <port> --profile "K5 PLA on P1S" --bore 32

Every size derives from `--bore` and the print profile; without --bore the
bore is searched for: the largest whole millimetre whose PLA pieces pack
into two of the profile's beds. What it builds, checks and predicts:
generators/engine/README.md.

Output (generated, untracked): scratch/engine/ (or $LOBOLE_SCRATCH/engine,
or --out) -- engine.dsgn, engine.json (the receipt), engine_card.md (the
test card), plate1.stl and plate2.stl (PLA), plate_tpu.stl, and stills.
"""
import argparse
import json
import math
import os
import sys
import time
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import engine_parts as P  # noqa: E402
import engine_spec as S  # noqa: E402
from _lib import (PRINT_ITEMS, bed_zones, check, check_print,  # noqa: E402
                  export_print_job, fresh_project, job_receipt,
                  print_item_line, print_receipt)
from lobole.sdk import SdkError, connect  # noqa: E402

ROOT = Path(HERE).resolve().parent.parent
OUT = Path(os.environ.get("LOBOLE_SCRATCH", ROOT / "scratch")) / "engine"
PHASES = []


class phase:
    def __init__(self, name):
        self.name = name

    def __enter__(self):
        self.t = time.perf_counter()
        print(f"[{self.name}]", flush=True)

    def __exit__(self, *exc):
        PHASES.append((self.name, time.perf_counter() - self.t))


def fail(msg):
    raise SystemExit(f"engine: {msg}")


# --- parts --------------------------------------------------------------------------

def build_parts(d, s, proc):
    """Every part, each verified. Returns {name: Part}."""
    parts = {}

    def add(p):
        parts[p.name] = p
    add(P.block(d, s, proc))
    add(P.bedplate(d, s, proc))
    add(P.sump(d, s, proc))
    add(P.head(d, s, proc))
    add(P.cam_cover(d, s, proc))
    add(P.crankshaft(d, s, proc))
    add(P.rod(d, s, proc))
    add(P.rod_cap(d, s, proc))
    add(P.pin_x(d, s, proc, "cap pin", s.d_cp, s.w_r))
    add(P.piston(d, s, proc))
    add(P.pin_x(d, s, proc, "wrist pin", s.d_wp, s.l_wp))
    add(P.valve(d, s, proc))
    add(P.bucket(d, s, proc))
    add(P.cam_bar(d, s, proc))
    for i, lb in enumerate(s.lobes):
        add(P.lobe(d, s, proc, i, lb))
    g = s.belt_gr
    cp, r_tip2, _ = P.pulley(
        d, s, proc, "cam pulley", s.r_p2, s.z2, s.phase2,
        (s.x_belt0 - g, s.x_belt1 + g), (s.x_f - g - s.t_fl, s.x_f - g),
        None, P.d_shape(s.r_bar), P.X_DOWN)
    cp.press(s.d_bar)
    add(cp)
    kp, r_tip1, _ = P.pulley(
        d, s, proc, "crank pulley", s.r_p1, s.z1, s.phase1,
        (s.x_belt0 - g, s.x_belt1 + g),
        (s.x_belt0 - g - s.t_fl, s.x_belt0 - g),
        (s.x_belt1 + g, s.x_f - s.g_ax), P.keel(s.r_stub, s.h_flat),
        P.X_UP)
    kp.press(2.0 * s.r_stub)
    add(kp)
    add(P.cam_collar(d, s, proc))
    add(P.flywheel(d, s, proc))
    add(P.crank_arm(d, s, proc))
    add(P.handle(d, s, proc))
    add(P.dowel(d, s, proc))
    sp, n, dz, dr = P.spring(d, s, s.l_free, "valve spring")
    s.spring_turns = n
    s.spring_dz = dz
    add(sp)
    add(P.spring(d, s, s.l_show, "valve spring (at full lift)")[0])
    add(P.belt(d, s))
    return parts


def verify_parts(d, names):
    bad = []
    for name in names:
        d.call("set_active_part", part=name)
        geo = d.call("check_geometry")
        if geo["receipt"]["standing"] != "verified":
            bad.append(f"{name}: {geo['receipt'].get('summary')}")
    if bad:
        fail("parts not verified: " + "; ".join(bad))
    return len(names)


# --- the assembly -------------------------------------------------------------------

def q_x(deg):
    return P.q_axis((1, 0, 0), deg)


class Asm:
    def __init__(self, d, name):
        self.d, self.name = d, name
        d.call("add_assembly", name=name)
        self.id = next(p["id"] for p in d.call("list_parts")["parts"]
                       if p["name"] == name)
        self.occ, self.mates, self.rel = {}, {}, {}

    def activate(self):
        self.d.call("set_active_part", part=self.name)

    def place(self, part, name, pos=(0.0, 0.0, 0.0), q=None, fixed=False):
        self.occ[name] = self.d.call(
            "add_occurrence", part=part, name=name, fixed=fixed,
            transform={"position": [float(v) for v in pos],
                       "orientation": q or [0.0, 0.0, 0.0, 1.0]})["id"]
        return self.occ[name]

    def mate(self, kind, name, a, b):
        r = self.d.call("add_mate", type=kind, name=name, a=a, b=b,
                        flipped=False)
        if r.get("errored_mates"):
            fail(f"mate {name} errored: {r}")
        self.mates[name] = r["id"]
        return r["id"]

    def on(self, carrier, part, name, frame=None, kind="fastened",
           own=None, pos=(0.0, 0.0, 0.0)):
        """`part` placed and fastened (or `kind`) to `carrier`: the
        carrier's `frame` (else its origin) to the part's `own` frame (else
        its origin)."""
        self.place(part, name, pos)
        a = {"occurrence": self.occ[carrier]}
        a.update({"frame": frame} if frame else {"origin": True})
        b = {"occurrence": self.occ[name]}
        b.update({"frame": own} if own else {"origin": True})
        return self.mate(kind, f"{name} on {carrier}", a, b)

    def relate(self, name, **kw):
        r = self.d.call("add_relation", name=name, **kw)
        if r.get("error"):
            fail(f"relation {name}: {r['error']}")
        self.rel[name] = r["id"]
        return r

    def solve(self):
        r = self.d.call("solve_assembly")
        if r.get("errored_mates"):
            errs = [(m["name"], m.get("error")) for m in r["mates"]
                    if m["status"] == "error"]
            fail(f"{self.name}: mates do not solve: {errs[:6]}")
        return r


def bottom_end(a, s, rod_part="rod"):
    """Block (fixed), crank, rods with their caps and pins, pistons with
    their wrist pins: four slider-crank loops."""
    a.place("block", "block", fixed=True)
    a.place("crankshaft", "crankshaft")
    a.mate("revolute", "crank", {"occurrence": a.occ["block"],
                                 "frame": "crank"},
           {"occurrence": a.occ["crankshaft"], "frame": "main"})
    L = s.L if rod_part == "rod" else s.L_long
    for k in range(4):
        c = k + 1
        x = s.x_cyl[k]
        zp = s.R * math.cos(math.radians(S.THROW_DEG[k]))
        rn, pn = f"rod {c}", f"piston {c}"
        a.place(rod_part, rn, (x, 0.0, zp))
        a.mate("revolute", f"{rn} big end",
               {"occurrence": a.occ["crankshaft"], "frame": f"pin {c}"},
               {"occurrence": a.occ[rn], "frame": "big_end"})
        a.place("piston", pn, (x, 0.0, zp + L))
        a.mate("revolute", f"{pn} wrist pin",
               {"occurrence": a.occ[rn], "frame": "small_end"},
               {"occurrence": a.occ[pn], "frame": "wrist_pin"})
        a.mate("slider", f"{pn} in its bore",
               {"occurrence": a.occ["block"], "frame": f"bore {c}"},
               {"occurrence": a.occ[pn], "frame": "slide"})
        if rod_part == "rod":
            a.on(rn, "rod cap", f"rod cap {c}", pos=(x, 0.0, zp))
            for j in (1, 2):
                a.on(rn, "cap pin", f"cap pin {c}.{j}", pos=(x, 0.0, zp),
                     frame=f"cap pin {j}")
        a.on(pn, "wrist pin", f"wrist pin {c}", pos=(x, 0.0, zp + L))


def build_engine(d, s, name="engine"):
    a = Asm(d, name)
    bottom_end(a, s)
    a.on("block", "bedplate", "bedplate")
    a.on("bedplate", "sump", "sump")
    a.on("block", "head", "head")
    a.on("head", "cam cover", "cam cover")
    for k in range(len(s.dowels_split)):
        a.on("block", "dowel", f"split dowel {k + 1}",
             frame=f"split dowel {k + 1}")
        a.on("bedplate", "dowel", f"sump dowel {k + 1}",
             frame=f"sump dowel {k + 1}")
        a.on("block", "dowel", f"deck dowel {k + 1}",
             frame=f"deck dowel {k + 1}")
        a.on("head", "dowel", f"cover dowel {k + 1}",
             frame=f"cover dowel {k + 1}")
    a.on("crankshaft", "crank pulley", "crank pulley")
    a.on("crankshaft", "flywheel", "flywheel")
    a.on("crankshaft", "crank arm", "crank arm")
    a.on("crank arm", "handle", "handle")
    a.place("cam bar", "cam bar", (0.0, 0.0, s.z_cam))
    a.mate("revolute", "cam", {"occurrence": a.occ["head"], "frame": "cam"},
           {"occurrence": a.occ["cam bar"], "frame": "axis"})
    for i in range(len(s.lobes)):
        a.on("cam bar", f"lobe {i + 1}", f"lobe {i + 1}",
             pos=(0.0, 0.0, s.z_cam))
    a.on("cam bar", "cam pulley", "cam pulley", pos=(0.0, 0.0, s.z_cam))
    a.on("cam bar", "cam collar", "cam collar", pos=(0.0, 0.0, s.z_cam))
    a.on("block", "timing belt", "timing belt")
    a.relate("timing belt", type="chain", a=a.mates["crank"],
             b=a.mates["cam"], teeth_a=s.z1, teeth_b=s.z2, phase=0.0)
    for i, lb in enumerate(s.lobes):
        vn = f"valve {i + 1}"
        a.place("valve", vn, (lb["x"], 0.0, s.z_deck))
        a.mate("slider", f"{vn} in its guide",
               {"occurrence": a.occ["head"], "frame": f"guide {i + 1}"},
               {"occurrence": a.occ[vn], "frame": "seat"})
        a.on(vn, "bucket", f"bucket {i + 1}", pos=(lb["x"], 0.0, s.z_deck))
        a.on("head", "valve spring (at full lift)", f"spring {i + 1}",
             frame=f"spring {i + 1}")
        a.relate(f"lobe {i + 1}", type="cam", a=a.mates["cam"],
                 b=a.mates[f"{vn} in its guide"],
                 lift=S.cam_table(s, lb["peak_cam_deg"], step=0.25))
    r = a.solve()
    return a, r


# --- the motion study ---------------------------------------------------------------

def traces(a, s):
    out = []
    for i in range(len(s.lobes)):
        out.append({"name": f"valve {i + 1}", "occurrence":
                    a.occ[f"valve {i + 1}"], "point_mm": [0.0, 0.0, 0.0]})
        out.append({"name": f"bucket {i + 1}", "occurrence":
                    a.occ[f"bucket {i + 1}"],
                    "point_mm": [0.0, 0.0, s.hz_u0]})
    for k in range(4):
        out.append({"name": f"piston {k + 1}", "occurrence":
                    a.occ[f"piston {k + 1}"],
                    "point_mm": [0.0, 0.0, s.h_cp]})
    return out


def run_motion(a, name, samples, clash, to_deg=720.0, check_clash=True,
               trace=True, s=None, configuration=0):
    """A study of two turns (or `to_deg`) under `configuration` (0: the
    base, every occurrence in it). The study SAYS which, so the app runs the
    same study the generator did, whatever is hidden or active then."""
    d = a.d
    a.activate()
    st = d.call("add_motion_study", driver=a.mates["crank"],
                dof="rotation", **{"from": 0.0}, to=to_deg, samples=samples,
                clash_mode="mesh", clash_deflection_mm=clash, name=name,
                check_clash=check_clash, configuration=configuration)
    if st.get("why_not"):
        fail(f"motion study {name}: {st['why_not']}")
    sid = st.get("id", st.get("study"))
    kw = {"trace": traces(a, s)} if trace else {}
    r = d.call("run_motion_study", study=sid, include_poses=False,
               _timeout=3600, **kw)
    r = r.get("result", r)
    r["study_id"] = sid
    return r


def clashing(result):
    return sorted({tuple(sorted((c["a_name"], c["b_name"])))
                   for smp in result.get("samples", [])
                   for c in smp.get("clashes", [])})


def jumps(result):
    """The samples flagged `branch_jump`. The watcher judges only what the
    loop solve chooses, so a valve its cam relation drives is never one
    (docs/assemblies/README.md#branch-jumps): any jump is a failure."""
    return [{"at_deg": smp["branch_jump"].get("at_deg"),
             "mate": smp["branch_jump"].get("name"),
             "jump_mm": smp["branch_jump"].get("jump_mm"),
             "expected_mm": smp["branch_jump"].get("expected_mm")}
            for smp in result.get("samples", []) if smp.get("branch_jump")]


def turned_clean(result):
    """Nothing clashes, every sample solved, no branch jump."""
    jumped = jumps(result)
    ok = (not clashing(result) and not unsolved(result) and not jumped
          and not result.get("rigid_clashes"))
    return ok, jumped


def unsolved(result):
    return [smp["value"] for smp in result.get("samples", [])
            if not smp.get("solved", True)]


def angles(result):
    return [smp["value"] for smp in result["samples"]]


def trace_of(result, name):
    return next(t for t in result["trace"] if t["name"] == name)


def timing(result, s):
    """The valve train read off the study's traces: each valve's lift
    against its lobe's table, its physical events (where the lift passes
    the lash), each cylinder's TDCs, and which TDC it fires on."""
    th = angles(result)
    out = {"valves": [], "cylinders": []}
    bad = []
    for i, lb in enumerate(s.lobes):
        z = [p[2] for p in trace_of(result, f"valve {i + 1}")["points_mm"]]
        lift = [s.z_deck - v for v in z]
        want = [S.model_lift(s, t, lb["peak_crank_deg"]) for t in th]
        err = max(abs(u - v) for u, v in zip(lift, want))
        # Physical: open where the lift is over the lash.
        phys = [v - s.lash for v in lift]
        ev = events(th, phys)
        tdc = S.firing_tdc(lb["cyl"])
        want_ev = ((S.IVO, S.IVC) if lb["kind"] == "intake" else
                   (S.EVO, S.EVC))
        got = [((e - tdc) % 720.0) for e in ev]
        row = {"valve": i + 1, "cyl": lb["cyl"], "kind": lb["kind"],
               "lift_max_mm": max(lift), "table_err_mm": err,
               "opens_deg": got[0] if got else None,
               "closes_deg": got[1] if len(got) > 1 else None,
               "want_deg": want_ev}
        out["valves"].append(row)
        step = th[1] - th[0]
        if err > 0.01:
            bad.append(f"valve {i + 1}'s lift is {err:.3f} mm off its "
                       f"lobe's table")
        if len(got) != 2 or any(
                abs(((g - w + 360.0) % 720.0) - 360.0) > step
                for g, w in zip(got, want_ev)):
            bad.append(f"valve {i + 1} ({lb['kind']} {lb['cyl']}) opens "
                       f"and closes at {got} after its TDC, not "
                       f"{list(want_ev)}")
    # TDCs: each piston's highest samples; the firing one has both valves
    # shut.
    fires = {}
    for k in range(4):
        cz = [p[2] for p in trace_of(result, f"piston {k + 1}")["points_mm"]]
        top = max(cz)
        tdcs = [t for t, v in zip(th, cz) if v > top - 1e-6 and t < 720.0]
        vk = [j for j, lb in enumerate(s.lobes) if lb["cyl"] == k + 1]
        shut = []
        for t in tdcs:
            j = th.index(t)
            lifts = [s.z_deck - trace_of(result, f"valve {v + 1}")
                     ["points_mm"][j][2] for v in vk]
            shut.append(all(x <= 1e-6 for x in lifts))
        fire = [t for t, sh in zip(tdcs, shut) if sh]
        out["cylinders"].append({"cyl": k + 1, "tdc_deg": tdcs,
                                 "fires_deg": fire})
        if len(fire) != 1:
            bad.append(f"cylinder {k + 1}: {len(fire)} TDCs with both "
                       f"valves shut ({tdcs})")
        else:
            fires[k + 1] = fire[0]
    if fires.get(1) not in (0.0, None) and 1 in fires:
        bad.append(f"cylinder 1 fires at {fires[1]}, not at crank 0")
    if len(fires) == 4:
        order = [c for _, c in sorted((t, c) for c, t in fires.items())]
        i = order.index(1)
        order = order[i:] + order[:i]
        out["firing_order"] = "-".join(map(str, order))
        gaps = sorted(fires.values())
        spacing = [(b - a) for a, b in zip(gaps, gaps[1:])]
        out["firing_spacing_deg"] = spacing
        if tuple(order) != S.FIRING_ORDER or any(
                abs(g - 180.0) > 1e-6 for g in spacing):
            bad.append(f"fires {out['firing_order']} at {spacing}")
    return out, bad


def events(th, f):
    """Where f crosses zero going up (in the first 720 degrees) and then
    down, linearly between samples; the study's 720 degrees is one cycle,
    so it is read twice over to close a valve that opens near the end."""
    tt = th[:-1] + [t + 720.0 for t in th]
    ff = f[:-1] + f
    up = None
    for i in range(1, len(tt)):
        a, b = ff[i - 1], ff[i]
        x = tt[i - 1] + (tt[i] - tt[i - 1]) * a / (a - b) if a != b else 0
        if up is None and a <= 0.0 < b and tt[i - 1] < 720.0:
            up = x
        elif up is not None and a > 0.0 >= b:
            return [up, x]
    return [up] if up is not None else []


def spring_squeeze(result, s):
    """Each spring's squeeze, the print's: the bucket's underside traced,
    raised by the seat travel (the print's valve sits on its seat, the
    model's a gap off it) and taken off the free length."""
    out = []
    for i in range(len(s.lobes)):
        z = [p[2] - (s.z_deck + s.hz_floor) + s.seat_travel
             for p in trace_of(result, f"bucket {i + 1}")["points_mm"]]
        # The print's valve opens `lash` less than the model's.
        rest = max(z)
        full = min(z) + s.lash
        out.append({"valve": i + 1, "rest_len_mm": rest,
                    "full_len_mm": full,
                    "squeeze_rest": (s.l_free - rest) / s.l_free,
                    "squeeze_full": (s.l_free - full) / s.l_free})
    return out


# --- print plates -------------------------------------------------------------------

def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz]


def rotate(q, v):
    x, y, z, w = q
    vx, vy, vz = v
    # v' = q v q*
    tx = 2.0 * (y * vz - z * vy)
    ty = 2.0 * (z * vx - x * vz)
    tz = 2.0 * (x * vy - y * vx)
    return (vx + w * tx + (y * tz - z * ty),
            vy + w * ty + (z * tx - x * tz),
            vz + w * tz + (x * ty - y * tx))


def bbox(d, part):
    r = d.call("mass_properties", part=part)["bbox_mm"]
    return r["min"], r["max"]


def turned_box(lo, hi, q):
    pts = [rotate(q, (x, y, z)) for x in (lo[0], hi[0])
           for y in (lo[1], hi[1]) for z in (lo[2], hi[2])]
    return ([min(p[i] for p in pts) for i in range(3)],
            [max(p[i] for p in pts) for i in range(3)])


PLA_PIECES = [("block", 1), ("bedplate", 1), ("sump", 1), ("head", 1),
              ("cam cover", 1), ("crankshaft", 1), ("rod", 4),
              ("rod cap", 4), ("cap pin", 8), ("piston", 4),
              ("wrist pin", 4), ("valve", 8), ("bucket", 8), ("cam bar", 1)] \
    + [(f"lobe {i + 1}", 1) for i in range(8)] + \
    [("cam pulley", 1), ("crank pulley", 1), ("cam collar", 1),
     ("flywheel", 1), ("crank arm", 1), ("handle", 1), ("dowel", 16)]
TPU_PIECES = [("timing belt", 1), ("valve spring", 8)]


def pieces(d, kinds):
    """Each piece: its part, label, print rotation and box as it prints."""
    out = []
    for part, n in kinds:
        q = P.PRINT[part]
        lo, hi = turned_box(*bbox(d, part), q)
        for k in range(n):
            out.append({"part": part, "label": part if n == 1 else
                        f"{part} {k + 1}", "q": q, "lo": lo, "hi": hi})
    return out


def lay_out(pcs, bed, gap, zones=()):
    items = [(i, p["hi"][0] - p["lo"][0], p["hi"][1] - p["lo"][1])
             for i, p in enumerate(pcs)]
    beds = S.pack(items, bed, gap, zones)
    out = []
    for b in beds:
        rows = []
        for i, turned, x, y in b:
            p = dict(pcs[i])
            q = p["q"]
            if turned:
                q = qmul(P.q_axis((0, 0, 1), 90.0), q)
            p["q_bed"] = q
            p["at"] = (x, y)
            rows.append(p)
        out.append(rows)
    return out


def make_plate(d, name, proc, rows, part_ids):
    d.call("add_part", name=name)
    if proc is not None:
        d.call("set_print_process", **proc)
    out = []
    for p in rows:
        lo0, hi0 = bbox(d, p["part"])
        lo, hi = turned_box(lo0, hi0, p["q_bed"])
        pos = [p["at"][0] - lo[0], p["at"][1] - lo[1], -lo[2]]
        check(d.call("add_feature", feature={
            "type": "Derived", "name": p["label"],
            "source_part": part_ids[p["part"]], "reference": False,
            "placement": {"position": pos, "orientation": p["q_bed"]}}),
            f"{name}: {p['label']}")
        out.append({"piece": p["label"], "part": p["part"],
                    "orientation": ORIENT[p["part"]],
                    "at_mm": [round(v, 1) for v in p["at"]]})
    return out


ORIENT = {
    "block": "deck down", "bedplate": "as it sits, open side down",
    "sump": "as it sits, floor down", "head": "deck down",
    "cam cover": "top down", "crankshaft": "on its flat (the plane of "
    "its throws)", "rod": "flat, lap tongues down", "rod cap": "flat, lap "
    "tongues down", "cap pin": "standing", "piston": "crown down",
    "wrist pin": "standing", "valve": "head down", "bucket": "crown down",
    "cam bar": "on its flat", "cam pulley": "flange down",
    "crank pulley": "flange down", "cam collar": "flat",
    "flywheel": "marked face up", "crank arm": "flat",
    "handle": "grip down", "dowel": "standing",
    "timing belt": "flat, on its edge", "valve spring": "standing",
}
for _i in range(8):
    ORIENT[f"lobe {_i + 1}"] = "flat, number up"


def print_plates(d, s, proc, prof, out):
    bed = (float(prof["bed_mm"][0]), float(prof["bed_mm"][1]))
    gap = S.spacing(s)
    ids = {p["name"]: p["id"] for p in d.call("list_parts")["parts"]
           if p["type"] == "part"}
    plates = {}
    zones = bed_zones(prof)
    pla = lay_out(pieces(d, PLA_PIECES), bed, gap, zones)
    tpu = lay_out(pieces(d, TPU_PIECES), bed, gap, zones)
    if len(tpu) != 1:
        fail(f"the TPU pieces need {len(tpu)} plates")
    jobs = [(f"plate {k + 1}", rows, proc, f"plate{k + 1}.stl")
            for k, rows in enumerate(pla)]
    jobs.append(("plate TPU", tpu[0], None, "plate_tpu.stl"))
    gs = S.Spec()
    del gs
    for name, rows, pp, fn in jobs:
        # The TPU plate is printed by the same printer: its process gives
        # the job its bed and layer, and its pieces' material says TPU.
        placed = make_plate(d, name, proc, rows, ids)
        geo = d.call("check_geometry")
        if geo["receipt"]["standing"] != "verified":
            fail(f"{name} is not verified: {geo['receipt'].get('summary')}")
        if pp is not None:
            pr = check_print(d, f"  {name}", need=PRINT_ITEMS, quiet=True)
        else:
            # TPU: the plate's process is the PLA profile's (the printer,
            # its bed and layer; the tables carry no TPU), so every limit
            # the PLA rows would set is stated: the profile's air and
            # chords, the 45-degree rule and the 10 mm bridge.
            pr = check_print(d, f"  {name}", need=PRINT_ITEMS, quiet=True,
                             max_overhang_deg=45.0, max_bridge_mm=10.0,
                             min_gap_mm=s.g_side_min,
                             stl_deflection_mm=prof["stl_deflection_mm"],
                             # the profile's air, else three layers, as
                             # check_print falls back itself
                             min_air_mm=prof.get("z_gap_mm") or
                             3.0 * prof["layer_height_mm"])
        if pr["shells"]["pieces"] != len(rows):
            fail(f"{name}: {pr['shells']['pieces']} pieces for the "
                 f"{len(rows)} placed")
        box = d.call("mass_properties")["bbox_mm"]
        if min(box["min"][:2]) < -1e-6 or box["max"][0] > bed[0] + 1e-6 \
                or box["max"][1] > bed[1] + 1e-6 or \
                abs(box["min"][2]) > 1e-6:
            fail(f"{name} is off the bed: {box}")
        kw = {} if pp is not None else {"deflection":
                                        prof["stl_deflection_mm"]}
        exp = d.call("export_stl", path=str(out / fn), **kw)
        mp = d.call("mass_properties")["total"]
        plates[name] = {"stl": str(out / fn), "pieces": placed,
                        "volume_cm3": mp["volume_mm3"] / 1000.0,
                        "mass_g": mp.get("mass_g"),
                        "printed_g": (mp.get("printed") or {}).get("mass_g"),
                        "printed_why": (mp.get("printed") or {}).get("why"),
                        "shells": exp.get("shells"),
                        "deflection_mm": exp.get("deflection_mm"),
                        "bbox_mm": box, "print": print_receipt(pr)}
        print(f"  {name}: {len(rows)} pieces, every check_print item "
              f"passes ({print_item_line('stacking', pr['stacking'])}; "
              f"{print_item_line('bed_fit', pr['bed_fit'])}) -> {fn}")
    return plates


# --- the test card ------------------------------------------------------------------

def card(s, pred, plates, profile, why):
    ev = pred["events"]
    rows_ev = "\n".join(
        f"| {v['valve']} | {v['cyl']} | {v['kind']} | "
        f"{v['opens_deg']:.1f} | {v['closes_deg']:.1f} |"
        for v in ev)
    rows_mass = "\n".join(
        f"| {Path(p['stl']).name} | {len(p['pieces'])} | "
        f"{p['volume_cm3']:.0f} cm³"
        + (f" ({p['mass_g']:.0f} g solid" if p.get("mass_g") else "")
        + (f", {p['printed_g']:.0f} g printed" if p.get("printed_g") else "")
        + (")" if p.get("mass_g") else "")
        + " |" for p in plates.values())
    rows_pl = []
    for name, p in plates.items():
        for r in p["pieces"]:
            rows_pl.append(f"| {Path(p['stl']).name} | {r['piece']} | "
                           f"{r['orientation']} |")
    return f"""# Engine test card

Bore {s.bore:g} mm ({why}), profile {profile}. Predicted before printing,
by generators/engine/build.py.

## Prediction

| what | predicted |
|---|---|
| bore x stroke | {s.bore:g} x {s.S:g} mm |
| swept volume, 4 cylinders | {pred['swept_cc']:.1f} cm³ |
| geometric compression ratio | {pred['cr']:.1f} : 1 |
| valve lift (at the valve, the lash taken up) | {s.lift:.2f} mm |
| valve lash (cam to bucket, valve seated) | {s.lash:.2f} mm |
| crank angle between TDC marks | {pred['tdc_spacing_deg']:g}° |
| firing order | {pred['firing_order']} |
| spring squeeze, valve shut / fully open | {100 * pred['squeeze_rest']:.0f} % / {100 * pred['squeeze_full']:.0f} % of {s.l_free:.1f} mm |
| valve to piston, least over 720° | {pred['valve_piston_mm']:.2f} mm |

Valve events, crank degrees after the cylinder's firing TDC (the flywheel's
`1 4` mark is cylinders 1 and 4 at TDC, `2 3` is 2 and 3; turn the hand
crank clockwise seen from the front). Ticks every 10°, long ones every 30°.

| valve | cylinder | | opens | closes |
|---|---|---|---|---|
{rows_ev}

Cylinder 1 fires at the `1 4` mark with both its valves shut; cylinder 4 is
then at the top of its exhaust stroke, both valves just open (overlap).

## Print

PLA plates on {profile}, TPU (95A) on its own. 0.2 mm layers, no supports,
no brim. The PLA parts at Bambu Studio's 0.20 mm Standard (2 walls, 5/3
shells, 15 % grid). The springs and the belt in TPU: 3 walls, 100 %
rectilinear infill on the belt (it is all walls at this size). The print
jobs carry every one of those per object: `engine_job.3mf` (the PLA
plates) and `engine_tpu_job.3mf` (the TPU plate, a spool of its own).

| plate | pieces | material: solid, and printed at the settings above |
|---|---|---|
{rows_mass}

| plate | piece | orientation |
|---|---|---|
{chr(10).join(rows_pl)}

## Assembly

1. Press the wrist pins through the pistons with the rods' small ends
   between the bosses. Drop the pistons into the bores from the deck.
2. Lay the crankshaft in the block's upper mains (the block on its deck).
   Close each rod's cap round its pin and press the two cap pins through
   both laps.
3. Press the dowels into the bedplate; press the bedplate on. The sump on
   its dowels.
4. Valves up through their seats; a spring over each stem into its bucket
   bore; press a bucket on each stem. The head on the deck's dowels.
5. Lobes on the bar by their numbers, front to back (each fits one way on
   the flat), the bar into the head's saddles, the collar on its rear end,
   the cam pulley on its front. The cover on its dowels.
6. Crank pulley on the nose, the crankshaft at the `1 4` mark, the belt
   over both pulleys with the cam's lobes 1 and 2 (cylinder 1) both shut.
   The hand crank on the nose, the flywheel on the tail.

## Test

| check | predicted | measured |
|---|---|---|
| turns through 720° by hand without binding | yes | |
| valve 2 (exhaust 1) opens, from `1 4` with cylinder 1 firing | {ev[1]['opens_deg']:.0f}° on | |
| valve 2 (exhaust 1) closes | {ev[1]['closes_deg'] - 360.0:.0f}° past `1 4` the next time round | |
| valve 1 (intake 1) opens | {360.0 - ev[0]['opens_deg']:.0f}° before that same mark | |
| valve 1 (intake 1) closes | {ev[0]['closes_deg'] - 540.0:.0f}° past `2 3` after it | |
| lift at a valve, calipers on the bucket | {s.lift:.2f} mm | |
| no valve touches a piston (turn slowly at TDC overlap) | none | |
"""


# --- main -------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int)
    ap.add_argument("--bore", type=float, default=None,
                    help="mm; without it the largest whose PLA pieces pack "
                         "into two of the profile's beds")
    ap.add_argument("--profile", default=None,
                    help="one of your print profiles (list_print_profiles); "
                         "it must state its bed (bed_mm)")
    ap.add_argument("--step", type=float, default=None,
                    help="the study's crank step, degrees (default: a "
                         "fifth of the belt's tooth step)")
    ap.add_argument("--fresh", action="store_true",
                    help="start a new project first (refused on unsaved work)")
    ap.add_argument("--out", default=None)
    ap.add_argument("--no-render", action="store_true",
                    help="skip the stills (a --headless instance has no "
                         "renderer)")
    args = ap.parse_args()
    out = Path(args.out) if args.out else OUT
    out.mkdir(parents=True, exist_ok=True)
    t_start = time.perf_counter()

    d = connect(args.port)
    if args.fresh:
        fresh_project(d)
    if not args.profile:
        fail("--profile is required: the engine's plates are packed into "
             "the profile's bed (list_print_profiles names yours)")
    prof = next((p for p in d.call("list_print_profiles")["profiles"]
                 if p["name"] == args.profile), None)
    if prof is None:
        fail(f"no print profile '{args.profile}'; list_print_profiles "
             "names yours")
    if not prof.get("bed_mm"):
        fail(f"profile '{args.profile}' states no bed_mm: set the printer's "
             "bed in Settings, Print profiles, or with save_print_profile")
    if prof.get("material") != "pla":
        fail("the engine's parts are PLA (its springs and belt TPU)")
    if not prof.get("layer_height_mm"):
        fail(f"profile '{args.profile}' states no layer height")
    proc = {"profile": args.profile}
    layer = prof["layer_height_mm"]
    bed = (float(prof["bed_mm"][0]), float(prof["bed_mm"][1]))
    allow = S.Allow(d, proc)

    with phase("sizes"):
        if args.bore is None:
            bore, n_plates, why, tried = S.search_bore(
                allow, layer, bed, zones=bed_zones(prof))
            print(f"engine: bore {bore} mm, {n_plates} PLA plates: {why}")
            for b, n, note in tried[:12]:
                print(f"  bore {b}: {n if n else '-'} plates ({note})")
        else:
            bore, why, tried = args.bore, "as asked", []
        s = S.solve(bore, allow, layer)
        s.g_side_min = min(allow("gap", dd) for dd in (s.d_s, s.d_bk)) / 2.0
        n_est = S.plates_needed(s, bed, bed_zones(prof))
        print(f"  bore {s.bore:g} x stroke {s.S:g}, rod {s.L:.1f}, pitch "
              f"{s.pitch:.1f}, deck {s.z_deck:.1f} over the crank, cam "
              f"{s.z_cam:.1f}; belt {s.belt_n} teeth of {s.belt_p:g} on "
              f"{s.z1}/{s.z2}; lift {s.lift:.2f}, lash {s.lash:.2f}; squish "
              f"{s.s_q:.2f}; {n_est} PLA plates by the footprints")
        print(f"  sizes at their printing floors: "
              f"{', '.join(f[0] for f in s.floors) or 'none'}")
        print(f"  the rods' sweep stays {s.sweep_margin:.2f} mm inside the "
              f"crankcase (at least {s.c_stack:.2f})")

    with phase("parts"):
        parts = build_parts(d, s, proc)
        for p in d.call("list_parts")["parts"]:
            if p["type"] == "part" and p["name"] not in parts:
                d.call("delete_part", part=p["name"])  # new_project's
        n_parts = verify_parts(d, list(parts))
        print(f"  {n_parts} parts, every one verified")

    clash = s.g_side_min / 5.0
    checks = {"clash_deflection_mm": clash}
    with phase("assembly"):
        a, sol = build_engine(d, s)
        print(f"  {len(a.occ)} occurrences, {len(a.mates)} mates, "
              f"{len(a.rel)} relations; dof {sol.get('dof')}, loops closed "
              f"{sol.get('loops_closed', True)}")
        # The belt is one rigid ring: it sits in its pulleys at whole teeth
        # and nowhere between, so the drive is turned without it and the
        # belt at every tooth (the "belt" phase). A configuration, not the
        # belt hidden: the study saves which, and so the app checks what was
        # checked here (a hidden belt saved shown re-ran the study with it).
        a.activate()
        no_belt = d.call("add_configuration", name="the drive, no belt")["id"]
        d.call("set_configuration", id=no_belt,
               suppressed_occurrences=[a.occ["timing belt"]])
        d.call("save_project", path=str(out / "engine.dsgn"))

    step = args.step or (360.0 / s.z1) / 5.0
    n = int(round(720.0 / step)) + 1
    with phase("study"):
        r = run_motion(a, f"720 deg at {step:g}", n, clash, s=s,
                       configuration=no_belt)
        bad = clashing(r)
        print(f"  720 deg, {n} samples: {r['collision_status']}, "
              f"unsolved {unsolved(r)[:4]}, clashing {bad[:6]} "
              f"({r.get('seconds', 0):.0f} s)")
        presses = r.get("press_fits", [])
        print(f"  {len(presses)} printed press fits relieved and listed "
              f"(every pressed pin, dowel, stub and bore); branch jumps "
              f"{r.get('branch_jumps', 0)}")
        if not presses:
            fail("the study lists no press fits: the pressed faces carry "
                 "no printed press band")
        checks["press_fits"] = len(presses)
        checks["study"] = {"samples": n, "step_deg": step,
                           "status": r["collision_status"],
                           "clashing": bad, "unsolved": unsolved(r),
                           "seconds": r.get("seconds")}
        ok, jumped = turned_clean(r)
        checks["study"]["branch_jumps"] = jumped
        if not ok:
            fail(f"the engine does not turn clean: {bad} {unsolved(r)[:4]} "
                 f"{jumped[:2]}")
        tm, bad_t = timing(r, s)
        checks["timing"] = tm
        for v in tm["valves"]:
            print(f"    valve {v['valve']} ({v['kind']} {v['cyl']}): lift "
                  f"{v['lift_max_mm']:.3f}, {v['table_err_mm']:.2e} off "
                  f"the table, opens {v['opens_deg']:.1f} closes "
                  f"{v['closes_deg']:.1f} (want {v['want_deg']})")
        print(f"  fires {tm.get('firing_order')} at "
              f"{tm.get('firing_spacing_deg')}; cylinder 1 at "
              f"{tm['cylinders'][0]['fires_deg']}")
        if bad_t:
            fail("timing: " + "; ".join(bad_t))
        sq = spring_squeeze(r, s)
        checks["springs"] = sq
        sq_full = max(x["squeeze_full"] for x in sq)
        sq_rest = min(x["squeeze_rest"] for x in sq)
        print(f"  springs: squeeze {100 * sq_rest:.1f} % shut, "
              f"{100 * sq_full:.1f} % at full lift")
        if abs(sq_full - S.SQUEEZE_LIFT) > 0.01 or \
                abs(sq_rest - S.SQUEEZE_REST) > 0.01:
            fail(f"the springs' squeeze: {sq}")
        main_study = r["study_id"]

    with phase("belt"):
        r = run_motion(a, "the belt at every tooth", 2 * s.z1 + 1, clash,
                       trace=False, s=s)
        belt_study = r["study_id"]
        bad = clashing(r)
        print(f"  the belt on its pulleys at each of {2 * s.z1} teeth: "
              f"{r['collision_status']} {bad[:6]}")
        ok, jumped = turned_clean(r)
        checks["belt"] = {"status": r["collision_status"], "clashing": bad,
                          "branch_jumps": jumped}
        if not ok:
            fail(f"the belt does not sit in its pulleys: {bad} {jumped[:2]}")

    with phase("controls"):
        rid = a.rel["timing belt"]
        tooth = 2.0 * math.pi / s.z2
        d.call("set_relation", id=rid, phase=tooth)
        a.solve()
        r = run_motion(a, "control: the cam pulley a tooth off", n, clash,
                       s=s)
        d.call("set_relation", id=rid, phase=0.0)
        a.solve()
        _, bad_t = timing(r, s)
        meets = [p for p in clashing(r) if any(x.startswith("valve") for x in
                                               p)]
        print(f"  negative control, the cam pulley a tooth off: "
              f"{'FAILS, as it must' if bad_t or meets else 'PASSES (bad)'}"
              f": {len(meets)} valves meet pistons {meets[:4]}, "
              f"{len(bad_t)} timing faults")
        checks["control: a tooth off"] = {"timing": bad_t, "valves_meet":
                                          meets}
        if not bad_t and not meets:
            fail("a cam a tooth off passes the timing check")
        s.L_long = 1.1 * s.L
        P.rod(d, s, proc, name="rod (10 % long)", length=s.L_long)
        ac = Asm(d, "control: rods 10 % long")
        bottom_end(ac, s, rod_part="rod (10 % long)")
        ac.on("block", "head", "head")
        ac.solve()
        r = run_motion(ac, "control: rods 10 % long", 73, clash,
                       to_deg=360.0, trace=False, s=s)
        bad = clashing(r)
        hits = [p for p in bad if "head" in p]
        print(f"  negative control, rods 10 % long: "
              f"{'FAILS, as it must' if hits else 'PASSES (bad)'}: "
              f"{hits[:4]}")
        checks["control: rods 10 % long"] = {"status":
                                             r["collision_status"],
                                             "clashing": bad}
        if not hits:
            fail("a rod 10 % long does not reach the head")

    with phase("requirements"):
        req_ids = []
        for i, lb in enumerate(s.lobes):
            rq = d.call("add_requirement", name=f"valve {i + 1} clears "
                        f"piston {lb['cyl']}", measure={
                            "kind": "clearance", "document": a.id,
                            "a": a.occ[f"valve {i + 1}"],
                            "b": a.occ[f"piston {lb['cyl']}"],
                            "mode": "exact", "motion_study": main_study},
                        bound={"kind": "at_least", "value": s.c_vp},
                        source="the tolerance stack of the five printed "
                               "sizes between a valve and its piston")
            req_ids.append(rq["id"])
        rq = d.call("add_requirement", name="the engine turns clean",
                    measure={"kind": "motion_clean", "document": a.id,
                             "study": main_study},
                    bound={"kind": "pass"},
                    source="every sample of two turns solves, nothing "
                           "clashes (the drive, without the belt)")
        req_ids.append(rq["id"])
        rq = d.call("add_requirement", name="the belt sits in its pulleys",
                    measure={"kind": "motion_clean", "document": a.id,
                             "study": belt_study},
                    bound={"kind": "pass"},
                    source="the belt is one rigid ring: at every tooth of "
                           "two turns it meets neither pulley")
        req_ids.append(rq["id"])
        rr = d.call("check_requirements", ids=req_ids, _timeout=3600)
        reqs = rr.get("requirements", rr.get("results", []))
        vp = []
        for q in reqs:
            ev = q.get("evidence", q)
            print(f"  {q.get('name')}: {ev.get('value')} -> "
                  f"{q.get('state', ev.get('pass'))}")
            if q.get("state") != "pass":
                fail(f"requirement {q.get('name')}: {q}")
            if isinstance(ev.get("value"), (int, float)):
                vp.append(ev["value"])
        checks["requirements"] = reqs
        checks["valve_piston_min_mm"] = min(vp) if vp else None

    with phase("predictions"):
        vc = math.pi / 4.0 * s.bore ** 2 * s.s_q
        vs = math.pi / 4.0 * s.bore ** 2 * s.S
        pred = {"swept_cc": 4.0 * vs / 1000.0, "cr": (vs + vc) / vc,
                "lift_mm": s.lift, "lash_mm": s.lash,
                "tdc_spacing_deg": 180.0,
                "firing_order": tm["firing_order"],
                "events": tm["valves"], "squeeze_rest": sq_rest,
                "squeeze_full": sq_full,
                "valve_piston_mm": checks["valve_piston_min_mm"]}
        print(f"  swept {pred['swept_cc']:.1f} cc, CR {pred['cr']:.1f}, "
              f"lift {s.lift:.2f}")

    with phase("print plates"):
        plates = print_plates(d, s, proc, prof, out)
        n_pla = sum(1 for k in plates if k != "plate TPU")
        print(f"  {n_pla} PLA plates and one TPU")
        # One job a spool: the PLA plates together, the TPU plate alone.
        pla_names = [k for k in plates if k != "plate TPU"]
        jobs = {"pla": job_receipt(export_print_job(
                    d, [(k, k) for k in pla_names], out / "engine_job.3mf",
                    "pla")),
                "tpu": job_receipt(export_print_job(
                    d, [("plate TPU", "TPU")], out / "engine_tpu_job.3mf",
                    "tpu"))}
        solid = sum(plates[k]["mass_g"] or 0.0 for k in pla_names)
        printed = [plates[k]["printed_g"] for k in pla_names]
        weight = {"pla_solid_g": solid,
                  "pla_printed_g": sum(printed) if all(printed) else None,
                  "tpu_solid_g": plates["plate TPU"]["mass_g"],
                  "tpu_printed_g": plates["plate TPU"]["printed_g"],
                  "tpu_printed_why": plates["plate TPU"]["printed_why"]}
        if weight["pla_printed_g"]:
            print(f"  the PLA plates print {weight['pla_printed_g']:.0f} g "
                  f"against {solid:.0f} g solid "
                  f"({100 * weight['pla_printed_g'] / solid:.0f} %)")

    if not args.no_render:
        with phase("renders"):
            # From the front and the window side, above; then cut through
            # the bores' plane (the -Y half kept), seen from +Y.
            a.activate()
            tgt = [(s.x_f + s.x_r) / 2.0, 0.0,
                   (s.z_sump + s.z_cam + s.h_cover) / 2.0]
            d.call("set_camera", target=tgt, direction=[-0.75, 1.0, 0.55],
                   up=[0, 0, 1])
            d.call("render_still", path=str(out / "engine.png"), width=1600,
                   height=1200, fit=True, mode="quick")
            d.call("set_section", base_plane="xz", offset=0.0, enabled=True)
            d.call("set_camera", target=tgt, direction=[-0.35, 1.0, 0.25],
                   up=[0, 0, 1])
            d.call("render_still", path=str(out / "engine_section.png"),
                   width=1600, height=1200, fit=True, mode="quick")
            d.call("set_section", enabled=False)

    a.activate()
    d.call("save_project", path=str(out / "engine.dsgn"))
    # What the app sees: the file reopened, every requirement current and
    # passing WITHOUT measuring (list_requirements reads hashes). A check
    # taken on a state the file does not keep -- the belt hidden, then
    # shown and saved -- opens stale, and the panel's Check measured a
    # different engine and failed it.
    d.call("open_project", path=str(out / "engine.dsgn"))
    reopened = d.call("list_requirements")["requirements"]
    not_pass = [(q["name"], q["state"], q.get("note")) for q in reopened
                if q["state"] != "pass"]
    print(f"  reopened: {len(reopened) - len(not_pass)} of {len(reopened)} "
          f"requirements pass as saved")
    if not_pass or len(reopened) != len(checks["requirements"]):
        fail(f"the saved engine's requirements do not stand as checked: "
             f"{not_pass}")
    (out / "engine_card.md").write_text(card(s, pred, plates, args.profile,
                                             why))
    spec = {k: v for k, v in vars(s).items()
            if k not in ("belt_path", "belt_teeth")}
    receipt = {"bore_mm": s.bore, "why": why, "search": tried,
               "process": proc, "profile": prof,
               "allowances": {f"{k}@{sz}": v for (k, sz), v in
                              allow.answers.items()},
               "sizes": spec, "predictions": pred, "checks": checks,
               "plates": plates, "jobs": jobs, "weight": weight,
               "seconds": round(time.perf_counter() - t_start, 1),
               "phases": {nm: round(t, 1) for nm, t in PHASES}}
    (out / "engine.json").write_text(json.dumps(receipt, indent=1,
                                                default=str))
    total = sum(t for _, t in PHASES)
    print(f"time: {total:.0f} s; " + ", ".join(f"{nm} {t:.0f}"
                                               for nm, t in PHASES))
    print(f"engine: done, {n_pla} PLA plates and one TPU -> {out}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SdkError as exc:
        raise SystemExit(f"engine: {exc}")
