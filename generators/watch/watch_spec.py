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
Z_CROWN_WHEEL, Z_WINDING_PIN = 24, 8  # 8: a 12-leaf pinion reaches the rotor

# Where the small seconds is: the fourth arbor, straight down at 6 o'clock.
FOURTH_Y = -7.5
ESCAPE_DIR_DEG = 35.0      # escape wheel's direction from the fourth arbor
BARREL_DIR_DEG = 50.0      # barrel's direction from the centre
BALANCE_X, BALANCE_Y = 7.6, -4.6
PALLET_FROM_ESCAPE = 3.3   # pallet arbor, along the escape->balance line;
                           # 1.35x the escape radius leaves room for the arms
CROWN_WHEEL_DIR_DEG = -60.0  # crown wheel's direction from the ratchet

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
RATCHET_D = 4.90           # on the barrel bridge (whose top is d 4.8)
RATCHET_T = 0.40
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
CROWN_D = 5.5
CROWN_L = 2.8
STEM_Z = -3.0              # the crown's height on the case side
STEM_R = 0.35


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
    c = math.radians(float(g["CROWN_WHEEL_DIR_DEG"]))
    r = float(g["CD_RATCHET_CROWN"])
    P["crown_wheel"] = (P["barrel"][0] + r * math.cos(c),
                        P["barrel"][1] + r * math.sin(c))
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
    "crown_wheel": Z_CROWN_WHEEL}
MESH_PAIRS = [("barrel", "center_pin"), ("center", "third_pin"),
              ("third", "fourth_pin"), ("fourth", "escape_pin"),
              ("minute_w", "cannon"), ("hour", "minute_p"),
              ("reduction_w", "rotor_pin"), ("ratchet", "reduction_p"),
              ("ratchet", "crown_wheel")]


def gearing():
    """Each tooth set's cycloidal design.

    Conjugacy wants each gear's addendum traced by the SAME rolling circle as
    its mate's dedendum. Every dedendum defaults to half its own pitch radius
    — a radial flank — and every addendum takes its mate's. The ratchet meshes
    two mates with one set of teeth, so its addendum can match only one of
    their dedenda: it takes the reduction pinion's, and the crown wheel's
    dedendum is traced by the same circle instead of its own (a hypocycloidal,
    not radial, flank), which keeps both pairs conjugate.
    """
    m = float(MODULE)
    mates = {k: [] for k in TOOTH_SETS}
    for a, b in MESH_PAIRS:
        mates[a].append(b)
        mates[b].append(a)
    r = {k: m * z / 2 for k, z in TOOTH_SETS.items()}
    rho_ded = {k: r[k] / 2 for k in TOOTH_SETS}
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
        leaf = z <= 12
        frac = float(TOOTH_FRAC_LEAF if leaf else TOOTH_FRAC_WHEEL)
        cap = float(ADDENDUM_LEAF if leaf else ADDENDUM_WHEEL)
        G[k] = dict(z=z, rho_add=rho_add[k], rho_ded=rho_ded[k], frac=frac,
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
            reach = GEAR[o]["rp"] - GEAR[k]["h_a"]
            core = CORE_R.get(o, ARBOR_R)
            if reach < core + 0.01:
                bad.append(f"{k}'s tip reaches {reach:.3f} from the {o} "
                           f"axis, onto the core it rides on (r {core})")
    if bad:
        raise ValueError("a mesh cannot run:\n  " + "\n  ".join(bad))
    return ratios


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
    ]
    # Each arbor's depth span. A wheel conflicts with an arbor only where
    # their depths overlap — which is how a balance rim can pass over the top
    # of a pallet arbor and not through it.
    full = (0.0, BRIDGE_D)
    arbors = {"barrel": full, "center": full, "third": full, "fourth": full,
              "escape": full, "balance": full,
              "pallet": (0.0, PALLET_BRIDGE_D + PALLET_BRIDGE_T)}
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


def _is_dimension(name, value):
    """A dimension: named, a float, not derived. Tooth counts are ints and
    VPH is an int — neither is a length, and both stay out of the document."""
    return (name.isupper() and not name.startswith("_")
            and isinstance(value, float) and name not in _DERIVED_NAMES)


def declare(app):
    """Declare every dimension here as a project variable and return the
    namespace rebound to Params (docs/peer-survey-plan.md, W1)."""
    base = {k: v for k, v in globals().items() if _is_dimension(k, v)}
    declared = app.params({k.lower(): v for k, v in sorted(base.items())},
                          comment="watch_spec")
    ns = {k: declared[k.lower()] for k in base}
    _derive(ns)
    return ns
