# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every size of the woodcutter automaton's mechanism, from the box's inside
width and the print's gap, and the two motions its cams are cut for.

Nothing here talks to the app: `solve()` takes the width and the gap
`print_allowance` answers and returns one dict. The base radii are only
STARTED here (a closed-form floor); build.py settles each against
CamProfile's own report.

Coordinates, the BOX frame every part is modelled in, the assembly's: X
along the camshaft (the box's inside width, 0 to W), Y across the box
(front +Y, where the log is), Z up; z = 0 is the floor's top face and the
top panel's upper face, the stage the figures stand on, is at `stage`. Each
moving part is modelled where it stands at rest (the crank at 0, both
followers on their base circles), so every mate starts at zero and every
pair of mate frames coincides as built.
"""
import math

NOZZLE_MM = 0.4      # the P1S's stock nozzle, which the allowances assume
MIN_WALL_MM = 4 * NOZZLE_MM   # four lines: the thinnest wall that carries
G = 9.80665          # standard gravity, m/s^2

HAND_RPM = 60.0      # a comfortable hand turn, 1 rev/s (the plan's)
MARGIN = 2.0         # the followers must hold at twice it
PRESSURE_LIMIT_DEG = 30.0  # a translating follower's usual limit: past it
#                            the side load in the guide grows as tan(phi)

# The motions, as fractions of the crank's turn (angles are not sizes).
# The axe: a slow cycloidal rise over most of the turn, the drop, and a
# dwell long enough for the fall to land within it at twice the hand speed.
AXE_RISE_TO_DEG = 210.0
# The bird pecks while the axe is half way up its rise, well clear of the
# drop: a cycloidal rise and fall (the peck) and a dwell either side.
BIRD_PECK_FROM_DEG = 25.0
BIRD_PECK_DEG = 85.0          # each of the rise and the fall
# A negative control: the axe's landing dwell shortened to this, under
# the fall's turn at 120 rpm, must fail drop_rpm.
SHORT_DWELL_DEG = 15.0

# The figures' swings (design choices, degrees from +Y toward +Z about +X)
ARM_DOWN_DEG = -40.0   # the axe on the log
ARM_UP_DEG = 80.0      # the axe overhead, short of the link's dead centre
BIRD_DOWN_DEG = -15.0  # the bird's tail lever at rest (head up)
BIRD_UP_DEG = 35.0     # the tail lever at the bottom of the peck


def axe_segments(dwell_deg=None):
    """The axe cam's motion: rise, drop, dwell. `dwell_deg` shortens the
    landing dwell (the negative control); the rise takes the rest."""
    rise_to = AXE_RISE_TO_DEG if dwell_deg is None else 360.0 - dwell_deg
    return rise_to, [
        {"to_deg": rise_to, "rise_mm": None, "law": "cycloidal"},
        {"to_deg": rise_to, "rise_mm": None, "law": "drop"},
        {"to_deg": 360.0, "law": "dwell"}]


def bird_segments():
    a0 = BIRD_PECK_FROM_DEG
    a1 = a0 + BIRD_PECK_DEG
    a2 = a1 + BIRD_PECK_DEG
    return [{"to_deg": a0, "law": "dwell"},
            {"to_deg": a1, "rise_mm": None, "law": "cycloidal"},
            {"to_deg": a2, "rise_mm": None, "law": "cycloidal"},
            {"to_deg": 360.0, "law": "dwell"}]


def with_rise(segs, h):
    """The segments with their rises filled in: up h, then down h."""
    out, up = [], True
    for s in segs:
        s = dict(s)
        if "rise_mm" in s:
            s["rise_mm"] = h if up else -h
            up = not up
        out.append(s)
    return out


# --- closed forms -------------------------------------------------------------

def cycloidal(h, beta, t):
    """A cycloidal rise of h over beta (rad), at t (rad from its start):
    (s, s', s'') in mm, mm/rad, mm/rad^2."""
    u = t / beta
    s = h * (u - math.sin(2 * math.pi * u) / (2 * math.pi))
    s1 = h / beta * (1 - math.cos(2 * math.pi * u))
    s2 = 2 * math.pi * h / beta ** 2 * math.sin(2 * math.pi * u)
    return s, s1, s2


def max_pressure_deg(h, beta, r_prime, n=2000):
    """The worst pressure angle of a cycloidal rise of h over beta for a
    radial follower whose centre runs on the prime circle r_prime (base
    radius plus tip radius): tan(phi) = s' / (r_prime + s)."""
    worst = 0.0
    for i in range(n + 1):
        s, s1, _ = cycloidal(h, beta, beta * i / n)
        worst = max(worst, math.degrees(math.atan2(s1, r_prime + s)))
    return worst


def base_for_pressure(h, beta, tip, limit_deg):
    """The least base radius, to 0.01 mm, that keeps a cycloidal rise of h
    over beta under limit_deg for a radial follower of tip radius `tip`."""
    lo, hi = 0.0, 10.0 * h
    for _ in range(60):
        mid = (lo + hi) / 2.0
        if max_pressure_deg(h, beta, mid + tip) > limit_deg:
            lo = mid
        else:
            hi = mid
    return math.ceil(hi * 100.0) / 100.0


def liftoff_rod_rpm(h, beta):
    """The cam speed a bare rod leaves a cycloidal rise at: its deceleration
    2 pi h / beta^2 * omega^2 reaches g (no loop, no friction)."""
    a = 2 * math.pi * (h / 1000.0) / beta ** 2
    return math.sqrt(G / a) * 60.0 / (2 * math.pi)


# --- the levers -----------------------------------------------------------------

def lever(stroke, down_deg, up_deg, side, rod_top_z, link):
    """A lever turned by a rod through a link: the rod's top pin rises
    `stroke` from rod_top_z at y = 0, and the lever's pin A, `l` from its
    pivot P, swings from down_deg to up_deg. `side` +1 puts A in front of
    the pivot (+Y, the woodcutter's arm), -1 behind it (the bird's tail).
    l is chosen so the pin rises the stroke over the swing; P so that A
    is over the rod at mid-swing (the link nearest upright); P's height so
    the link is `link` long at rest. Returns P, l, and the swing the loop
    really makes over the stroke (the link tilts, so it is not quite the
    one asked)."""
    d0, d1 = math.radians(down_deg), math.radians(up_deg)
    l = stroke / (math.sin(d1) - math.sin(d0))
    dm = (d0 + d1) / 2.0
    py = -side * l * math.cos(dm)

    def a_of(th, pz):
        return (py + side * l * math.cos(th), pz + l * math.sin(th))
    # P's height: the link `link` long between the rod's pin and A at rest.
    ay0 = a_of(d0, 0.0)[0]
    pz = rod_top_z + math.sqrt(link ** 2 - ay0 ** 2) - l * math.sin(d0)

    def swing(s):
        """The lever's angle with the rod's pin raised s: |A - R| = link,
        on the branch the rest pose is on (bisection, A above R)."""
        rz = rod_top_z + s
        lo, hi = d0 - 0.5, d1 + 0.5

        def f(th):
            ax, az = a_of(th, pz)
            return math.hypot(ax, az - rz) - link
        # f grows with th near the rest pose: the pin climbs away from R.
        # Find the root nearest the asked swing by stepping from d0.
        prev_t, prev_f = lo, f(lo)
        steps = 2000
        for i in range(1, steps + 1):
            t = lo + (hi - lo) * i / steps
            ft = f(t)
            if prev_f > 0 >= ft or prev_f < 0 <= ft:
                a, b = prev_t, t
                for _ in range(60):
                    m = (a + b) / 2.0
                    if (f(a) > 0) == (f(m) > 0):
                        a = m
                    else:
                        b = m
                return math.degrees((a + b) / 2.0)
            prev_t, prev_f = t, ft
        raise SystemExit("woodcutter: the lever's loop does not close")
    return {"P": (py, pz), "l": l, "link": link, "side": side,
            "down_deg": down_deg, "asked_up_deg": up_deg,
            "up_deg": swing(stroke), "rest_deg": swing(0.0),
            "A0": a_of(d0, pz)}


# --- every size -------------------------------------------------------------------

def solve(width, gap):
    """Every size from the box's inside width (mm) and the print's gap (the
    whole diametral clearance two printed surfaces need to slide, from
    print_allowance kind `gap`). Refuses what will not print."""
    W = float(width)
    g = gap / 2.0                      # a side
    s = {"width": W, "gap": gap, "g": g}
    # The box: a 30th of the width for walls (3.6 mm, nine lines, at 120),
    # half the width deep inside. Its height is set by the cams, below.
    s["wall"] = max(MIN_WALL_MM, W / 33.3)
    s["depth"] = 0.5 * W
    # The camshaft: Ø a 15th of the width (8 mm at 120: a printed PLA shaft
    # lying down, stiff over its span; phase 4's FEM checks it). The cams
    # are keyed to it by a printed flat, a sixth of its diameter deep.
    s["d_shaft"] = W / 15.0
    s["flat"] = s["d_shaft"] / 6.0
    s["d_bore"] = s["d_shaft"] + gap   # the end walls' bores, running
    # The cams: a tenth of the width wide, at a quarter and three quarters
    # of it, so the two figures stand half the box apart.
    s["cam_w"] = W / 10.0
    s["x_axe"] = W / 4.0
    s["x_bird"] = 3.0 * W / 4.0
    # The followers: rods of a 20th of the width (Ø6 at 120), their feet a
    # cylinder of the rod's radius across the cam: a knife whose tip radius
    # is the rod's.
    s["d_rod"] = W / 20.0
    s["tip"] = s["d_rod"] / 2.0
    s["d_guide"] = s["d_rod"] + gap
    # The strokes: an eighth of the width for the axe, a 15th for the bird.
    s["h_axe"] = W / 8.0
    s["h_bird"] = W / 15.0
    # The hub: the shaft's bore and two walls round it.
    s["hub_floor"] = s["d_shaft"] / 2.0 + g + 2.0 * MIN_WALL_MM
    # Base radii from the pressure-angle limit, floored by the hub. The
    # axe's rise is gentle, so its hub sets it; the bird's peck sets its own.
    axe_beta = math.radians(AXE_RISE_TO_DEG)
    bird_beta = math.radians(BIRD_PECK_DEG)
    s["axe_base_pressure"] = base_for_pressure(
        s["h_axe"], axe_beta, s["tip"], PRESSURE_LIMIT_DEG)
    s["bird_base_pressure"] = base_for_pressure(
        s["h_bird"], bird_beta, s["tip"], PRESSURE_LIMIT_DEG)
    s["axe_base"] = max(s["hub_floor"], s["axe_base_pressure"])
    s["bird_base"] = max(s["hub_floor"], s["bird_base_pressure"])
    # The pins: a 40th of the width (Ø3 at 120), in holes the gap larger;
    # the plates (link, arm, bird) as thick; the figures' bodies 4/3 of it.
    s["d_pin"] = W / 40.0
    s["d_pin_hole"] = s["d_pin"] + gap
    s["plate"] = W / 40.0
    s["body"] = W / 30.0
    s["eye"] = s["d_pin_hole"] / 2.0 + MIN_WALL_MM   # an eye's radius
    if s["plate"] < MIN_WALL_MM or s["d_pin"] < 2.0:
        raise SystemExit(f"woodcutter: {s['plate']:.2f} mm plates and Ø{s['d_pin']:.2f} pins at width "
                         f"{W:g} -- a wider box")
    return s


def layout(s, axe_base, bird_base):
    """The heights and the figures, once the base radii are settled."""
    W, t, g = s["width"], s["wall"], s["g"]
    s["axe_base"], s["bird_base"] = axe_base, bird_base
    r_max = max(axe_base + s["h_axe"], bird_base + s["h_bird"])
    s["r_max"] = r_max
    # The shaft above the floor by the larger cam and a 20th of the width.
    s["zc"] = r_max + W / 20.0
    # The guides: a boss under the top, the bearing three rods long.
    s["guide_len"] = 3.0 * s["d_rod"]
    s["boss_r"] = s["d_guide"] / 2.0 + 2.0 * MIN_WALL_MM
    # Inside height: the highest foot's top at full lift, a 40th of the
    # width under the boss, and the boss.
    foot_top = max(s["zc"] + axe_base + s["h_axe"],
                   s["zc"] + bird_base + s["h_bird"]) + 2.0 * s["tip"]
    s["H"] = foot_top + W / 40.0 + (s["guide_len"] - t)
    s["stage"] = s["H"] + t
    for k, base in (("axe", axe_base), ("bird", bird_base)):
        s[f"foot_{k}"] = s["zc"] + base + s["tip"]   # foot centre at rest
    # The rods' top pins, a 15th of the width over the stage at rest.
    s["rod_pin_z"] = s["stage"] + W / 15.0
    # The plates beside each rod, in -X: link, then the lever, then the
    # body (or perch) that carries the lever's pivot. Each slab [x0, x1].
    for k in ("axe", "bird"):
        x = s[f"x_{k}"]
        r = s["d_rod"] / 2.0
        link = (x - r - g - s["plate"], x - r - g)
        lev = (link[0] - g - s["plate"], link[0] - g)
        body = (lev[0] - g - s["body"], lev[0] - g)
        s[f"{k}_slabs"] = {"link": link, "lever": lev, "body": body}
    # The woodcutter: the arm from the shoulder to the axe's eye, three
    # tenths of the width; the head a 15th of it long to its edge.
    s["arm_reach"] = 0.3 * W
    s["axe_head"] = W / 15.0
    s["log_d"] = 0.12 * W
    # The blade stops above the log by a 120th of the width (1 mm at 120:
    # a design choice, so the blade meets the log without striking it),
    # held as a clearance requirement over the study.
    s["blade_gap"] = W / 120.0
    d0 = math.radians(ARM_DOWN_DEG)
    # The shoulder's height: the head's lowest corner at rest the gap over
    # the log's top. The link's length then follows from the lever.
    low = min(arm_point(s, p, ARM_DOWN_DEG) for p in axe_outline(s))[1:]
    log_top = s["stage"] + s["log_d"]
    shoulder_z = log_top + s["blade_gap"] - low[1]
    l = s["h_axe"] / (math.sin(math.radians(ARM_UP_DEG)) - math.sin(d0))
    dm = (d0 + math.radians(ARM_UP_DEG)) / 2.0
    ay0 = -l * math.cos(dm) + l * math.cos(d0)
    az0 = shoulder_z + l * math.sin(d0)
    link = math.hypot(ay0, az0 - s["rod_pin_z"])
    s["arm"] = lever(s["h_axe"], ARM_DOWN_DEG, ARM_UP_DEG, +1,
                     s["rod_pin_z"], link)
    # The bird: its tail lever a link of a ninth of the width over the rod.
    s["bird"] = lever(s["h_bird"], BIRD_DOWN_DEG, BIRD_UP_DEG, -1,
                      s["rod_pin_z"], W / 9.0)
    py, pz = s["arm"]["P"]
    s["blade_rest"] = (py + low[0], pz + low[1])
    s["log_y"] = s["blade_rest"][0]
    s["log_top"] = log_top
    if s["log_y"] + s["log_d"] / 2.0 > s["depth"] / 2.0 + t:
        raise SystemExit("woodcutter: the log overhangs the stage")
    return s


def axe_outline(s):
    """The axe's head in the arm's own coordinates (along the arm from the
    shoulder, across it toward the side the chop swings to): a wedge
    widening from the bar's end to the blade's edge."""
    k = s["width"] / 120.0
    R, hl = s["arm_reach"], s["axe_head"]
    return [(R - 3 * k, -1 * k), (R + 3 * k, -1 * k), (R + 5.5 * k, hl),
            (R - 5.5 * k, hl)]


def arm_point(s, p, deg):
    """An arm-local point (along, across) relative to the shoulder with the
    arm at `deg`: (z, y, z) -- sorted by height first, then (y, z)."""
    th = math.radians(deg)
    u = (math.cos(th), math.sin(th))
    n = (math.sin(th), -math.cos(th))
    y = p[0] * u[0] + p[1] * n[0]
    z = p[0] * u[1] + p[1] * n[1]
    return (z, y, z)
