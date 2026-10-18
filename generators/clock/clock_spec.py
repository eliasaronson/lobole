# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every size of the printed pendulum clock, from the pendulum's period and
the print profile, and the escapement's geometry and motion.

Nothing here talks to the app: `solve()` takes the profile's allowances (a
function the generator answers from `print_allowance`) and returns one dict,
and `escapement_sweep()` walks the escapement's own polygons through a cycle
before a part is built, so a geometry that cannot clear is refused here, in
a second, rather than found by a motion study. The study is still the proof.

Coordinates, the MOVEMENT frame every part is modelled in: seen from the
front, X right, Y up, Z out of the wall towards the viewer; Z = 0 is the
back plate's inner face and Z = H the front plate's. The centre arbor (the
minute hand) is at the origin, the escape wheel straight above it and the
anchor above that; the pendulum hangs from the anchor arbor behind the back
plate. A positive rotation is counter-clockwise seen from the front: the
escape wheel goes CLOCKWISE (negative), so the centre wheel, two meshes on,
turns clockwise too and the hands go the right way.
"""
import math

NOZZLE_MM = 0.4      # the P1S's stock nozzle, which the profiles assume
G = 9.80665          # standard gravity, m/s^2
PLA_G_CC = 1.24      # PLA's density (the app's `pla`), g/cm^3

# --- the escapement: a Graham deadbeat --------------------------------------
Z_ESCAPE = 30        # the plan's: a tooth a period, so one turn in 30 periods
SPAN_TEETH = 7.5     # the anchor spans 7.5 teeth: 90 degrees of the wheel
FRONT_UNDERCUT_DEG = 10.0  # a Graham tooth's front leans back 10 degrees so
#                            only its tip corner meets a locking face
# --- the going train ----------------------------------------------------------
MIN_LEAVES = 8       # 8 leaves or more: a clock pinion of fewer engages
MAX_LEAVES = 10      # ahead of the line of centres and runs on friction
MAX_TEETH = 120      # the largest wheel the layout is asked to fit
TOOTH_FRAC_WHEEL = 0.47   # as the watch: a wheel tooth's share of the pitch
ADDENDUM_WHEEL = 1.0      # modules: a clock wheel's, not the watch's 1.3,
#                           which leaves a printed tip under two lines
ADDENDUM_LEAF = 0.6       # a pinion is driven: its addendum only clears
ROOT_CLEARANCE = 0.3      # modules between a root and the mate's tip
MIN_CONTACT_RATIO = 1.1   # as the watch's
# The motion works: cannon pinion -> minute wheel, minute pinion -> hour
# wheel, on the same two axes, so their tooth sums match (10+30 = 8+32),
# and 3 x 4 = 12. The watch's own counts.
Z_CANNON, Z_MINUTE_W, Z_MINUTE_P, Z_HOUR = 10, 30, 8, 32
N_RATCHET = 24       # the click's ratchet teeth: 15 degrees of back-wind
N_PAWLS = 2          # two flexure pawls, opposite: balanced, and one holds
#                      while the other rides a tooth

SAND_G_ML = 1.6      # dry sand's bulk density, g/ml (1.5-1.7)
RATING_TRAVEL = 0.02  # the rating nut's travel either way, a fraction of the
#                       pendulum: +-1 % of the period, which covers a bob
#                       printed at a different infill from the rod
ARM_LINES = 6        # the anchor's arms, in extrusion lines across
WALL_LINES = 4       # a wall round a bore, in lines
ARBOR_LINES = 12     # an arbor's diameter in lines: 4.8 mm (it prints
#                      lying on a flat: stood up, it came off the bed)


def lines(n):
    return n * NOZZLE_MM


def up_to(mm, step):
    """mm rounded UP to whole steps (layers, lines, half millimetres)."""
    return step * math.ceil(mm / step - 1e-9)


def lie_depth(r, layer):
    """How deep a flat a round of radius `r` lies on is cut, from the
    round: whole layers, and at least to where the round meets it at 45
    degrees, so nothing over the flat faces down past 45."""
    return up_to(r * (1.0 - math.cos(math.radians(45.0))), layer)


def deg(r):
    return math.degrees(r)


def rad(d):
    return math.radians(d)


# --- the train ------------------------------------------------------------------

def train_counts(period_s):
    """Two stages from the escape wheel to the centre wheel (one turn an
    hour), whose product is exactly the hour over the escape wheel's turn.
    The smallest wheels with pinions of MIN_LEAVES or more; fewer only if no
    pair exists, then refused."""
    escape_turn_s = Z_ESCAPE * period_s
    want = 3600.0 / escape_turn_s
    found = []
    for leaves in (range(MIN_LEAVES, MAX_LEAVES + 1), range(7, MIN_LEAVES),
                   range(6, 7)):
        for pe in leaves:
            for p3 in leaves:
                for zc in range(p3 * 6, MAX_TEETH + 1):
                    num = want * pe * p3 / zc
                    z3 = round(num)
                    if z3 < pe * 6 or z3 > MAX_TEETH:
                        continue
                    if abs(zc * z3 / (pe * p3) - want) > 1e-9:
                        continue
                    # The third wheel turns inside the centre wheel's reach:
                    # it must be the smaller, by more than its pinion.
                    if z3 + 2 > zc + p3 - 4:
                        continue
                    found.append((zc + z3, -min(pe, p3), zc, z3, p3, pe))
        if found:
            break
    if not found:
        raise SystemExit(
            f"clock: no two-stage train turns the centre wheel once an hour "
            f"from a {Z_ESCAPE}-tooth escape wheel at {period_s:g} s "
            f"(escape to centre {want:.4f}:1, wheels up to {MAX_TEETH}, "
            f"pinions 6-{MAX_LEAVES}); a period whose 3600/{Z_ESCAPE}T is a "
            f"ratio of whole numbers does, e.g. 1, 0.8, 1.2, 1.5 or 2 s")
    found.sort()
    _, _, zc, z3, p3, pe = found[0]
    return {"centre": zc, "third": z3, "third_pinion": p3,
            "escape_pinion": pe, "escape_to_centre": want}


def design_gearing(module, counts, backlash_mm, clearance_mm):
    """Every tooth set, as cycloidal.pair() designs a pair. The wheel takes
    TOOTH_FRAC_WHEEL of the pitch and the leaf what the backlash leaves."""
    import cycloidal as C
    frac_leaf = 1.0 - TOOTH_FRAC_WHEEL - backlash_mm / (math.pi * module)
    sets = {}
    for wheel, zw, leaf, zl in (
            ("centre", counts["centre"], "third_pinion",
             counts["third_pinion"]),
            ("third", counts["third"], "escape_pinion",
             counts["escape_pinion"]),
            ("minute_wheel", Z_MINUTE_W, "cannon", Z_CANNON),
            ("hour", Z_HOUR, "minute_pinion", Z_MINUTE_P)):
        # The motion works' cannon DRIVES its wheel, as the train's wheels
        # drive their pinions; a 10- or 8-tooth set there is a leaf too.
        a, b = C.pair(module, zw, zl, TOOTH_FRAC_WHEEL, frac_leaf,
                      ADDENDUM_WHEEL, ADDENDUM_LEAF, clearance_mm)
        sets[wheel], sets[leaf] = a, b
        a["mate"], b["mate"] = leaf, wheel
    return sets, frac_leaf


def choose_module(counts, backlash_mm, clear_side_mm, bore_r):
    """The smallest module, in tenths, whose every tip is two lines across,
    every pair keeps a contact ratio of MIN_CONTACT_RATIO, and every
    pinion's root leaves two lines round the bore it is pressed on by."""
    import cycloidal as C
    why = []
    for tenth in range(8, 31):
        m = tenth / 10.0
        clearance = max(ROOT_CLEARANCE * m, clear_side_mm)
        sets, frac_leaf = design_gearing(m, counts, backlash_mm, clearance)
        bad = []
        if frac_leaf < 0.3:
            bad.append(f"a leaf {frac_leaf:.2f} of the pitch")
        for k, g in sets.items():
            land = C.tip_land(m, g)
            if land < lines(2) - 1e-9:
                bad.append(f"{k} tip {land:.2f} mm")
            root = g["rp"] - g["h_f"]
            if root < bore_r + lines(2) - 1e-9:
                bad.append(f"{k} root r{root:.2f}")
        for w in ("centre", "third", "minute_wheel", "hour"):
            cr = C.contact_ratio(m, sets[w], sets[sets[w]["mate"]])
            if cr < MIN_CONTACT_RATIO:
                bad.append(f"{w} contact {cr:.2f}")
        if not bad:
            return m, sets, clearance
        why.append(f"m{m:g}: " + ", ".join(bad))
    raise SystemExit("clock: no module up to 3 prints every tooth: "
                     + "; ".join(why[-3:]))


# --- the escapement ---------------------------------------------------------------

def escapement(g, g_small, play, hub_r):
    """The Graham escapement at the smallest swing whose own sweep is clean:
    `escapement_at` for one lock of supplementary arc past the impulse and
    the lock, then a quarter of a lock more at a time, the escape wheel
    following the pallets (window()), the first whose polygons clear by a
    fifth of the gap twice over and lock the rest gap off a face at every
    beat's end. Its window and sweep come with it (`window`, `sweep`), and
    the swings tried and why each failed (`swing_tried`)."""
    tried = []
    for quarter in range(4, 21):
        k = quarter / 4.0
        e = escapement_at(g, g_small, play, k)
        e["hub_r"] = hub_r
        try:
            win = window(e)
        except SystemExit as why:
            tried.append(f"{k:g} lock(s): {why}")
            continue
        sweep = escapement_sweep(e, win, 90, 2)
        rest = [dm for _, dm, _ in sweep["ends"]]
        if sweep["min_mm"] < 2.0 * g / 5.0:
            tried.append(f"{k:g} lock(s): {sweep['min_mm']:.3f} mm least")
            continue
        if any(abs(r - e["rest_gap_mm"]) > 0.02 for r in rest):
            tried.append(f"{k:g} lock(s): not locked at a beat's end")
            continue
        e.update(window=win, sweep=sweep, swing_tried=tried)
        return e
    raise SystemExit("clock: no swing up to five locks of supplementary arc "
                     "clears: " + "; ".join(tried))


def escapement_at(g, g_small, play, supplementary):
    """The Graham escapement's every size, from the tooth pitch it needs,
    at a swing `supplementary` locks past the impulse and the lock.

    A tooth's tip is two lines across; at rest it stands `g` (the gap a
    side) off the locking face it locks on; the DROP, the free flight after a
    tooth leaves an impulse face, is the `play` (every gap a side the chain
    from tooth to pallet holds: two printed faces and two arbors in their
    holes); the pallet's impulse face is at least four lines wide. Half the
    tooth pitch holds all four, which sizes the wheel."""
    t_tip = lines(2)
    m_rest = g
    drop = play
    w_min = lines(4)
    R = up_to(Z_ESCAPE * (t_tip + m_rest + drop + w_min) / math.pi, 1.0)
    half = math.pi * R / Z_ESCAPE
    w = half - t_tip - m_rest - drop
    lift = w / R                  # rad: the impulse face at 45 degrees
    lock = drop / R               # rad: a tooth lands `drop` deep
    delta = (lock - lift) / 2.0   # where each locking corner stands
    # The swing: the impulse and the lock, and `supplementary` locks of
    # arc past them (escapement() takes the fewest that sweep clean). A
    # wheel stepping uniformly needed two, to keep behind a face that ran
    # away from it; following the face (window()) it needs no more than
    # the escapement does.
    amp = (lift + lock) / 2.0 + supplementary * lock
    e = {"R": R, "Z": Z_ESCAPE, "pitch_deg": 360.0 / Z_ESCAPE,
         "half_pitch_mm": half, "tip_mm": t_tip, "rest_gap_mm": m_rest,
         "drop_mm": drop, "pallet_w": w, "lift_deg": deg(lift),
         "lock_deg": deg(lock), "delta_deg": deg(delta),
         "amplitude_deg": deg(amp), "supplementary_locks": supplementary,
         "anchor_d": R * math.sqrt(2.0),
         "arm_clear_mm": play, "arm_w": lines(ARM_LINES)}
    e["kappa_deg"] = e["amplitude_deg"] + deg(e["arm_clear_mm"] / R)
    e["tooth_h"] = tooth_height(e, g)
    return e


def tooth_height(e, g):
    """The tooth's depth: where the tooth ahead's back, sloping from its
    trailing corner to the next tooth's front root, still clears the
    pallet's deep corner by the gap a side at the pallet's deepest — the
    resting tooth on the locking face, the swing at its end."""
    R, w = e["R"], e["pallet_w"]
    depth = R * rad(e["delta_deg"] + e["lift_deg"] + e["amplitude_deg"])
    s_lead = -w / 2.0 - e["rest_gap_mm"]           # the resting tooth
    s_tc = s_lead + 2.0 * e["half_pitch_mm"] - e["tip_mm"]  # the one ahead
    u = math.tan(rad(FRONT_UNDERCUT_DEG))
    for tenth in range(int(depth * 10), 400):
        h = tenth / 10.0
        s_root = s_lead - h * u
        back = s_tc - (s_tc - s_root) * depth / h
        if back >= w / 2.0 + g + e["rest_gap_mm"]:
            return up_to(h * 1.15, 0.1)      # and 15 % for the arc
    raise SystemExit("clock: no tooth depth clears the pallets")


def wheel_outline(e, phase_deg=0.0, n_arc=4):
    """The escape wheel's outline as polar-then-XY points about its centre,
    one closed polygon, at rotation 0 plus `phase_deg`. Tooth k's leading
    tip corner stands at angle theta0 - 12k (the wheel turns clockwise: its
    leading corners are its lower angles). theta0 is chosen so that at
    rotation 0 a tooth rests on the left (entry) pallet `rest_gap_mm` off its
    locking face."""
    R, h = e["R"], e["tooth_h"]
    pitch = rad(e["pitch_deg"])
    tau = e["tip_mm"] / R
    eps = math.atan(h * math.tan(rad(FRONT_UNDERCUT_DEG)) / (R - h))
    # At rest on the left pallet the leading corner is at path coordinate
    # s = -w/2 - rest, i.e. that far before the tangent point at 135 deg.
    s = -e["pallet_w"] / 2.0 - e["rest_gap_mm"]
    theta0 = rad(135.0) - s / R + rad(phase_deg)
    pts = []
    for k in range(Z_ESCAPE):
        th = theta0 + pitch * k
        root = (R - h, th + eps)
        pts.append(root)
        pts.append((R, th))
        for i in range(1, n_arc):
            pts.append((R, th + tau * i / n_arc))
        pts.append((R, th + tau))
    return [(r * math.cos(a), r * math.sin(a)) for r, a in pts], theta0


def wheel_teeth_polys(e, rotation_deg, n_arc=4):
    """Each tooth as its own closed polygon (front root, tip, back root) for
    the sweep: a tooth is convex enough that distance and overlap between
    polygons is exact on it."""
    R, h = e["R"], e["tooth_h"]
    pitch = rad(e["pitch_deg"])
    tau = e["tip_mm"] / R
    eps = math.atan(h * math.tan(rad(FRONT_UNDERCUT_DEG)) / (R - h))
    s = -e["pallet_w"] / 2.0 - e["rest_gap_mm"]
    theta0 = rad(135.0) - s / R + rad(rotation_deg)
    out = []
    for k in range(Z_ESCAPE):
        th = theta0 + pitch * k
        poly = [(R - h, th + eps), (R, th)]
        poly += [(R, th + tau * i / n_arc) for i in range(1, n_arc)]
        poly += [(R, th + tau), (R - h, th + pitch + eps),
                 (R - h, th + pitch / 2.0 + eps)]
        out.append([(r * math.cos(a), r * math.sin(a)) for r, a in poly])
    return out


def pallet_outline(e, side, n_arc=6, deeper_deg=0.0, hub_r=None):
    """One arm and pallet of the anchor, in the ANCHOR's frame (its pivot at
    the origin, the wheel's centre at (0, -anchor_d)), at rotation 0.

    In polar (rho, phi) about the pivot, the tooth's path past a pallet is
    the tangent from the pivot to the wheel: phi = 225 deg on the left
    (entry), 315 on the right (exit). psi, the angle INTO the wheel from
    that line, is (phi - 225) on the left and (315 - phi) on the right. The
    pallet is the band rho in [R - w/2, R + w/2]: its locking face the arc
    on the side the tooth comes from (the outer on the left, the inner on
    the right), ending at its locking corner at psi = delta; the impulse
    face runs from there to the deep corner at psi = delta + lift on the
    other arc. The arm is the wedge from the hub to the band, outside the
    line by kappa (the swing and a play) so no tooth tip reaches it.
    `deeper_deg` sets the pallet that much deeper: the negative control."""
    R, w = e["R"], e["pallet_w"]
    rho1, rho2 = R - w / 2.0, R + w / 2.0
    base, sgn = (225.0, 1.0) if side == "left" else (315.0, -1.0)
    rho_lock, rho_deep = (rho2, rho1) if side == "left" else (rho1, rho2)
    d0 = e["delta_deg"] + deeper_deg
    lift = e["lift_deg"]
    kappa = e["kappa_deg"]
    kw = deg(e["arm_w"] / rho2)
    r_h = hub_r if hub_r is not None else e["hub_r"]

    def P(r, psi):
        a = rad(base + sgn * psi)
        return (r * math.cos(a), r * math.sin(a))

    def arc(r, psi0, psi1):
        return [P(r, psi0 + (psi1 - psi0) * i / n_arc)
                for i in range(1, n_arc)]

    pts = [P(r_h, -kappa), P(rho1, -kappa)]
    if rho_deep == rho1:
        pts += arc(rho1, -kappa, d0 + lift) + [P(rho1, d0 + lift),
                                               P(rho2, d0)]
        pts += arc(rho2, d0, -kappa - kw)
    else:
        pts += arc(rho1, -kappa, d0) + [P(rho1, d0), P(rho2, d0 + lift)]
        pts += arc(rho2, d0 + lift, -kappa - kw)
    pts += [P(rho2, -kappa - kw), P(r_h, -kappa - kw)]
    if side == "right":
        pts.reverse()
    return pts


# --- kinematics: the stand-in for the escapement's dynamics ------------------------

def anchor_deg(e, beat_deg):
    """The anchor's (and pendulum's) turn for the hidden beat's angle: a
    period is one turn of the beat, the swing A cos(beat), so beat 0 is the
    counter-clockwise end of a swing, a tooth resting on the left pallet."""
    return e["amplitude_deg"] * math.cos(rad(beat_deg))


def cam_table(e, step_deg=2.0):
    n = int(round(360.0 / step_deg))
    return [[i * step_deg, anchor_deg(e, i * step_deg)] for i in range(n)]


def pallet_clearance(e, beat_deg, wheel_deg, deeper_deg=0.0):
    """The least distance from either pallet to any tooth, the anchor where
    the beat puts it and the wheel at `wheel_deg`, in the escapement's own
    polygons: the wheel's centre at the origin, the anchor's pivot at
    (0, anchor_d)."""
    P = (0.0, e["anchor_d"])
    a = anchor_deg(e, beat_deg)
    teeth = wheel_teeth_polys(e, wheel_deg)
    dmin = float("inf")
    for side in ("left", "right"):
        q = _xf([(x, y + P[1]) for x, y in
                 pallet_outline(e, side, deeper_deg=deeper_deg)], a, P,
                (0.0, 0.0))
        cx = sum(x for x, _ in q) / len(q)
        cy = sum(y for _, y in q) / len(q)
        for t in teeth:
            tx, ty = t[1]
            if math.hypot(tx - cx, ty - cy) > 3.0 * e["tooth_h"] + 10:
                continue
            dmin = min(dmin, poly_distance(q, t))
    return dmin


def window(e, samples=60):
    """Where in the beat the escape wheel steps (the intermittent relation's
    window) and its law, a table (docs/assemblies/relations.md#intermittent).
    It opens where the left pallet's locking corner passes the resting tooth
    (anchor at -delta on the way down). From there the wheel FOLLOWS the
    pallets: at each of `samples` beats to the swing's end it stands as far
    on as it can go without coming nearer a pallet than the rest gap does
    across a 45° impulse face (rest_gap/√2) -- behind the impulse face,
    past its deep corner on the tooth's tip land, and then the drop -- and
    the window closes where it has gone the whole half tooth. Half a tooth
    a beat, clockwise."""
    A = e["amplitude_deg"]
    if A <= abs(e["delta_deg"]):
        raise SystemExit("the swing does not unlock")
    start = deg(math.acos(-e["delta_deg"] / A))
    step = -e["pitch_deg"] / 2.0
    keep = e["rest_gap_mm"] / math.sqrt(2.0)
    fine = 0.02  # of the step: how finely the way on is walked

    def clear(b, sv):
        # Both beats: the entry pallet's impulse, then the exit's, which
        # slants the other way, by the same law half a turn of the beat on.
        return min(pallet_clearance(e, b, step * sv),
                   pallet_clearance(e, b + 180.0, step * (1.0 + sv)))
    pts = [(start, 0.0)]
    s_at = 0.0
    for i in range(1, samples + 1):
        b = start + (180.0 - start) * i / samples
        s_new = s_at
        while s_new < 1.0:
            nxt = min(1.0, s_new + fine)
            if clear(b, nxt) < keep:
                lo, hi = s_new, nxt
                for _ in range(12):
                    mid = 0.5 * (lo + hi)
                    if clear(b, mid) < keep:
                        hi = mid
                    else:
                        lo = mid
                s_new = lo
                break
            s_new = nxt
        if s_new > s_at + 1e-12 or s_new >= 1.0:
            pts.append((b, s_new))
        s_at = s_new
        if s_at >= 1.0:
            break
    if s_at < 1.0:
        raise SystemExit("the wheel cannot go its half tooth within a beat")
    end = pts[-1][0]
    dur = end - start
    table = [[0.0, 0.0]]
    for b, sv in pts[1:]:
        table.append([(b - start) / dur, sv])
    table[-1] = [1.0, 1.0]
    return {"start_deg": start, "duration_deg": dur, "step_deg": step,
            "period_deg": 180.0, "table": table}


def table_law(table, t):
    """The window's progress at fraction t of it, linear between the
    table's points: IntermittentLaw in src/core/Occurrence.cpp."""
    t = min(max(t, 0.0), 1.0)
    for (f0, s0), (f1, s1) in zip(table, table[1:]):
        if t <= f1:
            return s0 + (s1 - s0) * (t - f0) / (f1 - f0)
    return table[-1][1]


def escape_deg(e, win, beat_deg):
    """The escape wheel's turn at the beat, as the intermittent relation
    computes it (table law, phase 0): IntermittentValue in
    src/core/Occurrence.cpp, docs/assemblies/relations.md#intermittent."""
    x = (beat_deg - win["start_deg"]) / win["period_deg"]
    n = math.floor(x)
    u = (x - n) * win["period_deg"]
    s = (table_law(win["table"], u / win["duration_deg"])
         if u < win["duration_deg"] else 1.0)
    return win["step_deg"] * (n + s)


# --- 2-D geometry for the sweep -----------------------------------------------------

def _seg_dist(p, q, a, b):
    def pt_seg(pt, s0, s1):
        dx, dy = s1[0] - s0[0], s1[1] - s0[1]
        L2 = dx * dx + dy * dy
        t = 0.0 if L2 == 0 else max(0.0, min(1.0, (
            (pt[0] - s0[0]) * dx + (pt[1] - s0[1]) * dy) / L2))
        return math.hypot(pt[0] - s0[0] - t * dx, pt[1] - s0[1] - t * dy)

    def cross(o, a_, b_):
        return (a_[0] - o[0]) * (b_[1] - o[1]) - (a_[1] - o[1]) * (b_[0] - o[0])
    d1, d2 = cross(p, q, a), cross(p, q, b)
    d3, d4 = cross(a, b, p), cross(a, b, q)
    if d1 * d2 < 0 and d3 * d4 < 0:
        return 0.0
    return min(pt_seg(p, a, b), pt_seg(q, a, b), pt_seg(a, p, q),
               pt_seg(b, p, q))


def _inside(pt, poly):
    x, y = pt
    c = False
    n = len(poly)
    for i in range(n):
        x1, y1 = poly[i]
        x2, y2 = poly[(i + 1) % n]
        if (y1 > y) != (y2 > y):
            if x < x1 + (y - y1) * (x2 - x1) / (y2 - y1):
                c = not c
    return c


def poly_distance(A, B):
    """Least distance between two closed polygons; 0 when they overlap."""
    if _inside(A[0], B) or _inside(B[0], A):
        return 0.0
    best = float("inf")
    for i in range(len(A)):
        p, q = A[i], A[(i + 1) % len(A)]
        for j in range(len(B)):
            best = min(best, _seg_dist(p, q, B[j], B[(j + 1) % len(B)]))
            if best == 0.0:
                return 0.0
    return best


def _xf(pts, deg_, about, offset):
    c, s = math.cos(rad(deg_)), math.sin(rad(deg_))
    return [(about[0] + c * (x - about[0]) - s * (y - about[1]) + offset[0],
             about[1] + s * (x - about[0]) + c * (y - about[1]) + offset[1])
            for x, y in pts]


def escapement_sweep(e, win, samples_per_beat=90, beats=2, deeper_deg=0.0):
    """The escapement's own polygons through `beats` beats: the least
    distance from either pallet to any tooth at each sample, the sample it
    came at, and the distance at each beat's end (the tooth locked, which
    should be the rest gap). The wheel's centre is the origin; the anchor's
    pivot at (0, anchor_d)."""
    P = (0.0, e["anchor_d"])
    pal = {side: [(x, y + P[1]) for x, y in
                  pallet_outline(e, side, deeper_deg=deeper_deg)]
           for side in ("left", "right")}
    worst = (float("inf"), None)
    ends = []
    n = samples_per_beat * beats
    for i in range(n + 1):
        b = 180.0 * beats * i / n
        a = anchor_deg(e, b)
        th = escape_deg(e, win, b)
        teeth = wheel_teeth_polys(e, th)
        dmin = float("inf")
        for side, poly in pal.items():
            q = _xf(poly, a, P, (0.0, 0.0))
            cx = sum(x for x, _ in q) / len(q)
            cy = sum(y for _, y in q) / len(q)
            for t in teeth:
                tx, ty = t[1]
                if math.hypot(tx - cx, ty - cy) > 3.0 * e["tooth_h"] + 10:
                    continue
                dmin = min(dmin, poly_distance(q, t))
        if dmin < worst[0]:
            worst = (dmin, b)
        if i % samples_per_beat == 0:
            ends.append((b, dmin, th))
    return {"min_mm": worst[0], "at_beat_deg": worst[1], "ends": ends}


# --- the whole clock ------------------------------------------------------------------

def solve(period_s, allow, layer, z_gap, weight_od, cord_mm):
    """Every size, from the period, the profile (`allow(kind, size_mm)`
    answers print_allowance's diametral allowance), the layer and Z gap the
    profile states, the weight's outer diameter (the jar's, from its own
    generator) and the cord's diameter."""
    if period_s <= 0:
        raise SystemExit(f"clock: a period of {period_s:g} s")
    s = {"period_s": period_s, "layer": layer, "z_gap": z_gap,
         "weight_od": weight_od, "cord_mm": cord_mm}
    d_a = lines(ARBOR_LINES)
    g_small = allow("gap", d_a) / 2.0
    # The gap a side at a wheel's size: every wheel is past 8 mm.
    g = allow("gap", 50.0) / 2.0
    play = 2.0 * (g + g_small)
    s.update(arbor_d=d_a, g=g, g_small=g_small, play=play,
             end_shake=play)
    # The anchor's hub, round its arbor's light press: the sweep's pallets
    # carry it.
    hub_r = (d_a + allow("hole", d_a) / 2.0) / 2.0 + lines(WALL_LINES)
    e = escapement(g, g_small, play, hub_r)
    s["escape"] = e
    if allow("gap", 2.0 * e["R"]) / 2.0 != g:
        raise SystemExit("clock: the escape wheel's gap is not the wheels'")
    counts = train_counts(period_s)
    s["counts"] = counts
    hole_a = allow("hole", d_a)
    press_d = d_a + hole_a / 2.0
    m, sets, clearance = choose_module(counts, play, play / 2.0,
                                       press_d / 2.0)
    s.update(module=m, gears=sets, root_clearance=clearance)
    # Thicknesses: a wheel two modules (whole layers), a pinion longer than
    # the wheel it meshes by an end shake each side.
    es = s["end_shake"]
    t_w = up_to(max(2.0 * m, lines(6)), layer)
    t_p = up_to(t_w + 2.0 * es, layer)
    s.update(t_w=t_w, t_p=t_p)
    # Bores. A light press (half the hole allowance over the shaft, so the
    # bore prints about that much under it) for what an arbor carries; the
    # gap at the arbor's size for what turns on it.
    s["press_d"] = press_d
    s["run_d"] = d_a + 2.0 * g_small
    s["flat"] = lines(2)            # the D's flat, from the round
    # A shaft prints LYING on a flat along its whole length: stood on end
    # it came off the bed (check_print's slender item). The flat is cut
    # whole layers deep, as deep as leaves the round over it at most 45
    # degrees from vertical where the two meet.
    s["lie_flat"] = lie_depth(d_a / 2.0, layer)
    s["hub_r"] = s["press_d"] / 2.0 + lines(WALL_LINES)
    assert e["hub_r"] == s["hub_r"]
    # Plates: a bearing as long as the arbor is thick.
    s["t_plate"] = up_to(d_a, layer)
    layers_z(s)
    layout(s)
    threads(s, allow)
    pendulum_shapes(s, allow)
    drum(s)
    motion_works(s, allow)
    frame(s)
    front_works(s)
    s["hole_allow_pillar"] = allow("hole", s["pillar_thread"]["major"])
    pillars(s)
    frames(s)
    return s


def layers_z(s):
    """Heights in the movement frame (Z = 0 the back plate's inner face)."""
    es, t_w, t_p = s["end_shake"], s["t_w"], s["t_p"]
    z = {}
    z["escape"] = (es, es + t_w)
    z["escape_pinion"] = (es + t_w, es + t_w + t_p)
    z["anchor"] = z["escape"]
    z["third"] = (z["escape_pinion"][0] + es, z["escape_pinion"][0] + es + t_w)
    # The centre wheel starts an end shake above the escape pinion's top,
    # which it overlaps seen from the front: two faces turning at different
    # rates must not share a face (they rub). The mesh clash check used to
    # call a shared face a clash too; it calls it touching now
    # (docs/inspect/README.md#touching-is-not-a-clash), and the play stays
    # for the rubbing. The third pinion runs on an end shake past it.
    z["centre"] = (z["escape_pinion"][1] + es, z["escape_pinion"][1] + es
                   + t_w)
    z["third_pinion"] = (z["third"][1], z["centre"][1] + es)
    s["z"] = z


def layout(s):
    """Where every arbor stands, front view, the centre arbor at the origin.
    The escape wheel straight above the centre, as far as the centre wheel's
    tips need to clear the escape wheel's hub tube; the third arbor where
    both its meshes meet, to the left; the anchor above the escape wheel."""
    m, gs, e = s["module"], s["gears"], s["escape"]
    c = s["counts"]
    c1 = m * (c["centre"] + c["third_pinion"]) / 2.0
    c2 = m * (c["third"] + c["escape_pinion"]) / 2.0
    tip = {k: v["rp"] + v["h_a"] for k, v in gs.items()}
    s["tip_r"] = tip
    clear = s["hub_r"] + s["g"] + lines(2)
    D = max(tip["centre"] + clear, abs(c1 - c2) + 1.0, e["R"] + clear)
    D = up_to(D, 0.5)
    if D > c1 + c2:
        raise SystemExit("clock: the third arbor cannot reach both meshes")
    # Third arbor: |X| = c1 from the centre and c2 from the escape (0, D).
    y3 = (c1 ** 2 - c2 ** 2 + D ** 2) / (2.0 * D)
    x3 = -math.sqrt(max(c1 ** 2 - y3 ** 2, 0.0))
    pos = {"centre": (0.0, 0.0), "escape": (0.0, D), "third": (x3, y3),
           "anchor": (0.0, D + e["anchor_d"])}
    s.update(pos=pos, c1=c1, c2=c2, D=D)


def threads(s, allow):
    """The pillars' and the rating nut's threads: 45-degree flanks (the
    jar's), M8's major, the smallest a printed thread is reliable at
    (docs/tolerances/README.md#what-the-checks-say), pitches in whole
    layers."""
    lay = s["layer"]
    major = 8.0
    for name, pitch in (("pillar", up_to(10 * lay, lay)),
                        ("rating", up_to(8 * lay, lay))):
        depth = 5.0 * (pitch / 2.0) / 8.0      # threads::BasicDepth at 45
        ext = major + allow("external_thread", major)
        bore = major - 2.0 * depth + allow("internal_thread", major)
        s[f"{name}_thread"] = {
            "major": major, "pitch": pitch, "depth": depth, "flank_deg": 45.0,
            "ext_d": ext, "nut_bore_d": bore, "nut_major_d": bore + 2 * depth,
            "radial_clear": (bore + 2 * depth - ext) / 2.0}


def pendulum_shapes(s, allow):
    """The rod, bob and rating nut, all but the rod's length (the generator
    finds it from the modelled masses). The rod is a flat bar printed on its
    face; its threaded end is D-flatted on the bed so no flank faces down
    past 45 degrees; the bob is a block printed standing, the bar through
    it; the nut a knurled ring printed flat."""
    lay, g = s["layer"], s["g"]
    th = s["rating_thread"]
    r_maj = th["ext_d"] / 2.0
    r_root = r_maj - th["depth"]
    # The axis this high over the bed cuts every face that would look
    # down steeper than 45 degrees (the root's and the crests', within 45 of
    # straight down).
    z_axis = up_to(math.cos(math.radians(45.0)) * r_root * 1.01, 0.01)
    t_b = up_to(z_axis + r_maj + lay, lay)
    w_b = up_to(th["major"] + lines(2), 0.1)
    s["rod"] = {"w": w_b, "t": t_b, "thread_axis_z": z_axis,
                "thread_r": r_maj, "thread_root_r": r_root}
    s["nut"] = {"d": up_to(2.5 * th["major"], 0.5),
                "h": up_to(1.25 * th["major"], lay), "notches": 24}
    s["bob_slot"] = (w_b + 2.0 * g, t_b + 2.0 * g)


def drum(s):
    """The cord drum on the centre arbor: the smallest core that holds a
    cord round a bore turning on the arbor at the gap, ten turns a layer,
    and a ratchet on its back flange for the click. Its z is frame()'s."""
    r_core = s["run_d"] / 2.0 + lines(WALL_LINES) + s["cord_mm"] / 2.0
    r_core = up_to(r_core, 0.1)
    d = {"r_core": r_core, "r_cord": r_core + s["cord_mm"] / 2.0,
         "t_flange": s["t_w"],
         "ratchet_tip_r": r_core + 4.0 * s["cord_mm"],
         "ratchet_h": up_to(max(lines(4), 1.5 * s["cord_mm"]), 0.1),
         "core": up_to(10.0 * s["cord_mm"], s["layer"]),
         "sleeve_r": r_core}
    d["ratchet_root_r"] = d["ratchet_tip_r"] - d["ratchet_h"]
    # Both cord flanges are 45-degree cones out to the ratchet's tip radius:
    # the cord holds four layers, and nothing faces down past 45.
    d["cone"] = d["ratchet_tip_r"] - d["r_core"]
    s["drum"] = d


def motion_works(s, allow):
    m = s["module"]
    gs = s["gears"]
    s["c_mw"] = m * (Z_CANNON + Z_MINUTE_W) / 2.0
    s["cannon_pipe_d"] = up_to(s["press_d"] + 2 * lines(WALL_LINES), 0.1)
    g_pipe = allow("gap", s["cannon_pipe_d"]) / 2.0
    s["hour_bore_d"] = s["cannon_pipe_d"] + 2.0 * g_pipe
    s["hour_pipe_d"] = up_to(s["hour_bore_d"] + 2 * lines(WALL_LINES), 0.1)
    s["stud_d"] = s["arbor_d"]
    s["mw_bore_d"] = s["run_d"]
    del gs


def frame(s):
    """The plate spacing. It is set by the weight: the jar must pass the
    pendulum's bob, which hangs behind the back plate, so the cord's
    rearmost turn leaves the drum far enough forward that the jar's radius
    and the bob's half-depth fit between them, with the bob's half-depth
    again as air for a jar that swings a little on its cord. The drum
    reaches forward to there on a sleeve from its ratchet, which sits on the
    centre wheel's pawls."""
    es, t_pl = s["end_shake"], s["t_plate"]
    lay = s["layer"]
    rod = s["rod"]
    bob_depth = up_to(3.0 * rod["t"], lay)
    s["bob_depth"] = bob_depth
    # The rod hangs far enough behind the back plate for the bob's front
    # wall (the gap and two lines) to stand an end shake clear of it: the
    # bob may hang level with the plate (a short pendulum's does).
    rz1 = -t_pl - es - s["g"] - lines(2)
    s["rod_z"] = (rz1 - rod["t"], rz1)
    s["bob_z"] = (-t_pl - es - bob_depth, -t_pl - es)
    z_rod = (s["bob_z"][0] + s["bob_z"][1]) / 2.0
    s["z_rod_mid"] = z_rod
    air = bob_depth / 2.0
    d = s["drum"]
    d["z0"] = s["z"]["centre"][1] + es             # the ratchet flange
    z_cord = z_rod + bob_depth / 2.0 + air + s["weight_od"] / 2.0
    # The rear cone ends where the cord's first turn begins.
    d["z_core0"] = max(up_to(z_cord - s["cord_mm"] / 2.0, lay),
                       d["z0"] + d["t_flange"] + d["cone"])
    d["z_cone_rear"] = d["z_core0"] - d["cone"]
    d["z_cone0"] = d["z_core0"] + d["core"]
    H = up_to(d["z_cone0"] + d["cone"] + es, lay)
    s["H"] = H
    d["z1"] = H - es
    s["z"]["drum"] = (d["z0"], d["z1"])
    s["cord_z_rear"] = d["z_core0"] + s["cord_mm"] / 2.0
    s["weight_air_mm"] = (s["cord_z_rear"] - z_rod - bob_depth / 2.0
                          - s["weight_od"] / 2.0)


# --- what stands in front of the front plate ------------------------------------------

def front_works(s):
    """Heights outside the front plate (F, its outer face): the motion
    works, then the dial ring on spacers over them, then the hour and the
    minute hand."""
    es, t_w, t_p, lay = s["end_shake"], s["t_w"], s["t_p"], s["layer"]
    F = s["H"] + s["t_plate"]
    z = s["z"]
    z["cannon"] = (F + es, F + es + t_p)
    z["minute_wheel"] = (F + 2 * es, F + 2 * es + t_w)
    z["minute_pinion"] = (z["minute_wheel"][1], z["minute_wheel"][1] + t_p)
    # The hour wheel an end shake over the cannon pinion (which it
    # overlaps), inside the minute pinion's length.
    z["hour"] = (z["cannon"][1] + es, z["cannon"][1] + es + t_w)
    if z["hour"][1] > z["minute_pinion"][1] + 1e-9:
        raise SystemExit("clock: the hour wheel overruns the minute pinion")
    t_ring = up_to(2.0 * t_w, lay)
    z["ring"] = (z["minute_pinion"][1] + es, z["minute_pinion"][1] + es
                 + t_ring)
    t_hand = up_to(lines(4), lay)
    boss = up_to(2.0 * t_hand, lay)
    z["hour_hand"] = (z["ring"][1] + es, z["ring"][1] + es + t_hand)
    z["hour_pipe"] = (z["hour"][1], z["hour_hand"][0] + boss)
    z["minute_hand"] = (z["hour_pipe"][1] + es, z["hour_pipe"][1] + es
                        + t_hand)
    z["cannon_pipe"] = (z["cannon"][1], z["minute_hand"][0] + boss)
    s.update(F=F, t_ring=t_ring, t_hand=t_hand, hand_boss=boss)
    # The centre arbor ends inside the cannon, short of its top.
    s["centre_arbor_top"] = z["cannon_pipe"][1] - lines(2)


def pillars(s):
    """The pillar's sizes and lengths, the same pillar in both stages."""
    th = s["pillar_thread"]
    lay, es = s["layer"], s["end_shake"]
    g = s["g"]
    r_ext = th["ext_d"] / 2.0
    hole_d = up_to(th["major"] + 2.0 * g + s["hole_allow_pillar"], 0.1)
    shoulder_r = up_to(hole_d / 2.0 + lines(WALL_LINES), 0.1)
    nut_h = up_to(0.8 * th["major"], lay)
    nut_od = 2.0 * th["major"]
    # Rear: the plate, its nut, a pitch spare; then the standoff to the
    # wall, which stands as far behind the pendulum's bob as the bob's own
    # half-depth.
    z_bob_back = s["z_rod_mid"] - s["bob_depth"] / 2.0
    z_wall = -up_to(-z_bob_back + s["bob_depth"] / 2.0, lay)
    # Each thread runs out, half a pitch on, into a relief a line inside
    # its root (clock_parts.build_pillar); the rear one ends under the
    # shoulder's 45-degree cone.
    r_rel = r_ext - th["depth"] - lines(1)
    cone = shoulder_r - r_rel
    rear = (-s["t_plate"] - nut_h - th["pitch"], -cone - th["pitch"] / 2.0)
    front0 = s["H"] + (r_ext - r_rel) + th["pitch"] / 2.0
    front1 = (s["z"]["ring"][1] + nut_h + th["pitch"])
    # It prints lying on a flat (clock_parts.build_pillar), cut from the
    # thread's root as an arbor's is from its round: the thread keeps the
    # rest of its turn, and the narrower runs (the standoff, the reliefs)
    # meet the flat under 45-degree chamfers.
    r_root = r_ext - th["depth"]
    lie_y = r_root - lie_depth(r_root, lay)
    s["pillar_foot_r"] = r_root - lines(2)    # the standoff's, its end
    engaged = 1.0 - 2.0 * math.degrees(math.acos(lie_y / r_root)) / 360.0
    s["pillar"] = {"hole_d": hole_d, "shoulder_r": shoulder_r,
                   "lie_y": lie_y, "thread_engaged": engaged,
                   "sink_r": shoulder_r, "pad_r": nut_od / 2.0 + lines(2),
                   "nut_h": nut_h, "nut_od": nut_od, "z_wall": z_wall,
                   "rear_thread": rear, "front_thread": (front0, front1),
                   "r_ext": r_ext}
    s["wall_z"] = z_wall
    s["pillar_height"] = front1 - z_wall


def _clear(p, circles, r):
    return all(math.hypot(p[0] - x, p[1] - y) >= rr + r for x, y, rr in
               circles)


def frames(s):
    """Where the pillars stand, for both stages, and the plates' axes.

    Stage 2: the lower two at 45 degrees either side of straight down from
    the centre arbor, just outside the centre wheel (they carry the dial
    ring, which the hands must sweep inside); the upper two level with the
    anchor, as close in as clears the anchor, the escape wheel and the third
    wheel. Stage 1: the same upper two, the lower two the same way round the
    escape wheel."""
    pos, e = s["pos"], s["escape"]
    pt = s["pillar"]
    g = s["g"]
    pad = pt["pad_r"]
    rs = pt["shoulder_r"]
    tips = s["tip_r"]
    px, py = pos["anchor"]
    reach = e["R"] + e["pallet_w"] / 2.0 + e["arm_w"]
    turning = [(pos["centre"][0], pos["centre"][1], tips["centre"] + g),
               (pos["third"][0], pos["third"][1], tips["third"] + g),
               (pos["escape"][0], pos["escape"][1], e["R"] + g),
               (px, py, max(reach * math.sin(math.radians(45.0)) + g,
                            s["hub_r"] + g))]
    r_lo = tips["centre"] + g + rs + lines(2)
    lo = [(r_lo * math.cos(math.radians(a)), r_lo * math.sin(math.radians(a)))
          for a in (225.0, 315.0)]
    up = []
    for sign in (-1.0, 1.0):
        x = s["hub_r"] + rs
        while not _clear((sign * x, py), turning, max(rs, pad) + lines(2)):
            x += 0.5
        up.append((sign * x, py))
    r1 = e["R"] + g + rs + lines(2)
    ex, ey = pos["escape"]
    lo1 = [(ex + r1 * math.cos(math.radians(a)),
            ey + r1 * math.sin(math.radians(a))) for a in (225.0, 315.0)]
    up1 = []
    for sign in (-1.0, 1.0):
        x = s["hub_r"] + rs
        while not _clear((sign * x, py), turning[2:],
                         max(rs, pad) + lines(2)):
            x += 0.5
        up1.append((sign * x, py))
    # The keyhole tab over the top of the back plate, on the anchor's line.
    tab_y = py + s["hub_r"] + lines(6) + 8.0
    key = (0.0, tab_y, 4.5, 2.5, 6.0)
    s["frame2"] = {"pillars": lo + up, "axes": {
        "centre": pos["centre"], "third": pos["third"],
        "escape": pos["escape"], "anchor": pos["anchor"]},
        "tab": [(0.0, tab_y + 3.0, 12.0)], "keyhole": key,
        "beat": (0.0, 0.0)}
    s["frame1"] = {"pillars": lo1 + up1, "axes": {
        "escape": pos["escape"], "anchor": pos["anchor"]},
        "tab": [(0.0, tab_y + 3.0, 12.0)], "keyhole": key,
        "beat": pos["escape"]}
    # The minute wheel down and to the left, under the dial ring's inside
    # edge so the ring keeps it on its stud.
    a = math.radians(225.0)
    s["minute_wheel_xy"] = (s["c_mw"] * math.cos(a), s["c_mw"] * math.sin(a))
    s["frame2"]["minute_wheel"] = s["minute_wheel_xy"]
    s["frame2"]["stud_top"] = s["z"]["minute_pinion"][1] + s["end_shake"]
    # The dial ring: from the minute wheel's centre out to the lower
    # pillars, and the hands inside the lower nuts' reach.
    s["ring"] = {"r_in": s["c_mw"],
                 "r_out": r_lo - pt["nut_od"] / 2.0 - lines(2),
                 "lug_r": pt["pad_r"], "pillars": lo}
    s["minute_hand_r"] = s["ring"]["r_out"] - lines(4)
    s["hour_hand_r"] = 0.65 * s["minute_hand_r"]
    s["turning_xy"] = turning


# --- the pendulum ---------------------------------------------------------------

def simple_length_mm(period_s, amp_deg):
    """The simple pendulum of `period_s` at `amp_deg` either way: the
    small-swing period is the period over the circular error's 1 + A^2/16."""
    a = math.radians(amp_deg)
    t0 = period_s / (1.0 + a * a / 16.0)
    return 1000.0 * G * (t0 / (2.0 * math.pi)) ** 2


def pendulum_sizes(s, l_eq):
    """The bob, the rating travel and the thread's length, from the
    pendulum's length: a bob a quarter of it wide and a fifth tall, the nut
    free to travel RATING_TRAVEL of the length either way."""
    lay = s["layer"]
    th = s["rating_thread"]
    travel = up_to(RATING_TRAVEL * l_eq, th["pitch"])
    s["pend"] = {"l_eq_mm": l_eq, "bob_w": up_to(l_eq / 4.0, 1.0),
                 "bob_h": up_to(l_eq / 5.0, lay), "travel": travel,
                 "thread_len": s["nut"]["h"] + 2.0 * travel + th["pitch"]}
    return s["pend"]


def nut_y(s, length):
    """The rating nut's lower face, nominal: the middle of its travel."""
    p = s["pend"]
    return -length + s["rating_thread"]["pitch"] / 2.0 + p["travel"]


def compound_period(bodies, amp_deg=0.0):
    """T = 2 pi sqrt(I / (M g d)) about the Z axis through the origin, from
    each body's (mass_g, centre of mass mm, I_zz about its centre g mm^2),
    and the circular error at `amp_deg`. Also what it needs: I, M, d."""
    inertia = sum(m * (c[0] ** 2 + c[1] ** 2) + izz for m, c, izz in bodies)
    mass = sum(m for m, _, _ in bodies)
    md = sum(m * -c[1] for m, c, _ in bodies)
    if md <= 0:
        raise SystemExit("clock: the pendulum's centre of mass is not below "
                         "its pivot")
    t0 = 2.0 * math.pi * math.sqrt(inertia / md * 1e-3 / G)
    a = math.radians(amp_deg)
    return {"period_s": t0 * (1.0 + a * a / 16.0), "small_swing_s": t0,
            "inertia_g_mm2": inertia, "mass_g": mass, "d_mm": md / mass,
            "equivalent_length_mm": inertia / md}
