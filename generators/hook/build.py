#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A printed PLA wall hook, sized for a load, with a predicted breaking load
for each way it can be printed, to break on purpose and compare.

    python3 generators/hook/build.py --port 8801 --fresh --profile "K5 PLA on P1S"
    python3 generators/hook/build.py --port 8801 --load-kg 8 --reach 40 --screw 5

A J: a back plate held to the wall by two countersunk wood screws, an arm
straight out, and a lip rising at 45 degrees. The load hangs in the saddle
between the arm and the lip. Every size derives from the load, the reach,
the screw and the print profile (generators/hook/README.md).

It is printed one of two ways, and they break differently:
- flat (the J on the bed): every bending stress runs along the layers;
- upright (the back plate on the bed): the arm's bending stress pulls the
  layers apart.

One static study at the rated load answers both: `study_results` and
`probe_study` take the layer direction (`layer_normal`) and report the
stress across those layers against the material's interlayer strength
(docs/simulation/README.md#printed-layers). Linear static scales with the
load, so each orientation's breaking load is the allowable over the stress
per newton.

Output (generated, untracked): scratch/hook/ — hook_flat.stl and
hook_upright.stl (each as it sits on the bed), hook.lob, hook.json (the
receipt) and hook_card.md, the test card.
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
from _lib import (arc_through, check, check_print, connect,  # noqa: E402
                  fresh_project, line, print_receipt)

ROOT = Path(HERE).resolve().parent.parent
OUT = Path(os.environ["LOBOLE_SCRATCH"]) / "hook" if \
    os.environ.get("LOBOLE_SCRATCH") else ROOT / "scratch" / "hook"

G = 9.81  # m/s^2, kg to N
# Prusament PLA's datasheet (v1.1, printed specimens, the source the app's
# `pla` row cites): flexural strength 83 +/- 6 MPa against a tensile yield
# of 51 +/- 3, both printed horizontally. A bending bar's "strength" is the
# linear-elastic stress at its peak load, the same calculation as this
# study, and it comes out 1.63 times the tensile number because the outer
# fibres yield before the bar breaks. The card's upper bracket.
FLEX_OVER_TENSILE = 83.0 / 51.0
# The lip's rise from the arm. 45 degrees is the steepest a printer bridges
# without support (Bambu's PLA datasheet allows 55), and the upright print
# stands the arm up, which tips the lip that far over the bed.
LIP_DEG = 45.0
# How far the root stress may move between the two meshes before the run
# calls the answer unconverged.
CONVERGED = 0.10


# --- geometry -------------------------------------------------------------

def solve(load_kg, reach, screw, design_factor, interlayer_mpa, hole_allow):
    """Every size of the hook, from the load, the reach, the screw and the
    weakest allowable (the interlayer strength: the upright print's arm
    breaks across its layers). Coordinates: x out of the wall, z up, the
    width along y."""
    head = 2.0 * screw  # a 90-degree countersunk wood screw's head is 2 d
    width = 2.0 * head
    force = load_kg * G * design_factor
    # The arm's root carries F * reach. Thick enough that the root's
    # bending stress, 6 M / (w t^2), reaches the interlayer strength at the
    # design load: a 0.1 mm step, rounded up.
    t = math.sqrt(6.0 * force * reach / (width * interlayer_mpa))
    t = math.ceil(t * 10.0) / 10.0
    r_root = 0.5 * t  # a fillet the stress spreads round, not a corner
    hole = screw + 0.5 + hole_allow  # clearance for the shank, as printed
    cs = head + hole_allow
    cs_depth = (cs - hole) / 2.0  # a 90-degree countersink
    # The plate stiffer than the arm (a 1.25 thicker section is twice as
    # strong in bending), and deep enough for the countersink and two
    # nozzle widths under it.
    plate = max(1.25 * t, cs_depth + 2.0)
    # The screws a head's diameter clear of the root fillets, the plate a
    # head's diameter past them.
    screw_z = t / 2.0 + r_root + head
    s = math.sin(math.radians(LIP_DEG))
    c = math.cos(math.radians(LIP_DEG))
    # The arm's centreline ends at xa; the lip's centreline runs from there
    # at LIP_DEG. The saddle (the lip's inner face meeting the arm's top)
    # sits `reach` out from the plate.
    lam = (t / 2.0) * (1.0 - c) / s
    xa = plate + reach + (t / 2.0) * s - lam * c
    # The lip rises half the reach above the arm: what hangs stays on.
    lip = (0.5 * reach + t / 2.0) / s
    return {"load_kg": load_kg, "reach": reach, "screw": screw,
            "design_factor": design_factor, "head": head, "width": width,
            "t": t, "r_root": r_root, "r_saddle": 0.5 * t,
            "r_corner": 0.25 * t, "r_bend": 0.5 * t, "hole": hole,
            "cs": cs, "cs_depth": cs_depth, "plate": plate,
            "screw_z": screw_z, "plate_half": screw_z + head, "xa": xa,
            "lip": lip, "force_design_n": force}


def _intersect(p, u, q, v):
    """Where the lines p + a u and q + b v meet."""
    det = u[0] * (-v[1]) - u[1] * (-v[0])
    a = ((q[0] - p[0]) * (-v[1]) - (q[1] - p[1]) * (-v[0])) / det
    return (p[0] + a * u[0], p[1] + a * u[1])


def outline(s):
    """The J as a closed polygon (x, z), counter-clockwise, with the radius
    each corner is rounded to."""
    t, P = s["t"], s["plate"]
    h = s["plate_half"]
    th = math.radians(LIP_DEG)
    u = (math.cos(th), math.sin(th))
    right = (u[1], -u[0])  # the lip's lower, outer side
    left = (-u[1], u[0])
    a0 = (s["xa"], 0.0)
    lo = (a0[0] + right[0] * t / 2, a0[1] + right[1] * t / 2)
    hi = (a0[0] + left[0] * t / 2, a0[1] + left[1] * t / 2)
    bend = _intersect((0.0, -t / 2), (1.0, 0.0), lo, u)
    saddle = _intersect((0.0, t / 2), (1.0, 0.0), hi, u)
    tip_lo = (lo[0] + u[0] * s["lip"], lo[1] + u[1] * s["lip"])
    tip_hi = (hi[0] + u[0] * s["lip"], hi[1] + u[1] * s["lip"])
    rc = s["r_corner"]
    return [((0.0, -h), 0.0), ((P, -h), rc), ((P, -t / 2), s["r_root"]),
            (bend, s["r_bend"]), (tip_lo, rc), (tip_hi, rc),
            (saddle, s["r_saddle"]), ((P, t / 2), s["r_root"]),
            ((P, h), rc), ((0.0, h), 0.0)]


def rounded(poly):
    """Lines and tangent arcs for a polygon whose corners carry radii."""
    n = len(poly)
    cuts = []  # per corner: (point on the incoming edge, mid, outgoing)
    for i, (p, r) in enumerate(poly):
        if r <= 0.0:
            cuts.append(None)
            continue
        a, b = poly[i - 1][0], poly[(i + 1) % n][0]
        ua = _unit((a[0] - p[0], a[1] - p[1]))
        ub = _unit((b[0] - p[0], b[1] - p[1]))
        half = math.acos(max(-1.0, min(1.0, ua[0] * ub[0] + ua[1] * ub[1])))
        d = r / math.tan(half / 2.0)
        bis = _unit((ua[0] + ub[0], ua[1] + ub[1]))
        hc = r / math.sin(half / 2.0)
        centre = (p[0] + bis[0] * hc, p[1] + bis[1] * hc)
        p1 = (p[0] + ua[0] * d, p[1] + ua[1] * d)
        p2 = (p[0] + ub[0] * d, p[1] + ub[1] * d)
        mid = (centre[0] - bis[0] * r, centre[1] - bis[1] * r)
        cuts.append((p1, mid, p2))
    ents = []
    for i in range(n):
        j = (i + 1) % n
        start = cuts[i][2] if cuts[i] else poly[i][0]
        end = cuts[j][0] if cuts[j] else poly[j][0]
        ents.append(line(start, end))
        if cuts[j]:
            ents.append(arc_through(*cuts[j]))
    return ents


def _unit(v):
    n = math.hypot(v[0], v[1])
    return (v[0] / n, v[1] / n)


# --- frames ---------------------------------------------------------------
# A hook frame maps the hook's own axes (x out of the wall, y across, z up
# as installed) to the world. "installed" is the hook on the wall, where the
# study runs; the two print frames lay it on the bed (world z = 0, up = +Z).

FRAMES = {
    "installed": {"x": (1, 0, 0), "y": (0, 1, 0), "z": (0, 0, 1)},
    # The J on the bed, its width up.
    "flat": {"x": (1, 0, 0), "y": (0, 0, 1), "z": (0, -1, 0)},
    # The back plate on the bed, the arm standing up.
    "upright": {"x": (0, 0, 1), "y": (0, 1, 0), "z": (-1, 0, 0)},
}


def to_world(frame, p, s):
    """A point in the hook's own axes, in the world of `frame`. The hook's
    width runs y = -w/2..w/2, so the flat print is lifted half of it onto
    the bed."""
    f = FRAMES[frame]
    lift = s["width"] / 2.0 if frame == "flat" else 0.0
    return [p[0] * f["x"][k] + p[1] * f["y"][k] + p[2] * f["z"][k]
            + (lift if k == 2 else 0.0) for k in range(3)]


def build(d, s, frame, proc, material):
    """One hook part in `frame`: the J extruded across its width, the two
    countersunk screw holes, the material. Returns the body's feature id."""
    f = FRAMES[frame]
    half_w = s["width"] / 2.0
    # The profile plane: u = the hook's x, v = its z, so the normal is
    # x cross z = -y; it sits at y = +w/2 and extrudes the width along -y.
    normal = [-c for c in f["y"]]
    sk = check(d.call("add_feature", feature={
        "type": "Sketch", "name": "profile",
        "plane": {"origin": to_world(frame, (0, half_w, 0), s),
                  "normal": normal, "uAxis": list(f["x"])},
        "entities": rounded(outline(s))}), "profile")
    body = check(d.call("add_feature", feature={
        "type": "Extrude", "name": "hook", "sketch_id": sk["id"],
        "distance": s["width"], "mode": "new_body"}), "hook")["id"]
    # The holes, drilled from the plate's front face back to the wall (a
    # Hole drills along -normal): u = the hook's y, v = its z.
    hs = check(d.call("add_feature", feature={
        "type": "Sketch", "name": "screw centres",
        "plane": {"origin": to_world(frame, (s["plate"], 0, 0), s),
                  "normal": list(f["x"]), "uAxis": list(f["y"])},
        "entities": [{"type": "point", "p": [0.0, z]}
                     for z in (s["screw_z"], -s["screw_z"])]}),
        "screw centres")
    check(d.call("add_feature", feature={
        "type": "Hole", "name": "screw holes", "sketch_id": hs["id"],
        "hole_kind": "countersink", "diameter": s["hole"],
        "depth": s["plate"], "through": True, "cs_diameter": s["cs"],
        "cs_depth": s["cs_depth"]}), "screw holes")
    d.call("set_material", feature_id=body, material=material)
    return body


# --- the study ------------------------------------------------------------

def faces_of(d):
    """The installed hook's faces the study needs, found by shape and place:
    the back face (on the wall), the two root fillets and the saddle."""
    body = d.call("list_elements")["bodies"][0]
    return body["created_by"], body["faces"]


def pick(faces, s):
    back, roots, saddle = [], {}, []
    for f in faces:
        x, y, z = f["centroid"]
        if f["kind"] == "plane" and abs(x) < 1e-6:
            back.append(f["name"])
        elif f["kind"] == "cylinder" and \
                abs(f.get("radius", 0) - s["r_root"]) < 1e-6 and \
                abs(x - s["plate"]) < s["r_root"]:
            roots["top" if z > 0 else "bottom"] = f["name"]
        elif f["kind"] == "cylinder" and \
                abs(f.get("radius", 0) - s["r_saddle"]) < 1e-6 and \
                x > s["plate"] + s["reach"] / 2 and z > 0:
            saddle.append(f["name"])
    if len(back) != 1 or set(roots) != {"top", "bottom"} or len(saddle) != 1:
        raise SystemExit(f"hook: expected one back face, two root fillets "
                         f"and one saddle; found {back}, {roots}, {saddle}")
    return back[0], roots, saddle[0]


def run_study(d, sid):
    run = d.call("run_study", study=sid, **{"async": True})
    while d.call("study_status", study=sid).get("running"):
        time.sleep(0.5)
    st = d.call("study_status", study=sid)
    if st.get("error") or not st.get("has_results"):
        raise SystemExit(f"hook: the study did not solve: {st}")
    st["reused"] = bool(run.get("reused"))
    return st


def read(d, sid, roots, layer_normals):
    """The rated-load stresses: the global peaks, and on the root fillets
    the largest tensile principal stress and, per orientation, the largest
    tension across its layers."""
    out = {"results": d.call("study_results", study=sid), "roots": {},
           "layers": {}}
    for k, face in roots.items():
        out["roots"][k] = d.call("probe_study", study=sid, face=face)
    for name, n in layer_normals.items():
        r = d.call("study_results", study=sid, layer_normal=list(n))
        probes = {k: d.call("probe_study", study=sid, face=face,
                            layer_normal=list(n))
                  for k, face in roots.items()}
        out["layers"][name] = {"results": r, "roots": probes}
    return out


def study(d, s, rated_n, levels):
    """One static study of the installed hook at the rated load, solved at
    each mesh level: the back face held (screwed tight to a stiff wall), the
    load straight down on the saddle's fillet."""
    body, faces = faces_of(d)
    back, roots, saddle = pick(faces, s)
    st = d.call("add_study", type="static", name="hook at the rated load")
    sid = st.get("id", st.get("study"))
    d.call("add_load", study=sid, type="fixed", face=back)
    d.call("add_load", study=sid, type="force", face=saddle,
           magnitude=rated_n, direction=[0.0, 0.0, -1.0])
    # Layer normals in the installed frame: flat prints the width up, so
    # its layers stack along y; upright stands the arm up, along x.
    normals = {"flat": (0.0, 1.0, 0.0), "upright": (1.0, 0.0, 0.0)}
    solves = []
    for k, (size, root) in enumerate(levels):
        d.call("set_study", study=sid, mesh_size=size)
        for face in roots.values():
            if k:
                d.call("remove_refinement", study=sid, face=face)
            d.call("add_refinement", study=sid, face=face, size=root)
        t0 = time.perf_counter()
        st = run_study(d, sid)
        r = read(d, sid, roots, normals)
        r.update({"mesh_size_mm": size, "root_size_mm": root,
                  "seconds": time.perf_counter() - t0,
                  "solver": st.get("solver"), "reused": st["reused"]})
        solves.append(r)
        how = ("reused: these inputs were solved before" if st["reused"]
               else f"{r['seconds']:.1f} s, {st.get('solver')}")
        print(f"  mesh {size:.2f} mm (roots {root:.2f}): "
              f"{r['results']['nodes']} nodes, {how}")
    return sid, roots, solves


def predict(solve, rated_n, yield_mpa):
    """Per orientation: the breaking load in kg and what breaks it. Each is
    the allowable over the root's stress per newton, the lower of in-layer
    tension (the yield) and tension across the layers (the interlayer)."""
    out = {}
    tension = max(solve["roots"].values(),
                  key=lambda p: p["principal_max_mpa"])
    for name, lay in solve["layers"].items():
        across = max(lay["roots"].values(),
                     key=lambda p: p["across_layers_max_mpa"])
        fos_il = lay["results"]["interlayer_factor_of_safety"]
        il_mpa = fos_il * lay["results"]["max_across_layers_mpa"]["value"]
        modes = [
            ("in-layer tension", yield_mpa, tension["principal_max_mpa"],
             tension["principal_max_at_mm"]),
            ("layers pulling apart", il_mpa, across["across_layers_max_mpa"],
             across["across_layers_max_at_mm"])]
        cands = [(allow / max(sig, 1e-9) * rated_n / G, mode, allow, sig, at)
                 for mode, allow, sig, at in modes]
        kg, mode, allow, sig, at = min(cands)
        # The root is where the prediction reads; if the part's worst
        # stress of either kind is somewhere else, it may break there
        # first, and the card must say so rather than quietly pick the root.
        elsewhere = []
        for what, peak, root in (
                ("in-layer tension", solve["results"]["max_principal_mpa"],
                 tension["principal_max_mpa"]),
                ("layers pulling apart",
                 lay["results"]["max_across_layers_mpa"],
                 across["across_layers_max_mpa"])):
            if peak["value"] > 1.05 * root:
                elsewhere.append({"mode": what, "mpa": peak["value"],
                                  "at_mm": peak["at"], "root_mpa": root})
        out[name] = {"kg": kg, "mode": mode, "allowable_mpa": allow,
                     "stress_at_rated_mpa": sig, "at_mm": at,
                     "kg_if_like_bending_bar": (kg * FLEX_OVER_TENSILE
                                                if mode == "in-layer tension"
                                                else None),
                     "other": [{"mode": m, "kg": k} for k, m, *_ in cands
                               if m != mode],
                     "peaks_elsewhere": elsewhere}
    return out


# --- output ---------------------------------------------------------------

def card(s, pred, conv, profile, stl):
    """The test card: what to print, how to load it, what the study says
    and a table for the answer."""
    where = {"in-layer tension": "the arm's root, top fillet: a crack "
             "across the arm starting at the top surface",
             "layers pulling apart": "the arm's root, top fillet: the arm "
             "snaps off along a layer line where it meets the plate"}
    rows, notes = [], []
    for name in ("flat", "upright"):
        for e in pred[name]["peaks_elsewhere"]:
            notes.append(f"- {name}: the worst {e['mode']} is NOT at the root "
                         f"but at {[round(v, 1) for v in e['at_mm']]} mm "
                         f"({e['mpa']:.1f} MPa against the root's "
                         f"{e['root_mpa']:.1f}); it may break there first.")
        p = pred[name]
        upper = (f"{p['kg_if_like_bending_bar']:.1f}"
                 if p["kg_if_like_bending_bar"] else "—")
        rows.append(f"| {name} | **{p['kg']:.1f}** | {upper} | {p['mode']} "
                    f"({p['allowable_mpa']:.0f} MPa) | {where[p['mode']]} |")
    return f"""# Hook test card

Rated for {s['load_kg']:g} kg; reach {s['reach']:g} mm, width {s['width']:g} mm,
arm {s['t']:g} mm thick, for {s['screw']:g} mm countersunk wood screws.
Profile: {profile}.

## Prediction

| print | breaks at (kg) | if it breaks like a bending bar (kg) | by | where |
|---|---|---|---|---|
{chr(10).join(rows)}
{chr(10) + chr(10).join(notes) + chr(10) if notes else ""}
The prediction is the linear-elastic stress at the root fillet, scaled to
the load where it reaches the allowable: in-layer tension against PLA's
tensile yield, tension across the layers against its interlayer adhesion
(Prusament PLA datasheet v1.1: 48-51 and 17 ± 3 MPa). A printed bending bar
of the same PLA breaks at a linear stress 1.6 times its tensile yield (83
against 51 MPa), so a flat hook may well land between the two columns;
where it lands is the finding. There is no bending-bar number across the
layers. The allowables are Prusament's; another PLA's differ, which is part
of what the test measures. Mesh check: the root stress moved {conv * 100:.1f} % from the
coarse mesh to the fine one.

## Print

- `{stl['flat']}`: on the bed as exported (the J flat).
- `{stl['upright']}`: on the bed as exported (the back plate down, the arm up).
- **100 % infill** (or enough walls that the section is solid): the study
  is of a solid hook. 0.2 mm layers, no supports; note the filament.

## Test

1. Screw the hook through both holes to a stiff board (not drywall) with
   {s['screw']:g} mm countersunk wood screws, snug. Clamp the board upright.
2. Hang a peak-hold luggage scale's hook in the saddle (the bottom of the
   V between the arm and the lip) and pull straight DOWN, slowly, a few
   seconds to failure. Or hang a bucket and add water.
3. Record the peak and where it broke.

| print | filament | layers / walls / infill | peak (kg) | where it broke | notes |
|---|---|---|---|---|---|
| flat | | | | | |
| flat | | | | | |
| upright | | | | | |
| upright | | | | | |
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int)
    ap.add_argument("--fresh", action="store_true",
                    help="start a new project first (an untitled scratch one is "
                         "discarded; a saved one with unsaved work refused)")
    ap.add_argument("--load-kg", type=float, default=5.0,
                    help="the rated load, kg")
    ap.add_argument("--reach", type=float, default=30.0,
                    help="the saddle's clear depth off the plate, mm: what "
                         "hangs needs it")
    ap.add_argument("--screw", type=float, default=4.0,
                    help="the wood screw's diameter, mm")
    ap.add_argument("--design-factor", type=float, default=2.0,
                    help="the weakest print breaks at this times the rated "
                         "load (by the beam formula; the study says what it "
                         "really is)")
    ap.add_argument("--process", default="fdm")
    ap.add_argument("--material", default="pla")
    ap.add_argument("--profile", default=None,
                    help="one of your print profiles (list_print_profiles); "
                         "without it the built-in --process/--material table")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()
    out = Path(args.out) if args.out else OUT

    d = connect(args.port)
    if args.fresh:
        fresh_project(d)
    proc = ({"profile": args.profile} if args.profile else
            {"process": args.process, "material": args.material})
    material = proc.get("material")
    if args.profile:
        for p in d.call("list_print_profiles")["profiles"]:
            if p["name"] == args.profile:
                material = p["material"]
        if material is None:
            raise SystemExit(f"hook: no print profile '{args.profile}'")
    mats = d.call("describe")["materials"]
    printed = sorted(k for k, v in mats.items() if v.get("interlayer_mpa"))
    mat = mats.get(material, {})
    if not mat.get("interlayer_mpa"):
        raise SystemExit(f"hook: {material} has no interlayer strength in "
                         f"the material library, so the upright print has no "
                         f"prediction; these do: {', '.join(printed)}")
    hole_allow = d.call("print_allowance", kind="hole", size_mm=args.screw,
                        **proc)["allowance_mm"]
    s = solve(args.load_kg, args.reach, args.screw, args.design_factor,
              mat["interlayer_mpa"], hole_allow)
    print(f"hook: {args.load_kg:g} kg at {args.reach:g} mm, arm "
          f"{s['t']:g} x {s['width']:g} mm, plate {s['plate']:.1f} mm, "
          f"{material} ({args.profile or 'built-in table'})")

    out.mkdir(parents=True, exist_ok=True)
    stl, prints = {}, {}
    for frame in ("flat", "upright"):
        d.call("add_part", name=f"hook {frame}")
        d.call("set_print_process", **proc)
        build(d, s, frame, proc, material)
        geo = d.call("check_geometry")
        if geo["receipt"]["standing"] != "verified":
            raise SystemExit(f"hook: the {frame} print is not verified: {geo}")
        # On the bed as modelled (the slicer places what it is given), one
        # watertight piece, and nothing overhanging past the process's limit
        # -- the lip is 45 degrees by design (check_print).
        pr = check_print(d, f"hook {frame}", need=("bed", "shells"))
        path = out / f"hook_{frame}.stl"
        d.call("export_stl", path=str(path))
        box = d.call("mass_properties")["bodies"][0]
        lo, hi = box["bbox_min"], box["bbox_max"]
        print(f"  {frame}: {hi[0] - lo[0]:.0f} x {hi[1] - lo[1]:.0f} x "
              f"{hi[2] - lo[2]:.0f} mm on the bed")
        prints[frame] = print_receipt(pr)
        stl[frame] = path.name

    d.call("add_part", name="hook")
    d.call("set_print_process", **proc)
    build(d, s, "installed", proc, material)
    geo = d.call("check_geometry")
    if geo["receipt"]["standing"] != "verified":
        raise SystemExit(f"hook: the hook is not verified: {geo}")
    rated = args.load_kg * G
    # Two meshes: a third of the arm's thickness with the root fillets at a
    # quarter of their radius, then both halved.
    coarse = (s["t"] / 3.0, s["r_root"] / 4.0)
    levels = [coarse, (coarse[0] / 2.0, coarse[1] / 2.0)]
    sid, roots, solves = study(d, s, rated, levels)
    preds = [predict(sv, rated, mat["yield_mpa"]) for sv in solves]
    conv = max(abs(preds[1][k]["kg"] - preds[0][k]["kg"]) / preds[1][k]["kg"]
               for k in preds[1])
    pred = preds[1]
    for name, p in pred.items():
        print(f"  {name}: breaks at {p['kg']:.1f} kg, {p['mode']} at the "
              f"root ({p['stress_at_rated_mpa']:.1f} MPa at {args.load_kg:g}"
              f" kg against {p['allowable_mpa']:.0f})")
    print(f"  mesh: the prediction moved {conv * 100:.1f} % coarse to fine")
    # The study against the beam formula: the bending stress where the root
    # fillet meets the arm, 6 M / (w t^2), with the load in the saddle a
    # reach out. The fillet raises it by its concentration factor; a ratio
    # far from the 1.2-1.5 of a filleted shoulder means a wrong load or
    # support, not a sharper fillet.
    beam = 6.0 * rated * (s["reach"] - s["r_root"]) / (s["width"] * s["t"] ** 2)
    kt = pred["flat"]["stress_at_rated_mpa"] / beam
    print(f"  beam formula at the fillet's end: {beam:.1f} MPa; the study's "
          f"root {pred['flat']['stress_at_rated_mpa']:.1f}, {kt:.2f} times it")
    if not 1.0 <= kt <= 2.0:
        raise SystemExit(f"hook: the root stress is {kt:.2f} times the beam "
                         f"formula's; a filleted root is 1.2-1.5")
    for name, p in pred.items():
        for e in p["peaks_elsewhere"]:
            print(f"  {name}: WARNING the worst {e['mode']} is not at the "
                  f"root: {e['mpa']:.1f} MPa at {e['at_mm']} against the "
                  f"root's {e['root_mpa']:.1f}")
    peak = solves[1]["results"]["max_principal_mpa"]
    if conv > CONVERGED:
        raise SystemExit(f"hook: the root stress moved {conv * 100:.0f} % "
                         f"between meshes; not converged")
    d.call("save_project", path=str(out / "hook.lob"))
    receipt = {"sizes": s, "process": proc, "material": mat,
               "rated_n": rated, "prediction": pred,
               "beam_formula_mpa": beam, "concentration": kt,
               "coarse_prediction": preds[0], "convergence": conv,
               "global_principal_peak": peak,
               "solves": [{k: sv[k] for k in ("mesh_size_mm", "root_size_mm",
                                              "seconds", "solver")}
                          | {"nodes": sv["results"]["nodes"]}
                          for sv in solves],
               "stl": stl, "print": prints}
    (out / "hook.json").write_text(json.dumps(receipt, indent=1))
    (out / "hook_card.md").write_text(
        card(s, pred, conv, args.profile or f"{args.process} {material}",
             stl))
    print(f"hook: done -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
