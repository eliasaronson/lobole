#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A printed screw-top jar: a PLA jar and lid with modelled 45-degree
threads, and a TPU 95A gasket in the lid that the lid squeezes by a set
fraction when it is screwed down to its stop.

    tools/headless-preview.sh --start <port>
    python3 generators/jar/build.py --port <port> --profile "K5 PLA on P1S"
    python3 generators/jar/build.py --port <port> --inner-diameter 80 --volume 400

Every size derives from `--inner-diameter` and `--volume` (the jar's
opening and what it holds) and from the print process's allowances
(`print_allowance`). What it builds and checks: generators/jar/README.md.

Output (generated, untracked): scratch/jar/ — jar.stl and lid.stl (PLA),
gasket.stl (TPU), each in its print orientation; jar.dsgn; jar.png and
jar-section.png; jar.json (the receipt: sizes, allowances, clearances, the
measured squeeze and every check's verdict).
"""
import argparse
import json
import math
import os
import sys
import time
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = Path(HERE).resolve().parent.parent
sys.path.insert(0, str(ROOT / "generators"))
from lobole.sdk import Plane, SdkError, connect  # noqa: E402
from _lib import (check_print, export_print_job, job_receipt,  # noqa: E402
                  print_receipt)

OUT = Path(os.environ.get("LOBOLE_SCRATCH", ROOT / "scratch")) / "jar"

NOZZLE_MM = 0.4  # the P1S's stock nozzle, which the allowance tables assume
LAYER_MM = 0.2   # the layer height the profiles were measured at
# Half the thread angle. A 60-degree ISO flank lies 30 degrees off the
# horizontal, so whichever flank faces down when printed overhangs 60
# degrees from vertical: on the jar printed upright and on the lid printed
# top down alike, and it is the LOAD flank on both. At 45 every flank is at
# the usual printable limit (docs/modeling/holes-and-threads.md#flank-angle).
FLANK_DEG = 45.0
# Squeeze as a fraction of the gasket's thickness: a static face seal is
# compressed 15-30 % (Parker O-ring Handbook ORD 5700, static axial
# glands); the middle of the band the job asks for, 20-30 %.
SQUEEZE = 0.25
SQUEEZE_BAND = (0.20, 0.30)
# Turns of thread on the neck: one and a half to two is a jar's (a
# continuous-thread closure engages about a turn and a half); two at this
# pitch also lifts the lid clear of the neck in two turns.
TURNS = 2
# Steepest overhang, from vertical, before a face needs support: 45, the
# rule of thumb, which check_print takes from each part's print process --
# the PLA jar's and lid's, and the TPU gasket's own (fdm tpu,
# docs/tolerances/README.md#tpu). check_print reads every angle off the
# B-rep's surface normals, so a surface AT 45 (the thread flanks, the edge
# chamfers) reads 45 rather than the 46-50 its facets lean to, and no slack
# is needed; an ISO 60-degree thread on the same neck fails (the negative
# control in main()).


def lines(mm, at_least):
    """A width rounded up to whole extrusion lines."""
    return max(at_least, math.ceil(mm / NOZZLE_MM - 1e-9)) * NOZZLE_MM


def layers(mm, at_least):
    """A height rounded to whole layers."""
    return max(at_least, round(mm / LAYER_MM)) * LAYER_MM


def solve(inner_d, volume_ml, allow, probe=False):
    """Every size, from the opening, the volume and the print allowances
    (`allow`: signed diametral allowances by kind, model = basic + it)."""
    ri = inner_d / 2.0
    # A thirtieth of the opening: 2.0 mm (five lines) at 60 mm, which holds
    # water and a dropped jar; never under four lines.
    wall = lines(inner_d / 30.0, 4)
    # A coarse pitch, a fifteenth of the opening to the half millimetre:
    # 4 mm at 60. Coarse is what prints: a thread is drawn in a few layers
    # per flank, and fewer turns to close.
    pitch = max(2.0, round(inner_d / 15.0 / 0.5) * 0.5)
    h = pitch / (2.0 * math.tan(math.radians(FLANK_DEG)))
    depth = 5.0 * h / 8.0  # threads::BasicDepth: 1.25 at 4 mm and 45 deg
    floor = wall
    h_in = volume_ml * 1000.0 / (math.pi * ri * ri)
    z_rim = floor + h_in
    # The thread: the neck's wall under the root is the jar's wall; the
    # basic major sits a thread depth outside it.
    major = 2.0 * (ri + wall + depth)
    neck_d = major + allow["external_thread"]       # the boss's face: major
    lid_bore_d = major - 2.0 * depth + allow["internal_thread"]  # basic minor
    lid_major = lid_bore_d + 2.0 * depth
    neck_root_r = neck_d / 2.0 - depth
    radial = (lid_major - neck_d) / 2.0  # at crest and root alike
    if radial <= 0.0 and not probe:
        raise SystemExit(f"jar: the allowances leave the thread no "
                         f"clearance ({radial:.3f} mm a side)")
    # Centred, each flank clears by half the axial play, along its normal.
    axial_play = 2.0 * radial * math.tan(math.radians(FLANK_DEG))
    flank_gap = axial_play / 2.0 * math.cos(math.radians(FLANK_DEG))
    thread_len = TURNS * pitch
    # The lip above the thread: a line inside the root, so the groove runs
    # out through the neck's top and the lid's crests clear it, and the
    # gasket inside the lid's crests still covers it.
    lip_r = neck_root_r - NOZZLE_MM
    lip_h = pitch / 2.0
    # The rim: chamfered on the outside to a flat two lines wide, which is
    # what presses into the gasket. A narrow flat needs less force for the
    # same squeeze.
    seal_w = 2.0 * NOZZLE_MM
    chamfer = lip_r - ri - seal_w
    gasket_t = layers(inner_d / 30.0, 6)  # ten layers at 60 mm
    squeeze = SQUEEZE * gasket_t
    if chamfer < squeeze + LAYER_MM:
        raise SystemExit(f"jar: the rim's chamfer ({chamfer:.2f} mm) is not "
                         f"deeper than the squeeze ({squeeze:.2f}) and a "
                         f"layer: a thicker wall (--inner-diameter)")
    # Half the gasket stands in its groove, half below the ceiling. At the
    # stop the rim sinks `squeeze` into it and still clears the ceiling.
    groove_depth = layers(gasket_t / 2.0, 1)
    rim_to_ceiling = gasket_t - groove_depth - squeeze
    lid_bore_r = lid_bore_d / 2.0
    lid_r = lid_major / 2.0 + wall
    top = groove_depth + wall
    # Ceiling to the skirt's mouth, so that with the mouth on the shoulder
    # the gasket's underside is `squeeze` below the rim.
    skirt = lip_h + thread_len + gasket_t - groove_depth - squeeze
    # The gasket ring: from two lines inside the bore to a line inside the
    # lid's thread crests, covering the whole lip.
    g_in = ri - 2.0 * NOZZLE_MM
    g_out = lid_bore_r - NOZZLE_MM
    if g_out < lip_r and not probe:
        raise SystemExit("jar: the gasket does not cover the lip")
    retain = 0.1  # a side: the TPU ring is pressed into its groove by this
    return {
        "inner_diameter": inner_d, "volume_ml": volume_ml,
        "wall": wall, "floor": floor, "inner_height": h_in,
        "pitch": pitch, "flank_deg": FLANK_DEG, "thread_depth": depth,
        "turns": TURNS, "thread_length": thread_len,
        "basic_major": major, "neck_face_d": neck_d,
        "lid_bore_d": lid_bore_d, "lid_major_d": lid_major,
        "thread_radial_clearance": radial, "thread_axial_play": axial_play,
        "thread_flank_clearance": flank_gap,
        "lip_r": lip_r, "lip_h": lip_h, "seal_flat": seal_w,
        "rim_chamfer": chamfer, "z_rim": z_rim,
        "z_shoulder": z_rim - lip_h - thread_len,
        "outer_d": 2.0 * lid_r, "lid_top": top, "skirt": skirt,
        "gasket_t": gasket_t, "squeeze": squeeze,
        "groove_depth": groove_depth, "rim_to_ceiling": rim_to_ceiling,
        # Printed sizes; the groove's are modelled with the allowances.
        "gasket_in_d": 2.0 * g_in + 2.0 * retain,
        "gasket_out_d": 2.0 * g_out + 2.0 * retain,
        "groove_in_d": 2.0 * g_in + allow["shaft"],
        "groove_out_d": 2.0 * g_out + allow["hole"],
        "retain": retain,
        "edge_chamfer": 2.0 * LAYER_MM,
    }


def squeeze_volume(s, depth):
    """The volume of jar above the plane `depth` under the rim: the seal
    flat's rectangle and the chamfer's triangle, revolved (Pappus)."""
    ri, w = s["inner_diameter"] / 2.0, s["seal_flat"]
    rect = w * depth * (ri + w / 2.0)
    tri = depth * depth / 2.0 * (ri + w + depth / 3.0)
    return 2.0 * math.pi * (rect + tri)


def build_jar(app, s, proc, material, name="jar", flank=FLANK_DEG):
    p = app.part(name)
    app.set_print_process(**proc)
    c, k = s["edge_chamfer"], s["wall"]
    ri, R = s["inner_diameter"] / 2.0, s["outer_d"] / 2.0
    zs, zr = s["z_shoulder"], s["z_rim"]
    rn, rl = s["neck_face_d"] / 2.0, s["lip_r"]
    pts = [(0, 0), (R - c, 0), (R, c), (R, zs), (rn, zs),
           (rn, zs + s["thread_length"]), (rl, zs + s["thread_length"]),
           (rl, zr - s["rim_chamfer"]), (ri + s["seal_flat"], zr), (ri, zr),
           (ri, s["floor"] + k), (ri - k, s["floor"]), (0, s["floor"])]
    p.revolve_profile(pts, plane=Plane.xz(), name=name)
    body = p.body()
    neck = body.face(kind="cylinder", radius=rn)
    t = p.thread(neck, "jar neck", modeled=True, pitch=s["pitch"],
                 flank_angle_deg=flank)
    p.material(material)
    p.check()
    # The frames the assembly mates by, on circular edges so their origins
    # are on the axis whatever the thread does to the faces.
    body = p.body()
    shoulder = body.edge(kind="circle", radius=R,
                         where=lambda e: abs(e.midpoint[2] - zs) < 1e-6)
    p.frame("shoulder", of=shoulder)
    return p, t


def build_lid(app, s, proc, material, name="lid", extra_skirt=0.0):
    """The lid, top down: as it prints. `extra_skirt` lengthens the skirt
    below the thread (the negative control: a lid that stops short)."""
    p = app.part(name)
    app.set_print_process(**proc)
    c = s["edge_chamfer"]
    R = s["outer_d"] / 2.0
    top, g = s["lid_top"], s["groove_depth"]
    mouth = top + s["skirt"] + extra_skirt
    rb = s["lid_bore_d"] / 2.0
    go, gi = s["groove_out_d"] / 2.0, s["groove_in_d"] / 2.0
    pts = [(0, 0), (R - c, 0), (R, c), (R, mouth), (rb, mouth), (rb, top),
           (go, top), (go, top - g), (gi, top - g), (gi, top), (0, top)]
    p.revolve_profile(pts, plane=Plane.xz(), name=name)
    body = p.body()
    bore = body.face(kind="cylinder", radius=rb)
    t = p.thread(bore, "jar lid", modeled=True, pitch=s["pitch"],
                 flank_angle_deg=FLANK_DEG)
    p.material(material)
    p.check()
    body = p.body()
    p.frame("mouth", of=body.edge(
        kind="circle", radius=R,
        where=lambda e: abs(e.midpoint[2] - mouth) < 1e-6))
    p.frame("groove", of=body.edge(
        kind="circle", radius=go,
        where=lambda e: abs(e.midpoint[2] - (top - g)) < 1e-6))
    return p, t


def build_gasket(app, s, tpu_proc, tpu_allow):
    """The gasket, flat: a rectangular ring, printed in TPU 95A
    (`tpu_proc`), its bore and its outside modelled at the size they
    should print plus the TPU table's hole and shaft allowances
    (`tpu_allow`, docs/tolerances/README.md#tpu)."""
    p = app.part("gasket")
    app.set_print_process(**tpu_proc)
    ri = (s["gasket_in_d"] + tpu_allow["hole"]) / 2.0
    ro = (s["gasket_out_d"] + tpu_allow["shaft"]) / 2.0
    t = s["gasket_t"]
    p.revolve_profile([(ri, 0), (ro, 0), (ro, t), (ri, t)],
                      plane=Plane.xz(), name="gasket")
    p.material("tpu")
    p.check()
    body = p.body()
    p.frame("seat", of=body.edge(kind="circle", radius=ro,
                                 where=lambda e: abs(e.midpoint[2] - t) < 1e-6))
    p.frame("underside", of=body.edge(
        kind="circle", radius=ro, where=lambda e: abs(e.midpoint[2]) < 1e-6))
    return p


# The gasket's slicer settings (set_print_settings): one wall and two
# layers top and bottom of 15 % gyroid, so the TPU stays soft enough to
# seal. Counts and a percentage.
GASKET_PRINT = {"walls": 1, "top_layers": 2, "bottom_layers": 2,
                "infill_pct": 15, "infill_pattern": "gyroid"}


def job_plate(app, name, proc, pieces, bed):
    """A print plate for a job: a Derived copy of each (part, x offset from
    the bed's middle) on the bed, every piece round its own axis, and its
    check_print. Returns the plate's check."""
    ids = {p["name"]: p["id"] for p in app.call("list_parts")["parts"]}
    app.call("add_part", name=name)
    app.call("set_print_process", **proc)
    for part, dx in pieces:
        r = app.call("add_feature", feature={
            "type": "Derived", "name": part, "source_part": ids[part],
            "reference": False,
            "placement": {"position": [bed[0] / 2.0 + dx, bed[1] / 2.0, 0.0],
                          "orientation": [0.0, 0.0, 0.0, 1.0]}})
        if not r.get("regen", {}).get("ok", True):
            raise SystemExit(f"jar: {name}: {part}: {r['regen']}")
    return check_print(app, f"jar: {name}", need=("bed", "shells", "bed_fit"),
                       quiet=True)


def frame_z(part, name):
    part.activate()
    return part.get_frame(name).z[2]


def verified(app, part):
    part.activate()
    geo = app.check_geometry()
    if geo["receipt"]["standing"] != "verified":
        raise SystemExit(f"jar: {part.name} is not verified: "
                         f"{geo['receipt']}")


def export(app, part, path, deflection=None, **limits):
    """Check that `part` prints -- on the bed, one watertight piece, nothing
    overhanging past the limit (check_print) -- then export it."""
    part.activate()
    pr = check_print(app, f"jar: {part.name}",
                     need=("bed", "shells", "overhang"),
                     quiet=True, **limits)
    kw = {"path": str(path)}
    if deflection is not None:
        kw["stl_deflection_mm"] = deflection
    exp = app.export_stl(**kw)
    oh = pr["overhang"]
    return {"path": str(path), "deflection_mm": exp.get("deflection_mm"),
            "worst_overhang_deg": round(oh["worst_deg"], 3),
            "overhang_mm2": round(oh["area_mm2"], 3),
            "shells": exp.get("shells"), "print": print_receipt(pr)}


class Jar:
    """The assembly: the jar fixed, the lid on a cylindrical mate from the
    jar's shoulder to its own mouth, the gasket fastened in the lid's
    groove."""

    def __init__(self, app, name, jar, lid, gasket):
        self.app = app
        self.asm = app.assembly(name)
        self.jar = self.asm.place(jar, fixed=True, name="jar")
        self.lid = self.asm.place(lid, name="lid")
        self.gasket = self.asm.place(gasket, name="gasket")
        # Face to face: the lid is upside down in use, so its frame's z
        # points the other way in the world. The frames come off circular
        # edges, whose normal may point either way along the axis: the flip
        # is chosen from them, and the placement checked below.
        zj, zl = frame_z(jar, "shoulder"), frame_z(lid, "mouth")
        self.asm.activate()
        self.screw = self.asm.cylindrical(
            (self.jar, "shoulder"), (self.lid, "mouth"),
            flipped=not math.isclose(zl, -zj), name="lid on the neck")["id"]
        zg, zgr = frame_z(gasket, "seat"), frame_z(lid, "groove")
        self.asm.activate()
        # The gasket stays the way up it printed: its top face against the
        # groove's floor.
        self.asm.fasten((self.lid, "groove"), (self.gasket, "seat"),
                        flipped=not math.isclose(zg, -zgr),
                        name="gasket in the groove")
        self.pose(0.0, 0.0)

    def pose(self, rotation_deg, offset):
        self.asm.activate()
        self.app.set_mate(id=self.screw, rotation_deg=rotation_deg,
                          offset=offset)
        r = self.asm.solve()
        if r.get("errored_mates"):
            raise SystemExit(f"jar: the mates did not solve: {r}")

    def interference(self, a, b):
        self.asm.activate()
        return self.app.interference(a=a, b=b)

    def squeeze(self, s, rim_z):
        """The squeeze, measured on the solved assembly: the rim's flat (the
        jar is fixed, so its part height is the world's) against the
        gasket's underside where the mates put it; and the exact overlap of
        gasket and jar against the rim's section above that depth."""
        under = self.gasket.frame("underside").origin[2]
        depth = rim_z - under
        r = self.interference("gasket", "jar")
        vol = r.get("volume_mm3", 0.0) if r.get("clash") else 0.0
        want = squeeze_volume(s, depth) if depth > 0.0 else 0.0
        frac = depth / s["gasket_t"]
        ok = (SQUEEZE_BAND[0] <= frac <= SQUEEZE_BAND[1]
              and abs(vol - want) <= 0.02 * max(want, 1e-9))
        return {"underside_z": under, "rim_z": rim_z, "squeeze_mm": depth,
                "fraction": frac, "overlap_mm3": vol,
                "expected_overlap_mm3": want, "ok": ok}


def find_phase(jar, s, step_deg=15.0):
    """The lid's turn at its stop at which its thread meets the jar's.
    Found, not computed from where the kernel starts each helix: the exact
    interference of lid and jar over a turn, and the middle of the clean
    window, refined at both edges to a tenth of a degree."""
    def vol(th):
        jar.pose(th, 0.0)
        r = jar.interference("lid", "jar")
        return r.get("volume_mm3", 0.0) if r.get("clash") else 0.0

    n = int(round(360.0 / step_deg))
    samples = [(i * step_deg, vol(i * step_deg)) for i in range(n)]
    clean = [th for th, v in samples if v <= 0.0]
    if not clean or len(clean) == n:
        raise SystemExit(f"jar: no clean window for the lid's thread at its "
                         f"stop ({len(clean)} of {n} turns clean): "
                         f"{samples}")
    # The clean run (it may wrap through 0): start where clean follows dirty.
    i0 = next(i for i in range(n)
              if samples[i][1] <= 0.0 and samples[i - 1][1] > 0.0)
    run = []
    i = i0
    while samples[i % n][1] <= 0.0:
        run.append(i)
        i += 1
    lo_in, hi_in = run[0] * step_deg, run[-1] * step_deg

    def edge(inside, outside):
        for _ in range(8):  # 15 deg / 2^8 < 0.1 deg
            mid = (inside + outside) / 2.0
            if vol(mid) <= 0.0:
                inside = mid
            else:
                outside = mid
        return inside

    lo = edge(lo_in, lo_in - step_deg)
    hi = edge(hi_in, hi_in + step_deg)
    return (lo + hi) / 2.0, hi - lo


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int)
    ap.add_argument("--inner-diameter", type=float, default=60.0,
                    help="the opening, mm (the bore is straight)")
    ap.add_argument("--volume", type=float, default=200.0,
                    help="what the jar holds to the rim, ml")
    ap.add_argument("--process", default="fdm")
    ap.add_argument("--material", default="pla")
    ap.add_argument("--profile", default=None,
                    help="one of your print profiles (list_print_profiles), "
                         "measured on your printer; without it the built-in "
                         "--process/--material table")
    ap.add_argument("--tpu-profile", default=None,
                    help="the gasket's TPU print profile; default the one "
                         "TPU profile of yours on --profile's printer (its "
                         "bed), else the built-in fdm tpu table")
    ap.add_argument("--fresh", action="store_true",
                    help="start a new project first (refused on unsaved work)")
    ap.add_argument("--out", default=None)
    ap.add_argument("--no-render", action="store_true",
                    help="skip the stills (a --headless instance has no "
                         "renderer)")
    args = ap.parse_args()
    out = Path(args.out) if args.out else OUT
    t0 = time.perf_counter()

    app = connect(args.port)
    if args.fresh:
        app.new()
    proc = ({"profile": args.profile} if args.profile else
            {"process": args.process, "material": args.material})
    material = args.material
    profiles = app.list_print_profiles()["profiles"]
    if args.profile:
        material = next((p["material"] for p in profiles
                         if p["name"] == args.profile), None)
        if material is None:
            raise SystemExit(f"jar: no print profile '{args.profile}'")
    # The gasket's process: TPU on the same printer -- the bed is the
    # printer's, so its TPU profile is the one sharing the PLA profile's
    # bed -- or the built-in TPU table without a profile.
    tpu_proc = {"process": "fdm", "material": "tpu"}
    if args.tpu_profile:
        if not any(p["name"] == args.tpu_profile and p["material"] == "tpu"
                   for p in profiles):
            raise SystemExit(f"jar: no TPU print profile "
                             f"'{args.tpu_profile}'")
        tpu_proc = {"profile": args.tpu_profile}
    elif args.profile:
        bed = next(p for p in profiles
                   if p["name"] == args.profile).get("bed_mm")
        mine = [p["name"] for p in profiles if p["material"] == "tpu"
                and bed and p.get("bed_mm") == bed]
        if len(mine) > 1:
            raise SystemExit(f"jar: several TPU profiles on that printer "
                             f"({', '.join(mine)}): pass --tpu-profile")
        if mine:
            tpu_proc = {"profile": mine[0]}

    # The allowances, each at the size it applies to. The thread's sizes
    # depend on its own allowances only through the clearance, so the
    # first solve's sizes are the ones to ask at.
    probe = solve(args.inner_diameter, args.volume,
                  {"external_thread": 0, "internal_thread": 0, "hole": 0,
                   "shaft": 0}, probe=True)
    sizes = {"external_thread": probe["basic_major"],
             "internal_thread": probe["basic_major"],
             "hole": probe["gasket_out_d"], "shaft": probe["gasket_in_d"]}
    allow, confirm = {}, None
    for kind, size in sizes.items():
        r = app.print_allowance(kind=kind, size_mm=size, **proc)
        allow[kind] = r["allowance_mm"]
        confirm = r.get("confirm", confirm)
    s = solve(args.inner_diameter, args.volume, allow)
    tpu_allow = {
        "hole": app.print_allowance(kind="hole", size_mm=s["gasket_in_d"],
                                    **tpu_proc)["allowance_mm"],
        "shaft": app.print_allowance(kind="shaft",
                                     size_mm=s["gasket_out_d"],
                                     **tpu_proc)["allowance_mm"]}
    print(f"jar: Ø{args.inner_diameter:g} x {s['inner_height']:.1f} "
          f"({args.volume:g} ml), thread {s['basic_major']:.2f} x "
          f"{s['pitch']:g} at {FLANK_DEG:g} deg, {s['turns']} turns; "
          f"clearance {s['thread_radial_clearance']:.3f} radial, "
          f"{s['thread_flank_clearance']:.3f} on a flank; gasket "
          f"{s['gasket_t']:g} mm squeezed {s['squeeze']:g} "
          f"({args.profile or args.material})")

    jar_p, jar_t = build_jar(app, s, proc, material)
    lid_p, lid_t = build_lid(app, s, proc, material)
    gasket_p = build_gasket(app, s, tpu_proc, tpu_allow)
    for p in (jar_p, lid_p, gasket_p):
        verified(app, p)

    # What the print reports for each thread: modelled with the allowance,
    # each must PRINT at the basic major.
    printed = {}
    for p, t in ((jar_p, jar_t), (lid_p, lid_t)):
        p.activate()
        r = app.get_print_process()
        row = next(x for x in r.get("threads", []) if x["feature_id"] == t.id)
        printed[p.name] = row
        # The overlay carries the face's radius as a float: a micrometre.
        if (abs(row["prints_major"] - s["basic_major"]) > 1e-4
                or abs(row["pitch"] - s["pitch"]) > 1e-9):
            raise SystemExit(f"jar: the {p.name}'s thread prints "
                             f"Ø{row['prints_major']:.3f}, not the basic "
                             f"Ø{s['basic_major']:.3f}: {row}")

    jar = Jar(app, "screw-top jar", jar_p, lid_p, gasket_p)
    # The lid upside down and on its stop: its mouth on the shoulder, and
    # its ceiling (the groove's floor) above it, which only an upside-down
    # lid has. A wrong flip puts either elsewhere.
    mouth = jar.lid.frame("mouth").origin[2]
    groove = jar.lid.frame("groove").origin[2]
    want_groove = s["z_shoulder"] + s["skirt"] + s["groove_depth"]
    if abs(mouth - s["z_shoulder"]) > 1e-4 or abs(groove - want_groove) > 1e-4:
        raise SystemExit(f"jar: the lid's mouth is at z {mouth:.3f} and its "
                         f"groove at {groove:.3f}, not {s['z_shoulder']:.3f} "
                         f"and {want_groove:.3f}")
    phase, window = find_phase(jar, s)
    phase = math.remainder(phase, 360.0)
    print(f"  lid thread meets the jar's at {phase:.1f} deg "
          f"(clean over {window:.1f} deg of turn)")
    checks = {"phase_deg": phase, "clean_window_deg": window}
    # A clean window is the axial play over the lead: anything much less is
    # a thread that binds, much more one that does not engage.
    want_window = 360.0 * s["thread_axial_play"] / s["pitch"]
    if not 0.5 * want_window <= window <= 1.5 * want_window:
        raise SystemExit(f"jar: the clean window is {window:.1f} deg, the "
                         f"play says {want_window:.1f}")

    # Negative control: half a pitch out of phase at the stop interferes.
    jar.pose(phase + 180.0, 0.0)
    bad = jar.interference("lid", "jar")
    checks["control_half_pitch_out"] = bad
    if not bad.get("clash"):
        raise SystemExit("jar: the lid half a pitch out of phase reads clean; "
                         "the thread check cannot see the thread")
    print(f"  control, lid half a pitch out: clash "
          f"{bad.get('volume_mm3', 0):.0f} mm3")
    jar.pose(phase, 0.0)

    # The squeeze at the stop.
    jar_p.activate()
    rim = jar_p.body().face(kind="plane", normal=(0, 0, 1),
                            where=lambda f: abs(f.centroid[2] - s["z_rim"])
                            < 1e-6)
    rim_z = rim.centroid[2]
    sq = jar.squeeze(s, rim_z)
    checks["squeeze"] = sq
    print(f"  squeeze {sq['squeeze_mm']:.3f} mm = {100 * sq['fraction']:.1f} %"
          f" of {s['gasket_t']:g}; overlap {sq['overlap_mm3']:.1f} mm3 "
          f"(the rim's section says {sq['expected_overlap_mm3']:.1f})")
    if not sq["ok"]:
        raise SystemExit(f"jar: the squeeze fails: {sq}")

    # Everything else: nothing clashes but the squeeze itself, nothing
    # floats. Clearances are running fits here (the thread, the gasket's
    # bore), so near misses are expected and not raised.
    rep = jar.asm.check(allow={("gasket", "jar"): 1.02 * sq["overlap_mm3"]},
                        clearances=False)
    checks["contact_report"] = {"clashes": rep.get("clashes"),
                                "components": len(rep.get("components", []))}

    # Unscrewing: a screw relation backed by the lid's thread couples its
    # turn to its rise. From the turn where the gasket lets go, two turns
    # out, with a clash check fine enough to see the flank clearance.
    lead = s["pitch"]
    jar.asm.activate()
    rel = jar.asm.screw(jar.screw, lid_t, name="lid thread")
    ratio = next(r for r in app.list_relations()["relations"]
                 if r["id"] == rel["id"]).get("ratio")
    # dof_b = ratio * dof_a + phase: zero offset at the found turn.
    app.set_relation(id=rel["id"], phase=-ratio * math.radians(phase))
    jar.pose(phase, 0.0)
    probe_deg = 10.0
    for sign in (1.0, -1.0):
        jar.asm.activate()
        app.set_mate(id=jar.screw, rotation_deg=phase + sign * probe_deg)
        jar.asm.solve()
        if jar.lid.frame("mouth").origin[2] > s["z_shoulder"] + 1e-6:
            out_sign = sign
            break
    else:
        raise SystemExit("jar: neither way round lifts the lid")
    release = 360.0 * (sq["squeeze_mm"] + LAYER_MM) / lead
    start = phase + out_sign * release
    end = start + out_sign * 360.0 * TURNS
    clash_mm = s["thread_flank_clearance"] / 5.0
    jar.pose(phase, 0.0)
    st = app.add_motion_study(driver=jar.screw, dof="rotation",
                              **{"from": start}, to=end,
                              samples=24 * TURNS, clash_mode="mesh",
                              clash_deflection_mm=clash_mm,
                              name="unscrew the lid")
    if st.get("why_not"):
        raise SystemExit(f"jar: unscrew study: {st['why_not']}")
    sid = st.get("id", st.get("study"))
    app.run_motion_study(study=sid, **{"async": True})
    while True:
        r = app.motion_status(study=sid)
        if not r.get("running") and not r.get("job"):
            break
        time.sleep(1.0)
    res = r["result"]
    checks["unscrew"] = {"from_deg": start, "to_deg": end,
                         "status": res["collision_status"],
                         "summary": res["receipt"]["summary"]}
    print(f"  unscrew {TURNS} turns from where the gasket lets go: "
          f"{res['collision_status']}")
    if not res["clean"]:
        raise SystemExit(f"jar: unscrewing the lid clashes: "
                         f"{res.get('table', res)}")
    jar.pose(phase, 0.0)
    app.remove_relation(id=rel["id"])

    # Negative control: a lid whose skirt is longer by the squeeze and a
    # layer stops on the shoulder short of the gasket. Its squeeze must
    # fail the same check.
    short_p, _ = build_lid(app, s, proc, material, name="lid (stops short)",
                           extra_skirt=s["squeeze"] + LAYER_MM)
    control = Jar(app, "screw-top jar (lid stops short)", jar_p, short_p, gasket_p)
    sq_short = control.squeeze(s, rim_z)
    checks["control_stops_short"] = sq_short
    print(f"  control, a lid that stops short: squeeze "
          f"{sq_short['squeeze_mm']:.3f} mm = "
          f"{100 * sq_short['fraction']:.1f} % -> "
          f"{'PASSES (bad)' if sq_short['ok'] else 'fails, as it must'}")
    if sq_short["ok"]:
        raise SystemExit("jar: a lid that stops short passes the squeeze "
                         "check; it cannot see a lid that does not seal")

    out.mkdir(parents=True, exist_ok=True)
    stls = {
        "jar": dict(export(app, jar_p, out / "jar.stl"), material=material),
        "lid": dict(export(app, lid_p, out / "lid.stl"), material=material),
        # Printed in TPU: its own process's limits and chords.
        "gasket": dict(export(app, gasket_p, out / "gasket.stl"),
                       material="tpu"),
    }
    # Negative control: the same jar with the ISO 60-degree flank must fail
    # the overhang check, or the check cannot see a thread.
    iso_p, _ = build_jar(app, s, proc, material, name="jar (ISO flank)",
                         flank=30.0)
    iso_p.activate()
    iso = check_print(app, "jar (ISO flank)", need=(), quiet=True)["overhang"]
    checks["control_iso_flank"] = {
        "worst_overhang_deg": round(iso["worst_deg"], 3),
        "overhang_mm2": round(iso["area_mm2"], 1), "status": iso["status"]}
    print(f"  control, the ISO 60-degree thread: {iso['area_mm2']:.0f} mm2 "
          f"past {iso['limit_deg']:g} deg (worst {iso['worst_deg']:.2f}) -> "
          f"{'fails, as it must' if iso['status'] == 'fail' else 'PASSES (bad)'}")
    if iso["status"] != "fail":
        raise SystemExit("jar: an ISO-flank thread passes the overhang check")
    # The jobs: the jar and the lid on one PLA plate, the gasket on a TPU
    # plate of its own -- one job a spool. Only with a profile's bed: the
    # built-in table is no printer to place them on.
    app.call("set_print_settings", part="gasket", **GASKET_PRINT)
    prof = (next((p for p in app.list_print_profiles()["profiles"]
                  if p["name"] == args.profile), None)
            if args.profile else None)
    jobs = {}
    if prof and prof.get("bed_mm"):
        bed = prof["bed_mm"]
        # Side by side, two walls apart.
        gap = s["outer_d"] / 2.0 + s["wall"]
        job_plate(app, "jar plate", proc, [("jar", -gap), ("lid", gap)], bed)
        jobs["pla"] = job_receipt(export_print_job(
            app, [("jar plate", "jar and lid")], out / "jar_job.3mf",
            material))
        if "profile" not in tpu_proc:
            raise SystemExit("jar: the gasket's job needs a TPU print "
                             "profile on the printer (print_coupon.py "
                             "--seed installs 'TPU 95A on P1S', or pass "
                             "--tpu-profile)")
        job_plate(app, "gasket plate", tpu_proc, [("gasket", 0.0)], bed)
        jobs["tpu"] = job_receipt(export_print_job(
            app, [("gasket plate", "gasket")], out / "jar_tpu_job.3mf", "tpu"))
        mp = app.call("mass_properties", part="gasket plate")["bodies"][0]
        jobs["gasket_g"] = {"solid": mp.get("mass_g"),
                            "printed": (mp.get("printed") or {}).get("mass_g")}
    else:
        jobs["skipped"] = ("no print profile with a bed: the built-in table "
                           "is no printer to place the plates on")
    for k, v in stls.items():
        print(f"  {k}.stl ({v['material']}): {v['shells']} shell, "
              f"chords {v['deflection_mm']} mm, steepest overhang "
              f"{v['worst_overhang_deg']} deg")

    if not args.no_render:
        jar.asm.activate()
        app.view(name="iso")
        app.render_still(path=str(out / "jar.png"), width=1600, height=1200)
        # Cut through the axis, seen from the cut side: the threads
        # interleaved and the gasket pressed onto the rim.
        app.set_section(base_plane="xz", offset=0.0, enabled=True)
        app.set_camera(direction=[0.0, 1.0, 0.0])
        app.render_still(path=str(out / "jar-section.png"), width=1600,
                         height=1200)
        app.set_section(enabled=False)

    receipt = {"process": proc, "allowances": allow, **s,
               "gasket_process": tpu_proc, "gasket_allowances": tpu_allow,
               "printed_threads": printed, "checks": checks, "stl": stls,
               "jobs": jobs, "gasket_print": GASKET_PRINT,
               "confirm": confirm,
               "seconds": round(time.perf_counter() - t0, 1)}
    (out / "jar.json").write_text(json.dumps(receipt, indent=1))
    jar.asm.activate()
    app.save_project(path=str(out / "jar.dsgn"))
    print(f"jar: done in {receipt['seconds']} s -> {out}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SdkError as e:
        raise SystemExit(f"jar: {e}")
