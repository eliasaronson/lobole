# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The tea-ready alarm's every size and number, with no app.

World frame: the mug's axis is Z, up; the rim's top at z = 0; the frame
rod stands inside the wall on -X, and the strip runs from it along +X to
the probe on the mug's axis. Every length in mm, temperature in degrees C,
force in N, mass in g unless named otherwise.

The strip is a straight KANTHAL 94S strip clamped at the frame and free at
the probe, its HIGH-expansion side UP: heated, it bends toward its
low-expansion side (down), so at the pour its tip is lowest and it rises as
the tea cools, lifting the probe (README.md#the-strip).

Every number below is cited where it is set, or derived from the inputs
(the strip and rod as ordered, the vessel, the trigger temperature, the
room). Nothing in millimetres is a literal.
"""
import math

# ---- cited numbers ----------------------------------------------------------

# KANTHAL 94S: Kanthal, Thermostatic Bimetal Handbook, catalogue 3-A-1-3,
# the KANTHAL 94S page (p. 40) and the type table (pp. 17-18). The app's
# library row `bimetal_kanthal_94s` carries the same; build.py checks them
# against describe.
K94S_A = 9.5e-6          # specific deflection a, 1/K (eq. [9], p. 11)
K94S_K = 17.8e-6         # specific curvature k, 1/K
K94S_K_TOL = 0.04        # +/- 4 % on k
K94S_E = 190000.0        # MPa at 20 C
K94S_LINEAR = (0.0, 200.0)
# The handbook's general yield of thermostatic bimetal, §2.1: 400-500 MPa.
# Its permissible stress per grade is a graph with no values in its text,
# so the lower bound of the general range is the bound this uses.
BIMETAL_YIELD_MPA = 400.0
# The strip is bought flat at room temperature; its constants are from
# 20 C (the handbook's reference). The set screw takes out any offset.
T_FLAT = 20.0
# Kanthal's minimum-volume rule (eq. [20]): the force may take at most
# half the free travel (T_F = T_A = 0.5).
MAX_FORCE_SHARE = 0.5

# 1.4301: SCI P413, Design Manual for Structural Stainless Steel, 4th ed.
# (2017): §2.3.1 (E, nu), Table 2.2 (f_y, cold-rolled strip t <= 8 mm),
# Table 2.7 (density, expansion, conductivity).
SS_E = 200000.0          # MPa
SS_NU = 0.3
SS_FY = 230.0            # MPa
SS_RHO = 7900.0          # kg/m^3
SS_K = 15.0              # W/(m K)

# The library's steel (a ball bearing's steel; the row's density).
STEEL_RHO = 7850.0
# Standard gravity, 9.80665 m/s^2 (3rd CGPM, 1901).
G = 9.80665
# ISO 261, M3 coarse: pitch 0.5 mm. The set screw is an ISO 4026 M3.
M3_PITCH = 0.5
M3_D = 3.0
# The design temperature: water boils at 100 C at sea level, the hottest
# tea there is (the user's decision).
T_POUR = 100.0
# Incropera, Fundamentals of Heat and Mass Transfer, Table 1.1: free
# convection in gases, 2-25 W/(m^2 K). No default: both ends are run.
FILM_W_M2K = (2.0, 25.0)
# The margin a printed part keeps under its service temperature (the
# requirement `temperature_margin at_least 10`).
MARGIN_K = 10.0
# The trigger band the user's inspiration targets around 54-55 C.
TRIGGER_RANGE = (40.0, 70.0)
# A band chosen to be heard (a report, not a pass).
HEARD_HZ = (500.0, 5000.0)
# The thinnest 1.4301 sheet the app's sheet-metal rules stock
# (src/core/SheetMetal.cpp: metric gauges down to "0.5mm"): the clip's.
CLIP_T = 0.5
CLIP_GAUGE = "0.5mm"
# The least inside bend radius those rules allow stainless: 1.5 t.
CLIP_BEND_R = 1.5 * CLIP_T
# The rod's former: the tube rules' least for a solid stainless rod, 2 x
# OD for D/t < 15 times 1.2 for stainless (src/core/tube/Rules.cpp).
ROD_FORMER = 2.0 * 1.2
# A clip leaf's root stress kept under 1.4301's yield with this to spare.
CLIP_SPARE = 1.5
# ISO 2306's tap drill for M3: 2.5 mm, into which the screw forms its
# thread in the printed arm. ISO 4762 M3: head 5.5 across, 3 high.
M3_TAP = 2.5
M3_HEAD_D, M3_HEAD_K = 5.5, 3.0
M3_LENGTHS = (5.0, 6.0, 8.0, 10.0, 12.0, 16.0, 20.0)
# Euler-Bernoulli: the first root of cos cosh = -1.
BETA1 = 1.8751040687


# ---- the strip (the handbook's equations) -------------------------------------

def tip_per_k(L, s):
    """Tip travel per kelvin, mm/K: eq. [9], A = a dT L^2 / s."""
    return K94S_A * L * L / s


def force_deflection(F, L, b, s):
    """The tip's give under a tip force F (N), mm: eq. [16], 4 F L^3 /
    (b s^3 E)."""
    return 4.0 * F * L ** 3 / (b * s ** 3 * K94S_E)


def k_per_n(L, b, s):
    """What a tip force costs in temperature, K per N: 4 L / (a E b s^2)."""
    return 4.0 * L / (K94S_A * K94S_E * b * s * s)


def strip_stress(F, L, b, s):
    """Bending stress at the clamp under a tip force, MPa: eq. [23]."""
    return 6.0 * F * L / (b * s * s)


def ball_mass_g(d):
    return STEEL_RHO * math.pi * d ** 3 / 6.0 * 1e-6


# ---- the closed forms the studies are held to ------------------------------

def timoshenko_curvature(t1, t2, e1, e2, a1, a2, dT):
    """1/mm: S. Timoshenko, J. Opt. Soc. Am. 11, 233-255 (1925); the
    Kanthal handbook's eq. [1]. Layer 1 expands by a1, layer 2 by a2."""
    m, n, h = t1 / t2, e1 / e2, t1 + t2
    return 6.0 * (a2 - a1) * dT * (1 + m) ** 2 / (
        h * (3.0 * (1 + m) ** 2 + (1 + m * n) * (m * m + 1.0 / (m * n))))


def fin_tip(T_base, T_inf, h, k, d_mm, L_mm):
    """A pin fin with an adiabatic tip (Incropera §3.6, case B): the tip's
    temperature. h W/(m^2 K), k W/(m K)."""
    m = math.sqrt(4.0 * h / (k * d_mm * 1e-3))
    return T_inf + (T_base - T_inf) / math.cosh(m * L_mm * 1e-3)


def fin_length_for(T_base, T_inf, h, k, d_mm, T_tip):
    """The dry length at which an adiabatic-tip pin fin's tip is T_tip."""
    m = math.sqrt(4.0 * h / (k * d_mm * 1e-3))
    return math.acosh((T_base - T_inf) / (T_tip - T_inf)) / m * 1e3


def euler_bernoulli_f1(E_mpa, rho, d_mm, L_mm):
    """A round cantilever's first frequency, Hz."""
    E = E_mpa * 1e6
    d, L = d_mm * 1e-3, L_mm * 1e-3
    I, A = math.pi * d ** 4 / 64.0, math.pi * d * d / 4.0
    return BETA1 ** 2 / (2 * math.pi) * math.sqrt(E * I / (rho * A * L ** 4))


def length_for_f1(E_mpa, rho, d_mm, f):
    E, d = E_mpa * 1e6, d_mm * 1e-3
    return BETA1 * math.sqrt(math.sqrt(E * d * d / (16.0 * rho)) /
                             (2 * math.pi * f)) * 1e3


def clip_grip(E, b, t, L, delta):
    """A leaf opened by delta at its tip: F = 3 E I delta / L^3 (N), and
    its root stress 3 E delta t / (2 L^2) (MPa)."""
    I = b * t ** 3 / 12.0
    return 3.0 * E * I * delta / L ** 3, 3.0 * E * delta * t / (2.0 * L * L)


# ---- the side cases ---------------------------------------------------------------

def arch(L, h, eps):
    """A circular arch of rise e between pins L apart, h deep, whose first
    harmonic is eps radii of gyration high (Timoshenko and Gere §7.6, as
    the [nonlinear] case holds it): its rise, radius and the uniform crown
    load (per unit length, N/mm) at its limit point, per unit EI k^4 r."""
    r = h / math.sqrt(12.0)
    e = eps * r * math.pi ** 3 / 32.0
    R = (L * L / 4.0 + e * e) / (2.0 * e)
    return e, R, r


def arch_limit_q(E, b, h, L, eps):
    """The uniform load on the crown (N/mm of span) at the limit point;
    None where 2 < eps < sqrt(22) does not hold."""
    if not 2.0 < eps < math.sqrt(22.0):
        return None
    r = h / math.sqrt(12.0)
    I = b * h ** 3 / 12.0
    k = math.pi / L
    xi = math.sqrt((eps * eps - 4.0) / 3.0)
    q1 = E * I * k ** 4 * r * ((eps - xi) + (eps * eps - xi * xi) * xi / 4.0)
    return math.pi / 4.0 * q1


def bilinear_section(b, h, E, fy, Et, N, M, n=400):
    """The section b x h under an axial force N (N, tension positive) and a
    moment M (N mm), its fibres elastic-plastic with a linear hardening
    slope Et past fy: the axial strain e0 and curvature kappa that carry
    them (Newton on the two equilibrium equations, the section in n
    strips). Returns (e0, kappa, the outer fibres' strains)."""
    ys = [-h / 2 + h * (i + 0.5) / n for i in range(n)]
    dA = b * h / n
    ey = fy / E

    def stress(e):
        if abs(e) <= ey:
            return E * e, E
        return math.copysign(fy + Et * (abs(e) - ey), e), Et

    e0, kap = N / (E * b * h), M / (E * b * h ** 3 / 12.0)
    for _ in range(200):
        fN = fM = 0.0
        a11 = a12 = a22 = 0.0
        for y in ys:
            s, t = stress(e0 + kap * y)
            fN += s * dA
            fM += s * y * dA
            a11 += t * dA
            a12 += t * y * dA
            a22 += t * y * y * dA
        rN, rM = fN - N, fM - M
        det = a11 * a22 - a12 * a12
        de0 = (rN * a22 - rM * a12) / det
        dk = (a11 * rM - a12 * rN) / det
        e0 -= de0
        kap -= dk
        if abs(de0) < 1e-14 and abs(dk) < 1e-14:
            break
    return e0, kap, (e0 - kap * h / 2, e0 + kap * h / 2)


# ---- the design ----------------------------------------------------------------------

def solve(strip_b, strip_s, rod_d, vessel_d, vessel_depth, vessel_wall,
          ready_c, room_c):
    """Every size, from the order (the strip, the rod), the vessel, the
    trigger temperature and the room."""
    lo, hi = TRIGGER_RANGE
    if not lo <= ready_c <= hi:
        raise ValueError(f"--ready-c {ready_c:g} is outside the band the "
                         f"knob is made for, {lo:g}-{hi:g} C")
    if not K94S_LINEAR[0] <= room_c < ready_c:
        raise ValueError(f"--room-c {room_c:g} must be below --ready-c and "
                         f"inside KANTHAL 94S's linear range")
    s = {"b": strip_b, "s": strip_s, "d": rod_d, "D": vessel_d,
         "H": vessel_depth, "W": vessel_wall, "ready": ready_c,
         "room": room_c}
    R = vessel_d / 2.0
    s["R"] = R
    # The frame rod stands one diameter in from the wall (a radius of air
    # between them), the strip clamped against its inner face, and the
    # strip reaches the mug's axis, where the probe stands.
    # The clip (1.4301 sheet, the thinnest gauge the sheet-metal rules
    # stock: 0.5 mm) grips the rim; the rod stands against its inner leaf.
    s["clip_t"] = CLIP_T
    s["clip_w"] = 2.0 * rod_d
    s["x_rod"] = -R + CLIP_T + rod_d / 2.0
    s["x_clamp"] = s["x_rod"] + rod_d / 2.0
    L = -s["x_clamp"]
    if L <= 0:
        raise ValueError("the vessel is too narrow for the rod")
    s["L"] = L
    s["A"] = tip_per_k(L, strip_s)                      # mm/K
    s["k_per_n"] = k_per_n(L, strip_b, strip_s)         # K/N
    s["drop_pour"] = s["A"] * (T_POUR - T_FLAT)         # below flat at 100 C
    # The strip's clamp sits a pour's drop above the bottom plus that drop
    # again, so its tip clears the bottom by a drop at its lowest.
    s["z_strip"] = -vessel_depth + 2.0 * s["drop_pour"]
    # Above the rim: everything is sized off the rod (the bought stock the
    # probe is cut from too) and the ball.
    s["probe_d"] = rod_d
    s["guide_d"] = rod_d + rod_d / 6.0          # a sixth of the rod's play
    s["arm_w"] = 4.0 * rod_d                    # the ramp's width
    s["arm_t"] = 2.0 * rod_d                    # its depth
    s["r_arm"] = 5.0 * rod_d                    # pivot to probe
    s["pivot_d"] = rod_d                        # a stub of the same rod
    # The trigger band: no sharper than the strip itself, whose k is +/- 4 %
    # over the cooling from the pour to the trigger.
    s["band_k"] = K94S_K_TOL * (T_POUR - ready_c)
    s["phi"] = s["A"] * s["band_k"] / s["r_arm"]   # rad: the detent's slope
    # The knob: an M3 set screw in the arm over the probe; one turn moves
    # the trigger by a pitch's worth of tip travel.
    s["k_per_turn"] = M3_PITCH / s["A"]
    s["turns"] = math.ceil((hi - lo) / s["k_per_turn"])
    s["deg_per_k"] = 360.0 / s["k_per_turn"]
    # The screw stands a pitch out of the arm's underside for the coolest
    # trigger (the probe must rise furthest to reach it) and the range's
    # travel further for the hottest.
    s["screw_out_hot"] = M3_PITCH
    s["screw_out_max"] = M3_PITCH + (hi - lo) * s["A"]
    above_rim(s)
    clip_sizes(s)
    return s


def above_rim(s):
    """The block, the ramp and what hangs off them, every size from the
    rod; and the riser's height from the thermal bound: the dry rod is a
    pin fin from the rim (tea up to the rim, the hottest case) to the
    block's mount face, its tip adiabatic, in the still-air film, and long
    enough to leave the printed block the margin under the profile's
    material's service temperature (README.md#the-heat-path)."""
    d = s["d"]
    s["bend_r"] = ROD_FORMER * d
    s["play"] = d / 6.0
    s["guide_d"] = d + s["play"]
    s["pivot_bore"] = d + s["play"]
    s["cheek_t"] = d
    s["cheek_l"] = 2.0 * d
    s["block_w"] = s["arm_w"] + 2.0 * (s["cheek_t"] + s["play"])
    s["block_h"] = 3.0 * d
    s["mount_depth"] = 2.0 * d
    s["x_block0"] = s["x_rod"] + s["bend_r"] + d
    s["x_block1"] = s["r_arm"] + s["cheek_l"] / 2.0
    # The ramp: a probe arm r_arm long with a tail under the knob, a ball
    # arm two thirds of it, the ball's stop by the pivot.
    s["knob_d"] = 2.0 * M3_HEAD_D
    s["knob_h"] = M3_HEAD_K + d / 2.0
    s["head_d"] = M3_HEAD_D + s["play"]
    s["head_h"] = M3_HEAD_K
    s["arm_tail"] = s["knob_d"] / 2.0
    s["x_lip"] = s["r_arm"] + 2.0 * s["r_arm"] / 3.0
    s["stop_t"] = d / 2.0
    s["stop_h"] = d          # holds a ball of up to twice its height
    s["x_stop"] = s["r_arm"] + s["pivot_bore"] / 2.0 + d / 2.0
    s["screw_bore"] = M3_TAP
    s["chime_in"] = 2.0 * d
    # The chime: a cantilever of the rod whose first mode sits in the
    # middle (geometrically) of the band chosen to be heard.
    s["chime_f_aim"] = math.sqrt(HEARD_HZ[0] * HEARD_HZ[1])
    s["chime_len"] = length_for_f1(SS_E, SS_RHO, d, s["chime_f_aim"])
    s["hanger_t"] = d
    s["dish_wall"] = d / 3.0


def riser(s, t_tip_max, film, room):
    """The rod's run height z_run (centreline) and the block's top: the dry
    path from the rim, up the riser, round its bend and across to the
    block, as long as the fin needs for its tip to stay at t_tip_max."""
    need = fin_length_for(T_POUR, room, film, SS_K, s["d"], t_tip_max)
    r = s["bend_r"]
    run = s["x_block0"] - s["x_rod"]
    # Corner to corner less what the bend's arc cuts: 2 r - pi r / 2.
    s["z_run"] = need - run + r * (2.0 - math.pi / 2.0)
    s["z_top"] = s["z_run"] + s["block_h"] / 2.0
    s["dry_mm"] = need
    s["z_arm"] = s["z_top"]
    s["z_pivot"] = s["z_arm"] + s["arm_t"] / 2.0
    s["z_knob"] = s["z_arm"] + s["arm_t"]
    s["z_chime"] = s["z_top"] - s["block_h"] / 2.0
    return s


def ball_place(s, ball_d):
    """The ball against its stop, on the ball arm's rising top."""
    s["ball_d"] = ball_d
    s["x_ball"] = s["x_stop"] + s["stop_t"] + ball_d / 2.0
    top = s["z_arm"] + s["arm_t"] + (s["x_ball"] - s["r_arm"]) * \
        math.tan(s["phi"])
    s["z_ball"] = top + ball_d / 2.0 / math.cos(s["phi"])
    s["x_ball_from_pivot"] = s["x_ball"] - s["r_arm"]
    # Off the lip: once the ramp has tipped onto its far stop, the ball
    # runs a radius down to the lip (v^2 = 10/7 g dh, a rolling sphere) and
    # falls onto the chime, a ball's diameter under the lip.
    s["fall"] = (s["z_arm"] + s["arm_t"]) - (s["z_chime"] + s["d"] / 2.0)
    v = math.sqrt(10.0 / 7.0 * G * ball_d / 2.0 * 1e-3)
    t = math.sqrt(2.0 * (s["fall"] + ball_d / 2.0) * 1e-3 / G)
    s["v_lip"] = v
    s["x_land"] = s["x_lip"] + v * t * 1e3
    s["dish_r"] = 2.0 * ball_d
    s["dish_h"] = 1.5 * ball_d
    s["x_dish"] = s["x_land"]
    s["z_dish"] = s["z_chime"] - s["d"] / 2.0 - ball_d - s["dish_h"]
    return s


def clip_frame(E, b, t, h, lb, delta):
    """The clip as a U frame on its inner leaf (held by the rod along its
    length): a tip force F on the outer leaf, h below the bridge, bends the
    leaf (F h^3 / 3EI) and turns the bridge's outer end (F h Lb / EI) under
    the moment F h, so the tip moves delta = F (h^3/3 + h^2 Lb) / EI
    (bends taken as corners). Returns the grip that opens it by delta (N)
    and the stress at the leaf's root, 6 F h / (b t^2) (MPa)."""
    I = b * t ** 3 / 12.0
    F = delta * E * I / (h ** 3 / 3.0 + h * h * lb)
    return F, 6.0 * F * h / (b * t * t)


def clip_sizes(s):
    """The clip over the rim: its leaves grip the wall's faces where the
    clip is (the inner face's sagitta over the clip's width added), and
    the outer leaf is the spring, opened from closed by that thickness,
    long enough to keep its root stress under 1.4301's yield with
    CLIP_SPARE."""
    R, W, w, t = s["R"], s["W"], s["clip_w"], s["clip_t"]
    s["x_leaf_in"] = -math.sqrt(R * R - (w / 2.0) ** 2)
    s["x_leaf_out"] = -(R + W)
    s["delta"] = s["x_leaf_in"] - s["x_leaf_out"]
    # The bridge's centreline span, and the lever from it to the tip.
    s["clip_lb"] = s["delta"] + t
    leaf = 5.0 * s["delta"]
    while True:
        h = leaf + CLIP_BEND_R + t / 2.0
        F, sigma = clip_frame(SS_E, w, t, h, s["clip_lb"], s["delta"])
        if sigma * CLIP_SPARE <= SS_FY:
            break
        leaf += t
    s["leaf"], s["clip_h"] = leaf, h
    # The jaw: the band at the outer leaf's tip that bears on the wall,
    # four thicknesses long.
    s["jaw"] = 4.0 * t
    s["grip_n"], s["leaf_stress"] = F, sigma
    # The rim's top is a full round, as a mug's is: radius half the wall.
    s["rim_r"] = W / 2.0
    # The wall the contact study takes: the clip's width either side.
    s["sector_deg"] = math.degrees(3.0 * w / R)


def largest_ball(m_lever_gmm, x_ball0, r_ball_of=lambda d: d / 2.0):
    """The largest steel ball (diameter, mm) whose moment in the detent,
    x_ball0 + its radius behind the pivot, leaves the ramp resting on the
    probe: m_b g x_b < the ramp's own static moment m_lever_gmm (g mm)."""
    lo, hi = 0.0, 50.0
    for _ in range(80):
        mid = (lo + hi) / 2.0
        if ball_mass_g(mid) * (x_ball0 + r_ball_of(mid)) < m_lever_gmm:
            lo = mid
        else:
            hi = mid
    return lo


def trigger_c(s, screw_mm, force_n):
    """The temperature at which the probe has lifted the ramp by phi, the
    screw standing screw_mm out of the arm's underside, the probe pressing
    force_n (the ramp's weight on it) once it touches: tip height
    z_strip - A (T - 20) - give(F) + probe = arm - screw + r phi."""
    give = force_deflection(force_n, s["L"], s["b"], s["s"])
    rise = (s["z_strip"] - give + s["probe_len"] - s["z_arm"] + screw_mm -
            s["r_arm"] * s["phi"])
    return T_FLAT + rise / s["A"]


def screw_for(s, T, force_n):
    """The screw's stand-out that triggers at T."""
    return (T - trigger_c(s, 0.0, force_n)) * s["A"]
