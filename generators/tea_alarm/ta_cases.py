# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The side cases: the study types the alarm does not use, each a project
of its own, each against its closed form and with a negative control
(README.md#the-side-cases).

- **Snap-through**: a shallow PETG arch latch between two pins at eps = 4,
  a uniform pressure on its crown, solved large-deformation with
  snap_through: its limit point against Timoshenko and Gere §7.6 (5 %, as
  the [nonlinear] case holds it), and its first-yield factor against the
  peak -- does a printed latch snap before it yields? The control: the
  same arch at eps < 2, which has no limit point, ends at its deflection
  limit.
- **Plasticity**: a library-steel L bracket, its foot clamped, a load at an
  arm a off the upright, so the upright between its two thickened ends
  carries a uniform moment F a and F along it. Loaded at 1.25 times the
  load whose moment is M_p, plastic, at small deformation and again with
  large deformation: first yield of the span against the section's
  closed form; the span's outer-fibre plastic strain against the bilinear
  section's moment-curvature under the library's curve (EN 1993-1-5 C.6(2),
  H = E/99); the top's residual turn against that curvature unloaded and
  integrated over the span. The control: under its yield, nothing left.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import ta_fem as F  # noqa: E402
import ta_parts as P  # noqa: E402
import ta_spec as S  # noqa: E402
from _lib import line  # noqa: E402


def fail(msg):
    raise SystemExit(f"tea alarm side case: {msg}")


# ---- the snap-through latch ---------------------------------------------------------

def arch_part(d, L, h, b, eps, name):
    """A circular arch between pins at x = 0 and L, its section h deep and
    b wide, its first harmonic eps radii of gyration high; it runs past
    each pin by 3h/4, and each pin's bore is 0.4 h across (the [nonlinear]
    case's proportions)."""
    e, R, _ = S.arch(L, h, eps)
    zc = e - R
    over = 0.75 * h
    d.call("add_part", name=name)
    b_ = P.Builder(d)

    def ang(r, x):
        return math.atan2(math.sqrt(r * r - (x - L / 2) ** 2), x - L / 2)

    ro, ri = R + h / 2.0, R - h / 2.0
    pts = {}
    for k, r in (("o", ro), ("i", ri)):
        a0, a1 = ang(r, L + over), ang(r, -over)
        pts[k] = (a0, a1)
    def at(r, a):
        return (L / 2 + r * math.cos(a), zc + r * math.sin(a))
    prof = [{"type": "arc", "center": [L / 2, zc], "radius": ro,
             "start_angle": pts["o"][0], "end_angle": pts["o"][1]},
            line(at(ro, pts["o"][1]), at(ri, pts["i"][1])),
            {"type": "arc", "center": [L / 2, zc], "radius": ri,
             "start_angle": pts["i"][0], "end_angle": pts["i"][1]},
            line(at(ri, pts["i"][0]), at(ro, pts["o"][0]))]
    body = b_.extrude("Arch", P.plane_xz(b / 2.0), prof, b)
    for i, x in enumerate((0.0, L)):
        hole = b_.cyl(f"Bore {i + 1}", (x, b, 0.0), [0.0, -1.0, 0.0],
                      0.2 * h, 2 * b)
        body = b_.boolean(f"Bored {i + 1}", body, hole, "subtract")
    return {"rise": e, "radius": R, "over": over}


def arch_study(d, part, mat, L, h, b, eps, p0, mesh, timeout_s,
               limit_mm=None):
    d.call("set_active_part", part=part)
    bodyid = d.call("list_elements")["bodies"][0]["created_by"]
    d.call("set_material", feature_id=bodyid, material=mat)
    _, R, _ = S.arch(L, h, eps)
    fs = F.faces_where(d, part, lambda f: f["kind"] == "cylinder")
    pins = [f["name"] for f in fs if abs(f["radius"] - 0.2 * h) < 1e-6]
    crown = [f["name"] for f in fs if abs(f["radius"] - (R + h / 2)) < 1e-6]
    if len(pins) != 2 or not crown:
        fail(f"{part}: {len(pins)} pin bores, {len(crown)} crown faces")
    d.call("set_active_part", part=part)
    sid = d.call("add_study", name="snap", type="static")["id"]
    nl = {"large_deformation": True, "snap_through": True,
          "imperfection_mm": 0.0}
    if limit_mm:
        nl["deflection_limit_mm"] = limit_mm
    d.call("set_study", id=sid, mesh_size=mesh, nonlinear=nl)
    for n in pins:
        d.call("add_load", study=sid, type="pin", face=n)
    for n in crown:
        d.call("add_load", study=sid, type="pressure", face=n, magnitude=p0)
    r = F.run(d, sid, timeout_s)
    nl = r["bodies"][0]["nonlinear"]
    return sid, nl, r


def snap_case(d, out, timeout_s):
    """The latch: PETG, 60 between its pins, 1.2 deep and wide (span over
    depth 50, the [nonlinear] arch's), eps 4."""
    mats = d.call("describe")["materials"]
    E = mats["petg"]["young_gpa"] * 1000.0
    L, h, b, eps = 60.0, 1.2, 1.2, 4.0
    q = S.arch_limit_q(E, b, h, L, eps)          # N/mm of span
    p0 = q / b                                   # MPa: the factor is 1
    d.call("new_project")
    geo = arch_part(d, L, h, b, eps, "latch")
    sid, nl, r = arch_study(d, "latch", "petg", L, h, b, eps, p0,
                            0.75 * h, timeout_s)
    peak = nl.get("peak_factor")
    rec = {"span_mm": L, "depth_mm": h, "eps": eps, "rise_mm": geo["rise"],
           "pressure_mpa": p0, "closed_form_factor": 1.0,
           "peak_factor": peak, "peak_at": nl.get("peak_at"),
           "first_yield_factor": nl.get("first_yield_factor"),
           "linear_factor": nl.get("linear_factor")}
    vm = r.get("max_von_mises_mpa")
    rec["peak_von_mises_mpa"] = vm.get("value") if isinstance(vm, dict) \
        else vm
    rec["yield_mpa"] = mats["petg"].get("yield_mpa")
    rec["off"] = peak - 1.0 if peak else None
    rec["yields_first"] = (rec["first_yield_factor"] is not None and
                           peak is not None and
                           rec["first_yield_factor"] < peak)
    d.call("save_project", path=str(out / "latch.lob"))
    # The control: eps 1.5, below 2, has no limit point: loaded on, it
    # flattens, and ends where its crown has come down by its own rise.
    d.call("new_project")
    flat = arch_part(d, L, h, b, 1.5, "flat latch")
    _, nl2, _ = arch_study(d, "flat latch", "petg", L, h, b, 1.5, p0,
                           0.75 * h, timeout_s, limit_mm=flat["rise"])
    rec["control"] = {"eps": 1.5, "peak_at": nl2.get("peak_at"),
                      "factor": nl2.get("peak_factor")}
    d.call("save_project", path=str(out / "latch_control.lob"))
    ok = (rec["peak_at"] in ("limit_point", "snap") and
          abs(rec["off"]) < 0.05 and
          rec["control"]["peak_at"] == "deflection_limit")
    return rec, ok


# ---- the plastic bracket -------------------------------------------------------------

def bracket_part(d, s):
    """The L bracket: a foot plate, the upright's base block, the span (b
    wide, h deep, `span` long), and a head; base and head twice the span's
    depth, so the span is the weakest place and its moment uniform."""
    b, h, span = s["b"], s["h"], s["span"]
    d.call("add_part", name="bracket")
    B = P.Builder(d)
    foot = B.box("Foot", (-3 * h, -b / 2, 0.0), (3 * h, b / 2, h))
    base = B.box("Base", (-h, -b / 2, h), (h, b / 2, 3 * h))
    body = B.boolean("Base on", foot, base, "union")
    mid = B.box("Span", (-h / 2, -b / 2, 3 * h), (h / 2, b / 2,
                                                  3 * h + span))
    body = B.boolean("Span on", body, mid, "union")
    head = B.box("Head", (-h, -b / 2, 3 * h + span),
                 (h, b / 2, 5 * h + span))
    return B.boolean("Head on", body, head, "union")


def bracket_faces(d, s):
    fs = F.faces_where(d, "bracket", lambda f: f["kind"] == "plane")
    top = 5 * s["h"] + s["span"]
    out = {}
    for f in fs:
        c, n = f["centroid"], f["normal"]
        if n[2] < -0.99 and F.near(c[2], 0.0):
            out["foot"] = f["name"]
        elif n[2] > 0.99 and F.near(c[2], top):
            out["top"] = f["name"]
        elif n[0] > 0.99 and F.near(c[0], s["h"] / 2):
            out["span +x"] = f["name"]
        elif n[0] < -0.99 and F.near(c[0], -s["h"] / 2):
            out["span -x"] = f["name"]
    if len(out) != 4:
        fail(f"the bracket's faces: {sorted(out)}")
    return out


def bracket_run(d, s, faces, load_n, nonlinear, timeout_s):
    d.call("set_active_part", part="bracket")
    sid = d.call("add_study", name="bracket", type="static")["id"]
    d.call("set_study", id=sid, mesh_size=s["mesh"], converge="never",
           nonlinear=nonlinear)
    d.call("add_load", study=sid, type="fixed", face=faces["foot"])
    top = 5 * s["h"] + s["span"]
    d.call("add_load", study=sid, type="remote", face=faces["top"],
           force_n=load_n, direction=[0.0, 0.0, -1.0],
           point_mm=[s["a"], 0.0, top])
    r = F.run(d, sid, timeout_s)
    return sid, r


def plastic_case(d, out, timeout_s):
    mats = d.call("describe")["materials"]
    st = mats["steel"]
    E, fy = st["young_gpa"] * 1000.0, st["yield_mpa"]
    H = E / 99.0                        # EN 1993-1-5 C.6(2): Et = E/100
    Et = E * H / (E + H)
    # The span twelve depths long: where it meets its thickened ends the
    # section is held and yields less, over about a depth at each end.
    s = {"b": 10.0, "h": 10.0, "span": 120.0}
    s["a"] = 5.0 * s["h"]               # the load's arm off the span's axis
    s["mesh"] = s["h"] / 3.0
    b, h, a = s["b"], s["h"], s["a"]
    A, Z, Zp = b * h, b * h * h / 6.0, b * h * h / 4.0
    Fy = fy / (1.0 / A + a / Z)                      # first yield, N
    Fp = fy * Zp / a                                 # M_p's load, N
    load = 1.25 * Fp
    d.call("new_project")
    bracket_part(d, s)
    d.call("set_active_part", part="bracket")
    bodyid = d.call("list_elements")["bodies"][0]["created_by"]
    d.call("set_material", feature_id=bodyid, material="steel")
    faces = bracket_faces(d, s)
    # The span's closed form: the bilinear section under N = -F, M = F a.
    e0, kap, (e_lo, e_hi) = S.bilinear_section(b, h, E, fy, Et, -load,
                                               load * a)
    # Plastic strain of the tension fibre (x = +h/2 if M bends it there).
    e_out = max(abs(e_lo), abs(e_hi))
    sig = fy + Et * (e_out - fy / E)
    ep_out = e_out - sig / E
    # Unloaded elastically: the curvature left, over the span.
    kap_res = abs(kap) - load * a / (E * b * h ** 3 / 12.0)
    turn_res = kap_res * s["span"]
    rec = {"first_yield_n": Fy, "mp_load_n": Fp, "load_n": load,
           "first_yield_factor_closed": Fy / load,
           "outer_plastic_strain_closed": ep_out,
           "residual_turn_closed_rad": turn_res}
    sid, r = bracket_run(d, s, faces, load, {"plasticity": True}, timeout_s)
    body = r["bodies"][0]
    nl = body.get("nonlinear", {})
    rec["first_yield_factor"] = nl.get("first_yield_factor")
    probes = {k: d.call("probe_study", id=sid, face=faces[k])
              for k in ("span +x", "span -x", "top")}
    tens = max(("span +x", "span -x"),
               key=lambda k: probes[k].get("plastic_strain_mean", 0.0))
    rec["outer_plastic_strain_mean"] = probes[tens].get("plastic_strain_mean")
    rec["outer_plastic_strain_max"] = probes[tens].get("plastic_strain_max")
    rec["max_plastic_strain"] = (r.get("max_plastic_strain") or {}).get(
        "value")
    rt = probes["top"].get("residual_rotation_rad") or [0, 0, 0]
    rec["residual_turn_rad"] = abs(rt[1])
    rec["reached_load"] = not (r.get("nonlinear") or {}).get(
        "collapse_factor")
    # Its first yield: the span's outer fibre, from the same answer's mean
    # stress there scaled back to yield (the run is linear to first yield).
    d.call("save_project", path=str(out / "bracket.lob"))
    # Again with large deformation: does a hardening steel report a
    # collapse?
    sid2, r2 = bracket_run(d, s, faces, load,
                           {"plasticity": True, "large_deformation": True},
                           timeout_s)
    nl2 = r2.get("nonlinear") or {}
    rec["large_deformation"] = {
        "collapse_factor": nl2.get("collapse_factor"),
        "max_plastic_strain": (r2.get("max_plastic_strain") or {}).get(
            "value")}
    # The control: under its first yield, nothing left once unloaded.
    sid3, r3 = bracket_run(d, s, faces, 0.8 * Fy, {"plasticity": True},
                           timeout_s)
    rec["control"] = {
        "load_n": 0.8 * Fy,
        "max_plastic_strain": (r3.get("max_plastic_strain") or {}).get(
            "value", 0.0),
        "max_residual_mm": (r3.get("max_residual_displacement_mm") or
                            {}).get("value", 0.0)}
    d.call("save_project", path=str(out / "bracket.lob"))
    rec["off"] = {
        "first_yield": (rec["first_yield_factor"] or 0) /
        rec["first_yield_factor_closed"] - 1.0,
        "plastic_strain": (rec["outer_plastic_strain_max"] or 0) /
        ep_out - 1.0,
        "residual_turn": rec["residual_turn_rad"] / turn_res - 1.0}
    return rec
