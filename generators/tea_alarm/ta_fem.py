# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The tea alarm's studies, each held to its closed form
(README.md#what-is-checked):

- **The strip check**: the bought strip publishes no layers, so the app's
  bimetal physics is checked on a pair whose layers are all cited -- a
  library steel layer under a C26000 brass one, 0.5 x 5 x 40 each, bonded
  in an assembly, clamped across one end and heated from 20 to 100 C (a
  uniform thermal load on an assembly study,
  docs/simulation/README.md#a-thermal-load-on-an-assembly). The free end's
  turn, read by probe_study as a rigid body, against Timoshenko's; in-house
  and on CalculiX; and the control, both layers steel, which must not curl.
- **The heat path**: the frame rod's wetted length held at 100 C, the rest
  convecting, its end in the block adiabatic -- the pin fin with an
  adiabatic tip -- at both ends of Incropera's free-convection range; then
  the printed block's own study, its mount held at the rod's tip.
- **The clip**: its spring leaf deflected by its grip (beam theory), and
  resting frictionless on the vessel under that grip (contact).
- **The chime**: its first mode against Euler-Bernoulli.
"""
import math
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import ta_parts as P  # noqa: E402
import ta_spec as S  # noqa: E402

TIMEOUT_S = 900.0


def fail(msg):
    raise SystemExit(f"tea alarm FEM: {msg}")


def run(d, sid, timeout_s=None):
    """Run a study to its end, bounded; the study_results it answers."""
    timeout_s = timeout_s or TIMEOUT_S
    d.call("run_study", id=sid, **{"async": True})
    t0 = time.monotonic()
    while d.call("study_status", id=sid).get("running"):
        if time.monotonic() - t0 > timeout_s:
            d.call("cancel_study", id=sid)
            fail(f"study {sid} ran past {timeout_s:.0f} s")
        time.sleep(0.5)
    st = d.call("study_status", id=sid)
    if st.get("error") or not st.get("has_results"):
        fail(f"study {sid} did not solve: {st.get('error') or st}")
    return d.request("study_results", {"id": sid}, timeout=120.0)


def face_where(d, part, kind, pred):
    """The one face of `part` of that kind whose description `pred`
    accepts."""
    d.call("set_active_part", part=part)
    hits = [f for b in d.call("list_elements")["bodies"] for f in b["faces"]
            if f["kind"] == kind and pred(f)]
    if len(hits) != 1:
        fail(f"{part}: {len(hits)} {kind} faces match, not one")
    return hits[0]["name"]


def faces_where(d, part, pred):
    d.call("set_active_part", part=part)
    return [f for b in d.call("list_elements")["bodies"] for f in b["faces"]
            if pred(f)]


def near(a, b, tol=1e-6):
    return abs(a - b) < tol


def rel(a, b):
    return a / b - 1.0


# ---- the strip check ------------------------------------------------------------

STRIP_L, STRIP_B, STRIP_T = 40.0, 5.0, 0.5
STRIP_DT = (20.0, 100.0)


def strip_check(d, describe, out):
    """The steel/brass pair, in its own project (strip_check.lob)."""
    mats = describe["materials"]
    steel, brass = mats["steel"], mats["brass"]
    for k in ("young_gpa", "expansion_per_k"):
        if not (steel.get(k) and brass.get(k)):
            fail(f"the library states no {k} for steel or brass")
    d.call("new_project")
    for name, mat, z in (("steel layer", "steel", 0.0),
                         ("brass layer", "brass", STRIP_T)):
        d.call("add_part", name=name)
        b = P.Builder(d)
        b.box("Layer", (0.0, -STRIP_B / 2, z), (STRIP_L, STRIP_B / 2,
                                                  z + STRIP_T))
        body = d.call("list_elements")["bodies"][0]["created_by"]
        d.call("set_material", feature_id=body, material=mat)
    d.call("add_assembly", name="strip check")
    occ = {n: d.call("add_occurrence", part=n, name=n, fixed=True)["id"]
           for n in ("steel layer", "brass layer")}
    sid = d.call("add_study", name="heated from flat", type="static")["id"]
    d.call("set_study", id=sid, mesh_size=STRIP_T, converge="never",
           participants=[{"occurrence": occ["steel layer"], "role": "meshed"},
                         {"occurrence": occ["brass layer"],
                          "role": "bonded"}],
           thermal_load={"uniform_c": STRIP_DT[1],
                         "stress_free_c": STRIP_DT[0]})
    ends = {}
    for n in occ:
        root = face_where(d, n, "plane", lambda f: near(f["centroid"][0], 0))
        ends[n] = face_where(d, n, "plane",
                             lambda f: near(f["centroid"][0], STRIP_L))
        d.call("set_active_part", part="strip check")
        d.call("add_load", study=sid, type="fixed", face=root,
               occurrence=occ[n])
    d.call("set_active_part", part="strip check")

    def tip():
        r = [d.call("probe_study", id=sid, face=ends[n], occurrence=occ[n])
             for n in occ]
        rot = sum(x["rotation_rad"][1] for x in r) / len(r)
        drop = -sum(x["translation_mm"][2] for x in r) / len(r)
        return rot, drop

    kappa = S.timoshenko_curvature(
        STRIP_T, STRIP_T, steel["young_gpa"], brass["young_gpa"],
        steel["expansion_per_k"], brass["expansion_per_k"],
        STRIP_DT[1] - STRIP_DT[0])
    theory = (kappa * STRIP_L, kappa * STRIP_L ** 2 / 2.0)
    rec = {"curvature_per_mm": kappa, "rotation_rad": theory[0],
           "drop_mm": theory[1]}
    t0 = time.monotonic()
    res = run(d, sid)
    rec["mesh_mm"] = STRIP_T
    rec["in_house"] = dict(zip(("rotation_rad", "drop_mm"), tip()),
                           seconds=round(time.monotonic() - t0, 1))
    d.call("set_study", id=sid, solver="calculix")
    res = run(d, sid)
    rec["calculix"] = dict(zip(("rotation_rad", "drop_mm"), tip()),
                           solver=res.get("solver"))
    own, ccx = rec["in_house"], rec["calculix"]
    rec["agreement"] = abs(rel(ccx["rotation_rad"], own["rotation_rad"]))
    for k in ("in_house", "calculix"):
        rec[k]["rotation_off"] = rel(rec[k]["rotation_rad"], theory[0])
        rec[k]["drop_off"] = rel(rec[k]["drop_mm"], theory[1])
    # The control: both layers steel.
    d.call("set_active_part", part="brass layer")
    body = d.call("list_elements")["bodies"][0]["created_by"]
    d.call("set_material", feature_id=body, material="steel")
    d.call("set_active_part", part="strip check")
    d.call("set_study", id=sid, solver="in_house")
    run(d, sid)
    rec["control_rotation_rad"] = tip()[0]
    d.call("set_active_part", part="brass layer")
    d.call("set_material", feature_id=body, material="brass")
    d.call("save_project", path=str(out / "strip_check.lob"))
    ok_rot = all(abs(rec[k]["rotation_off"]) < 0.02
                 for k in ("in_house", "calculix"))
    # The clamp holds both layers across the width and thickness and turns
    # the root a little more than the free span's curvature: measured on
    # the tip's drop (README.md#the-strip-check).
    ok_drop = all(abs(rec[k]["drop_off"]) < 0.04
                  for k in ("in_house", "calculix"))
    ok_ccx = rec["agreement"] < 1e-5
    ok_control = abs(rec["control_rotation_rad"]) < 1e-3 * theory[0]
    print(f"strip check: steel/brass 0.5 + 0.5 x 5 x 40, 20 -> 100 C, "
          f"meshed at {rec['mesh_mm']:g} mm: tip turns {own['rotation_rad']:.6f} rad "
          f"against Timoshenko's {theory[0]:.6f} "
          f"({100 * own['rotation_off']:+.2f} %), drops "
          f"{own['drop_mm']:.5f} mm against {theory[1]:.5f} "
          f"({100 * own['drop_off']:+.2f} %); CalculiX on the same mesh "
          f"within {rec['agreement']:.1e}; control (both steel) turns "
          f"{rec['control_rotation_rad']:.2e} rad")
    if not (ok_rot and ok_drop and ok_ccx and ok_control):
        fail(f"the strip check missed: {rec}")
    print("strip check: within 2 % of Timoshenko's turn in-house and on "
          "CalculiX, and the control does not curl")
    return rec


# ---- the heat path ---------------------------------------------------------------

def rod_faces(d):
    """The frame rod's faces: its riser, its bend, its run to the block,
    its foot in the tea and its end in the block."""
    fs = faces_where(d, "frame rod", lambda f: True)
    by = {}
    for f in fs:
        if f["kind"] == "cylinder" and abs(f["axis"][2]) > 0.99:
            by["riser"] = f["name"]
        elif f["kind"] == "cylinder" and abs(f["axis"][0]) > 0.99:
            by["run"] = f["name"]
        elif f["kind"] == "torus":
            by["bend"] = f["name"]
        elif f["kind"] == "plane" and f["normal"][2] < -0.99:
            by["foot"] = f["name"]
        elif f["kind"] == "plane" and f["normal"][0] > 0.99:
            by["end"] = f["name"]
    if len(by) != 5:
        fail(f"the frame rod's faces are not riser, bend, run, foot and "
             f"end: {sorted(by)}")
    return by


def fin(d, s, room, film, solver="in_house", mesh_mm=None):
    """One thermal study of the rod: the tea up to the rim (the hottest the
    rod can be wetted) held at the pour, the dry rod convecting to the room
    up to the block's face, its end in the block's bore left adiabatic --
    the bound for an insulating mount. Against the pin fin with an
    adiabatic tip over the dry path's length (Incropera §3.6)."""
    f = rod_faces(d)
    d.call("set_active_part", part="frame rod")
    sid = d.call("add_study", name=f"heat path, {film:g} W/m2K",
                 type="thermal")["id"]
    d.call("set_study", id=sid, mesh_size=mesh_mm or s["d"] / 3.0,
           converge="never", solver=solver)
    below = {"normal": [0.0, 0.0, 1.0], "from_mm": s["z_strip"] - s["d"],
             "to_mm": 0.0}
    above = {"normal": [0.0, 0.0, 1.0], "from_mm": 0.0,
             "to_mm": s["z_run"] + s["d"]}
    clear = {"normal": [1.0, 0.0, 0.0], "from_mm": s["x_rod"] - s["d"],
             "to_mm": s["x_block0"]}
    hot = {"type": "temperature", "temperature_c": S.T_POUR}
    air = {"type": "convection", "film_w_m2k": film, "ambient_c": room}
    d.call("add_load", study=sid, face=f["foot"], **hot)
    d.call("add_load", study=sid, face=f["riser"], region=below, **hot)
    d.call("add_load", study=sid, face=f["riser"], region=above, **air)
    d.call("add_load", study=sid, face=f["bend"], **air)
    d.call("add_load", study=sid, face=f["run"], region=clear, **air)
    r = run(d, sid)
    body = r["bodies"][0]
    tip = (body.get("min_temperature_c") or r["min_temperature_c"])["value"]
    theory = S.fin_tip(S.T_POUR, room, film, S.SS_K, s["d"], s["dry_mm"])
    off = (tip - room) / (theory - room) - 1.0
    return {"study": sid, "film_w_m2k": film, "tip_c": tip,
            "fin_c": theory, "excess_off": off, "solver": r.get("solver")}


def block_heat(d, s, room, film, t_mount, materials):
    """The printed block's own study: its mount (the rod's bore) held at
    the rod's tip temperature, every other face convecting to the room in
    the still-air film; for each candidate material, the margin to its
    service temperature, held by a requirement (temperature_margin at least
    MARGIN_K). Conduction only: steam off the tea warms the block too, and
    the app has no air (README.md#the-heat-path)."""
    d.call("set_active_part", part="block")
    body = d.call("list_elements")["bodies"][0]
    feature = body["created_by"]
    mount, outside = [], []
    for f in body["faces"]:
        c = f["centroid"]
        in_bore = (f["kind"] == "cylinder" and abs(f["axis"][0]) > 0.99 and
                   abs(f.get("radius", 0) - s["d"] / 2.0) < 1e-6 and
                   c[0] < s["x_block0"] + s["mount_depth"] + 1e-6 and
                   abs(c[2] - s["z_run"]) < 1e-6)
        bore_end = (f["kind"] == "plane" and f["normal"][0] < -0.99 and
                    abs(c[0] - (s["x_block0"] + s["mount_depth"])) < 1e-6)
        (mount if in_bore or bore_end else outside).append(f["name"])
    if not mount:
        fail("the block has no mount bore")
    sid = d.call("add_study", name="block over the rod", type="thermal")["id"]
    d.call("set_study", id=sid, mesh_size=s["d"] / 2.0, converge="never")
    for n in mount:
        d.call("add_load", study=sid, type="temperature", face=n,
               temperature_c=t_mount)
    for n in outside:
        d.call("add_load", study=sid, type="convection", face=n,
               film_w_m2k=film, ambient_c=room)
    doc = next(p["id"] for p in d.call("list_parts")["parts"]
               if p["name"] == "block")
    req = d.call("add_requirement", name="the block keeps its stiffness",
                 measure={"kind": "study_result", "document": doc,
                          "study": sid, "quantity": "temperature_margin"},
                 bound={"kind": "at_least", "value": S.MARGIN_K},
                 source=f"{S.MARGIN_K:g} K under the printed material's "
                        "service temperature (its datasheet's HDT at "
                        "0.45 MPa)")["id"]
    out = {}
    for mat in materials:
        d.call("set_active_part", part="block")
        d.call("set_material", feature_id=feature, material=mat)
        r = run(d, sid)
        chk = d.call("check_requirements", ids=[req])["requirements"][0]
        out[mat] = {"peak_c": r["max_temperature_c"]["value"],
                    "margin_k": r["temperature_margin_k"]["value"],
                    "basis": r["temperature_margin_k"]["basis"],
                    "requirement": chk["state"]}
    return sid, req, out


# ---- the clip --------------------------------------------------------------------

def clip_faces(d, s):
    """The clip's faces that matter: the inner leaf's rod side, the outer
    leaf's tip (its end), and each leaf's wall side."""
    t = s["clip_t"]
    fs = faces_where(d, "clip", lambda f: f["kind"] == "plane")
    out = {}
    for f in fs:
        c, n = f["centroid"], f["normal"]
        if n[0] > 0.99 and near(c[0], s["x_leaf_in"] + t, 1e-4):
            out["rod side"] = f["name"]
        elif n[0] < -0.99 and near(c[0], s["x_leaf_in"], 1e-4):
            out["inner wall side"] = f["name"]
        elif n[0] > 0.99 and near(c[0], s["x_leaf_out"], 1e-4):
            out["outer wall side"] = f["name"]
        elif n[2] < -0.99 and c[0] < s["x_leaf_out"]:
            out["outer tip"] = f["name"]
        elif n[0] < -0.99 and near(c[0], s["x_leaf_out"] - t, 1e-4):
            out["outer back"] = f["name"]
    if len(out) != 5:
        fail(f"the clip's faces: found {sorted(out)}")
    return out


def clip_linear(d, s, mesh_mm=None):
    """The clip held by the rod along its inner leaf, its grip at the outer
    leaf's tip: the tip must move by the wall's thickness (the U frame's
    beam theory), within 2 %, and stay under 1.4301's yield."""
    f = clip_faces(d, s)
    d.call("set_active_part", part="clip")
    sid = d.call("add_study", name="grip", type="static")["id"]
    d.call("set_study", id=sid, mesh_size=mesh_mm or s["clip_t"],
           converge="never")
    d.call("add_load", study=sid, type="fixed", face=f["rod side"])
    # The bends meshed at half the sheet. At the sheet's own size the CPU's
    # iterative solve was once refused on it as unconstrained: a slow
    # solve read as a stalled one, after the GPU fell short
    # (docs/simulation/README.md#the-solver).
    for b in faces_where(d, "clip", lambda x: x["kind"] == "cylinder"):
        d.call("set_active_part", part="clip")
        d.call("add_refinement", study=sid, face=b["name"],
               size=s["clip_t"] / 2.0)
    tip = next(x for x in faces_where(d, "clip", lambda x: True)
               if x["name"] == f["outer tip"])["centroid"]
    d.call("set_active_part", part="clip")
    d.call("add_load", study=sid, type="remote", face=f["outer tip"],
           force_n=s["grip_n"], direction=[-1.0, 0.0, 0.0], point_mm=tip)
    r = run(d, sid)
    p = d.call("probe_study", id=sid, face=f["outer tip"])
    moved = -p["translation_mm"][0]
    return {"study": sid, "grip_n": s["grip_n"], "tip_moved_mm": moved,
            "delta_mm": s["delta"], "off": rel(moved, s["delta"]),
            "peak_mpa": r["max_von_mises_mpa"]["value"]
            if isinstance(r["max_von_mises_mpa"], dict)
            else r["max_von_mises_mpa"],
            "beam_stress_mpa": s["leaf_stress"],
            "factor_of_safety": (r.get("factor_of_safety") or {}).get("value")
            if isinstance(r.get("factor_of_safety"), dict)
            else r.get("factor_of_safety")}


def clip_contact(d, s, o_clip, o_vessel, mesh_mm=None):
    """An assembly study: the clip meshed, its inner leaf held where the rod
    clamps it, its outer leaf's jaw (a band at its tip) resting
    frictionless on the vessel (a rigid occurrence, its wall curved) and
    pressed onto it by the grip. The vessel must carry the grip, on its
    line, and close along the jaw.

    Held by the contact alone (the inner leaf pulled in toward the axis),
    the study is refused: the weak springs that hold a part until its
    contact closes are 1e-6 E L, and a 0.5 mm spring clip is softer than
    that -- they carried 62 % of the grip (docs/known-bugs.md). And the
    grip itself cannot be the wall's interference: the app has no
    interference preload or prescribed displacement, so it is the beam
    theory's force (README.md#what-the-round-exposed)."""
    f = clip_faces(d, s)
    d.call("set_active_part", part="clip on the rim")
    sid = d.call("add_study", name="clip on the rim", type="static")["id"]
    d.call("set_study", id=sid, mesh_size=mesh_mm or s["clip_t"],
           converge="never",
           participants=[{"occurrence": o_clip, "role": "meshed"},
                         {"occurrence": o_vessel, "role": "rigid"}])
    lid = d.call("add_load", study=sid, type="contact",
                 contact_with=o_vessel)["id"]
    # A contact is nonlinear by itself; two increments carry a grip this
    # small.
    d.call("set_study", id=sid, nonlinear={"increments": 2})
    d.call("add_load", study=sid, type="fixed", face=f["rod side"],
           occurrence=o_clip)
    # The jaw: the outer leaf's tip, pressed onto the wall by the grip (an
    # assembly study loads whole faces: the tip's end is the jaw's face).
    tip = next(x for x in faces_where(d, "clip", lambda x: True)
               if x["name"] == f["outer tip"])["centroid"]
    d.call("set_active_part", part="clip on the rim")
    d.call("add_load", study=sid, type="force", face=f["outer tip"],
           occurrence=o_clip, magnitude=s["grip_n"],
           direction=[1.0, 0.0, 0.0])
    r = run(d, sid)
    body = r["bodies"][0]
    react = next(x for x in body["reactions"] if x.get("support") == lid)
    Fv, Mv = react["force_n"], react["moment_n_mm"]
    g = s["grip_n"]
    # Along the grip, and the height its line of action crosses the wall at
    # (from the moment about Y: My = z Fx - x Fz, x the wall's face).
    along_off = abs(-Fv[0] / g - 1.0)
    tangential = math.hypot(Fv[1], Fv[2]) / g
    z_line = (Mv[1] + s["x_leaf_out"] * Fv[2]) / Fv[0]
    on_jaw = tip[2] - 1e-6 <= z_line <= tip[2] + s["jaw"]
    nl = body.get("nonlinear") or {}
    contact = (body.get("contacts") or [{}])[0]
    return {"study": sid, "force_n": Fv, "along_off": along_off,
            "tangential_share": tangential, "line_z_mm": z_line,
            "tip_z_mm": tip[2], "on_jaw": on_jaw,
            "closed_area_mm2": contact.get("closed_area_mm2"),
            "max_pressure_mpa": contact.get("max_pressure_mpa"),
            "imbalance": (nl.get("equilibrium") or {}).get("imbalance"),
            "reactions": body["reactions"], "solver": r.get("solver")}


# ---- the chime -----------------------------------------------------------------

def chime_modal(d, s, mesh_mm=None):
    """The chime's first mode, clamped where it is pressed into the block,
    against Euler-Bernoulli's round cantilever (within 2 %), and whether it
    lies in the band chosen to be heard (a report, not a pass)."""
    fs = faces_where(d, "chime", lambda f: f["kind"] == "cylinder")
    if len(fs) != 1:
        fail(f"the chime has {len(fs)} round faces, not one")
    d.call("set_active_part", part="chime")
    sid = d.call("add_study", name="ring", type="modal")["id"]
    d.call("set_study", id=sid, mesh_size=mesh_mm or s["d"] / 3.0,
           num_modes=2)
    # Held over the length pressed into the block's bore.
    d.call("add_load", study=sid, type="fixed", face=fs[0]["name"],
           region={"normal": [1.0, 0.0, 0.0],
                   "from_mm": s["x_block1"] - s["chime_in"] - 1.0,
                   "to_mm": s["x_block1"]})
    r = run(d, sid)
    f1 = r["bodies"][0]["frequencies_hz"][0]
    eb = S.euler_bernoulli_f1(S.SS_E, S.SS_RHO, s["d"], s["chime_len"])
    lo, hi = S.HEARD_HZ
    return {"study": sid, "f1_hz": f1, "euler_bernoulli_hz": eb,
            "off": rel(f1, eb), "heard": lo <= f1 <= hi}
