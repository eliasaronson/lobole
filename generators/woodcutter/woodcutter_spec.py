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

# A child leaning on the crank (woodcutter_fem.py): the push that turns it, the
# worst torque the followers need over a turn (check_followers) over the
# crank's radius, times this. Turning takes almost nothing (a few
# hundredths of a newton at the grip: the followers weigh grams and friction
# is not modelled), so the factor is large: at 120 it makes the push about
# 94 N, a 9.6 kg lean -- a small child's weight on one hand. The studies
# must hold it with LEAN_NEED to spare, against yield and across the
# printed layers; the camshaft at work, at its own load, with WORK_NEED.
LEAN_FACTOR = 3000.0
LEAN_NEED = 2.0
WORK_NEED = 2.0

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

def solve(width, gap, press=0.0):
    """Every size from the box's inside width (mm), the print's gap (the
    whole diametral clearance two printed surfaces need to slide, from
    print_allowance kind `gap`) and its press (a pressed bore less its pin,
    print_allowance kind `press`). Refuses what will not print."""
    W = float(width)
    g = gap / 2.0                      # a side
    s = {"width": W, "gap": gap, "g": g, "press": press}
    # The pins and the pegs: a 40th of the width (Ø3 at 120), one stock.
    s["d_pin"] = W / 40.0
    s["d_peg"] = s["d_pin"]
    # The box: printed panels pegged at their edges, so a panel is a peg
    # and four lines either side of it thick (6.2 mm at 120), and never
    # under a 33rd of the width; half the width deep inside. Its height is
    # set by the cams, below.
    s["wall"] = max(MIN_WALL_MM, W / 33.3, s["d_peg"] + 2.0 * MIN_WALL_MM)
    s["depth"] = 0.5 * W
    # The camshaft: Ø a 15th of the width (8 mm at 120: a printed PLA shaft
    # lying down, stiff over its span: at work it carries the followers
    # with a factor over a thousand, woodcutter_fem.py; leaned on at the
    # crank it does not hold, README "Not done here"). The cams
    # are keyed to it by a printed flat, a sixth of its diameter deep, the
    # whole length: the shaft prints lying on it, and a flat deeper than
    # the round's 45-degree chord leaves no face of it looking down past
    # 45 degrees.
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
    # Each rod prints lying on a flat at its 45-degree chord on its -X
    # side, which also keys it in its guide (a D bore) so its figure's link
    # stays square to the shaft.
    s["rod_flat"] = s["d_rod"] / 2.0 / math.sqrt(2.0)
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
    # The pins run in holes the gap larger and are pressed into holes the
    # press larger; the plates (link, arm, bird) as thick as a pin; the
    # figures' bodies 4/3 of it.
    s["d_pin_hole"] = s["d_pin"] + gap
    s["d_pin_press"] = s["d_pin"] + press
    s["plate"] = W / 40.0
    s["body"] = W / 30.0
    s["eye"] = s["d_pin_hole"] / 2.0 + MIN_WALL_MM   # an eye's radius
    box_parts(s)
    if s["plate"] < MIN_WALL_MM or s["d_pin"] < 2.0:
        raise SystemExit(f"woodcutter: {s['plate']:.2f} mm plates and Ø{s['d_pin']:.2f} pins at width "
                         f"{W:g} -- a wider box")
    return s


def box_parts(s):
    """The panels' pegs, the camshaft's bushings and the crank, from the
    width, the wall and the fits (README, where every size comes from)."""
    W, t, g = s["width"], s["wall"], s["g"]
    # A peg sits in a blind hole either side of a joint, each a panel less
    # its four-line skin deep, and stops a gap short of each hole's floor.
    s["peg_hole"] = t - MIN_WALL_MM
    s["peg_len"] = 2.0 * (s["peg_hole"] - g)
    s["d_peg_bore"] = s["d_peg"] + s["press"]
    # The camshaft lies on its flat to print, so its journals would run on
    # a D: each end is pressed into a printed bushing, round outside, that
    # turns in the end wall's bore and has a flange outside the wall (the
    # two flanges hold the shaft along its axis). Its sleeve is two
    # four-line walls round the shaft; its flange two more, a 60th of the
    # width thick.
    s["r_bush"] = s["d_shaft"] / 2.0 + 2.0 * MIN_WALL_MM
    s["r_flange"] = s["r_bush"] + 2.0 * MIN_WALL_MM
    s["flange"] = W / 60.0
    s["d_wall_bore"] = 2.0 * s["r_bush"] + s["gap"]
    # The crank: a plate as thick as the figures' bodies, on the shaft's
    # -X end outside the left bushing; its handle a quarter of the box's
    # depth from the shaft, so the grip passes the table under the box by
    # more than a finger's thickness (README).
    s["crank_r"] = s["depth"] / 4.0
    s["crank_t"] = W / 30.0
    s["r_hub"] = s["r_bush"]
    # The handle: a grip turning on a headed pin pressed into the crank, the
    # pin a 24th of the width (Ø5 at 120), the grip a fifth of it long.
    s["d_handle"] = W / 24.0
    s["d_grip_bore"] = s["d_handle"] + s["gap"]
    s["r_grip"] = s["d_grip_bore"] / 2.0 + 2.0 * MIN_WALL_MM
    s["grip_len"] = W / 5.0
    s["r_head"] = s["r_grip"]
    s["head"] = 2.0 * MIN_WALL_MM
    # Along the shaft (x): the left bushing's flange, then the crank.
    s["x_bush_l"] = -t - g                       # flange's inner face
    s["x_crank1"] = s["x_bush_l"] - s["flange"] - g   # crank's inner face
    s["x_crank0"] = s["x_crank1"] - s["crank_t"]
    s["x_shaft0"] = s["x_crank0"]
    s["x_bush_r"] = W + t + g
    s["x_shaft1"] = s["x_bush_r"] + s["flange"]
    # The figures stand in slots through the top on a tab a 12th of the
    # width long, the log on a rib a 30th of it wide; each tab the panel
    # deep, pressed (the press allowance on the slot's width).
    s["tab_len"] = W / 12.0
    s["rib_w"] = W / 30.0
    # Plates: pieces this far apart and from the bed's edges.
    s["spacing"] = W / 30.0
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
