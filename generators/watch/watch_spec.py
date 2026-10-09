# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Dimensions for an automatic mechanical watch — time only, small seconds.

Everything the model is built from, in millimetres, in one place, and in the
same three kinds the drone's spec keeps apart:

  CLASS     what this watch IS: a 40 mm case, a 26 mm movement, 21,600 vph.
  MOVEMENT  the calibre: tooth counts, modules, the depth each wheel runs at,
            plate and bridge thicknesses. Tooth counts are INTEGERS and stay
            out of the document — a count is not a dimension, and the
            document must not claim you can have 72.5 teeth.
  CASE      the case, dial and hands around it.

The layout is derived, never transcribed, and CHECKED at import: every centre
distance is m(z1+z2)/2 plus backlash, no wheel passes over an arbor except the
one whose pinion it drives, and no two wheels that overlap in plan share a
depth. The first version of the plan had an 84-tooth fourth wheel that swept
through the third arbor; that is the check that caught it.

Coordinates: world Z up, the watch lying dial-up, movement centre at the
origin, 3 o'clock = +X. Z = 0 is the mainplate's DIAL face. Below it is the
mainplate, and below that the "bridge side", where depths `d` are measured
down from the mainplate's bridge face: Z = -MAINPLATE_T - d.
"""
import math

# --- CLASS ------------------------------------------------------------------
CASE_D = 40.0              # case middle, outside
MOVEMENT_D = 26.0          # the calibre
VPH = 21600                # 3 Hz — an integer rate, not a dimension

# --- MOVEMENT: the going train ---------------------------------------------
MODULE = 0.10              # going train, winding train and motion works
# Cycloidal teeth (watch_gearing.py) run at the EXACT pitch centre distance —
# their action does not forgive a spread centre — so the backlash lives in the
# teeth: a pinion leaf takes less than half the pitch.
BACKLASH = 0.0
PRESSURE_DEG = 20.0        # the ratchet/crown-wheel sizes only; no involutes
TOOTH_FRAC_WHEEL = 0.47    # a wheel tooth's share of the pitch
TOOTH_FRAC_LEAF = 0.42     # a pinion leaf's (twelve teeth or fewer)
ADDENDUM_WHEEL = 1.30      # modules, or less where the ogive meets first
ADDENDUM_LEAF = 0.80
ROOT_CLEARANCE = 0.30      # modules between a root and the mate's tip
# The rotor's staff, which its pinion rides on. At 0.40 it was wider than the
# rotor pinion's ROOT (0.34), so it filled the gaps between the leaves and the
# reduction wheel's tips struck the staff, not the teeth — 0.00035 mm3 at rest.
ROTOR_STAFF_R = 0.25
MIN_CONTACT_RATIO = 1.1

Z_BARREL = 96
Z_CENTER, Z_CENTER_PIN = 80, 12
Z_THIRD, Z_THIRD_PIN = 75, 10
Z_FOURTH, Z_FOURTH_PIN = 72, 10       # 72, not 84: an 84-tooth wheel sweeps
Z_ESCAPE, Z_ESCAPE_PIN = 15, 6        # through the third arbor
# Motion works: both pairs share the same two axes, so their tooth SUMS must
# match (10+30 = 8+32 = 40). 12->36 / 10->40 gives 12:1 too and cannot exist.
Z_CANNON, Z_MINUTE_W, Z_MINUTE_P, Z_HOUR = 10, 30, 8, 32
# Winding: rotor pinion -> reduction wheel/pinion -> ratchet, and the crown
# wheel beside the ratchet.
Z_ROTOR_PIN, Z_REDUCTION_W, Z_REDUCTION_P, Z_RATCHET = 10, 40, 11, 48
# Keyless works. The crown wheel's arbor stands on the stem's line (y = 0) at
# the first point there that clears the barrel: 70 teeth put it 5.90 from the
# barrel's axis against the barrel's 4.93 tip. Below, at plate level, it
# carries the winding wheel the stem drives; the setting wheel meshes the
# minute wheel. Both stem pinions drive them through CONTRATE teeth — rack
# teeth on an end face, at right angles to the wheel.
Z_CROWN_WHEEL = 70
Z_WINDING_W, Z_SETTING_W = 16, 16
Z_CONTRATE = 10            # contrate teeth on the winding pinion and clutch

# Where the small seconds is: the fourth arbor, straight down at 6 o'clock.
FOURTH_Y = -7.5
ESCAPE_DIR_DEG = 35.0      # escape wheel's direction from the fourth arbor
BARREL_DIR_DEG = 50.0      # barrel's direction from the centre
BALANCE_X, BALANCE_Y = 7.6, -4.6
PALLET_FROM_ESCAPE = 3.3   # pallet arbor, along the escape->balance line;
                           # 1.35x the escape radius leaves room for the arms

# Thicknesses and depths (d below the mainplate's bridge face)
MAINPLATE_T = 1.2
BRIDGE_T = 1.2
BRIDGE_D = 3.6             # bridges' underside
WHEEL_T = 0.25
ARBOR_R = 0.15             # pivot/shaft radius — what a wheel must not cross
PINION_R_MARGIN = 0.05     # how far a pinion's leaves stand past its level

BARREL_D0, BARREL_D1 = 0.30, 2.30
CENTER_WHEEL_D = 2.70
THIRD_WHEEL_D = 1.40
FOURTH_WHEEL_D = 2.00
ESCAPE_WHEEL_D = 1.00
ESCAPE_T = 0.20
ESCAPE_OUTER_R = 2.45
BALANCE_D0, BALANCE_D1 = 2.60, 3.20
BALANCE_R = 4.00
# The pallet arbor is SHORT: it pivots between the mainplate and a pallet
# bridge that sits UNDER the balance, so the balance rim passes over its top.
# Running it the full depth like the train arbors put it straight through the
# rim — the layout check refused exactly that.
PALLET_BRIDGE_D = 2.10
PALLET_BRIDGE_T = 0.35
# Every bridge stands on PILLARS down to the mainplate, sited by pillars()
# where nothing turns between the plate and the bridges.
PILLAR_R = 0.45
PILLAR_CLEAR = 0.15        # from anything that turns
PALLET_REACH = 2.20        # the pallet fork's swing, round its arbor
RATCHET_D = 4.90           # on the barrel bridge (whose top is d 4.8)
RATCHET_T = 0.40
REDUCTION_BORE_CLEAR = 0.03  # the loose reduction wheel's bore over its staff
# The click: a pawl on the barrel bridge dropping into the ratchet's teeth.
# 60 degrees round the wheel is clear of both reverser pinions (125-150 and
# 210-235) and of the crown wheel (277-353).
CLICK_AT_DEG = 60          # an int: an angle, not a length to declare
CLICK_ARM_DEG = 26         # the pivot, that far round from the tip
CLICK_PIVOT_R = 3.40       # the pivot, from the barrel's axis
CLICK_DIP = 0.05           # how far the tip sits inside the tips, in a gap
CLICK_CLEAR = 0.05         # and how far it stands clear riding over a tooth
CLICK_TIP_W = 0.030        # half the tip's width: it lives in a 4 deg gap
REDUCTION_D = 5.35
ROTOR_D = 5.70             # rotor plate's underside
ROTOR_T = 0.50
ROTOR_WEIGHT_T = 1.00      # the gold segment is thicker
CASEBACK_D = 7.30          # the caseback window's inner face

# --- CASE, dial and hands (dial side: +Z) ----------------------------------
DIAL_D = 30.0
DIAL_Z = 1.10              # dial underside, above the motion works
DIAL_T = 0.40
MW_Z = 0.15                # motion works' lowest face
MINUTE_WHEEL_T = 0.20
SUBDIAL_R = 3.2            # small-seconds register
SUBDIAL_SINK = 0.15
HOUR_HAND_Z = 1.75
MINUTE_HAND_Z = 2.05
SECONDS_HAND_Z = 1.60      # under the hour hand, which starts at 1.75
HAND_T = 0.08
CRYSTAL_Z = 2.70           # crystal underside
CRYSTAL_T = 1.50
CASE_Z0, CASE_Z1 = -9.0, 2.70
BEZEL_W = 2.2
LUG_W = 20.0               # between the lugs (strap width)
LUG_T = 2.4                # each lug's thickness
LUG_ROOT = 0.8             # how far a lug's root runs into the wall
CROWN_D = 5.0
CROWN_L = 2.8

# --- KEYLESS WORKS (dial side, in a slot and pockets in the mainplate) ------
# The stem's height is DERIVED (STEM_Z): the clutch's contrate teeth must meet
# the setting wheel, which runs at the minute wheel's level.
STEM_R = 0.30
STEM_SQUARE = 0.40         # the square the clutch slides on
STEM_PULL = 1.0            # crown pulled out: the setting position
CLUTCH_TRAVEL = 0.30       # how far the yoke moves the clutch inward
KEYLESS_R = 0.65           # clutch and winding pinion, outside
# A contrate ring meets its wheel at right angles, so only at ONE radius do
# the two move at the same speed: across a wheel t thick the ring's teeth run
# (t/2)/r_c fast or slow of it — the pitch surfaces of a right-angle pair are
# cones, and a flat wheel is not one. Real keyless works run on backlash over
# a narrow face; so do these: thin wheels (+-10%, not the +-20% a 0.20 wheel
# gave, which interfered), and contrate teeth thinner than half the pitch.
KEYLESS_WHEEL_T = 0.10     # setting and winding wheels
CONTRATE_W = 0.24          # contrate teeth's radial face width
RACK_FRAC = 0.25           # a contrate tooth's share of the pitch
DOG_H = 0.12               # clutch dogs' engagement, plus 0.02 end play
DOG_N = 4                  # an int: how many dogs, and the freewheel's teeth
WINDING_PIN_L = 0.50
MIN_CLUTCH_L = 0.40
STEM_GROOVE_X = 9.5        # where the setting lever's pin rides the stem
SETTING_LEVER_ARM = 3.5
YOKE_ARM = 2.8
LEVER_Z = 0.50             # levers, under the keyless bridge
LEVER_T = 0.12
KEYLESS_BRIDGE_Z = 0.80
KEYLESS_BRIDGE_T = 0.15


# --- DERIVED LAYOUT -----------------------------------------------------------
# Written once and evaluated twice, as the drone's spec does: over floats at
# import, and again over Params in declare(), so what can stay a formula does.
# Positions come out of trigonometry and a square root, which is not a
# formula the document is asked to hold; they fall back to plain numbers.

def _cd(module, za, zb, backlash):
    return module * (za + zb) / 2.0 + backlash


def _meet(p, r1, q, r2, side):
    """The point r1 from p and r2 from q, on `side` (+1 left / -1 right)."""
    dx, dy = q[0] - p[0], q[1] - p[1]
    d = math.hypot(dx, dy)
    if d > r1 + r2 or d < abs(r1 - r2):
        raise ValueError(f"no point is {r1} from one arbor and {r2} from the "
                         f"other when they are {d:.3f} apart")
    a = (r1 * r1 - r2 * r2 + d * d) / (2 * d)
    h = math.sqrt(r1 * r1 - a * a)
    return (float(p[0] + a * dx / d - side * h * dy / d),
            float(p[1] + a * dy / d + side * h * dx / d))


def _derive(g):
    m, bl = float(g["MODULE"]), float(g["BACKLASH"])
    g["CD_BARREL_CENTER"] = _cd(m, Z_BARREL, Z_CENTER_PIN, bl)
    g["CD_CENTER_THIRD"] = _cd(m, Z_CENTER, Z_THIRD_PIN, bl)
    g["CD_THIRD_FOURTH"] = _cd(m, Z_THIRD, Z_FOURTH_PIN, bl)
    g["CD_FOURTH_ESCAPE"] = _cd(m, Z_FOURTH, Z_ESCAPE_PIN, bl)
    # Both motion-works pairs share two axes: equal tooth SUMS (40) put them
    # at the same pitch distance, which they must be.
    g["CD_MOTION"] = _cd(m, Z_MINUTE_W, Z_CANNON, bl)
    g["CD_ROTOR_REDUCTION"] = _cd(m, Z_REDUCTION_W, Z_ROTOR_PIN, bl)
    g["CD_REDUCTION_RATCHET"] = _cd(m, Z_RATCHET, Z_REDUCTION_P, bl)
    g["CD_RATCHET_CROWN"] = _cd(m, Z_RATCHET, Z_CROWN_WHEEL, bl)
    g["CD_SETTING"] = _cd(m, Z_MINUTE_W, Z_SETTING_W, bl)
    # The stem's axis sits one contrate pitch radius below the setting
    # wheel's mid-plane, which is the minute wheel's.
    g["SETTING_MID_Z"] = g["MW_Z"] + g["MINUTE_WHEEL_T"] / 2
    g["STEM_Z"] = g["SETTING_MID_Z"] - m * Z_CONTRATE / 2

    P = {"center": (0.0, 0.0), "fourth": (0.0, float(g["FOURTH_Y"]))}
    P["third"] = _meet(P["center"], float(g["CD_CENTER_THIRD"]),
                       P["fourth"], float(g["CD_THIRD_FOURTH"]), -1)
    a = math.radians(float(g["ESCAPE_DIR_DEG"]))
    r = float(g["CD_FOURTH_ESCAPE"])
    P["escape"] = (r * math.sin(a), float(g["FOURTH_Y"]) + r * math.cos(a))
    b = math.radians(float(g["BARREL_DIR_DEG"]))
    r = float(g["CD_BARREL_CENTER"])
    P["barrel"] = (r * math.cos(b), r * math.sin(b))
    P["balance"] = (float(g["BALANCE_X"]), float(g["BALANCE_Y"]))
    ex, ey = P["escape"]
    bx, by = P["balance"]
    L = math.hypot(bx - ex, by - ey)
    t = float(g["PALLET_FROM_ESCAPE"]) / L
    P["pallet"] = (ex + (bx - ex) * t, ey + (by - ey) * t)
    P["minute"] = (float(g["CD_MOTION"]), 0.0)          # motion works at 3
    P["reduction"] = _meet(P["center"], float(g["CD_ROTOR_REDUCTION"]),
                           P["barrel"], float(g["CD_REDUCTION_RATCHET"]), +1)
    # The second reverser wheel: a wheel's pitch distance from the first
    # (wheel to wheel, so it turns the other way) and its pinion's from the
    # ratchet, which both pinions drive (generators/watch/README.md). The other
    # intersection is 3.0 from the crown wheel, which needs 4.26.
    P["reverser"] = _meet(P["reduction"], m * Z_REDUCTION_W + bl,
                          P["barrel"], float(g["CD_REDUCTION_RATCHET"]), +1)
    a = math.radians(CLICK_AT_DEG + CLICK_ARM_DEG)
    P["click"] = (P["barrel"][0] + float(g["CLICK_PIVOT_R"]) * math.cos(a),
                  P["barrel"][1] + float(g["CLICK_PIVOT_R"]) * math.sin(a))
    # The crown wheel on the stem's line, its pitch distance from the ratchet.
    r, (bx, by) = float(g["CD_RATCHET_CROWN"]), P["barrel"]
    P["crown_wheel"] = (bx + math.sqrt(r * r - by * by), 0.0)
    P["setting"] = (P["minute"][0] + float(g["CD_SETTING"]), 0.0)
    g["POS"] = P

    # Depth (Z) of each level, from the stack above.
    zb = lambda d: -float(g["MAINPLATE_T"]) - float(d)   # noqa: E731
    g["Z_OF"] = zb
    return g


import watch_gearing as _G  # noqa: E402 — the tooth forms

# Every tooth set, and what it meshes. A wheel and its pinion on one arbor are
# two tooth sets; only the ratchet meshes two mates with the same teeth.
TOOTH_SETS = {
    "barrel": Z_BARREL, "center_pin": Z_CENTER_PIN, "center": Z_CENTER,
    "third_pin": Z_THIRD_PIN, "third": Z_THIRD, "fourth_pin": Z_FOURTH_PIN,
    "fourth": Z_FOURTH, "escape_pin": Z_ESCAPE_PIN, "cannon": Z_CANNON,
    "minute_w": Z_MINUTE_W, "minute_p": Z_MINUTE_P, "hour": Z_HOUR,
    "rotor_pin": Z_ROTOR_PIN, "reduction_w": Z_REDUCTION_W,
    "reduction_p": Z_REDUCTION_P, "ratchet": Z_RATCHET,
    "crown_wheel": Z_CROWN_WHEEL, "winding_w": Z_WINDING_W,
    "setting_w": Z_SETTING_W,
    # Contrate teeth: racks, their count on the face of a stem pinion.
    "wp_rack": Z_CONTRATE, "clutch_rack": Z_CONTRATE}
RACKS = {"wp_rack", "clutch_rack"}
MESH_PAIRS = [("barrel", "center_pin"), ("center", "third_pin"),
              ("third", "fourth_pin"), ("fourth", "escape_pin"),
              ("minute_w", "cannon"), ("hour", "minute_p"),
              ("reduction_w", "rotor_pin"),
              # The two reverser wheels mesh each OTHER: one set meeting
              # another of its own kind, which no other pair here does. Its
              # addendum and dedendum come out on one rolling circle, which
              # is what wheel-to-wheel conjugacy wants.
              ("reduction_w", "reduction_w"), ("ratchet", "reduction_p"),
              ("ratchet", "crown_wheel"), ("minute_w", "setting_w"),
              ("setting_w", "clutch_rack"), ("winding_w", "wp_rack")]


def gearing():
    """Each tooth set's cycloidal design.

    Conjugacy wants each gear's addendum traced by the SAME rolling circle as
    its mate's dedendum. Every dedendum defaults to half its own pitch radius
    — a radial flank — and every addendum takes its mate's. The ratchet meshes
    two mates with one set of teeth, so its addendum can match only one of
    their dedenda: it takes the reduction pinion's, and the crown wheel's
    dedendum is traced by the same circle instead of its own (a hypocycloidal,
    not radial, flank), which keeps both pairs conjugate. The minute wheel and
    the setting wheel are two more of those: the setting wheel's flank takes
    the cannon pinion's circle, and the clutch's rack flank the minute
    wheel's.

    A rack (contrate teeth) has no pitch radius to halve; its own flank circle
    defaults to half its wheel's pitch radius, and its `rp` is the radius of
    the contrate ring on the pinion's face.
    """
    m = float(MODULE)
    mates = {k: [] for k in TOOTH_SETS}
    for a, b in MESH_PAIRS:
        mates[a].append(b)
        mates[b].append(a)
    r = {k: m * z / 2 for k, z in TOOTH_SETS.items()}
    rho_ded = {k: (r[mates[k][0]] if k in RACKS else r[k]) / 2
               for k in TOOTH_SETS}
    rho_add = {}
    for k in TOOTH_SETS:                       # multi-mate gears choose first
        if len(mates[k]) > 1:
            rho_add[k] = rho_ded[mates[k][0]]
            for other in mates[k][1:]:
                rho_ded[other] = rho_add[k]
    for k in TOOTH_SETS:
        rho_add.setdefault(k, rho_ded[mates[k][0]])
    G = {}
    for k, z in TOOTH_SETS.items():
        if k in RACKS:
            frac, cap = float(RACK_FRAC), float(ADDENDUM_LEAF)
            G[k] = dict(z=z, rho_add=rho_add[k], rho_ded=rho_ded[k],
                        frac=frac, rack=True,
                        h_a=_G.rack_addendum_height(m, rho_add[k], frac, cap))
            continue
        leaf = z <= 12
        frac = float(TOOTH_FRAC_LEAF if leaf else TOOTH_FRAC_WHEEL)
        cap = float(ADDENDUM_LEAF if leaf else ADDENDUM_WHEEL)
        G[k] = dict(z=z, rho_add=rho_add[k], rho_ded=rho_ded[k], frac=frac,
                    rack=False,
                    h_a=_G.addendum_height(m, z, rho_add[k], frac, cap))
    for k in TOOTH_SETS:
        G[k]["h_f"] = max(G[o]["h_a"] for o in mates[k]) \
            + float(ROOT_CLEARANCE) * m
        G[k]["rp"] = r[k]
        G[k]["mates"] = mates[k]
    return G


GEAR = gearing()

# What each tooth set is built AROUND at its own level — a staff, or nothing
# wider than its bore. The mate's tips reach toward it, so it has to stay
# inside the root; a core wider than the root fills the gaps between the
# leaves. The first draft had the cannon pinion's tube and the rotor's staff
# both at 0.40 around 0.34 roots, and the only two meshes that failed at rest
# were exactly those two.
CORE_R = {"rotor_pin": ROTOR_STAFF_R, "cannon": 0.17}


def tip_r(key):
    return GEAR[key]["rp"] + GEAR[key]["h_a"]


def root_of(key):
    return GEAR[key]["rp"] - GEAR[key]["h_f"]


def check_meshes():
    """Refuse a mesh that cannot run. At import, like the layout check.

    Continuity: the arc of action — recess on one gear's addendum plus
    approach on the other's, each the pitch arc its rolling circle has rolled
    when the contact reaches that tip — over the circular pitch must stay
    above MIN_CONTACT_RATIO, or one tooth pair leaves before the next arrives.
    Clearance: no tip may reach its mate's staff, which runs through every
    pinion's root.
    """
    m = float(MODULE)
    bad, ratios = [], {}
    for a, b in MESH_PAIRS:
        arc = 0.0
        for k in (a, b):
            g = GEAR[k]
            if g["rack"]:
                arc += _G.rack_arc(g["rho_add"], g["h_a"])
                continue
            t_a = _G._solve(lambda t: math.hypot(*_G._epi(g["rp"], g["rho_add"],
                                                          t)),
                            g["rp"] + g["h_a"], 1e-9,
                            math.pi * g["rho_add"] / g["rp"] * 0.999)
            arc += g["rp"] * t_a
        eps = arc / (math.pi * m)
        ratios[f"{a}/{b}"] = eps
        if eps < float(MIN_CONTACT_RATIO):
            bad.append(f"{a}/{b}: contact ratio {eps:.2f} < "
                       f"{float(MIN_CONTACT_RATIO)}")
        for k, o in ((a, b), (b, a)):
            if GEAR[o]["rack"]:
                continue        # a rack's root is its face; h_f clears it
            reach = GEAR[o]["rp"] - GEAR[k]["h_a"]
            core = CORE_R.get(o, ARBOR_R)
            if reach < core + 0.01:
                bad.append(f"{k}'s tip reaches {reach:.3f} from the {o} "
                           f"axis, onto the core it rides on (r {core})")
    if bad:
        raise ValueError("a mesh cannot run:\n  " + "\n  ".join(bad))
    return ratios


def click_tip():
    """Where the pawl meets the wheel: its tip's centre, CLICK_DIP inside the
    ratchet's tips, in a gap between two teeth."""
    a = math.radians(CLICK_AT_DEG)
    r = tip_r("ratchet") - float(CLICK_DIP)
    return (POS["barrel"][0] + r * math.cos(a),
            POS["barrel"][1] + r * math.sin(a))


def click_lift_deg():
    """How far the pawl turns to ride over a tooth — derived from where its
    pivot stands, not chosen: the turn that lifts its tip CLICK_DIP +
    CLICK_CLEAR, clear of the tips."""
    piv, tip, B = POS["click"], click_tip(), POS["barrel"]
    want = float(CLICK_DIP) + float(CLICK_CLEAR)

    def out(deg):
        t = math.radians(deg)
        x, y = tip[0] - piv[0], tip[1] - piv[1]
        p = (piv[0] + x * math.cos(t) - y * math.sin(t),
             piv[1] + x * math.sin(t) + y * math.cos(t))
        return math.dist(p, B) - math.dist(tip, B)

    lo, hi = 0.0, 20.0
    for _ in range(60):                      # the tip rises monotonically
        mid = (lo + hi) / 2
        lo, hi = (mid, hi) if out(mid) < want else (lo, mid)
    return (lo + hi) / 2


def _ratchet_material():
    """The ratchet's own outline as (radius, half-angle of material about a
    tooth's centre), root to tip — read from the curves the teeth are drawn
    from, so the pawl is fitted to the wheel it sits on and not to a guess."""
    g = GEAR["ratchet"]
    r = float(MODULE) * Z_RATCHET / 2.0
    beta = g["frac"] * math.pi / Z_RATCHET
    out = []
    ded, add = _G.flank_points(r, g["rho_add"], g["rho_ded"], g["h_a"],
                               g["h_f"], samples=17)
    for x, y in ded + add:
        rr = math.hypot(x, y)
        out.append((rr, max(0.0, beta - math.atan2(y, x))))
    out.sort()
    return out


def click_outline():
    """The pawl's plan outline, in its own frame with the pivot at the
    origin: a beak from a wide root to a point at click_tip(), and a tail
    behind the pivot where a real one takes its spring. Here rather than in
    watch_parts because click_table fits the table to THIS shape — the beak's
    sloping sides come 0.011 nearer the wheel than its point does, which the
    exact sweep caught."""
    piv, tip = POS["click"], click_tip()
    t = (tip[0] - piv[0], tip[1] - piv[1])
    L = math.hypot(*t)
    u = (t[0] / L, t[1] / L)
    v = (-u[1], u[0])
    w, tw = 0.22, float(CLICK_TIP_W)
    return [(v[0] * w, v[1] * w),
            (t[0] + u[0] * tw, t[1] + u[1] * tw),
            (-v[0] * w, -v[1] * w),
            (-u[0] * 0.55 - v[0] * 0.10, -u[1] * 0.55 - v[1] * 0.10),
            (-u[0] * 0.55 + v[0] * 0.10, -u[1] * 0.55 + v[1] * 0.10)]


def click_radius(deg):
    """How near the pawl comes to the barrel's axis, turned that far from
    where it is drawn — the whole outline, not just its point."""
    piv, B = POS["click"], POS["barrel"]
    t = math.radians(deg)
    pts = [(piv[0] + x * math.cos(t) - y * math.sin(t),
            piv[1] + x * math.sin(t) + y * math.cos(t))
           for x, y in click_outline()]
    best = float("inf")
    for i, p in enumerate(pts):
        q = pts[(i + 1) % len(pts)]
        dx, dy = q[0] - p[0], q[1] - p[1]
        f = max(0.0, min(1.0, ((B[0] - p[0]) * dx + (B[1] - p[1]) * dy)
                         / (dx * dx + dy * dy)))
        best = min(best, math.hypot(B[0] - p[0] - f * dx,
                                    B[1] - p[1] - f * dy))
    return best


def _ratchet_reach(bins=721):
    """How far the ratchet's material reaches at each angle off a tooth's
    centre: the tooth itself out to its tip, the root circle between them."""
    g = GEAR["ratchet"]
    r = float(MODULE) * Z_RATCHET / 2.0
    rf = r - g["h_f"]
    pitch = 2 * math.pi / Z_RATCHET
    reach = [rf] * bins
    for rr, half in _ratchet_material():
        i = int(round(half / (pitch / 2) * (bins - 1)))
        for k in range(0, min(i, bins - 1) + 1):
            reach[k] = max(reach[k], rr)
    return reach


def click_table(per_tooth=48, clear=0.09, outline_points=96):
    """The pawl's angle against the barrel arbor's, fitted to the ratchet's
    own flank AND to the pawl's own outline: at every angle of the wheel it
    sits as deep as it can with every point of its beak clear of the teeth,
    and zero is where it is DRAWN, dropped in.

    Tooth 0 points along +X (watch_gearing.profile), so a tooth's centre is
    at the arbor's angle plus 2*pi*k/z.
    """
    pitch = 2 * math.pi / Z_RATCHET
    reach = _ratchet_reach()
    bins = len(reach)
    B = POS["barrel"]
    piv = POS["click"]

    def material(off):
        i = int(round(abs(off) / (pitch / 2) * (bins - 1)))
        return reach[min(i, bins - 1)]

    # The pawl's outline, walked finely, in its own frame.
    pts = []
    out = click_outline()
    for i, a in enumerate(out):
        b = out[(i + 1) % len(out)]
        for k in range(outline_points // len(out) + 1):
            f = k / (outline_points // len(out) + 1)
            pts.append((a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f))

    def clears(deg, v):
        """Every point of the pawl, at that angle, clear of the wheel at v."""
        t = math.radians(deg)
        c, si = math.cos(t), math.sin(t)
        for x, y in pts:
            px = piv[0] + x * c - y * si - B[0]
            py = piv[1] + x * si + y * c - B[1]
            rr = math.hypot(px, py)
            if rr > float(MODULE) * Z_RATCHET / 2 + GEAR["ratchet"]["h_a"] + 0.3:
                continue
            off = (math.atan2(py, px) - v + pitch / 2) % pitch - pitch / 2
            if rr < material(off) + clear:
                return False
        return True

    need = []
    for i in range(Z_RATCHET * per_tooth):
        v = 2 * math.pi * i / (Z_RATCHET * per_tooth)
        lo, hi = 0.0, 12.0                   # 0 is as drawn, dropped in
        if clears(lo, v):
            need.append(0.0)
            continue
        for _ in range(24):                  # the pawl rises monotonically
            mid = (lo + hi) / 2
            lo, hi = (mid, hi) if not clears(mid, v) else (lo, mid)
        need.append(hi)
    # Each point carries its NEIGHBOURS' requirement too. The table is read
    # through a spline, and between two points it runs between their values:
    # without this the pawl sat 0.07 degrees up where that angle of the wheel
    # wanted more, and took 0.0003 mm3 out of a tooth.
    n = len(need)
    return [[360.0 * i / n, max(need[i - 1], need[i], need[(i + 1) % n])]
            for i in range(n)]


def check_layout(P):
    """Refuse a layout that cannot run. Raised at import, so no document is
    ever built from one."""
    wheels = [  # name, arbor, tip radius, depth span, the arbor it drives
        ("barrel", "barrel", tip_r("barrel"), (BARREL_D0, BARREL_D1), "center"),
        ("centre wheel", "center", tip_r("center"),
         (CENTER_WHEEL_D, CENTER_WHEEL_D + WHEEL_T), "third"),
        ("third wheel", "third", tip_r("third"),
         (THIRD_WHEEL_D, THIRD_WHEEL_D + WHEEL_T), "fourth"),
        ("fourth wheel", "fourth", tip_r("fourth"),
         (FOURTH_WHEEL_D, FOURTH_WHEEL_D + WHEEL_T), "escape"),
        ("escape wheel", "escape", ESCAPE_OUTER_R,
         (ESCAPE_WHEEL_D, ESCAPE_WHEEL_D + ESCAPE_T), "pallet"),
        ("balance", "balance", BALANCE_R, (BALANCE_D0, BALANCE_D1), None),
        ("crown wheel", "crown_wheel", tip_r("crown_wheel"),
         (RATCHET_D, RATCHET_D + RATCHET_T), "barrel"),
    ]
    # Each arbor's depth span. A wheel conflicts with an arbor only where
    # their depths overlap — which is how a balance rim can pass over the top
    # of a pallet arbor and not through it.
    full = (0.0, BRIDGE_D)
    arbors = {"barrel": full, "center": full, "third": full, "fourth": full,
              "escape": full, "balance": full,
              "pallet": (0.0, PALLET_BRIDGE_D + PALLET_BRIDGE_T),
              # From the winding wheel in the plate to the crown wheel on
              # the barrel bridge: through the barrel's level and the
              # balance's, which is why it stands where it does.
              "crown_wheel": (0.0, RATCHET_D + RATCHET_T)}
    bad = []
    for name, arb, rt, span, drives in wheels:
        if math.hypot(*P[arb]) + rt > MOVEMENT_D / 2:
            bad.append(f"{name} leaves the {MOVEMENT_D} mm movement")
        for other, ospan in arbors.items():
            if other in (arb, drives):
                continue
            if not (span[0] < ospan[1] and ospan[0] < span[1]):
                continue
            d = math.dist(P[arb], P[other])
            if d < rt + ARBOR_R:
                bad.append(f"{name} (tip {rt:.2f}) sweeps over the {other} "
                           f"arbor, {d:.2f} away")
    for i, (n1, a1, r1, z1, _) in enumerate(wheels):
        for n2, a2, r2, z2, _ in wheels[i + 1:]:
            if (math.dist(P[a1], P[a2]) < r1 + r2
                    and z1[0] < z2[1] and z2[0] < z1[1]):
                bad.append(f"{n1} and {n2} overlap in plan AND in depth")
    if bad:
        raise ValueError("the movement cannot run:\n  " + "\n  ".join(bad))


_DERIVED_NAMES = tuple(
    _derive({k: v for k, v in globals().items() if k.isupper()}).keys()
    - set(k for k in list(globals()) if k.isupper()))
_derive(globals())
check_layout(POS)  # noqa: F821 — written by _derive
CONTACT_RATIOS = check_meshes()


def check_case():
    """A lug's root runs LUG_ROOT into the wall at its outer face, where the
    wall is nearest the axis; the root's innermost corner must still be in
    solid wall, not in the dial or movement seat."""
    R = CASE_D / 2
    x_in, x_out = LUG_W / 2, LUG_W / 2 + LUG_T
    if x_out >= R - LUG_ROOT:
        raise ValueError(f"lugs {LUG_W} + 2x{LUG_T} apart leave the "
                         f"{CASE_D} case: nothing to root in")
    root = math.sqrt(R * R - x_out * x_out) - LUG_ROOT
    seat = max(DIAL_D / 2 + 0.30, MOVEMENT_D / 2 + 0.05)
    if math.hypot(x_in, root) <= seat + 0.3:
        raise ValueError(f"a lug's root reaches r {math.hypot(x_in, root):.2f}"
                         f", into the {seat:.2f} seat")


check_case()


def keyless():
    """Where each keyless part stands along the stem (x), derived from the
    two fixed ends: the setting wheel meshing the minute wheel, and the
    winding wheel on the crown wheel's arbor. The clutch takes the slack
    between them, and is refused if that leaves it too short.

    A contrate ring's PITCH line stands h_f off the face it is cut on, so a
    face meets its wheel at the wheel's pitch radius plus that."""
    K = {}
    wp, cr = GEAR["wp_rack"], GEAR["clutch_rack"]
    x_c, x_s = POS["crown_wheel"][0], POS["setting"][0]
    K["wp_face"] = x_c - GEAR["winding_w"]["rp"] - wp["h_f"]  # contrate face
    K["wp_inner"] = K["wp_face"] - WINDING_PIN_L                # dog face
    K["clutch_outer"] = K["wp_inner"] - DOG_H - 0.02  # dogs overlap DOG_H-.02
    K["clutch_inner_set"] = x_s + GEAR["setting_w"]["rp"] + cr["h_f"]
    K["clutch_inner"] = K["clutch_inner_set"] + CLUTCH_TRAVEL   # winding
    K["clutch_l"] = K["clutch_outer"] - K["clutch_inner"]
    # The stem's inner end stays inside the clutch when pulled out, and its
    # square covers the clutch in both positions.
    K["stem_end"] = K["clutch_inner_set"] - STEM_PULL - 0.02
    K["square_to"] = K["clutch_outer"] + 0.05
    K["wp_tip"] = K["wp_face"] + wp["h_f"] + wp["h_a"]
    bad = []
    if K["clutch_l"] < MIN_CLUTCH_L:
        bad.append(f"the clutch is {K['clutch_l']:.3f} long between the "
                   f"setting wheel and the winding pinion (< {MIN_CLUTCH_L})")
    # Winding position: the clutch's contrate tips stand off the setting
    # wheel's tips by the travel less both addenda.
    off = CLUTCH_TRAVEL - cr["h_a"] - GEAR["setting_w"]["h_a"]
    if off < 0.04:
        bad.append(f"crown in, the clutch is only {off:.3f} off the setting "
                   f"wheel")
    # Crown out, its dogs clear the winding pinion's.
    if CLUTCH_TRAVEL - DOG_H < 0.10:
        bad.append("crown out, the clutch's dogs do not clear the winding "
                   "pinion's")
    # The stem clears the wheels either side of it: the setting wheel above,
    # the winding wheel below.
    above = SETTING_MID_Z - KEYLESS_WHEEL_T / 2 - (STEM_Z + STEM_R)
    below = (STEM_Z - STEM_R) - (STEM_Z - wp["rp"] + KEYLESS_WHEEL_T / 2)
    for what, gap in (("setting wheel", above), ("winding wheel", below)):
        if gap < 0.05:
            bad.append(f"the stem passes the {what} {gap:.3f} away")
    if x_c + tip_r("crown_wheel") > MOVEMENT_D / 2 - 0.3:
        bad.append("the crown wheel leaves the movement")
    if bad:
        raise ValueError("the keyless works cannot run:\n  "
                         + "\n  ".join(bad))
    return K


KEYLESS = keyless()


def pillars():
    """Where each bridge stands on the mainplate. A pillar runs from the
    plate to the bridge, through the whole level every wheel turns in, so it
    must clear every wheel, the balance, the pallet fork's swing and every
    arbor there — whatever their depth. The candidates are the free points
    near the rim; each bridge takes the nearest to itself (the balance cock
    one, beside the balance; the train and barrel bridges two, spread apart),
    and no two bridges' pillars crowd each other. The first model had none:
    its bridges floated 3.6 mm over the plate on a mate."""
    P = POS
    sweep = [(P["barrel"], tip_r("barrel")), (P["center"], tip_r("center")),
             (P["third"], tip_r("third")), (P["fourth"], tip_r("fourth")),
             (P["escape"], ESCAPE_OUTER_R), (P["balance"], BALANCE_R),
             (P["pallet"], PALLET_REACH), (P["crown_wheel"], ARBOR_R)]
    need = PILLAR_R + PILLAR_CLEAR
    rim = MOVEMENT_D / 2 - PILLAR_R - 0.4
    free = []
    for i in range(360):
        a = math.radians(i)
        for j in range(int((rim - 6.0) / 0.1) + 1):
            c = ((6.0 + 0.1 * j) * math.cos(a), (6.0 + 0.1 * j) * math.sin(a))
            if all(math.dist(c, s) - r >= need for s, r in sweep):
                free.append(c)
    train = tuple(sum(P[k][i] for k in ("center", "third", "fourth", "escape"))
                  / 4 for i in (0, 1))
    taken, out = [], {}

    def pick(near, n, spread, rim_only=0.0):
        got = []
        for _ in range(n):
            ok = [c for c in free if math.hypot(*c) >= rim_only
                  and all(math.dist(c, t) >= 2.5 for t in taken)
                  and all(math.dist(c, g) >= spread for g in got)]
            if not ok:
                return None
            best = min(ok, key=lambda c: math.dist(c, near))
            got.append(best)
            taken.append(best)
        return got
    out["cock"] = pick(P["balance"], 1, 0.0, rim_only=10.0)
    out["train"] = pick(train, 2, 5.5)
    out["barrel"] = pick(P["barrel"], 2, 5.5)
    missing = [k for k, v in out.items() if not v]
    if missing:
        raise ValueError(f"no room for a pillar under: {', '.join(missing)}")
    return out


PILLARS = pillars()


def _is_dimension(name, value):
    """A dimension: named, a float, not derived. Tooth counts are ints and
    VPH is an int — neither is a length, and both stay out of the document."""
    return (name.isupper() and not name.startswith("_")
            and isinstance(value, float) and name not in _DERIVED_NAMES)


def declare(app):
    """Declare every dimension here as a project variable and return the
    namespace rebound to Params (docs/scripting/sdk.md#parameters-carry-their-expression)."""
    base = {k: v for k, v in globals().items() if _is_dimension(k, v)}
    declared = app.params({k.lower(): v for k, v in sorted(base.items())},
                          comment="watch_spec")
    ns = {k: declared[k.lower()] for k in base}
    _derive(ns)
    return ns


# --- the escapement's action -------------------------------------------------
# Two cams from the escape wheel animate it (generators/watch/README.md).
# One full oscillation of the balance lets one tooth pass, so it swings once
# per tooth pitch: the escape wheel turns 12 times a minute, 15 teeth, 3 Hz —
# 21,600 beats an hour. The fork points at the impulse pin while the pin is in
# its notch and rests at its banking angle once it has left.
IMPULSE_PIN_R = 0.30       # the pin's circle about the balance staff. At 0.55
                           # the fork swung 13.5 deg either way and its
                           # stones ran 0.37 mm into the wheel
IMPULSE_PIN_W = 0.06       # the pin's own radius
ROLLER_R = 0.45
HORN_TIP = 0.03            # the fork's horns run this far past the pin at rest
PIN_CLEAR = 0.02           # the pin is out of the notch this far past the tips
STONE_CLEAR = 0.05         # a stone from the tip circle at its deepest
PALLET_FOOT_R = 1.2        # the pallet bridge's feet, from the pallet arbor
PALLET_FOOT_DEG = 70       # an int: either side of the fork's line
BALANCE_AMPLITUDE_DEG = 270  # an int: an angle, not a length to declare


def fork_follow_deg(theta_deg):
    """The fork's angle while the impulse pin is in its notch: pointing at
    the pin. The pallet's frame has +X at the balance, the balance's +X at
    the pallet, and both turn positive about +Z, so the pin at balance angle
    t is at (L - r cos t, -r sin t) in the pallet's frame."""
    L = math.dist(POS["pallet"], POS["balance"])
    r = float(IMPULSE_PIN_R)
    t = math.radians(theta_deg)
    return math.degrees(math.atan2(-r * math.sin(t), L - r * math.cos(t)))


def fork_exit_deg():
    """The balance angle where the pin has left the notch: its centre past
    the horn tips by its own radius and PIN_CLEAR."""
    L = math.dist(POS["pallet"], POS["balance"])
    r = float(IMPULSE_PIN_R)
    d = (L - r) + float(HORN_TIP) + float(IMPULSE_PIN_W) + float(PIN_CLEAR)
    return math.degrees(math.acos((L * L + r * r - d * d) / (2 * L * r)))


def fork_bank_deg():
    """How far the fork swings either way: where the pin leaves it."""
    return abs(fork_follow_deg(fork_exit_deg()))


def fork_deg(theta_deg):
    x = fork_exit_deg()
    return fork_follow_deg(max(-x, min(x, theta_deg)))


def balance_deg(escape_deg):
    """Harmonic in time, as a balance is: one period per tooth pitch."""
    return BALANCE_AMPLITUDE_DEG * math.sin(
        2 * math.pi * escape_deg / (360.0 / Z_ESCAPE))


def escapement_tables():
    """The balance's and the fork's lift tables against the escape wheel's
    angle, degrees in both columns. A degree apart through the swing, and
    twenty-one points across each passage of the pin through the notch —
    where the fork turns 13 degrees in about 1.5 degrees of the wheel, and
    the spline must keep the pin inside the notch's 0.04 mm of play."""
    P = 360.0 / Z_ESCAPE
    x = fork_exit_deg() + 5.0
    h = math.degrees(math.asin(min(1.0, x / BALANCE_AMPLITUDE_DEG))) / 360 * P
    es = set()
    for k in range(Z_ESCAPE):
        e0 = k * P
        for j in range(24):
            es.add(round(e0 + j * P / 24, 6))
        for c in (e0, e0 + P / 2):
            for j in range(-10, 11):
                es.add(round((c + h * j / 10) % 360.0, 6))
    es = sorted(es)
    return ([[e, balance_deg(e)] for e in es],
            [[e, fork_deg(balance_deg(e))] for e in es])
