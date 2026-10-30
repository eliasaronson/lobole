# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The tea alarm's parts and its trigger, through the app
(README.md#the-trigger):

- every part built and verified, the metal ones in their materials, the
  printed ones on the profile with the way each prints;
- the ramp's static moment about its pivot from the app's mass properties
  (as printed) and the screw's; the largest ball that still leaves the ramp
  resting on the probe, and the ball the build uses; the probe's force;
- the assembly: the probe on a slider in its guide, the ramp on a revolute
  on its pivot, the screw, the knob and the ball fastened to the ramp, the
  rest fixed where they stand;
- the trigger: the probe's push on the ramp turns it, through a rack
  relation (the stand-in for a contact relation, which takes a turning cam
  only); `tea_c` swept from the pour down, the ramp's angle read back, and
  the temperature where it reaches the detent's slope phi.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import ta_parts as P  # noqa: E402
import ta_spec as S  # noqa: E402

ASM = "tea alarm"
PRINTED = ("block", "ramp", "knob", "dish")
# Bambu Studio's "0.20mm Standard" on the P1S, as the woodcutter prints;
# the knob and the ramp with four walls (they carry the screw's thread and
# its turning).
PRINT = {"walls": 2, "top_layers": 5, "bottom_layers": 3,
         "infill_pct": 15, "infill_pattern": "grid"}
SOLID_WALLS = ("ramp", "knob")
# The way each printed part goes on the bed (its own +direction up): the
# block and the dish standing, the ramp on its side (its bores then point
# up or lie in a layer), the knob on its face (the head's pocket open
# upward).
PRINT_UP = {"block": [0.0, 0.0, 1.0], "ramp": [0.0, -1.0, 0.0],
            "knob": [0.0, 0.0, -1.0], "dish": [0.0, 0.0, 1.0]}
# What each part looks like (linear RGB).
LOOKS = {"strip": (0.55, 0.55, 0.58), "frame rod": (0.7, 0.7, 0.72),
         "probe": (0.7, 0.7, 0.72), "chime": (0.8, 0.8, 0.82),
         "pivot": (0.7, 0.7, 0.72), "clip": (0.72, 0.72, 0.74),
         "ball": (0.8, 0.8, 0.82), "block": (0.05, 0.25, 0.45),
         "ramp": (0.75, 0.55, 0.05), "knob": (0.5, 0.05, 0.03),
         "dish": (0.05, 0.25, 0.45), "vessel": (0.85, 0.85, 0.8)}


def fail(msg):
    raise SystemExit(f"tea alarm: {msg}")


def part(d, name, mat, fn, s, proc=None, up=None):
    """A part: built by fn, one body, verified, in its material."""
    d.call("add_part", name=name)
    if proc is not None:
        d.call("set_print_process", **proc)
        settings = dict(PRINT, direction=list(up))
        if name in SOLID_WALLS:
            settings["walls"] = 4
        d.call("set_print_settings", **settings)
    out = fn(P.Builder(d), s)
    bodies = d.call("list_elements")["bodies"]
    if len(bodies) != 1:
        fail(f"{name} is {len(bodies)} bodies, not one")
    geo = d.call("check_geometry")
    if geo["receipt"]["standing"] != "verified":
        fail(f"{name} is not verified: {geo['receipt']}")
    d.call("set_material", feature_id=bodies[0]["created_by"], material=mat)
    if name in LOOKS:
        d.call("set_appearance", feature_id=bodies[0]["created_by"],
               color=list(LOOKS[name]), roughness=0.4 if mat != "pla" and
               mat != "petg" else 0.6)
    return out


def metal_parts(d, s):
    """The strip, the rod, the pivot, the chime, the clip, the vessel."""
    part(d, "strip", "bimetal_kanthal_94s", P.strip, s)
    part(d, "frame rod", "stainless_steel", P.frame_rod, s)
    part(d, "pivot", "stainless_steel", P.pivot, s)
    part(d, "chime", "stainless_steel", P.chime, s)
    clip = part(d, "clip", "stainless_steel", _clip, s)
    sm = d.call("check_sheet_metal")
    if not sm["ok"]:
        fail(f"the clip fails check_sheet_metal: {sm['violations']}")
    # The vessel stands in for the mug: glazed stoneware is no library
    # material, and a rigid support's material is not read.
    part(d, "vessel", "glass", P.vessel, s)
    return clip


def _clip(b, s):
    b.d.call("set_sheet_metal_rules", material="stainless_steel",
             gauge=S.CLIP_GAUGE)
    return P.clip(b, s)


def ramp_statics(d, s, proc, describe):
    """The ramp's own static moment about its pivot (the printed ramp's mass
    at its centre, which the printed estimate spreads evenly) and the
    screw's, both on the probe's side; the largest ball whose moment in the
    detent behind the pivot leaves them the larger, and the ball the build
    uses: the largest whole millimetre under it, less one so the probe
    keeps a load."""
    part(d, "ramp", proc_material(d, proc), P.ramp, s, proc,
         PRINT_UP["ramp"])
    mp = d.call("mass_properties", part="ramp")["total"]
    m_ramp = (mp.get("printed") or {}).get("mass_g") or fail(
        "the ramp has no printed mass: is its profile stating its layer?")
    x_ramp = s["r_arm"] - mp["center_of_mass_mm"][0]   # toward the probe
    d.call("add_assembly", name="screw store")
    screw_len = next((L for L in S.M3_LENGTHS
                      if L >= s["arm_t"] + s["screw_out_max"] + S.M3_PITCH),
                     None) or fail("no M3 length reaches")
    d.call("add_fastener", standard="iso4762", size="M3", length=screw_len,
           position=[0.0, 0.0, 0.0], orientation=[0.0, 0.0, 0.0, 1.0])
    screw = f"ISO 4762 M3x{screw_len:g}"
    d.call("delete_part", part="screw store")
    smp = d.call("mass_properties", part=screw)["total"]
    m_screw = smp["mass_g"]
    # The screw's centre of mass is on the probe's axis (x = 0).
    x_screw = s["r_arm"]
    lever = m_ramp * x_ramp + m_screw * x_screw          # g mm
    x_ball0 = s["x_stop"] + s["stop_t"] - s["r_arm"]
    d_max = S.largest_ball(lever, x_ball0)
    ball_d = math.floor(d_max) - 1.0
    if ball_d < 1.0:
        fail(f"the ramp's own moment ({lever:.1f} g mm) leaves no room for "
             f"a ball (the largest is {d_max:.2f} mm)")
    m_ball = S.ball_mass_g(ball_d)
    x_ball = x_ball0 + ball_d / 2.0
    net = lever - m_ball * x_ball                          # g mm
    force = net * 1e-3 * S.G / s["r_arm"]                  # N
    rec = {"ramp_printed_g": m_ramp, "ramp_lever_mm": x_ramp,
           "screw": screw, "screw_g": m_screw, "lever_gmm": lever,
           "largest_ball_mm": d_max, "ball_mm": ball_d, "ball_g": m_ball,
           "ball_lever_mm": x_ball, "probe_force_n": force}
    return rec


def proc_material(d, proc):
    for p in d.call("list_print_profiles")["profiles"]:
        if p["name"] == proc["profile"]:
            return p["material"]
    fail(f"no print profile '{proc['profile']}'")


def strip_budget(s, force):
    """What the strip leaves: the force's temperature cost (an offset the
    screw takes out), Kanthal's minimum-volume rule on it over the
    coolest trigger's travel, the K per mN friction costs, and the stress at
    the clamp."""
    lo, _ = S.TRIGGER_RANGE
    cost = force * s["k_per_n"]                              # K
    share = cost / (S.T_POUR - lo)
    stress = S.strip_stress(force, s["L"], s["b"], s["s"])
    return {"offset_k": cost, "force_share": share,
            "k_per_mn": s["k_per_n"] / 1000.0, "stress_mpa": stress,
            "stress_bound_mpa": S.BIMETAL_YIELD_MPA}


def screw_and_probe(s, force):
    """The probe's length that puts --ready-c on the screw's line (40 C a
    pitch out, 70 C the range's travel further: ta_spec.solve)."""
    lo, hi = S.TRIGGER_RANGE
    give = S.force_deflection(force, s["L"], s["b"], s["s"])
    p_ready = S.M3_PITCH + (s["ready"] - lo) * s["A"]
    s["probe_len"] = ((s["ready"] - S.T_FLAT) * s["A"] - s["z_strip"] + give
                      - p_ready + s["z_arm"] + s["r_arm"] * s["phi"])
    s["screw_ready"] = p_ready
    # The screw's under-head plane, and the knob over its head.
    s["z_knob"] = s["z_arm"] - p_ready + s["screw_len"]
    return s


def build(d, s, proc, describe):
    """Every part, the statics that size the ball and the probe, and the
    assembly. Returns the receipt's `mechanism` and the mates."""
    mat = proc_material(d, proc)
    clip = metal_parts(d, s)
    stat = ramp_statics(d, s, proc, describe)
    s["screw_len"] = float(stat["screw"].split("x")[-1])
    S.ball_place(s, stat["ball_mm"])
    screw_and_probe(s, stat["probe_force_n"])
    part(d, "probe", "stainless_steel", P.probe, s)
    part(d, "ball", "steel", P.ball, s)
    for name, fn in (("block", P.block), ("knob", P.knob),
                     ("dish", P.dish)):
        part(d, name, mat, fn, s, proc, PRINT_UP[name])
    o, m = assembly(d, s, stat["screw"])
    return {"statics": stat, "clip": clip, "occurrences": o, "mates": m}


FIXED = ("block", "frame rod", "strip", "clip", "chime", "pivot", "dish")


def mate(d, kind, a, fa, b, fb, name):
    return d.call("add_mate", type=kind, name=name,
                  a={"occurrence": a, "frame": fa},
                  b={"occurrence": b, "frame": fb}, flipped=False)["id"]


def fasten(d, a, b, name):
    """b fastened to a where it already stands."""
    return d.call("add_mate", type="fastened", name=name,
                  a={"occurrence": a, "origin": True},
                  b={"occurrence": b, "origin": True}, derive="pose")["id"]


def assembly(d, s, screw):
    d.call("add_assembly", name=ASM)
    o = {n: d.call("add_occurrence", part=n, name=n, fixed=True)["id"]
         for n in FIXED}
    for n in ("probe", "ramp", "knob", "ball"):
        o[n] = d.call("add_occurrence", part=n, name=n)["id"]
    # The screw, head up, its under-head plane where the knob sits.
    r = d.call("add_fastener", standard="iso4762", size="M3",
               length=s["screw_len"], position=[0.0, 0.0, s["z_knob"]],
               orientation=[1.0, 0.0, 0.0, 0.0])
    o["screw"] = r["occurrence"]
    m = {"probe": mate(d, "slider", o["block"], "guide", o["probe"], "guide",
                       "probe in its guide"),
         "ramp": mate(d, "revolute", o["block"], "pivot", o["ramp"], "pivot",
                      "ramp on its pivot")}
    for n in ("screw", "knob", "ball"):
        fasten(d, o["ramp"], o[n], f"{n} on the ramp")
    sol = d.call("solve_assembly")
    if sol.get("errored_mates"):
        fail(f"the assembly does not solve: {sol}")
    return o, m


# ---- the trigger ---------------------------------------------------------------

def contact_refused(d, m, o):
    """The plan's driver: the probe's slide pushing the ramp through a
    `contact` relation. A contact relation's driver is a cam's turn, so a
    translating pusher is refused; the refusal is the gap recorded, and
    the ramp is posed by hand below (README.md#what-the-round-exposed)."""
    try:
        r = d.call("add_relation", type="contact", a=m["probe"], b=m["ramp"],
                   cam=o["probe"], follower=o["ramp"],
                   name="probe lifts the ramp")
    except Exception as e:  # noqa: BLE001 -- the refusal is the answer
        return str(e)
    d.call("remove_relation", id=r["id"])
    return None


def pose(d, s, m, T, force, screw_out):
    """The probe where the strip's tip puts it at T, and the ramp where the
    probe lifts it (by hand: the push past touching, over the arm), solved.
    Returns (the ramp's angle as the app reads it back, the push)."""
    flat = s["z_strip"]
    tip = flat - s["A"] * (T - S.T_FLAT)
    push = tip + s["probe_len"] - (s["z_arm"] - screw_out)
    if push > 0.0:   # in contact: the ramp's weight bends the strip
        tip -= S.force_deflection(force, s["L"], s["b"], s["s"])
        push = max(0.0, tip + s["probe_len"] - (s["z_arm"] - screw_out))
    d.call("set_mate", id=m["probe"], offset=tip - flat)
    # Lifting the probe's end is a negative turn about the pivot's +Y.
    d.call("set_mate", id=m["ramp"],
           rotation_deg=-math.degrees(push / s["r_arm"]))
    sol = d.call("solve_assembly")
    if sol.get("errored_mates"):
        fail(f"the assembly does not solve at {T:g} C: {sol}")
    got = next(x for x in d.call("list_mates")["mates"]
               if x["id"] == m["ramp"])
    return -math.radians(got["rotation_deg"]), push


def sweep(d, s, m, force, screw_out, step=None):
    """tea_c from the pour down, by a tenth of the trigger's band: the
    temperature at which the ramp's angle first reaches phi (the detent's
    slope), interpolated between the two samples either side."""
    step = step or s["band_k"] / 10.0
    T, prev = S.T_POUR, None
    samples = []
    while T >= S.K94S_LINEAR[0]:
        d.call("set_variable", name="tea_c", expression=f"{T:.6f}")
        ang, push = pose(d, s, m, T, force, screw_out)
        samples.append((T, ang))
        if ang >= s["phi"]:
            if prev is None:
                fail(f"the ramp is already lifted at the pour ({T:g} C)")
            T0, a0 = prev
            return T0 + (T - T0) * (s["phi"] - a0) / (ang - a0), samples
        prev = (T, ang)
        T -= step
    fail("the ramp never reaches its trigger angle")


def trigger(d, s, o, m, force):
    """The trigger at --ready-c's setting, and at both ends of the knob's
    range; the knob's marks; and the rest pose left in the document."""
    d.call("set_active_part", part=ASM)
    d.call("set_variable", name="tea_c", expression=f"{S.T_POUR:g}",
           comment="the tea's temperature, degrees C (the sweep's)")
    refused = contact_refused(d, m, o)
    lo, hi = S.TRIGGER_RANGE
    ready, _ = sweep(d, s, m, force, s["screw_ready"])
    coolest, _ = sweep(d, s, m, force, s["screw_out_hot"])
    hottest, _ = sweep(d, s, m, force, s["screw_out_max"])
    closed = (S.trigger_c(s, s["screw_ready"], force),
              S.trigger_c(s, s["screw_out_hot"], force),
              S.trigger_c(s, s["screw_out_max"], force))
    # The knob, marked in C: one turn is k_per_turn, so a mark every 5 C
    # sits deg_per_k * 5 round from the last; 0 deg is --ready-c.
    marks = [{"c": c, "deg": (c - s["ready"]) * s["deg_per_k"]}
             for c in range(int(math.ceil(lo / 5.0) * 5), int(hi) + 1, 5)]
    pose(d, s, m, S.T_POUR, force, s["screw_ready"])
    d.call("set_variable", name="tea_c", expression=f"{S.T_POUR:g}")
    return {"ready_c": ready, "range_c": [coolest, hottest],
            "closed_form_c": closed, "contact_refused": refused,
            "marks": marks, "k_per_turn": s["k_per_turn"],
            "turns": (s["screw_out_max"] - s["screw_out_hot"]) / S.M3_PITCH}
