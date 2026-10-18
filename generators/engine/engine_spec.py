# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The printed inline four's sizes, every one from the bore and the print
profile, and the plain-number geometry the parts and the checks share: the
cam lobe and its lift table, the valve timing, the toothed belt's path and
teeth, the piston's travel, and each part's footprint on the bed in the way
it prints (for the bore search, before anything is built).

The ENGINE FRAME: X along the crank axis, front (timing belt, hand crank)
at -X, rear (flywheel) at +X; Z up the bores; the crank axis is the X axis.
Cylinder 1 is the front one. A crank angle is the crank revolute's
rotation about +X: seen from the front, clockwise. At crank 0 cylinder 1 is
at the TDC it fires on.

Provenance is said on the line: a DESIGN choice is ours, a RULE is common
practice, and the rest is derived from those and the profile.
"""
import math

NOZZLE_MM = 0.4          # the P1S's nozzle: a line is this wide (machine)


def lines(n):
    return n * NOZZLE_MM


class Allow:
    """print_allowance's diametral allowance by kind and size, asked once
    (generators/clock/build.py's)."""

    def __init__(self, d, proc):
        self.d, self.proc, self.cache, self.answers = d, proc, {}, {}

    def __call__(self, kind, size):
        k = (kind, round(size, 4))
        if k not in self.cache:
            r = self.d.call("print_allowance", kind=kind, size_mm=size,
                            **self.proc)
            self.cache[k] = r["allowance_mm"]
            self.answers[k] = {key: r.get(key) for key in (
                "allowance_mm", "from", "note", "source")}
        return self.cache[k]


class Table:
    """A stand-in for Allow off the instance: the profile's gap rows as
    numbers, for the bore search's arithmetic (build.py checks the search's
    answer against the instance's own)."""

    def __init__(self, gap_rows, press=0.0):
        self.gap_rows, self.press = gap_rows, press

    def __call__(self, kind, size):
        if kind == "press":
            return self.press
        for lo, hi, a in self.gap_rows:
            if lo <= size < hi:
                return a
        raise ValueError(f"no {kind} row for {size}")


# --- valve timing -------------------------------------------------------------
# A mild road cam (DESIGN, within the range a four-stroke's timing diagram
# shows: exhaust opens 40 before BDC and closes 10 after TDC, intake opens
# 10 before TDC and closes 40 after BDC), crank degrees after the cylinder's
# FIRING TDC. Physical events: where the valve leaves and meets its seat,
# the lash taken up.
EVO, EVC = 180.0 - 40.0, 360.0 + 10.0
IVO, IVC = 360.0 - 10.0, 540.0 + 40.0
FIRING_ORDER = (1, 3, 4, 2)
# The crank throws of a flat-plane four: 1 and 4 up together, 2 and 3 down.
THROW_DEG = (0.0, 180.0, 180.0, 0.0)
# Squeeze of a TPU spring (DESIGN, from the jar's 25 % static seal: a
# spring works further): 10 % at rest, so the valve is held shut, and 30 %
# at full lift.
SQUEEZE_REST, SQUEEZE_LIFT = 0.10, 0.30


def firing_tdc(cyl):
    """Cylinder `cyl` (1-based)'s firing TDC, crank degrees: 180 apart in
    the firing order, cylinder 1 at 0."""
    return 180.0 * FIRING_ORDER.index(cyl)


def lobes_of(cyl):
    """(kind, peak crank angle) of the cylinder's two valves."""
    t = firing_tdc(cyl)
    return (("intake", (t + (IVO + IVC) / 2.0) % 720.0),
            ("exhaust", (t + (EVO + EVC) / 2.0) % 720.0))


def piston_z(s, theta_deg, cyl, rod=None):
    """The wrist pin's height above the crank axis at a crank angle."""
    L = rod if rod is not None else s.L
    a = math.radians(theta_deg + THROW_DEG[cyl - 1])
    return s.R * math.cos(a) + math.sqrt(L * L - (s.R * math.sin(a)) ** 2)


def lobe_lift(s, dpsi_deg):
    """The lobe's rise H over its base circle at a cam angle `dpsi_deg`
    from its peak: the support function of the hull of the base circle and
    the nose circle, which is what a flat follower rides (the follower
    meets the base circle or the nose, never the straight flanks)."""
    return max(0.0, s.cam_d * math.cos(math.radians(dpsi_deg)) + s.r_nose
               - s.r_base)


def lobe_contact(s, dpsi_deg):
    """Where on the bucket the lobe touches, mm off the bucket's centre:
    the support function's slope (zero on the base circle)."""
    if lobe_lift(s, dpsi_deg) <= 0.0:
        return 0.0
    return abs(s.cam_d * math.sin(math.radians(dpsi_deg)))


def cam_table(s, peak_cam_deg, step=1.0):
    """The cam relation's lift table, cam degrees 0..360: the modelled
    valve offset, which is the rise H itself -- the model keeps the bucket
    `c_model` off the lobe at every angle, so a lobe and its bucket never
    touch in the clash check, and the valve sits `lash` lower than the
    print's would while it is open."""
    n = int(round(360.0 / step))
    out = []
    for i in range(n):
        psi = i * step
        d = (psi - peak_cam_deg + 180.0) % 360.0 - 180.0
        out.append([psi, round(lobe_lift(s, d), 6)])
    return out


def model_lift(s, theta_deg, peak_crank_deg):
    d = ((theta_deg - peak_crank_deg) / 2.0 + 180.0) % 360.0 - 180.0
    return lobe_lift(s, d)


# --- the toothed belt -----------------------------------------------------------

def belt_length(r1, r2, c):
    """The pitch length of a belt on circles r1 < r2, centres c apart."""
    phi = math.asin((r2 - r1) / c)
    return (2.0 * c * math.cos(phi) + r1 * (math.pi - 2.0 * phi)
            + r2 * (math.pi + 2.0 * phi))


def belt_centre(r1, r2, length):
    """The centre distance at which a belt of `length` runs on r1 and r2."""
    lo, hi = (r2 - r1) * 1.0001 + 1e-6, length
    for _ in range(200):
        mid = (lo + hi) / 2.0
        if belt_length(r1, r2, mid) < length:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2.0


class BeltPath:
    """The belt's pitch line in the belt plane (u = engine Y, v = engine Z):
    round the crank pulley (centre 0, 0, radius r1) under, up the +u span,
    over the cam pulley (centre 0, c, radius r2), down the -u span. A point
    at arc length s gives (u, v), the unit tangent and the unit normal
    pointing INTO the loop (towards the pulleys)."""

    def __init__(self, r1, r2, c):
        self.r1, self.r2, self.c = r1, r2, c
        self.phi = math.asin((r2 - r1) / c)
        self.span = c * math.cos(self.phi)
        # Segments in order: arc 1 from angle (pi + phi) to (2 pi - phi),
        # span right, arc 2 from -phi to pi + phi, span left.
        self.a1 = r1 * (math.pi - 2.0 * self.phi)
        self.a2 = r2 * (math.pi + 2.0 * self.phi)
        self.length = 2.0 * self.span + self.a1 + self.a2
        n = (math.cos(self.phi), -math.sin(self.phi))
        self.t1r = (r1 * n[0], r1 * n[1])
        self.t2r = (r2 * n[0], c + r2 * n[1])
        self.t2l = (-r2 * n[0], c + r2 * n[1])
        self.t1l = (-r1 * n[0], r1 * n[1])

    def at(self, s):
        s %= self.length
        if s < self.a1:
            a = math.pi + self.phi + s / self.r1
            p = (self.r1 * math.cos(a), self.r1 * math.sin(a))
            return p, (-math.sin(a), math.cos(a)), (-math.cos(a),
                                                    -math.sin(a)), 1, a
        s -= self.a1
        if s < self.span:
            d = ((self.t2r[0] - self.t1r[0]) / self.span,
                 (self.t2r[1] - self.t1r[1]) / self.span)
            p = (self.t1r[0] + d[0] * s, self.t1r[1] + d[1] * s)
            return p, d, (-d[1], d[0]), 0, None
        s -= self.span
        if s < self.a2:
            a = -self.phi + s / self.r2
            p = (self.r2 * math.cos(a), self.c + self.r2 * math.sin(a))
            return p, (-math.sin(a), math.cos(a)), (-math.cos(a),
                                                    -math.sin(a)), 2, a
        s -= self.a2
        d = ((self.t1l[0] - self.t2l[0]) / self.span,
             (self.t1l[1] - self.t2l[1]) / self.span)
        p = (self.t2l[0] + d[0] * s, self.t2l[1] + d[1] * s)
        return p, d, (-d[1], d[0]), 0, None


def trapezoid(p, t, n, inner_off, base_w, tip_w, height, into=0.0):
    """A tooth in the local frame at pitch point p (tangent t, normal n
    into the loop): its base `inner_off` in from the pitch line, `height`
    further in, `into` back out past the base (to fuse with a band)."""
    def pt(a, b):
        return (p[0] + t[0] * a + n[0] * b, p[1] + t[1] * a + n[1] * b)
    b0, b1 = inner_off - into, inner_off + height
    return [pt(-base_w / 2.0 - into * 0.0, b0), pt(base_w / 2.0, b0),
            pt(tip_w / 2.0, b1), pt(-tip_w / 2.0, b1)]


# --- the sizes ------------------------------------------------------------------

class Spec:
    pass


def solve(bore, allow, layer, slender, d_stem=None, stem_tries=0):
    """Every size from the bore `bore` (mm), the profile's allowances
    (`allow(kind, size)`, diametral) and its layer height. Raises
    ValueError when the bore is too small for a size to print. `slender`
    is check_print's slender limit (the profile's, _lib.slender_ratio): a
    valve stands on its head, so its stem is thick enough for the height
    over it -- solved again with that stem when the first is not."""
    s = Spec()
    s.stem_tries = stem_tries
    s.bore = B = float(bore)
    s.layer = layer

    def gap(d):
        return allow("gap", d)

    def side(d):
        return gap(d) / 2.0

    def layers(n):
        return n * layer

    floors = []

    def at_least(name, value, floor):
        """A size and its printing floor: the floor is recorded when it
        binds, which is what 'the detail does not hold' means."""
        if value < floor - 1e-9:
            floors.append((name, value, floor))
            return floor
        return value
    s.floors = floors

    # The stack of printed tolerances a running clearance must absorb in a
    # chain of five printed sizes (rod, piston, block deck, head seat,
    # valve), each about the gap a side: RULE, FDM holds about +-0.1 a face.
    s.c_stack = 5.0 * side(B)
    # --- the bottom end ---
    s.S = B                                   # DESIGN: square, stroke = bore
    s.R = s.S / 2.0
    s.d_j = 0.4 * B                           # DESIGN: journals and pins alike
    s.r_j = s.d_j / 2.0
    # The crank prints on the plane of its throws with everything cut flat
    # this far below the axis: no round face then looks down past 45.
    s.h_flat = s.r_j * math.cos(math.radians(45.0))
    s.g_j = gap(s.d_j)                        # a journal in its bore
    s.r_jb = (s.d_j + s.g_j) / 2.0
    s.g_ax = side(s.d_j)                      # axial play a side
    s.w_r = 0.3 * B                           # DESIGN: rod big end width (X)
    s.l_pin = s.w_r + 2.0 * s.g_ax
    s.t_web = 0.2 * B                         # DESIGN: web thickness (X)
    s.b_t = 0.4 * B                           # DESIGN: a main bulkhead (X)
    s.l_j = s.b_t + 2.0 * s.g_ax
    s.pitch = s.l_j + 2.0 * s.t_web + s.l_pin  # cylinder to cylinder
    s.x_main = [k * s.pitch for k in range(5)]
    s.x_cyl = [(k + 0.5) * s.pitch for k in range(4)]
    s.x_f = -s.b_t / 2.0                      # the block's front face
    s.x_r = 4.0 * s.pitch + s.b_t / 2.0       # and its rear face
    # Rod: the big end splits across the pin; a lap each side of the bore
    # carries two pressed pins along X (vertical as the rod prints).
    s.d_cp = at_least("cap pin", 0.08 * B, lines(5))
    s.w_eye = at_least("big-end wall", 0.25 * s.r_jb, lines(5))
    s.r_be = s.r_jb + s.w_eye
    s.y_cp = s.r_jb + lines(2) + s.d_cp / 2.0     # cap pins' centres
    s.y_ear = s.y_cp + s.d_cp / 2.0 + lines(2)    # the laps' outer edge
    s.z_lap = s.d_cp / 2.0 + lines(2)             # the lap band, a side
    s.d_wp = 0.25 * B                         # DESIGN: wrist pin
    s.g_wp = gap(s.d_wp)
    s.r_seb = (s.d_wp + s.g_wp) / 2.0
    s.r_se = s.r_seb + at_least("small-end wall", 0.3 * s.r_seb, lines(4))
    s.w_shank0 = 2.0 * (s.r_be - s.w_eye / 2.0)   # at the big end
    s.w_shank1 = 2.0 * s.r_se * 0.8               # at the small end
    # Piston: bore less the gap, crown, a skirt below the pin, two bosses.
    s.g_bore = gap(B)
    s.d_p = B - s.g_bore
    s.t_skirt = at_least("skirt", 0.06 * B, lines(4))
    s.t_crown = at_least("crown", 0.05 * B, layers(6))
    s.g_wp_ax = side(s.d_wp)
    s.x_boss = s.w_r / 2.0 + s.g_wp_ax            # boss inner faces
    s.r_boss = s.r_seb - s.g_wp / 2.0 + lines(3)  # round the pressed pin
    s.r_wpb = s.d_wp / 2.0                        # pressed: the pin's size
    s.h_cp = max(0.4 * B, s.r_boss + s.t_crown + lines(1))  # pin to crown
    s.s_below = 0.4 * B                       # DESIGN: skirt below the pin
    s.l_wp = s.d_p - 2.0 * lines(2)
    # Crank webs: from the flat (-h_flat) up past the pin; ends at the pin
    # and the counterweight equally far out.
    s.r_web = s.r_j + lines(4)
    s.y_web = s.r_j + lines(4)                # the web's top, +Y
    # Everything turning with the crank stays inside r_sweep.
    big_end = math.hypot(s.y_ear, s.z_lap)
    s.r_sweep = max(s.R + max(s.r_be, big_end),
                    math.hypot(s.y_web, s.R + s.r_web))
    s.L = max(1.8 * s.S,                      # DESIGN: rod 1.8 strokes
              s.R + s.s_below + s.r_sweep + s.c_stack)
    s.z_bb = max(s.r_sweep + s.c_stack, s.L - s.R - s.s_below)  # bores end
    # --- valves (two a cylinder, in a row under one cam) ---
    s.a_v = 0.25 * B                          # DESIGN: valve off the bore's
    s.d_v = 0.36 * B                          # DESIGN: valve head
    s.r_v = s.d_v / 2.0
    s.lift = 0.25 * s.d_v                     # RULE: L/D 0.25 (curtain = throat)
    s.d_s = at_least("valve stem", max(0.3 * s.d_v, d_stem or 0.0),
                     lines(8))
    s.r_s = s.d_s / 2.0
    s.t_e = at_least("valve head edge", 0.08 * s.d_v, layers(5))
    s.g_seat = side(s.d_v)
    s.r_t = 0.8 * s.r_v                       # DESIGN: throat
    s.r_guide = (s.d_s + gap(s.d_s)) / 2.0
    s.d_bk = 0.4 * B                          # DESIGN: bucket
    s.r_bk = s.d_bk / 2.0
    s.r_bkb = (s.d_bk + gap(s.d_bk)) / 2.0
    s.h_bk = 0.6 * s.d_bk                     # DESIGN: bucket height
    s.t_bkt = at_least("bucket crown", 0.08 * s.d_bk, layers(5))
    s.lash = side(s.d_bk)                     # cam to bucket, the print
    # The model's bucket-to-base-circle air: the lash plus the travel that
    # closes the modelled gap at the seat (a 45 cone, so sqrt 2 of it).
    s.seat_travel = s.g_seat * math.sqrt(2.0)
    s.c_model = s.lash + s.seat_travel
    # --- cam: base circle and a nose circle, their hull, a flat follower ---
    s.dur_cam = (EVC - EVO) / 2.0             # cam degrees open (both alike)
    if abs((IVC - IVO) - (EVC - EVO)) > 1e-9:
        raise ValueError("the lobes are one shape: equal durations")
    half = math.radians(s.dur_cam / 2.0)
    # The lift reaches `lash` at the physical events and lash + lift at the
    # peak: d (1 - cos half) = lift.
    s.cam_d = s.lift / (1.0 - math.cos(half))
    s.d_bar = 0.25 * B                        # DESIGN: the cam bar
    s.r_bar = s.d_bar / 2.0
    s.h_bar = s.r_bar * math.cos(math.radians(45.0))  # its flat
    s.r_bar_b = (s.d_bar + gap(s.d_bar)) / 2.0
    r_n_min = lines(2)
    s.r_base = max(s.r_bar + lines(3),
                   s.cam_d - s.lift - s.lash + r_n_min)
    s.r_nose = s.r_base + s.lift + s.lash - s.cam_d
    s.r_lobe = s.r_base + s.lift + s.lash     # the lobe's reach
    # The follower: the contact runs this far off the bucket's centre.
    dpsi0 = math.degrees(math.acos((s.r_base - s.r_nose) / s.cam_d))
    s.contact_max = s.cam_d * math.sin(math.radians(dpsi0))
    margin = lines(1)
    if s.contact_max + margin > s.r_bk:
        raise ValueError(f"the lobe's contact runs {s.contact_max:.2f} off "
                         f"a {s.r_bk:.2f} bucket")
    s.w_lobe = 2.0 * (math.sqrt(s.r_bk ** 2 - (s.contact_max + margin) ** 2)
                      - margin)
    s.w_lobe = at_least("lobe width", s.w_lobe, lines(6))
    # The one-piece camshaft prints standing, each lobe's underside a 45
    # degree taper of its outline down to the bar: as deep as the base
    # circle stands over the bar, and half a line into it. A taper past the
    # nose circle's radius closes the nose up, which the tapered extrude
    # refuses: the lobes would have to be pressed on a bar again.
    s.d_taper = s.r_base - s.r_bar + lines(1) / 2.0
    if s.d_taper >= s.r_nose:
        raise ValueError(f"a lobe's underside taper ({s.d_taper:.2f} mm) "
                         f"closes its nose (r {s.r_nose:.2f}): no one-piece "
                         "camshaft")
    s.r_trough = s.r_lobe + side(2.0 * s.r_lobe)
    # --- head heights, from its deck (hz) ---
    s.hz_cb = s.t_e + (math.sqrt(2.0) - 1.0) * s.g_seat  # counterbore top
    s.hz_throat = s.hz_cb + (s.r_v + s.g_seat - s.r_t)   # cone to throat
    s.hz_port = s.hz_throat                              # port axis start
    s.hz_guide0 = s.hz_throat + (s.r_t - s.r_guide)      # 45 cone up
    s.w_floor = lines(3)
    s.hz_floor = (s.hz_port + s.r_bkb + math.sqrt(2.0) * s.r_t + s.w_floor)
    s.hz_floor = max(s.hz_floor, s.hz_guide0 + 2.0 * s.d_s)
    # TPU spring: free length from the squeeze at full lift.
    s.l_free = s.lift / (SQUEEZE_LIFT - SQUEEZE_REST)
    s.l_inst = (1.0 - SQUEEZE_REST) * s.l_free   # physical, valve seated
    s.l_full = s.l_inst - s.lift                 # physical, full lift
    # The model's closed bucket sits a seat travel below the seated print.
    s.r_sp_i = s.r_s + lines(2)
    s.r_sp_o = s.r_bkb - lines(2)
    s.t_sp = lines(2)                            # wall, across it
    # --- the timing belt ---
    s.belt_p = lines(10)       # DESIGN: a printed TPU tooth is lines wide
    s.belt_h = 0.24 * s.belt_p         # RULE: T-profile tooth height
    s.belt_base = 0.53 * s.belt_p      # RULE: T-profile base
    s.belt_tip = s.belt_base - 2.0 * s.belt_h * math.tan(math.radians(20.0))
    s.belt_t = lines(4)                # backing
    s.belt_pld = s.belt_t / 2.0        # the pitch line in the backing
    s.belt_w = at_least("belt width", 0.2 * B, lines(10))
    s.r_stub = 0.75 * s.r_j            # DESIGN: crank nose/tail stubs
    # The smallest crank pulley whose root clears the stub.
    z1 = 10
    while True:
        r1 = z1 * s.belt_p / (2.0 * math.pi)
        root = r1 - s.belt_pld - side(2 * r1) - s.belt_h - side(2 * r1)
        if root >= s.r_stub * math.sqrt(2.0) + lines(3) and z1 % 2 == 0:
            break
        z1 += 1
    s.z1, s.z2 = z1, 2 * z1
    s.r_p1 = s.z1 * s.belt_p / (2.0 * math.pi)
    s.r_p2 = s.z2 * s.belt_p / (2.0 * math.pi)
    s.belt_gr = side(2.0 * s.r_p1)      # pulley-belt radial and flank gap
    s.t_fl = at_least("pulley flange", 0.04 * B, layers(6))
    # Along X, front of the block: the cam pulley's flange, the belt, the
    # crank pulley's flange.
    g = s.belt_gr
    s.x_belt1 = s.x_f - g - s.t_fl - g          # the belt's rear edge
    s.x_belt0 = s.x_belt1 - s.belt_w            # and its front edge
    # --- deck and head ---
    s.c_vp = s.c_stack                    # valve to piston, at least
    s.d_v_half = s.dur_cam
    worst = -1e9
    for i in range(0, 7200):
        th = i * 0.1
        drop = (piston_z(s, 0.0, 1) - piston_z(s, th, 1))
        for kind, peak in lobes_of(1):
            worst = max(worst, model_lift(s, th, peak) - drop)
    s.s_q = max(s.c_vp, s.c_vp + worst)   # squish: crown to deck at TDC
    s.worst_lift_over_drop = worst
    s.z_deck = s.L + s.R + s.h_cp + s.s_q
    if 0.1 * s.L <= s.s_q + s.c_vp:
        raise ValueError("a rod 10 % long would not reach the head")
    # The bucket and the cam over the floor (hz), before the belt's length
    # moves the cam up a little.
    hz_u0 = s.hz_floor + s.l_inst - s.seat_travel
    hz_cam = hz_u0 + s.h_bk + s.c_model + s.r_base
    c_min = s.z_deck + hz_cam
    s.belt_n = math.ceil(belt_length(s.r_p1, s.r_p2, c_min) / s.belt_p
                         - 1e-9)
    s.c_belt = belt_centre(s.r_p1, s.r_p2, s.belt_n * s.belt_p)
    s.raise_floor = s.c_belt - c_min      # the guides take it
    s.hz_floor += s.raise_floor
    s.hz_u0 = s.hz_floor + s.l_inst - s.seat_travel   # model, closed
    s.hz_bt = s.hz_u0 + s.h_bk                        # bucket top, closed
    s.hz_cam = s.hz_bt + s.c_model + s.r_base
    s.z_cam = s.z_deck + s.hz_cam
    assert abs(s.z_cam - s.c_belt) < 1e-9
    s.hz_tip = s.hz_bt - s.t_bkt                      # valve's stem tip
    s.l_show = s.hz_u0 - (s.lift + s.lash) - s.hz_floor - side(s.d_bk)
    # --- block ---
    s.t_wall = at_least("crankcase wall", 0.07 * B, lines(5))
    s.y_case = s.r_sweep + s.c_stack          # crankcase, inside, a side
    s.w_case = 2.0 * (s.y_case + s.t_wall)
    s.w_barrel = B + 2.0 * at_least("barrel wall", 0.1 * B, lines(6))
    # The barrels' narrow block flares to the crankcase at 45 degrees.
    s.z_flare = s.z_bb - (s.w_case - s.w_barrel) / 2.0
    if s.z_flare < s.c_stack:
        raise ValueError("the barrels' flare reaches the split")
    s.w_head = s.w_barrel
    # Dowels, everywhere: pressed in both halves.
    s.d_dw = at_least("dowel", 0.1 * B, lines(8))
    s.dw_depth = 1.5 * s.d_dw
    s.l_dw = 2.0 * s.dw_depth - layer
    # The camshaft prints as two halves split along its axis (Y = 0), each
    # lying on its cut: stood on end it is 204 mm on a bar's end, and its
    # lobes cannot lie whole (a flat under them cuts into every direction
    # some lobe lifts in). A dowel in the end lobes keys the halves, as
    # deep into each as leaves three lines over it in the base circle.
    s.key_depth = s.r_base - lines(3)
    s.l_key = 2.0 * s.key_depth - layer
    s.y_dw_deck = s.w_barrel / 2.0 - s.d_dw / 2.0 - lines(4)
    s.y_dw_split = s.y_case + s.t_wall - s.d_dw / 2.0 - lines(3)
    if s.y_dw_split - s.d_dw / 2.0 < s.r_jb + lines(3):
        raise ValueError("the split's dowels meet the main bore")
    # --- bedplate and sump ---
    s.z_bp = -(s.r_sweep + s.c_stack)         # the bedplate's bottom
    s.h_sump = 0.3 * B                        # DESIGN: the sump's depth
    s.t_floor = at_least("sump floor", 0.05 * B, layers(6))
    s.z_sump = s.z_bp - s.h_sump - s.t_floor  # the engine stands here
    # --- flywheel, hand crank, pointer ---
    s.r_fw = -s.z_sump - lines(6)            # clears the table
    s.t_fw = 0.25 * B                         # DESIGN
    s.r_arm = 0.8 * s.r_fw                    # DESIGN
    s.t_arm = at_least("crank arm", 0.12 * B, lines(8))
    s.d_handle = at_least("handle", 0.25 * B, lines(10))
    s.l_handle = 0.8 * B
    s.d_hpin = 0.4 * s.d_handle
    # Stubs, nose and tail: the arm and the pulley on the nose.
    s.l_nose = s.x_f - s.g_ax - (s.x_belt0 - g - s.t_fl - g) + s.t_arm
    s.x_nose = s.x_f - s.g_ax - s.l_nose      # its front end
    s.l_tail = s.t_fw
    s.x_tail = s.x_r + s.g_ax + s.l_tail
    s.d_engrave = lines(2)
    s.y_case_w = s.w_case / 2.0
    # The cover: a cam carrier over the head.
    s.t_cover = at_least("cover top", 0.06 * B, lines(5))
    s.w_cover = 2.0 * (s.r_trough + 2.0 * s.d_dw)
    s.y_dw_cover = s.r_trough + s.d_dw
    s.h_cover = s.r_trough + s.t_cover
    # Cam towers: where the trough is not cut. The lobes' X spans.
    s.lobes = []
    for cyl in (1, 2, 3, 4):
        x = s.x_cyl[cyl - 1]
        for kind, peak in lobes_of(cyl):
            xv = x - s.a_v if kind == "intake" else x + s.a_v
            s.lobes.append({"cyl": cyl, "kind": kind, "x": xv,
                            "peak_crank_deg": peak,
                            "peak_cam_deg": (peak / 2.0) % 360.0})
    s.g_lobe_ax = side(2.0 * s.r_lobe)
    # The rear collar, against the rear tower's back face, holds the cam
    # forward; the pulley's flange against the front face holds it back.
    s.t_collar = at_least("cam collar", 0.15 * B, lines(8))
    s.r_collar = s.r_bar + lines(5)
    s.x_cam1 = s.x_r + s.g_lobe_ax + s.t_collar     # the bar's rear end
    s.x_cam0 = s.x_belt0 - g                        # and its front end
    # The cam's troughs, a cylinder's two lobes in each; towers between.
    # Each trough reaches its first lobe's tapered underside (toward -X).
    s.troughs = [(x - s.a_v - s.w_lobe / 2.0 - s.d_taper - s.g_lobe_ax,
                  x + s.a_v + s.w_lobe / 2.0 + s.g_lobe_ax) for x in s.x_cyl]
    for (a0, a1), (b0, b1) in zip(s.troughs, s.troughs[1:]):
        if b0 - a1 < 2.0 * s.d_dw:
            raise ValueError("the cam towers are too thin")
    x_t0 = (s.x_f + s.troughs[0][0]) / 2.0
    x_t4 = (s.troughs[-1][1] + s.x_r) / 2.0
    s.dowels_split = [(s.x_main[0], y) for y in (s.y_dw_split,
                                                 -s.y_dw_split)] + \
        [(s.x_main[-1], y) for y in (s.y_dw_split, -s.y_dw_split)]
    s.dowels_deck = [(s.x_main[0], y) for y in (s.y_dw_deck,
                                                -s.y_dw_deck)] + \
        [(s.x_main[-1], y) for y in (s.y_dw_deck, -s.y_dw_deck)]
    s.dowels_cover = [(x_t0, y) for y in (s.y_dw_cover, -s.y_dw_cover)] + \
        [(x_t4, y) for y in (s.y_dw_cover, -s.y_dw_cover)]
    # The crankcase inside narrows with the flare to the bores' width.
    s.y_rect = max(B / 2.0, s.w_barrel / 2.0 - s.t_wall * math.sqrt(2.0))
    # Cutaway windows into each bay and bore, +Y; pointed at both ends.
    s.w_window = (s.pitch - s.b_t) - 2.0 * lines(4)
    s.z_win = (0.3 * s.z_bb, s.z_bb + 0.55 * (s.z_deck - s.z_bb))
    if s.z_win[1] - s.z_win[0] < s.w_window + lines(4):
        raise ValueError("the window is too short for its width")
    # The TDC pointer over the flywheel's rim, on the block's rear face.
    s.pointer = lines(4)
    s.tick, s.tick_w = lines(5), lines(2)
    s.label_h = lines(6)
    s.z_pointer = s.r_fw + lines(3) + s.pointer
    if s.z_pointer + s.pointer > s.z_deck:
        raise ValueError("the pointer is over the deck")
    s.r_arm_hub = max(s.r_stub * math.sqrt(2.0) + lines(4),
                      s.d_hpin / 2.0 + lines(4))
    # The belt's teeth on the pitch line, one centred under the crank
    # pulley; the pulleys' grooves where they fall.
    s.belt_path = path = BeltPath(s.r_p1, s.r_p2, s.c_belt)
    s0 = (s.r_p1 * (math.pi / 2.0 - path.phi)) % s.belt_p
    s.belt_teeth = []
    s.phase1 = 1.5 * math.pi
    s.phase2 = None
    for k in range(s.belt_n):
        pt, tg, nm, seg, ang = path.at(s0 + k * s.belt_p)
        s.belt_teeth.append((pt, tg, nm))
        if seg == 2 and s.phase2 is None:
            s.phase2 = ang
    sweep_check(s)
    # The valve prints standing on its head: over the stem's foot (the top
    # of the head's cone) stands the rest of it, which the stem carries.
    s.slender = slender
    s.z_stem0 = s.t_e + s.r_v - s.r_s
    # check_print reads the stem's width off its tessellation, a chord's
    # sagitta inside the round: a fiftieth over the limit's.
    need = 1.02 * (s.hz_tip - s.z_stem0) / slender
    if s.d_s < need - 1e-9:
        # A thicker stem lengthens the valve a little (the port floor
        # stands two stems over the guide's foot): solved again, in tenths,
        # until the stem carries what stands over it.
        tries = getattr(s, "stem_tries", 0)
        if tries >= 20:
            raise ValueError(f"the valve stem stays slender: Ø{s.d_s:.2f} "
                             f"under {s.hz_tip - s.z_stem0:.1f} mm")
        t = solve(bore, allow, layer, slender,
                  max(math.ceil(need * 10.0) / 10.0, s.d_s + 0.1),
                  tries + 1)
        return t
    return s


def cavity_half(s, z):
    """The crankcase's inside half-width at height z (below the bores)."""
    hw = s.w_case / 2.0
    za = s.z_flare - s.t_wall * (math.sqrt(2.0) - 1.0)
    if z <= za:
        return s.y_case
    return max(s.y_rect, s.y_case - (z - za))


def rod_points(s):
    """The rod's outline in its own (y, z), sampled."""
    pts = [(sy * s.y_ear, sz * s.z_lap) for sy in (1, -1) for sz in (1, -1)]
    for k in range(36):
        a = math.radians(10.0 * k)
        pts.append((s.r_be * math.cos(a), s.r_be * math.sin(a)))
        pts.append((s.r_se * math.cos(a), s.L + s.r_se * math.sin(a)))
    for f in [i / 10.0 for i in range(11)]:
        w = s.w_shank0 + (s.w_shank1 - s.w_shank0) * f
        pts += [(w / 2.0, s.L * f), (-w / 2.0, s.L * f)]
    return pts


def sweep_check(s):
    """The rods and webs over a turn, in (y, z) against the crankcase,
    before anything is built: everything below the bores' ends stays
    c_stack inside the walls."""
    worst = 1e9
    # In a bore the rod's corners (w_r/2 off the bore's axis) meet a
    # chord, not the radius: a 45-degree notch at the bore's mouth, across
    # the rod's width, gives them `e_mouth` more at z_bb, less higher up.
    chord = math.sqrt((s.bore / 2.0) ** 2 - (s.w_r / 2.0) ** 2)
    s.e_mouth = 0.0
    pts = rod_points(s)
    for i in range(360):
        th = math.radians(i)
        py, pz = -s.R * math.sin(th), s.R * math.cos(th)
        wz = pz + math.sqrt(s.L ** 2 - py ** 2)
        u = (-py / s.L, (wz - pz) / s.L)
        up = (u[1], -u[0])
        for yr, zr in pts:
            y = py + zr * u[0] + yr * up[0]
            z = pz + zr * u[1] + yr * up[1]
            if z < s.z_bb:
                worst = min(worst, cavity_half(s, z) - abs(y))
            elif abs(y) + s.c_stack > chord:
                s.e_mouth = max(s.e_mouth, abs(y) + s.c_stack - chord
                                + (z - s.z_bb))
    s.sweep_margin = worst
    if worst < s.c_stack - 1e-9:
        raise ValueError(f"a rod comes {worst:.2f} mm off the crankcase "
                         f"wall")
    if s.e_mouth > s.w_barrel / 2.0 - s.bore / 2.0 - lines(3):
        raise ValueError(f"the bores' mouths need {s.e_mouth:.2f} mm")


def detail_holds(s):
    """Whether every size printed: solve() raises where one cannot (a lobe
    whose contact runs off its bucket, dowels in a bore). A size raised to
    its printing floor still prints, and s.floors says which were."""
    return True


def print_footprints(s):
    """Every printed piece's footprint on the bed as it prints, (w, h) in
    mm, with how many: what the bore search packs before anything is
    built. The parts' own boxes are checked against it after."""
    L = s.x_r - s.x_f
    dz_block = s.z_deck
    head_h = s.hz_cam
    items = [
        ("block", (L, s.w_case), 1),
        ("bedplate", (L, s.w_case), 1),
        ("sump", (L, s.w_case), 1),
        ("head", (L, s.w_head), 1),
        ("cam cover", (s.x_cam1 - s.x_f, s.w_cover), 1),
        ("crankshaft", (s.x_tail - s.x_nose,
                        2.0 * (s.R + s.r_web)), 1),
        ("rod", (2.0 * s.y_ear, s.L + s.r_se), 4),
        ("rod cap", (2.0 * s.y_ear, s.r_be), 4),
        ("cap pin", (s.d_cp, s.d_cp), 8),
        ("piston", (s.d_p, s.d_p), 4),
        ("wrist pin", (s.d_wp, s.d_wp), 4),
        ("valve", (s.d_v, s.d_v), 8),
        ("bucket", (s.d_bk, s.d_bk), 8),
        ("camshaft half", (s.x_cam1 - s.x_cam0, 2 * s.r_lobe), 2),
        ("cam key", (s.d_dw, s.d_dw), 2),
        ("cam pulley", (2 * (s.r_p2 + s.belt_t), 2 * (s.r_p2 + s.belt_t)),
         1),
        ("crank pulley", (2 * (s.r_p1 + s.belt_t), 2 * (s.r_p1 + s.belt_t)),
         1),
        ("cam collar", (2 * s.r_collar, 2 * s.r_collar), 1),
        ("flywheel", (2 * s.r_fw, 2 * s.r_fw), 1),
        ("crank arm", (s.r_arm + 2 * s.d_handle, 2 * s.d_handle), 1),
        ("handle", (s.d_handle, s.d_handle), 1),
        ("dowel", (s.d_dw, s.d_dw), 16),
    ]
    del dz_block, head_h
    return items


def pack(items, bed, gap, zones=()):
    """MaxRects, best short side (generators/strandbeest/build.py's packer,
    on rectangles): each (key, w, h) onto beds of bed[0] x bed[1], `gap`
    between boxes and from the edges and from each of the bed's keep-out
    `zones` [x0, y0, x1, y1], each turned a quarter if that fits better.
    Returns beds of [(key, turned, x, y)], (x, y) the low corner of the
    footprint as placed."""
    W, H = bed[0] - gap, bed[1] - gap
    order = sorted(items, key=lambda it: -(it[1] * it[2]))
    beds = []

    def place(free, w, h):
        best = None
        for fi, (fx, fy, fw, fh) in enumerate(free):
            if w <= fw + 1e-9 and h <= fh + 1e-9:
                score = min(fw - w, fh - h)
                if best is None or score < best[0]:
                    best = (score, fx, fy)
        return best

    def split(free, x, y, w, h):
        out = []
        for fx, fy, fw, fh in free:
            if x >= fx + fw or x + w <= fx or y >= fy + fh or y + h <= fy:
                out.append((fx, fy, fw, fh))
                continue
            if x > fx:
                out.append((fx, fy, x - fx, fh))
            if x + w < fx + fw:
                out.append((x + w, fy, fx + fw - x - w, fh))
            if y > fy:
                out.append((fx, fy, fw, y - fy))
            if y + h < fy + fh:
                out.append((fx, y + h, fw, fy + fh - y - h))
        keep = []
        for i, a in enumerate(out):
            inside = any(i != j and a[0] >= b[0] - 1e-9 and a[1] >= b[1] -
                         1e-9 and a[0] + a[2] <= b[0] + b[2] + 1e-9 and
                         a[1] + a[3] <= b[1] + b[3] + 1e-9 and
                         (a != b or j < i) for j, b in enumerate(out))
            if not inside:
                keep.append(a)
        return keep

    def try_bed(b, key, w, h):
        for turned, (bw, bh) in ((False, (w + gap, h + gap)),
                                 (True, (h + gap, w + gap))):
            spot = place(b["free"], bw, bh)
            if spot:
                _, fx, fy = spot
                b["items"].append((key, turned, fx + gap, fy + gap))
                b["free"] = split(b["free"], fx, fy, bw, bh)
                return True
        return False

    for key, w, h in order:
        if all(not try_bed(b, key, w, h) for b in beds):
            # A box at free-space x spans the bed's x + gap .. x + w + gap,
            # so a zone taken out from x0 - gap to x1 keeps a gap round it.
            free = [(0.0, 0.0, W, H)]
            for z in zones:
                free = split(free, z[0] - gap, z[1] - gap,
                             z[2] - z[0] + gap, z[3] - z[1] + gap)
            b = {"free": free, "items": []}
            if not try_bed(b, key, w, h):
                raise ValueError(f"{key} ({w:.0f} x {h:.0f} mm) does not "
                                 f"fit the {bed[0]:g} x {bed[1]:g} mm bed")
            beds.append(b)
    return [b["items"] for b in beds]


def spacing(s):
    """Between pieces on a plate: wide enough for a slicer's skirt and a
    clean first layer (DESIGN), and far over the gap the clearance item
    asks."""
    return lines(10)


def plates_needed(s, bed, zones=()):
    items = []
    for name, (w, h), n in print_footprints(s):
        for k in range(n):
            items.append(((name, k), w, h))
    return len(pack(items, bed, spacing(s), zones))


def search_bore(table, layer, bed, slender, lo=20, hi=60, zones=()):
    """The default bore: the largest whole millimetre at which the PLA
    pieces pack into two beds and every size still prints (solve() does
    not refuse it), else the largest that packs into three. Returns (bore,
    plates, why, tried)."""
    tried = []
    best = {}
    for b in range(hi, lo - 1, -1):
        try:
            s = solve(b, table, layer, slender)
            n = plates_needed(s, bed, zones)
        except ValueError as exc:
            tried.append((b, None, str(exc)))
            continue
        tried.append((b, n, ", ".join(f[0] for f in s.floors)))
        for k in (2, 3):
            if n <= k and k not in best:
                best[k] = b
    if 2 in best:
        return best[2], 2, "the largest bore whose pieces pack in two", tried
    if 3 in best:
        return best[3], 3, ("no bore whose sizes all print packs into two "
                            "plates"), tried
    raise ValueError(f"no bore from {lo} to {hi} mm packs into three "
                     f"plates: {tried}")
