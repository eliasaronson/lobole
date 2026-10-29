# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The woodcutter's loaded parts on pins: static studies of the camshaft, the
crank and the handle's pin, each part on `pin` supports where it turns or is
pressed (docs/simulation/README.md#pin-supports), with every load read from
`check_followers` (docs/assemblies/relations.md#contact-forces), never
estimated.

- **The camshaft at work**: on a pin at each bushing, loaded by both
  followers' pushes on their cams at the sample where the crank needs the
  most torque, and by the hand's push at the grip that balances them.
- **A child leaning on the crank**: the same torque's push at the grip
  (torque / crank radius) times `S.LEAN_FACTOR`, straight down the crank
  at the top of its turn, on the camshaft's crank end (the shaft on its
  bushings), the crank (on the shaft) and the handle's pin (in the crank).

- **Buckling**: the crank and the handle's pin leaned on, each also as a
  buckling study of the same loads (docs/simulation/README.md#buckling),
  its base state solved once: whether either is buckling-limited at the
  lean -- its knocked-down elastic factor below its yield and interlayer
  ones -- and the governing factor of the two together.

Each study's factor of safety against yield is the receipt's claim; the
stress across the printed layers (`layer_normal`, the part's print
direction) is read beside it against PLA's interlayer strength. The pins'
reactions are held to the loads' statics: what they carry together, force
and moment, against the loads' to 1 %, and a study's one pin on its own.
The shaft's two bushings share as the FEM's stiffness says (a pin with no
clearance takes moment along its band): each one's share against the
lever rule is recorded beside it, not held to.

The camshaft at work must hold with S.WORK_NEED to spare and every study
must balance, or the run stops; the lean's verdict is recorded either way
(generators/woodcutter/README.md, "Not done here").

Every part is modelled in the box frame where it stands at rest, and the
cams, the crank and the bushings are fastened to the shaft at frames that
coincide, so a cam part's frame is the camshaft's: `check_followers`'
`in_cam_part` loads the shaft as it stands.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import woodcutter_spec as S  # noqa: E402
from _lib import _run_static  # noqa: E402

X = [1.0, 0.0, 0.0]
STATICS_TOL = 0.01


def fail(msg):
    raise SystemExit(f"woodcutter FEM: {msg}")


def add(a, b):
    return [a[i] + b[i] for i in range(3)]


def scale(a, k):
    return [v * k for v in a]


def norm(a):
    return math.sqrt(sum(v * v for v in a))


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]]


def band(x0, x1):
    """The part of a face between two planes across the shaft (along X)."""
    return {"normal": X, "from_mm": float(min(x0, x1)),
            "to_mm": float(max(x0, x1))}


def round_faces(d, part, radius):
    """The cylindrical faces of `part` of that radius, each with its share
    of their area: a D's round is cut into more than one face where the
    circle's seam fell. Each spans the part's whole length, so a band takes
    the same share of each."""
    d.call("set_active_part", part=part)
    body = d.call("list_elements")["bodies"][0]
    faces = [(f["name"], f["area_mm2"]) for f in body["faces"]
             if f.get("kind") == "cylinder" and
             abs(f.get("radius", 0.0) - radius) < 1e-6]
    if not faces:
        fail(f"{part}: no round face of radius {radius:g}")
    total = sum(a for _, a in faces)
    return [(n, a / total) for n, a in faces]


def spread(key, faces, kw, magnitude):
    """One load over several faces, each its area's share of `magnitude`
    (a remote force's `force_n`, a force's `magnitude`): what the faces
    carry together is the whole load, at the same point."""
    out = []
    for i, (face, share) in enumerate(faces):
        k = dict(kw, face=face)
        k["force_n" if kw["type"] == "remote" else "magnitude"] = \
            magnitude * share
        out.append((f"{key}#{i}", k))
    return out


def pins(key, faces, region, fix_rotation=False):
    """A pin over a band of each face: one support, as several faces. One
    of them held from turning holds the whole round."""
    out = []
    for i, (face, _) in enumerate(faces):
        kw = {"type": "pin", "face": face}
        if region:
            kw["region"] = region
        if fix_rotation and i == 0:
            kw["fix_rotation"] = True
        out.append((f"{key}#{i}", kw))
    return out


# ---- the loads, from check_followers ---------------------------------------

def follower_loads(fr, rel):
    """The worst sample of the crank turn, by the driver's torque summed
    over both followers, and each follower's push on its cam there (the
    camshaft's frame: see the docstring)."""
    worst = fr["driver_torque"]["max"]
    if worst is None:
        fail("check_followers gave no sample with every follower's force")
    idx = worst["sample"]
    by_rel = {r["relation"]: r for r in fr["relations"]}
    loads = {}
    for k, rid in rel.items():
        smp = next(p for p in by_rel[rid]["per_sample"]
                   if p["sample"] == idx)
        loads[k] = {"contact_mm": smp["in_cam_part"]["contact_mm"],
                    "force_n": smp["in_cam_part"]["force_on_cam_n"],
                    "contact_force_n": smp["contact_force_n"],
                    "driver_torque_n_mm": smp["driver_torque_n_mm"],
                    "pressure_angle_deg": smp["pressure_angle_deg"],
                    "cam_deg": smp["cam_deg"]}
        if smp["contact_force_n"] <= 0.0:
            fail(f"the {k} rod lifts off its cam at the worst sample "
                 f"({smp['contact_force_n']} N): no push to load")
    return idx, worst["driver_torque_n_mm"], loads


def moment_about_shaft(s, point, force):
    """A force's moment about the shaft's axis (+X through (0, 0, zc)),
    N mm."""
    r = [point[0], point[1], point[2] - s["zc"]]
    return cross(r, force)[0]


def grip_point(s):
    """The middle of the grip, in the camshaft's (and the crank's) frame,
    with the crank at rest: straight up from the shaft."""
    x = s["x_crank0"] - s["g"] - s["grip_len"] / 2.0
    return [x, 0.0, s["zc"] + s["crank_r"]]


# ---- a study and its checks ------------------------------------------------

def run(d, part, name, loads, layer_normal, mesh_mm):
    """A static study of `part`: `loads` [(key, add_load kwargs)], meshed at
    `mesh_mm`, solved, read with the print direction. Returns (study,
    {key: load id}, results)."""
    d.call("set_active_part", part=part)
    st = d.call("add_study", type="static", name=name)
    sid = st.get("id", st.get("study"))
    ids = {k: d.call("add_load", study=sid, **kw)["id"] for k, kw in loads}
    d.call("set_study", study=sid, mesh_size=float(mesh_mm))
    _run_static(d, sid, 900.0)
    r = d.call("study_results", study=sid, layer_normal=list(layer_normal))
    return sid, ids, r


def receipt_fos(r):
    """The factor of safety against yield, as the receipt claims it."""
    for c in r.get("receipt", {}).get("claims", []):
        if c.get("claim", c.get("what", "")).startswith("factor of safety"):
            return c["value"]
    fail(f"the study's receipt claims no factor of safety: "
         f"{r.get('receipt')}")


def reactions(r):
    out = {}
    for b in r.get("bodies", []):
        for x in b.get("reactions", []):
            out[x["support"]] = x
    return out


def statics(r, ids, applied, lever=None):
    """The pins' reactions against the loads `applied` [(force N, point
    mm)]: what the pins carry together, force and moment about the part's
    origin, is minus the loads' to STATICS_TOL of theirs; a study on one pin
    holds that pin to it. Two pins share it as their stiffness says, since
    a pin along a band takes moment (the FEM's pin has no clearance): each
    one's share against the lever rule over the bands' middles (`lever`
    {key: (force, centre)}) and the moment it takes about its middle are
    recorded, not held to."""
    rs = reactions(r)
    force = [0.0, 0.0, 0.0]
    moment = [0.0, 0.0, 0.0]
    for f, at in applied:
        force = add(force, f)
        moment = add(moment, cross(at, f))
    held_f = [0.0, 0.0, 0.0]
    held_m = [0.0, 0.0, 0.0]
    pins = {}
    for key, lid in ids.items():
        if lid not in rs:
            continue
        x = rs[lid]
        if "moment_n_mm" not in x:
            fail(f"support {lid}'s reaction carries no moment: a result "
                 f"saved before it was kept")
        held_f = add(held_f, x["force_n"])
        held_m = add(held_m, x["moment_n_mm"])
        p = pins.setdefault(key.split("#")[0], {
            "force_n": [0.0, 0.0, 0.0], "moment_n_mm": [0.0, 0.0, 0.0],
            "contact_area_mm2": 0.0, "face_area_mm2": 0.0})
        p["force_n"] = add(p["force_n"], x["force_n"])
        p["moment_n_mm"] = add(p["moment_n_mm"], x["moment_n_mm"])
        for k in ("contact_area_mm2", "face_area_mm2"):
            p[k] += x.get(k) or 0.0
    out = {"applied_n": force, "applied_moment_n_mm": moment,
           "held_n": held_f, "held_moment_n_mm": held_m,
           "force_error": norm(add(held_f, force)) / norm(force),
           "moment_error": norm(add(held_m, moment)) / norm(moment),
           "pins": pins}
    checks = [out["force_error"], out["moment_error"]]
    if len(pins) == 1:
        (key, p), = pins.items()
        p["force_error"] = norm(add(p["force_n"], force)) / norm(force)
        p["moment_error"] = norm(add(p["moment_n_mm"], moment)) / norm(moment)
        checks += [p["force_error"], p["moment_error"]]
    for key, (want, centre) in (lever or {}).items():
        p = pins[key]
        p["lever_rule_n"] = want
        # Across the shaft only: along it both flanges hold it.
        p["lever_rule_error"] = (
            norm([0.0, p["force_n"][1] - want[1], p["force_n"][2] - want[2]])
            / norm([0.0, want[1], want[2]]))
        p["moment_about_middle_n_mm"] = add(
            p["moment_n_mm"], scale(cross(centre, p["force_n"]), -1.0))
    out["error"] = max(checks)
    out["ok"] = out["error"] <= STATICS_TOL
    return out


def peak(r, key):
    """A maximum the study reports, {value, at, body} or a bare number."""
    v = r.get(key)
    return (v.get("value"), v.get("at")) if isinstance(v, dict) else (v, None)


def verdict(r, name, need):
    """What the study says. The headline is the receipt's factor of safety
    against yield and the factor across the layers beside it; the governing
    one is the smaller. A peak on a pin's edge is recorded with the stress
    away from it, never as the headline."""
    fos = receipt_fos(r)
    if r.get("factor_of_safety") is not None and \
            abs(fos - r["factor_of_safety"]) > 1e-9 * fos:
        fail(f"{name}: the receipt's factor {fos} is not the result's "
             f"{r['factor_of_safety']}")
    vm, vm_at = peak(r, "max_von_mises_mpa")
    pr, pr_at = peak(r, "max_principal_mpa")
    ac, ac_at = peak(r, "max_across_layers_mpa")
    dx, dx_at = peak(r, "max_displacement_mm")
    out = {"factor_of_safety": fos,
           "max_von_mises_mpa": vm, "max_von_mises_at": vm_at,
           "max_principal_mpa": pr, "max_principal_at": pr_at,
           "max_across_layers_mpa": ac, "max_across_layers_at": ac_at,
           "interlayer_factor_of_safety": r["interlayer_factor_of_safety"],
           "max_displacement_mm": dx, "max_displacement_at": dx_at,
           "nodes": r.get("nodes"), "elements": r.get("elements"),
           "need": need}
    edge = r.get("peak_at_support_edge")
    if edge:
        out["peak_at_support_edge"] = edge
    il = out["interlayer_factor_of_safety"]
    out["governing"] = ("across the layers" if il < fos
                        else "yield (von Mises)")
    out["governing_at"] = ac_at if il < fos else vm_at
    out["governing_factor"] = min(fos, il)
    out["pass"] = out["governing_factor"] >= need
    return out


def report(name, v, st):
    edge = v.get("peak_at_support_edge")
    note = (f"; the yield peak sits on pin {edge['support']}'s edge, "
            f"{edge.get('factor_of_safety_away', 0):.3g} away from it"
            if edge else "")
    at = v["governing_at"]
    where = (f" at ({at[0]:.1f}, {at[1]:.1f}, {at[2]:.1f})"
             if at else "")
    print(f"  FEM {name}: factor {v['factor_of_safety']:.1f} against yield "
          f"({v['max_von_mises_mpa']:.3g} MPa), "
          f"{v['interlayer_factor_of_safety']:.1f} across the layers "
          f"({v['max_across_layers_mpa']:.3g} MPa); governs: "
          f"{v['governing']}{where}; the pins answer the loads' force to "
          f"{100 * st['force_error']:.3f} % and moment to "
          f"{100 * st['moment_error']:.3f} %" +
          "".join(f"; {k} carries {norm(p['force_n']):.3g} N, the lever "
                  f"rule's {norm(p['lever_rule_n']):.3g}"
                  for k, p in st["pins"].items() if "lever_rule_n" in p) +
          note)


# ---- the studies -----------------------------------------------------------

def studies(d, s, fr_turn, rel):
    """Every study; returns the receipt's section. Stops the run on a part
    that does not hold, or on reactions that do not answer the loads."""
    idx, t_worst, fl = follower_loads(fr_turn, rel)
    r_shaft = s["d_shaft"] / 2.0
    shaft = round_faces(d, "camshaft", r_shaft)
    # The bushings' sleeves turn in the walls: that is where the shaft is
    # held, each a pin over the sleeve's length in its wall. The FEM's pin
    # has no clearance, so each takes moment along its band as well as
    # force, and the two share the load as their stiffness says rather than
    # by the lever rule over their middles (recorded beside the reactions).
    # A bushing with the print's gap tilts in its bore instead; the bending
    # moment at the left bushing, which governs the shaft's crank end, is
    # the overhang's either way.
    mesh_shaft = s["d_shaft"] / 4.0
    bush_l = (s["x_bush_l"], -s["g"])
    bush_r = (s["width"] + s["g"], s["x_bush_r"])
    xl, xr = sum(bush_l) / 2.0, sum(bush_r) / 2.0
    crank_end = (s["x_crank0"], s["x_crank1"])
    layers_shaft = print_up("camshaft")
    bushings = (pins("bushing L", shaft, band(*bush_l), fix_rotation=True) +
                pins("bushing R", shaft, band(*bush_r)))
    out = {"sample": idx, "speed_rpm": fr_turn["speed_rpm"],
           "driver_torque_n_mm": t_worst, "followers": fl,
           "need": {"at_work": S.WORK_NEED, "leaned_on": S.LEAN_NEED}}

    # The followers' pushes, each on the shaft over its cam's width, and
    # the hand's push at the grip that balances their torque.
    loads, applied = [], []
    turning = 0.0
    for k, f in fl.items():
        x0 = s[f"x_{k}"] - s["cam_w"] / 2.0
        loads += spread(f"{k} cam", shaft,
                        {"type": "remote", "direction": f["force_n"],
                         "point_mm": f["contact_mm"],
                         "region": band(x0, x0 + s["cam_w"])},
                        norm(f["force_n"]))
        applied.append((f["force_n"], f["contact_mm"]))
        turning += moment_about_shaft(s, f["contact_mm"], f["force_n"])
    # The moment the followers put on the shaft is what the driver must
    # answer: check_followers' torque, read two ways.
    if abs(turning + t_worst) > STATICS_TOL * abs(t_worst):
        fail(f"the followers' pushes turn the shaft by {turning:.4g} N mm "
             f"where check_followers says the crank needs {t_worst:.4g}")
    grip = grip_point(s)
    hand = [0.0, turning / s["crank_r"], 0.0]  # its moment -R F_y = -turning
    loads += spread("hand", shaft,
                    {"type": "remote", "direction": hand, "point_mm": grip,
                     "region": band(*crank_end)}, norm(hand))
    applied.append((hand, grip))
    loads += bushings
    sid, ids, r = run(d, "camshaft", "camshaft at work", loads,
                      layers_shaft, mesh_shaft)
    st = statics(r, ids, applied, lever_rule(applied, xl, xr, s["zc"]))
    v = verdict(r, "camshaft at work", S.WORK_NEED)
    report("camshaft at work", v, st)
    out["camshaft"] = {"study": sid, "hand_n": hand, "grip_mm": grip,
                       "followers_turn_n_mm": -turning, **v, "statics": st}

    # A child leaning on the crank: straight down it at the top of its
    # turn. A lean that turns the crank turns it (the followers hold back
    # only the torque above); the push the crank is held against is the
    # one through the shaft.
    push = abs(t_worst) / s["crank_r"]
    lean = S.LEAN_FACTOR * push
    down = [0.0, 0.0, -lean]
    out["lean"] = {"turning_push_n": push, "factor": S.LEAN_FACTOR,
                   "push_n": lean, "lean_kg": lean / S.G,
                   "direction": [0.0, 0.0, -1.0], "at_mm": grip}
    print(f"  FEM loads: at sample {idx} the crank needs "
          f"{t_worst:.3g} N mm ({push:.3g} N at the grip); a child's lean "
          f"{S.LEAN_FACTOR:g} times that, {lean:.1f} N ({lean / S.G:.1f} kg)")
    loads = spread("hand", shaft,
                   {"type": "remote", "direction": [0.0, 0.0, -1.0],
                    "point_mm": grip, "region": band(*crank_end)}, lean)
    loads += bushings
    sid, ids, r = run(d, "camshaft", "camshaft's crank end, leaned on",
                      loads, layers_shaft, mesh_shaft)
    st = statics(r, ids, [(down, grip)],
                 lever_rule([(down, grip)], xl, xr, s["zc"]))
    v = verdict(r, "camshaft's crank end", S.LEAN_NEED)
    report("camshaft's crank end, leaned on", v, st)
    out["shaft_end"] = {"study": sid, **v, "statics": st}

    # The crank: on the shaft (its hub's bore a pin that cannot turn: the
    # D keys it), pushed through its handle's bore by the pin.
    loads, mesh_crank = crank_loads(d, s, lean, grip)
    sid, ids, r = run(d, "crank", "crank, leaned on", loads,
                      print_up("crank"), mesh_crank)
    st = statics(r, ids, [(down, grip)])
    v = verdict(r, "crank", S.LEAN_NEED)
    report("crank, leaned on", v, st)
    out["crank"] = {"study": sid, **v, "statics": st}

    # The handle's pin: pressed in the crank (a pin over the crank's
    # thickness, held from turning), the grip pushing on it over its length.
    x1 = s["x_crank0"] - s["g"]
    loads, mesh_pin = pin_loads(d, s, lean)
    sid, ids, r = run(d, "handle pin", "handle pin, leaned on", loads,
                      print_up("handle pin"), mesh_pin)
    # The grip's push on the pin, uniform over its length: at the band's
    # middle, on the plane of symmetry through the pin's axis.
    st = statics(r, ids, [(down, [x1 - s["grip_len"] / 2.0, 0.0,
                                  s["zc"] + s["crank_r"]])])
    v = verdict(r, "handle pin", S.LEAN_NEED)
    report("handle pin, leaned on", v, st)
    out["handle_pin"] = {"study": sid, **v, "statics": st}

    # Whether the crank and the handle's pin are buckling-limited at the
    # lean (docs/simulation/README.md#buckling).
    out["buckling"] = leaned_buckling(d, s, lean, grip)

    # What each part leaned on does hold, with its factor to spare: the
    # study is linear, so the push scales with the governing factor.
    for k in LEANED:
        v = out[k]
        v["holds_push_n"] = lean * v["governing_factor"] / v["need"]
        edge = v.get("peak_at_support_edge")
        if edge and edge.get("factor_of_safety_away") is not None:
            v["holds_push_away_from_pins_n"] = lean * min(
                edge["factor_of_safety_away"],
                v["interlayer_factor_of_safety"]) / v["need"]
        if not v["pass"]:
            print(f"  FEM {k.replace('_', ' ')}: does NOT hold the lean with "
                  f"{v['need']:g} to spare: it holds "
                  f"{v['holds_push_n']:.1f} N at the grip "
                  f"({v['holds_push_n'] / S.G:.2f} kg)")
    keys = ("camshaft",) + LEANED
    out["unbalanced"] = [k for k in keys if not out[k]["statics"]["ok"]]
    out["lean_breaks"] = [k for k in LEANED if not out[k]["pass"]]
    out["holds_lean_n"] = min(out[k]["holds_push_n"] for k in LEANED)
    # The gate: the camshaft holds its working load, and every study's pins
    # answer its loads. The lean is the design question the studies answer,
    # recorded either way (README, "Not done here").
    out["pass"] = out["camshaft"]["pass"] and not out["unbalanced"]
    return out


LEANED = ("shaft_end", "crank", "handle_pin")


def crank_loads(d, s, lean, grip):
    """The crank leaned on: on the shaft (its hub's bore a pin that cannot
    turn: the D keys it), pushed through its handle's bore by the pin.
    Returns (loads, mesh size)."""
    r_shaft = s["d_shaft"] / 2.0
    hub = round_faces(d, "crank", r_shaft + s["press"] / 2.0)
    bore = round_faces(d, "crank", (s["d_handle"] + s["press"]) / 2.0)
    loads = spread("hand", bore, {"type": "remote",
                                  "direction": [0.0, 0.0, -1.0],
                                  "point_mm": grip}, lean)
    loads += pins("shaft", hub, None, fix_rotation=True)
    return loads, s["crank_t"] / 3.0


def pin_loads(d, s, lean):
    """The handle's pin leaned on: pressed in the crank (a pin over the
    crank's thickness, held from turning), the grip pushing on it over its
    length. Returns (loads, mesh size)."""
    shank = round_faces(d, "handle pin", s["d_handle"] / 2.0)
    x1 = s["x_crank0"] - s["g"]
    loads = spread("hand", shank,
                   {"type": "force", "direction": [0.0, 0.0, -1.0],
                    "region": band(x1 - s["grip_len"], x1)}, lean)
    loads += pins("crank", shank, band(s["x_crank0"], s["x_crank1"]),
                  fix_rotation=True)
    return loads, s["d_handle"] / 4.0


def buckle(d, part, name, loads, layer_normal, mesh_mm, timeout_s=1200.0):
    """A buckling study of `part` under `loads` (the static study's), its
    base state solved once (converge: never: the static study beside it
    settles the stress), read with the print direction. Returns what it
    says, and the seconds it took."""
    import time
    d.call("set_active_part", part=part)
    st = d.call("add_study", type="buckling", name=name)
    sid = st.get("id", st.get("study"))
    for _, kw in loads:
        d.call("add_load", study=sid, **kw)
    d.call("set_study", study=sid, mesh_size=float(mesh_mm), converge="never")
    t0 = time.monotonic()
    _run_static(d, sid, timeout_s)
    seconds = time.monotonic() - t0
    r = d.call("study_results", study=sid, layer_normal=list(layer_normal))
    b = r["bodies"][0]
    knocked = (b["buckling_factor"] or 0.0) * b["knockdown"]["value"]
    plastic = min(v for v in (b["yield_factor"], b["interlayer_factor"])
                  if v) if (b["yield_factor"] or b["interlayer_factor"]) \
        else None
    return {"study": sid, "seconds": seconds, "nodes": r["nodes"],
            "buckling_factors": b["buckling_factors"],
            "buckling_factor": b["buckling_factor"],
            "why_none": b.get("why_none"),
            "knockdown": b["knockdown"],
            "yield_factor": b["yield_factor"],
            "interlayer_factor": b["interlayer_factor"],
            "governing_factor": r["governing_factor"],
            # Buckling-limited: the knocked-down elastic factor is below the
            # material's own.
            "buckling_limited": bool(knocked and plastic and knocked < plastic),
            "warnings": r.get("warnings", [])}


def leaned_buckling(d, s, lean, grip):
    """The crank and the handle's pin leaned on, as buckling studies: is
    either buckling-limited at the lean?"""
    out = {}
    for key, part, name, (loads, mesh) in (
            ("crank", "crank", "crank, leaned on, buckling",
             crank_loads(d, s, lean, grip)),
            ("handle_pin", "handle pin", "handle pin, leaned on, buckling",
             pin_loads(d, s, lean))):
        v = buckle(d, part, name, loads, print_up(part), mesh)
        out[key] = v
        bf = v["buckling_factor"]
        print(f"  FEM {name}: "
              + (f"buckles at {bf:.3g} x the lean (knockdown "
                 f"{v['knockdown']['value']:.3g}, {v['knockdown']['class']})"
                 if bf else f"does not buckle ({v['why_none']})")
              + f"; governing {v['governing_factor']:.3g} with yield "
              f"{v['yield_factor']}, across the layers "
              f"{v['interlayer_factor']}: "
              + ("buckling-limited" if v["buckling_limited"]
                 else "not buckling-limited")
              + f" ({v['seconds']:.0f} s, {v['nodes']} nodes)")
    return out


def lever_rule(applied, xl, xr, zc):
    """Each bushing's reaction to `applied` [(force, point)] across the
    shaft by the lever rule: a beam on two supports at xl and xr (the
    sleeves' middles), with each middle on the axis."""
    rr = [0.0, 0.0, 0.0]
    total = [0.0, 0.0, 0.0]
    for f, at in applied:
        rr = add(rr, scale(f, -(at[0] - xl) / (xr - xl)))
        total = add(total, f)
    rl = add(scale(total, -1.0), scale(rr, -1.0))
    return {"bushing L": (rl, [xl, 0.0, zc]), "bushing R": (rr, [xr, 0.0, zc])}


def print_up(part):
    import woodcutter_parts as P
    return P.PRINT_UP[part]
