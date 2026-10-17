#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A print-in-place hinged box: a shallow box and its lid printed flat side
by side, joined along one edge by a hinge of alternating teardrop knuckles
on 45-degree cone pins. Off the bed, the lid swings 180 degrees onto the
rim.

    python3 generators/hinge_box/build.py --port 8801 --profile "K5 PLA on P1S"
    python3 generators/hinge_box/build.py --port 8801 --size 80

The test piece for the two rules a print-in-place part follows
(docs/tolerances/README.md#one-part-printed-over-another): every piece starts on the bed, and
where one part prints over another the face above is self-supporting (45
degrees, or a bridge between its own walls) with at least the print's
minimum air under it. Here the only faces over another part are the cone
sockets over the cone pins.

Every size derives from `--size` (the box's inside width along the hinge),
the print process's gap (`print_allowance` kind `gap`, half of it a side)
and the minimum air (check_print's stacking limit: the profile's
`z_gap_mm`, else three layers of its `layer_height_mm`, else of `--layer`,
else of the FDM preset's 0.2 mm): nothing here is a literal in millimetres
(generators/hinge_box/README.md#where-every-size-comes-from).

What it builds:
- parts `box` and `lid`, each with a Frame `hinge` on the hinge axis;
- the part `hinge box print`: both as Derived copies where they print,
  exported as ONE STL of two shells;
- an assembly `hinge box` with the lid on a revolute about the hinge axis
  and a motion study from 0 to 180 degrees;
- the negative controls, parts of their own (README).

Output (generated, untracked): scratch/hinge_box/ -- hinge_box.stl,
hinge_box.dsgn and hinge_box.json, the receipt.
"""
import argparse
import json
import math
import os
import sys
import time
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from _lib import (PRINT_ITEMS, check, check_print, connect,  # noqa: E402
                  export_print_job, fresh_project, job_receipt, line,
                  print_item_line, print_receipt)

ROOT = Path(HERE).resolve().parent.parent
OUT = Path(os.environ["LOBOLE_SCRATCH"]) / "hinge_box" if \
    os.environ.get("LOBOLE_SCRATCH") else ROOT / "scratch" / "hinge_box"

KNUCKLES = 5      # box, lid, box, lid, box: the box carries both ends
# The air under a face over another piece, in layers, when the profile
# states no z_gap_mm: check_print's stacking limit (printfit::MinAirMm).
AIR_LAYERS = 3
FDM_LAYER_MM = 0.2  # Bambu Studio's "0.20 mm" preset: the FDM fallback
NOZZLE_MM = 0.4  # the P1S's stock nozzle, which the allowance table assumes
MIN_WALL_MM = 4 * NOZZLE_MM


# ---- sizes ----------------------------------------------------------------

def box_sizes(size):
    """The box from its inside width: walls, floor and lid a 25th of it
    (2.4 mm, six 0.4 mm lines, at 60), as deep as 0.6 of it and as high as
    0.3 outside."""
    w = 0.04 * size
    return {"wall": w, "floor": w, "lid": w, "depth_in": 0.6 * size,
            "height": 0.3 * size}


def axis_height(size):
    """Half the box's height plus the lid's thickness: the lid turned 180
    degrees about an axis that high lies on the rim. It is also the
    knuckle's radius, so the knuckle's circle meets the bed."""
    b = box_sizes(size)
    return (b["height"] + b["lid"]) / 2.0


def solve(size, gap, layer, min_air):
    """Every size from the box's inside width, the gap a side, the layer
    and the minimum air. Refuses what will not print."""
    g = gap / 2.0
    s = {"size": size, "gap": gap, "g": g, "layer": layer,
         "min_air": min_air, **box_sizes(size)}
    w = s["wall"]
    s["width"] = size + 2.0 * w                  # outside, along the hinge
    s["depth"] = s["depth_in"] + 2.0 * w         # outside, across it
    h = axis_height(size)
    s["axis_z"] = h
    # The teardrop: the knuckle's circle about the axis, as large as the
    # axis is high, with 45-degree flanks down to a flat on the bed.
    s["r_knuckle"] = h
    s["flat"] = (math.sqrt(2.0) - 1.0) * h        # the flat's half width
    s["swept"] = math.hypot(s["flat"], h)         # the flat's corners
    # Each part stands off the axis by the radius the other part's knuckles
    # sweep, plus the gap: what the knuckle swings through is relieved.
    s["standoff"] = s["swept"] + g
    s["knuckle_len"] = (s["width"] - (KNUCKLES - 1) * g) / KNUCKLES
    # The cone pin: 45 degrees, on the box's knuckle, in a cone socket in
    # the lid's. A 45-degree cone's vertical air is its normal clearance
    # times sqrt 2, and the air must be the stacking limit: so the socket is
    # the pin moved `air` along the axis. At least the gap a side normal to
    # the cone, in whole layers.
    air = max(min_air, layer * math.ceil(g * math.sqrt(2.0) / layer - 1e-9))
    s["cone_air"] = air
    s["cone_clearance"] = air / math.sqrt(2.0)
    s["r_pin"] = 0.45 * h
    s["r_tip"] = 0.3 * s["r_pin"]
    s["pin_len"] = s["r_pin"] - s["r_tip"]
    socket_depth = s["pin_len"] + g
    if 2.0 * socket_depth > 0.8 * s["knuckle_len"]:
        raise SystemExit(f"hinge_box: a lid knuckle {s['knuckle_len']:.2f} "
                         f"mm long cannot hold two {socket_depth:.2f} mm "
                         f"sockets -- a larger --size")
    if h - (s["r_pin"] + air) < w / 2.0:
        raise SystemExit("hinge_box: the socket leaves the knuckle too thin "
                         "a wall -- a larger --size")
    if w < MIN_WALL_MM:
        raise SystemExit(f"hinge_box: {w:.2f} mm walls at --size {size:g} "
                         f"-- a larger --size")
    return s


def segments(s):
    """[(x0, x1, owner)] along the hinge, owner 'box' or 'lid'."""
    out = []
    x = 0.0
    for k in range(KNUCKLES):
        out.append((x, x + s["knuckle_len"], "box" if k % 2 == 0 else "lid"))
        x += s["knuckle_len"] + s["g"]
    return out


# ---- sketching --------------------------------------------------------------

def polygon(pts):
    return [line(pts[i], pts[(i + 1) % len(pts)]) for i in range(len(pts))]


def plane_yz(x0):
    """The YZ plane at x0: u = y, v = z (V = normal x u = X x Y = Z)."""
    return {"origin": [x0, 0.0, 0.0], "normal": [1, 0, 0],
            "uAxis": [0, 1, 0]}


def plane_z(z):
    return {"origin": [0.0, 0.0, z], "normal": [0, 0, 1], "uAxis": [1, 0, 0]}


def plane_xz():
    """The XZ plane through the origin: u = x, v = z (normal -Y)."""
    return {"origin": [0.0, 0.0, 0.0], "normal": [0, -1, 0],
            "uAxis": [1, 0, 0]}


def teardrop(s):
    """The knuckle's section in the YZ plane: an arc over the top and
    45-degree flanks down to the flat."""
    h, r, f = s["axis_z"], s["r_knuckle"], s["flat"]
    t = r / math.sqrt(2.0)
    left, right = (-t, h - t), (t, h - t)
    return [line((-f, 0.0), (f, 0.0)), line((f, 0.0), right),
            {"type": "arc", "center": [0.0, h], "radius": r,
             "start_angle": -math.pi / 4.0,
             "end_angle": math.pi + math.pi / 4.0},
            line(left, (-f, 0.0))]


class Builder:
    """Features into the active part, each body by the id that made it."""

    def __init__(self, d):
        self.d = d

    def sketch(self, name, plane, ents):
        return check(self.d.call("add_feature", feature={
            "type": "Sketch", "name": name, "plane": plane,
            "entities": ents}), name)["id"]

    def extrude(self, name, plane, ents, distance, mode="new_body", **kw):
        sk = self.sketch(f"{name} sketch", plane, ents)
        return check(self.d.call("add_feature", feature={
            "type": "Extrude", "name": name, "sketch_id": sk,
            "distance": distance, "mode": mode, **kw}), name)["id"]

    def revolve_x(self, name, profile, z_axis, mode):
        """A solid of revolution about the hinge axis (along X at height
        z_axis): `profile` is (x, z) points with z >= z_axis, a closed
        polygon touching the axis."""
        sk = self.sketch(f"{name} profile", plane_xz(), polygon(profile))
        return check(self.d.call("add_feature", feature={
            "type": "Revolve", "name": name, "sketch_id": sk,
            "axis": {"kind": "custom", "origin": [0.0, 0.0, z_axis],
                     "direction": [1.0, 0.0, 0.0]},
            "angle_deg": 360.0, "mode": mode}), name)["id"]

    def frame(self, s):
        check(self.d.call("add_feature", feature={
            "type": "Frame", "name": "hinge", "kind": "explicit",
            "origin": [0.0, 0.0, s["axis_z"]], "z_axis": [1, 0, 0],
            "x_axis": [0, 1, 0]}), "hinge frame")


def rect(x0, y0, x1, y1):
    return polygon([(x0, y0), (x1, y0), (x1, y1), (x0, y1)])


# ---- the parts ------------------------------------------------------------
# Both in the print's frame: X along the hinge, the axis at y = 0 and
# z = axis_z, the box on -Y and the lid on +Y, the bed at z = 0.

def build_box(b, s):
    W, D, H, w = s["width"], s["depth"], s["height"], s["wall"]
    a = s["standoff"]
    b.extrude("box shell", plane_z(0.0), rect(0.0, -a - D, W, -a), H,
              mode="new_body")
    b.extrude("box pocket", plane_z(s["floor"]),
              rect(w, -a - D + w, W - w, -a - w), H, mode="cut")
    segs = segments(s)
    for k, (x0, x1, owner) in enumerate(segs):
        if owner != "box":
            continue
        b.extrude(f"box knuckle {k + 1}", plane_yz(x0), teardrop(s),
                  x1 - x0, mode="add")
        # The web from the knuckle to the back wall, up to the axis.
        b.extrude(f"box web {k + 1}", plane_yz(x0),
                  rect(-a - w / 2.0, 0.0, 0.0, s["axis_z"]), x1 - x0,
                  mode="add")
    # A cone pin on each end of a box knuckle that has a lid knuckle beside
    # it.
    for k, (x0, x1, owner) in enumerate(segs):
        if owner != "box":
            continue
        if k + 1 < len(segs):
            b.revolve_x(f"box pin {k + 1}+", pin_profile(s, x1, 1.0),
                        s["axis_z"], "add")
        if k > 0:
            b.revolve_x(f"box pin {k + 1}-", pin_profile(s, x0, -1.0),
                        s["axis_z"], "add")
    b.frame(s)


def pin_profile(s, face, sign):
    """The pin's half section (x, z) about the axis, from inside the
    knuckle out to its flat tip: 45 degrees, r_pin at the knuckle's end
    face. `sign` is the direction it points along X."""
    h, rp, L = s["axis_z"], s["r_pin"], s["pin_len"]
    sink = s["g"]  # into the knuckle, so the union is a solid
    pts = [(-sink, 0.0), (-sink, rp + sink), (L, rp - L), (L, 0.0)]
    return [(face + sign * u, h + v) for u, v in pts]


def socket_profile(s, face, sign, air):
    """The socket's half section: the pin moved `air` along the axis away
    from its knuckle (a normal clearance of air / sqrt 2 on the cone, and
    `air` of vertical air over the pin's top and under its bottom), to the
    gap a side past the pin's flat tip. `face` is the box knuckle's end
    face the pin stands on; the socket opens a little before the lid
    knuckle's end face, a gap further on."""
    h, rp, L, g = s["axis_z"], s["r_pin"], s["pin_len"], s["g"]
    u0 = g / 2.0                       # in the gap, before the lid's face
    u1 = L + g                         # the socket's flat bottom
    pts = [(u0, 0.0), (u0, rp + air - u0), (u1, rp + air - u1), (u1, 0.0)]
    return [(face + sign * u, h + v) for u, v in pts]


def build_lid(b, s, air=None):
    """The lid, its sockets at `air` of vertical air over the pins (default
    the solved one; half of it is a negative control)."""
    air = s["cone_air"] if air is None else air
    W, D, T, w = s["width"], s["depth"], s["lid"], s["wall"]
    a = s["standoff"]
    b.extrude("lid plate", plane_z(0.0), rect(0.0, a, W, a + D), T,
              mode="new_body")
    segs = segments(s)
    for k, (x0, x1, owner) in enumerate(segs):
        if owner != "lid":
            continue
        b.extrude(f"lid knuckle {k + 1}", plane_yz(x0), teardrop(s),
                  x1 - x0, mode="add")
        b.extrude(f"lid web {k + 1}", plane_yz(x0),
                  rect(0.0, 0.0, a + w / 2.0, T), x1 - x0, mode="add")
        # A socket at each end, for the box knuckle's pin beside it.
        for face, sign in ((x0 - s["g"], 1.0), (x1 + s["g"], -1.0)):
            b.revolve_x(f"lid socket {k + 1}{'+' if sign < 0 else '-'}",
                        socket_profile(s, face, sign, air), s["axis_z"],
                        "cut")
    b.frame(s)


# ---- the project ------------------------------------------------------------

def material_of(d, proc):
    if "material" in proc:
        return proc["material"]
    for p in d.call("list_print_profiles")["profiles"]:
        if p["name"] == proc["profile"]:
            return p["material"]
    raise SystemExit(f"hinge_box: no print profile '{proc['profile']}'")


def new_part(d, name, proc):
    d.call("add_part", name=name)
    d.call("set_print_process", **proc)


def part_id(d, name):
    for p in d.call("list_parts")["parts"]:
        if p["name"] == name and p["type"] == "part":
            return p["id"]
    raise SystemExit(f"hinge_box: no part '{name}'")


def one_body(d, what):
    bodies = d.call("list_elements")["bodies"]
    if len(bodies) != 1:
        raise SystemExit(f"hinge_box: {what} is {len(bodies)} bodies, not "
                         f"one: {[b_['name'] for b_ in bodies]}")
    return bodies[0]["created_by"]


def build_part(d, name, proc, mat, fn, *a, **kw):
    new_part(d, name, proc)
    fn(Builder(d), *a, **kw)
    body = one_body(d, name)
    d.call("set_material", feature_id=body, material=mat)
    return body


def plate(d, name, proc, placed, at=(0.0, 0.0)):
    """A part of Derived copies, each where it prints: `placed` is
    [(label, source part, z lift)], all moved by `at` on the bed."""
    ids = {src: part_id(d, src) for _, src, _ in placed}
    new_part(d, name, proc)
    for label, src, lift in placed:
        check(d.call("add_feature", feature={
            "type": "Derived", "name": label, "source_part": ids[src],
            "reference": False,
            "placement": {"position": [at[0], at[1], lift],
                          "orientation": [0.0, 0.0, 0.0, 1.0]}}),
            f"derived {label}")


def bed_centre(d, parts, bed):
    """The move that puts `parts`, together, in the middle of the bed: a
    plate's x and y are the bed's from its front-left corner, where a job
    puts them and its keep-out zones are judged. None without a bed."""
    if not bed:
        return (0.0, 0.0)
    lo, hi = [1e300, 1e300], [-1e300, -1e300]
    for p in parts:
        b = d.call("mass_properties", part=p)["bbox_mm"]
        for k in range(2):
            lo[k] = min(lo[k], b["min"][k])
            hi[k] = max(hi[k], b["max"][k])
    return tuple(bed[k] / 2.0 - (lo[k] + hi[k]) / 2.0 for k in range(2))


def build_assembly(d):
    d.call("add_assembly", name="hinge box")
    box = d.call("add_occurrence", part="box", name="box", fixed=True)["id"]
    lid = d.call("add_occurrence", part="lid", name="lid")["id"]
    mate = d.call("add_mate", type="revolute", name="hinge",
                  a={"occurrence": box, "frame": "hinge"},
                  b={"occurrence": lid, "frame": "hinge"},
                  flipped=False)["id"]
    d.call("solve_assembly")
    return box, lid, mate


def lid_points(s):
    """The lid's points the swing is judged by, in the lid's own (print)
    frame: its centre and its four inner corners, on its inner face."""
    W, D, T, a = s["width"], s["depth"], s["lid"], s["standoff"]
    pts = {"centre": (W / 2.0, a + D / 2.0, T)}
    for i, x in enumerate((0.0, W)):
        for j, y in enumerate((a, a + D)):
            pts[f"corner {i}{j}"] = (x, y, T)
    return pts


def run_swing(d, s, mate, lid_occ, clash, samples):
    """The lid from the bed (0) over onto the rim (180)."""
    st = d.call("add_motion_study", driver=mate, dof="rotation",
                **{"from": 0.0}, to=180.0, samples=samples,
                clash_mode="mesh", clash_deflection_mm=clash,
                name="lid 0 -> 180")
    if st.get("why_not"):
        raise SystemExit(f"hinge_box: motion study: {st['why_not']}")
    sid = st.get("id", st.get("study"))
    # The study traces the lid's points itself (run_motion_study `trace`):
    # where each stands at every sample, in world mm.
    r = d.call("run_motion_study", study=sid, trace=[
        {"name": n, "occurrence": lid_occ, "point_mm": list(p)}
        for n, p in lid_points(s).items()])
    return r.get("result", r)


def check_swing(s, res, lid_occ):
    """Clean all the way over, and the lid on the rim at the end: its inner
    face (z = lid thickness on the bed) at the box's height. The mesh check
    calls a face seated on a face touching, not a clash
    (docs/inspect/README.md#touching-is-not-a-clash), so the last sample is
    clean too, with the lid touching the box."""
    samples = res["samples"]
    D, a = s["depth"], s["standoff"]
    path = {t["name"]: t["points_mm"] for t in res["trace"]}
    if any(pt is None for pts in path.values() for pt in pts):
        raise SystemExit(f"hinge_box: a sample of the swing did not solve: "
                         f"{res.get('table')}")
    up = path["centre"][len(samples) // 2]
    if up[2] < s["axis_z"]:
        raise SystemExit(f"hinge_box: half way over the lid's centre is at "
                         f"z {up[2]:.2f}, under the axis: it swings through "
                         f"the bed")
    corners = [pts[-1] for n, pts in path.items() if n.startswith("corner")]
    rim = [c[2] for c in corners]
    off = max(abs(z - s["height"]) for z in rim)
    over = all(-a - D - 1e-6 <= c[1] <= -a + 1e-6 for c in corners)
    if off > 1e-6 or not over:
        raise SystemExit(f"hinge_box: at 180 the lid's inner face stands at "
                         f"z {rim} over y {[c[1] for c in corners]}, not on "
                         f"the rim at {s['height']:g}")
    before = [smp for smp in samples[:-1] if smp.get("clashes")]
    last = {tuple(sorted((c["a_name"], c["b_name"])))
            for c in samples[-1].get("clashes", [])}
    touching = {tuple(sorted((c["a_name"], c["b_name"])))
                for c in samples[-1].get("touching", [])}
    if before:
        raise SystemExit(f"hinge_box: the lid clashes on its way over at "
                         f"{[smp['value'] for smp in before]} deg: "
                         f"{res.get('table')}")
    if last:
        raise SystemExit(f"hinge_box: at 180 {last} clash: the lid on the "
                         f"rim should touch it, no more")
    if ("box", "lid") not in touching:
        raise SystemExit(f"hinge_box: at 180 the lid does not touch the "
                         f"box's rim: touching {touching}")
    return {"status": res["collision_status"], "samples": len(samples),
            "clean_before_180": True,
            "at_180": ["box x lid touching"],
            "lid_inner_face_z": rim, "rim_z": s["height"],
            "half_way_centre_z": up[2]}


def stacking_fails(r, what, pair=None, lifted=None):
    """A control must fail check_print's stacking item: with `pair`, as air
    under the limit between a piece holding that body and one holding the
    other; with `lifted`, as the piece holding that body off the bed."""
    st = r["stacking"]
    if st["status"] != "fail":
        raise SystemExit(f"hinge_box: {what} passes stacking "
                         f"({print_item_line('stacking', st)}); the check "
                         f"cannot see it")

    def has(piece, name):
        return any(n == name or n.startswith(name + " ") for n in piece)
    if pair:
        hit = [p for p in st["pairs"]
               if (has(p["above"], pair[0]) and has(p["below"], pair[1]))
               or (has(p["above"], pair[1]) and has(p["below"], pair[0]))]
        if not hit:
            raise SystemExit(f"hinge_box: {what} fails stacking, but not "
                             f"between {pair[0]} and {pair[1]}: {st}")
        return {"pairs": [{k: p[k] for k in ("above", "below", "min_air_mm",
                                             "area_mm2")} for p in hit],
                "limit_mm": st["limit_mm"]}
    if lifted:
        hit = [p for p in st["lifted"] if has(p["piece"], lifted)]
        if not hit:
            raise SystemExit(f"hinge_box: {what} fails stacking, but not as "
                             f"{lifted} off the bed: {st}")
        return {"lifted": hit, "limit_mm": st["limit_mm"]}
    return st


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int)
    ap.add_argument("--size", type=float, default=60.0,
                    help="the box's inside width along the hinge, mm")
    ap.add_argument("--layer", type=float, default=None,
                    help="the layer height the parts are sliced at, mm "
                         "(default: the profile's layer_height_mm, else "
                         "0.2, Bambu Studio's 0.20 mm preset)")
    ap.add_argument("--process", default="fdm")
    ap.add_argument("--material", default="pla")
    ap.add_argument("--profile", default=None,
                    help="one of your print profiles (list_print_profiles)")
    ap.add_argument("--samples", type=int, default=37,
                    help="motion study samples, 0 to 180 degrees")
    ap.add_argument("--fresh", action="store_true",
                    help="start a new project first (refused on unsaved work)")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()
    out = Path(args.out) if args.out else OUT
    out.mkdir(parents=True, exist_ok=True)

    d = connect(args.port)
    if args.fresh:
        fresh_project(d)
    proc = ({"profile": args.profile} if args.profile else
            {"process": args.process, "material": args.material})
    prof = next((p for p in d.call("list_print_profiles")["profiles"]
                 if p["name"] == args.profile), None) if args.profile else {}
    if args.profile and prof is None:
        raise SystemExit(f"hinge_box: no print profile '{args.profile}'")
    process = prof.get("process", args.process)
    if process != "fdm":
        raise SystemExit(f"hinge_box: a print-in-place hinge is an FDM "
                         f"part; the process is {process}")
    layer = args.layer or prof.get("layer_height_mm") or FDM_LAYER_MM
    layer_from = ("--layer" if args.layer else "the profile"
                  if prof.get("layer_height_mm") else "the FDM preset")
    if prof.get("z_gap_mm"):
        min_air, air_from = prof["z_gap_mm"], "the profile's z_gap_mm"
    else:
        min_air = AIR_LAYERS * layer
        air_from = f"{AIR_LAYERS} layers of {layer:g} ({layer_from})"
    # The gap at the knuckle's size: the joint the gap is for.
    t0 = time.perf_counter()
    gap = d.call("print_allowance", kind="gap",
                 size_mm=2.0 * axis_height(args.size), **proc)
    s = solve(args.size, gap["allowance_mm"], layer, min_air)
    g = s["g"]
    print(f"hinge_box: box {s['width']:.1f} x {s['depth']:.1f} x "
          f"{s['height']:.1f} mm outside, lid {s['lid']:.2f} thick; "
          f"{KNUCKLES} teardrop knuckles Ø{2 * s['r_knuckle']:.2f} on an "
          f"axis {s['axis_z']:.2f} up, swept r{s['swept']:.2f}; gap "
          f"{g:g} a side ({args.profile or args.material}); cone pins "
          f"r{s['r_pin']:.2f} with {s['cone_air']:g} mm of air "
          f"({s['cone_clearance']:.3f} normal), min air {min_air:g} "
          f"({air_from})")

    mat = material_of(d, proc)
    build_part(d, "box", proc, mat, build_box, s)
    build_part(d, "lid", proc, mat, build_lid, s)
    centre = bed_centre(d, ("box", "lid"), prof.get("bed_mm"))
    plate(d, "hinge box print", proc, [("box", "box", 0.0),
                                       ("lid", "lid", 0.0)], at=centre)
    geo = d.call("check_geometry")
    if geo["receipt"]["standing"] != "verified":
        raise SystemExit(f"hinge_box: the print part is not verified: {geo}")
    # Every item: on the bed, two pieces, no overhang past 45 degrees (the
    # teardrops and cones are 45), no ceiling, the gap a side between every
    # pair of faces, the chords a fifth of it, and no face within the
    # minimum air over the other piece, each piece on the bed.
    # bed_fit too: the plate in the printer's bed, the profile's bed_mm
    # (on the built-in table there is no printer, and it says so).
    pr = check_print(d, "hinge box", need=PRINT_ITEMS)
    bed = prof.get("bed_mm")
    if bed:
        print(f"  hinge box: the plate, "
              f"{' x '.join(f'{v:.1f}' for v in pr['bed_fit']['extent_mm'])}"
              f" mm, in {args.profile}'s "
              f"{' x '.join(f'{v:g}' for v in bed)} mm bed")
    lim = pr["stacking"]["limit_mm"]
    if abs(lim - min_air) > 1e-9:
        raise SystemExit(f"hinge_box: check_print's stacking limit is "
                         f"{lim:g} mm ({pr['stacking']['limit_from']}), but "
                         f"the cones were sized for {min_air:g} ({air_from})")
    if pr["shells"]["pieces"] != 2:
        raise SystemExit(f"hinge_box: {pr['shells']['pieces']} pieces, not "
                         f"the box and the lid")
    stl = out / "hinge_box.stl"
    exp = d.call("export_stl", path=str(stl))
    # The job: one plate, the box and the lid printed in place. Only with a
    # profile: the built-in table is no printer to place them on.
    job = ({"skipped": "no print profile: the built-in table is no "
                       "printer to place the plate on"}
           if not prof.get("bed_mm") else
           job_receipt(export_print_job(d, [("hinge box print", "hinge box")],
                                        out / "hinge_box_job.3mf", mat)))
    d.call("save_project", path=str(out / "hinge_box.dsgn"))

    # The swing.
    _, lid_occ, mate = build_assembly(d)
    clash = g / 5.0
    res = run_swing(d, s, mate, lid_occ, clash, args.samples)
    swing = check_swing(s, res, lid_occ)
    print(f"  motion, lid 0 -> 180 deg ({swing['samples']} samples, mesh "
          f"at {clash:g}): clean to 180, at 180 "
          f"{', '.join(swing['at_180'])}; the lid's inner face at z "
          f"{max(swing['lid_inner_face_z']):.4f} on the rim at "
          f"{s['height']:g}")

    # Negative controls: each must fail stacking.
    controls = {}
    half = s["cone_air"] / 2.0
    build_part(d, "lid, half the cone's air", proc, mat, build_lid, s,
               air=half)
    plate(d, "control: half the cone's air", proc,
          [("box", "box", 0.0), ("lid", "lid, half the cone's air", 0.0)])
    r = check_print(d, "control: half the cone's air", need=(), quiet=True)
    controls["the cone's air at half"] = {
        "air_mm": half, "stacking": stacking_fails(
            r, "the cone's air at half", pair=("box", "lid"))}
    print(f"  negative control, the cone's air at half ({half:g} mm): "
          f"stacking {r['stacking']['status']}")
    plate(d, "control: lid a layer up", proc,
          [("box", "box", 0.0), ("lid", "lid", layer)])
    r = check_print(d, "control: lid a layer up", need=(), quiet=True)
    controls["the lid a layer off the bed"] = {
        "lift_mm": layer, "bed": r["bed"]["status"],
        "stacking": stacking_fails(r, "the lid a layer off the bed",
                                   lifted="lid")}
    print(f"  negative control, the lid {layer:g} mm off the bed: stacking "
          f"{r['stacking']['status']} (bed {r['bed']['status']}: the print "
          f"as a whole is on it)")

    d.call("set_active_part", part="hinge box print")
    receipt = {"size": args.size, "process": proc,
               "layer_mm": layer, "layer_from": layer_from,
               "min_air_mm": min_air, "min_air_from": air_from,
               "sizes": s, "segments": segments(s),
               "print": print_receipt(pr), "motion": swing,
               "bed_mm": bed, "bed_from": args.profile if bed else None,
               "negative_controls": controls, "stl": str(stl),
               "stl_shells": exp.get("shells"),
               "stl_deflection_mm": exp.get("deflection_mm"),
               "job": job, "confirm": gap.get("confirm")}
    (out / "hinge_box.json").write_text(json.dumps(receipt, indent=1,
                                                   default=str))
    d.call("save_project", path=str(out / "hinge_box.dsgn"))
    print(f"hinge_box: {exp.get('shells')} shells -> {stl} "
          f"({time.perf_counter() - t0:.0f} s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
