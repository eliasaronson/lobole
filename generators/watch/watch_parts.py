"""The watch's parts — one function per part, each its own part document.

Every ARBOR part is built on its own axis at its WORLD depth: local Z is world
Z, the arbor runs along local Z through the origin, and tooth 0 of every gear
points +X. Placed at the arbor's (x, y) and mated by its "axis" frame to the
mainplate's pivot frame, it lands where the spec says — and a gear relation's
phase follows from the centre line alone, because at dof 0 every tooth 0
points the same way.

Every other part (plates, bridges, dial, case) is built in WORLD coordinates
and carries a "seat" frame at the origin, so one fastened mate to the
mainplate's "seat" puts it where it was drawn.

Nothing here knows about the assembly; watch.py places, mates and couples.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import watch_gearing  # noqa: E402
from _lib import arc_through  # noqa: E402
from designer_sdk import Param  # noqa: E402
import watch_spec as S  # noqa: E402
from watch_spec import *  # noqa: F401,F403,E402 — rebound by declare()

# --- how things look ----------------------------------------------------------
# There is no rhodium, gilt, ruby or blued-steel material, so these are
# appearance overrides on real slugs; mass comes from the slug.
LOOK = {
    "rhodium": dict(color=[0.80, 0.81, 0.82], metallic=1.0, roughness=0.30),
    "gilt": dict(color=[0.86, 0.68, 0.34], metallic=1.0, roughness=0.22),
    "ruby": dict(color=[0.62, 0.02, 0.07], metallic=0.0, roughness=0.05),
    "blued": dict(color=[0.06, 0.13, 0.48], metallic=0.9, roughness=0.22),
    "cream": dict(color=[0.93, 0.89, 0.80], metallic=0.05, roughness=0.45),
    "polished": dict(color=[0.90, 0.90, 0.91], metallic=1.0, roughness=0.10),
    "brushed": dict(color=[0.84, 0.84, 0.85], metallic=1.0, roughness=0.38),
    "gold": dict(color=[0.95, 0.76, 0.36], metallic=1.0, roughness=0.18),
    "glucydur": dict(color=[0.82, 0.62, 0.36], metallic=1.0, roughness=0.28),
}


def look(app, body, name):
    app.call("set_appearance", body=body, **LOOK[name])


def material(app, body, slug):
    """By body INDEX — set_material wants the index or the creating feature."""
    app.call("set_material", body=body, material=slug)


# --- sketch and extrude ---------------------------------------------------------

def plane(z):
    return {"origin": [0.0, 0.0, float(z)], "normal": [0, 0, 1],
            "uAxis": [1, 0, 0]}


def plane_x(x):
    """A YZ plane at x, normal +X: u = world Y, v = world Z."""
    return {"origin": [float(x), 0.0, 0.0], "normal": [1, 0, 0],
            "uAxis": [0, 1, 0]}


def circ(c, r):
    return {"type": "circle", "center": [float(c[0]), float(c[1])],
            "radius": float(r)}


def seg(a, b):
    return {"type": "line", "a": [float(a[0]), float(a[1])],
            "b": [float(b[0]), float(b[1])]}


def arc(c, r, a0, a1):
    return {"type": "arc", "center": [float(c[0]), float(c[1])],
            "radius": float(r), "start_angle": float(a0),
            "end_angle": float(a1)}


def pol(r, a, c=(0.0, 0.0)):
    return (c[0] + r * math.cos(a), c[1] + r * math.sin(a))


def poly(pts):
    return [seg(pts[i], pts[(i + 1) % len(pts)]) for i in range(len(pts))]


def prism(app, ents, z0, h, mode, name, pl=None):
    sid = app.call("add_feature", feature={
        "type": "Sketch", "name": name, "plane": pl or plane(z0),
        "entities": ents})["id"]
    f = {"type": "Extrude", "name": name, "sketch_id": sid,
         "distance": float(h), "mode": mode}
    # A thickness that is a Param stays a formula in the document (W1): edit
    # wheel_t and every wheel regenerates thicker.
    if isinstance(h, Param):
        f["distance_expr"] = h.expr
    return app.call("add_feature", feature=f)


def zb(d):
    """World Z of depth d below the mainplate's bridge face."""
    return -MAINPLATE_T - d


def slab(app, ents, d0, d1, mode, name):
    """A prism spanning depths d0..d1 (d1 further from the dial)."""
    return prism(app, ents, zb(d1), d1 - d0, mode, name)


def hull(points):
    """Convex hull, counter-clockwise (monotone chain)."""
    pts = sorted(set((round(p[0], 6), round(p[1], 6)) for p in points))

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    lower, upper = [], []
    for p in pts:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], p) <= 0:
            lower.pop()
        lower.append(p)
    for p in reversed(pts):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], p) <= 0:
            upper.pop()
        upper.append(p)
    return lower[:-1] + upper[:-1]


def pads(centres, r, n=24):
    """The hull of a disc of radius r around each centre — a bridge outline.
    `r` is one radius for all of them, or one per centre."""
    rs = list(r) if isinstance(r, (list, tuple)) else [r] * len(centres)
    return poly(hull([pol(rr, 2 * math.pi * k / n, c)
                      for c, rr in zip(centres, rs) for k in range(n)]))


# --- gears ----------------------------------------------------------------------

def teeth(key):
    """A tooth set's cycloidal outline (watch_gearing.py), by its name in
    watch_spec.TOOTH_SETS — the design is per tooth SET, not per tooth
    count, because each addendum is traced by its mate's rolling circle."""
    g = S.GEAR[key]
    return watch_gearing.profile(float(MODULE), g["z"], g["rho_add"],
                                 g["rho_ded"], g["frac"], g["h_a"],
                                 g["h_f"])[0]


def root_r(key):
    return S.root_of(key)


def crossings(n, hub_r, rim_r, spoke_w, rot=0.0):
    """The windows between a wheel's spokes ("crossings"): n annular sectors,
    each bounded by the rim, the hub and two spoke edges of constant width."""
    ents = []
    ao = math.asin(spoke_w / 2 / rim_r)
    ai = math.asin(spoke_w / 2 / hub_r)
    for k in range(n):
        t0 = rot + 2 * math.pi * k / n
        t1 = rot + 2 * math.pi * (k + 1) / n
        ents += [arc((0, 0), rim_r, t0 + ao, t1 - ao),
                 seg(pol(rim_r, t1 - ao), pol(hub_r, t1 - ai)),
                 arc((0, 0), hub_r, t0 + ai, t1 - ai),
                 seg(pol(hub_r, t0 + ai), pol(rim_r, t0 + ao))]
    return ents


def arbor(app, name, staff, wheel=None, pinion=None, spokes=5, hub=0.6,
          look_as="gilt"):
    """A wheel and its pinion on one staff — what every train arbor is, and
    what gear.py cannot build (it makes one gear per part).

    staff  = (d_top, d_bottom): depth span of the staff; negative depths run
             up through the mainplate towards the dial.
    wheel  = (tooth set, d, thickness)
    pinion = (tooth set, d0, d1)
    """
    part = app.part(name)
    slab(app, [circ((0, 0), ARBOR_R)], staff[0], staff[1], "new_body", "staff")
    if wheel:
        key, d, t = wheel
        slab(app, teeth(key), d, d + t, "add", "wheel")
        if spokes:
            rim = root_r(key) - 0.30
            slab(app, crossings(spokes, hub, rim, 0.26), d, d + t, "cut",
                 "crossings")
    if pinion:
        key, d0, d1 = pinion
        slab(app, teeth(key), d0, d1, "add", "pinion")
    part.frame("axis", origin=(0.0, 0.0, -float(MAINPLATE_T)))
    material(app, 0, "brass")
    look(app, 0, look_as)
    return part


# --- the going train ---------------------------------------------------------------

def barrel(app):
    """The drum: a tooth ring at the mainplate end, the drum behind it, and
    the arbor passing through. The mainspring is inside and not drawn."""
    part = app.part("Barrel")
    ring = 0.30
    slab(app, teeth("barrel"), BARREL_D0, BARREL_D0 + ring, "new_body",
         "teeth")
    slab(app, [circ((0, 0), root_r("barrel") - 0.05)], BARREL_D0 + ring,
         BARREL_D1, "add", "drum")
    slab(app, [circ((0, 0), 0.50)], BARREL_D0 - 0.01, BARREL_D1 + 0.01, "cut",
         "arbor hole")
    # The cover's snailing groove, the one detail of a barrel that shows.
    for r in (1.6, 2.4, 3.2):
        slab(app, [circ((0, 0), r + 0.06), circ((0, 0), r)],
             BARREL_D1 - 0.04, BARREL_D1 + 0.001, "cut", "groove")
    part.frame("axis", origin=(0.0, 0.0, -float(MAINPLATE_T)))
    material(app, 0, "brass")
    look(app, 0, "gilt")
    return part


def barrel_arbor(app):
    """The barrel ARBOR, which the drum turns around: wound by the ratchet
    wheel on top of the barrel bridge, and turning separately from the drum."""
    part = app.part("Barrel arbor")
    slab(app, [circ((0, 0), 0.45)], 0.10, RATCHET_D, "new_body", "arbor")
    slab(app, teeth("ratchet"), RATCHET_D, RATCHET_D + RATCHET_T, "add",
         "ratchet")
    # Sunburst on the ratchet: radial grooves, the classic finish.
    n = 36
    ents = []
    for k in range(n):
        a = 2 * math.pi * k / n
        ents += poly([pol(0.8, a - 0.012), pol(root_r("ratchet") - 0.2,
                      a - 0.006), pol(root_r("ratchet") - 0.2, a + 0.006),
                      pol(0.8, a + 0.012)])
    slab(app, ents, RATCHET_D + RATCHET_T - 0.03, RATCHET_D + RATCHET_T + 0.001,
         "cut", "sunburst")
    slab(app, [circ((0, 0), 0.55)], RATCHET_D + RATCHET_T,
         RATCHET_D + RATCHET_T + 0.15, "add", "screw seat")
    part.frame("axis", origin=(0.0, 0.0, -float(MAINPLATE_T)))
    material(app, 0, "steel")
    look(app, 0, "rhodium")
    return part


def train(app):
    """The centre, third, fourth and escape arbors. The centre and fourth
    run up through the mainplate: the centre carries the cannon pinion, the
    fourth the seconds hand."""
    top = BRIDGE_D + 0.6
    parts = {}
    parts["center"] = arbor(
        app, "Centre wheel", staff=(-MAINPLATE_T - 1.95, top),
        wheel=("center", CENTER_WHEEL_D, WHEEL_T),
        pinion=("center_pin", BARREL_D0 - 0.05, BARREL_D0 + 0.35), hub=0.7)
    parts["third"] = arbor(
        app, "Third wheel", staff=(0.10, top),
        wheel=("third", THIRD_WHEEL_D, WHEEL_T),
        pinion=("third_pin", CENTER_WHEEL_D - 0.05,
                CENTER_WHEEL_D + WHEEL_T + 0.05))
    parts["fourth"] = arbor(
        app, "Fourth wheel", staff=(-MAINPLATE_T - SECONDS_HAND_Z - HAND_T,
                                    top),
        wheel=("fourth", FOURTH_WHEEL_D, WHEEL_T),
        pinion=("fourth_pin", THIRD_WHEEL_D - 0.05,
                THIRD_WHEEL_D + WHEEL_T + 0.05))
    parts["escape"] = escape_wheel(app, top)
    return parts


def escape_wheel(app, top):
    """Fifteen club teeth — a slanted tooth with a flat impulse face at its
    tip — four crossings, and the six-leaf pinion the fourth wheel drives."""
    part = app.part("Escape wheel")
    slab(app, [circ((0, 0), ARBOR_R)], 0.10, top, "new_body", "staff")
    R, Rr = float(ESCAPE_OUTER_R), float(ESCAPE_OUTER_R) - 0.50
    p = 2 * math.pi / Z_ESCAPE
    pts = []
    for k in range(Z_ESCAPE):
        a = k * p
        pts += [pol(Rr, a), pol(R - 0.18, a + 0.28 * p), pol(R, a + 0.42 * p),
                pol(R, a + 0.58 * p), pol(Rr + 0.08, a + 0.82 * p)]
    slab(app, poly(pts), ESCAPE_WHEEL_D, ESCAPE_WHEEL_D + ESCAPE_T, "add",
         "wheel")
    slab(app, crossings(4, 0.45, Rr - 0.28, 0.20), ESCAPE_WHEEL_D,
         ESCAPE_WHEEL_D + ESCAPE_T, "cut", "crossings")
    slab(app, teeth("escape_pin"), FOURTH_WHEEL_D - 0.05,
         FOURTH_WHEEL_D + WHEEL_T + 0.05, "add", "pinion")
    part.frame("axis", origin=(0.0, 0.0, -float(MAINPLATE_T)))
    material(app, 0, "steel")
    look(app, 0, "polished")
    return part


# --- the escapement ----------------------------------------------------------------

def pallet_fork(app):
    """The lever. Built in its own frame: +X points at the balance, the
    escape wheel's centre is at (-PALLET_FROM_ESCAPE, 0).

    The fork is cut clear of the escape wheel's tip circle by construction,
    and the two stones sit just outside it rather than in lock — so the
    placed escapement is clash-free at any phase of the train. Its action is
    intermittent and is not simulated (docs/modeling/automatic-watch-plan.md).
    """
    part = app.part("Pallet fork")
    e = float(PALLET_FROM_ESCAPE)
    L = math.dist(POS["pallet"], POS["balance"])
    pin_r = 0.55
    xn = L - pin_r                       # the notch, where the impulse pin is
    d0, d1 = ESCAPE_WHEEL_D, ESCAPE_WHEEL_D + ESCAPE_T
    slab(app, [circ((0, 0), ARBOR_R)], 0.10, PALLET_BRIDGE_D + 0.2,
         "new_body", "staff")
    slab(app, [circ((0, 0), 0.38)], d0, d1, "add", "hub")
    w = 0.12
    # The notch runs 0.10 past the impulse pin's centre: the pin is r 0.06,
    # and a notch bottom at xn - 0.05 (the first draft) put 0.01 of ruby into
    # the fork — the exact sweep's 0.0001 mm3.
    slab(app, poly([(0.2, w), (xn - 0.30, w), (xn - 0.30, 0.32),
                    (xn + 0.15, 0.32), (xn + 0.15, 0.10), (xn - 0.10, 0.10),
                    (xn - 0.10, -0.10), (xn + 0.15, -0.10), (xn + 0.15, -0.32),
                    (xn - 0.30, -0.32), (xn - 0.30, -w), (0.2, -w)]),
         d0, d1, "add", "fork")
    R = float(ESCAPE_OUTER_R)
    stones = []
    for sgn in (1, -1):
        a = math.radians(30.0) * sgn
        sx, sy = -e + (R + 0.18) * math.cos(a), (R + 0.18) * math.sin(a)
        n = (-sy, sx)
        k = 0.13 / math.hypot(*n)
        n = (n[0] * k, n[1] * k)
        slab(app, poly([(n[0], n[1]), (sx + n[0], sy + n[1]),
                        (sx - n[0], sy - n[1]), (-n[0], -n[1])]),
             d0, d1, "add", "arm")
        stones.append((a, sgn))
    # Clear of the escape wheel by construction, not by luck.
    slab(app, [circ((-e, 0), R + 0.04)], d0 - 0.01, d1 + 0.01, "cut",
         "escape clearance")
    for a, sgn in stones:
        c0, c1 = R + 0.05, R + 0.32
        t = 0.08
        u = (math.cos(a), math.sin(a))
        v = (-u[1], u[0])
        pts = [(-e + u[0] * r + v[0] * s, u[1] * r + v[1] * s)
               for r, s in ((c0, -t), (c1, -t), (c1, t), (c0, t))]
        slab(app, poly(pts), d0, d1, "new_body", "stone")
    part.frame("axis", origin=(0.0, 0.0, -float(MAINPLATE_T)))
    material(app, 0, "steel")
    look(app, 0, "polished")
    for b in (1, 2):
        material(app, b, "ruby")
        look(app, b, "ruby")
    return part


def balance(app):
    """Three-armed balance with timing screws, a roller and its impulse pin.
    Built in its own frame: +X points at the pallet arbor, which is where
    the impulse pin sits at rest, in the fork's notch."""
    part = app.part("Balance")
    slab(app, [circ((0, 0), ARBOR_R)], 0.20, BRIDGE_D + 0.6, "new_body",
         "staff")
    r_out, r_in = float(BALANCE_R), float(BALANCE_R) - 0.40
    slab(app, [circ((0, 0), r_out), circ((0, 0), r_in)], BALANCE_D0,
         BALANCE_D1, "add", "rim")
    slab(app, [circ((0, 0), 0.45)], BALANCE_D0, BALANCE_D0 + 0.20, "add",
         "hub")
    for k in range(3):
        a = 2 * math.pi * k / 3 + math.pi / 6
        u, v = (math.cos(a), math.sin(a)), (-math.sin(a), math.cos(a))
        h = 0.14
        slab(app, poly([(u[0] * 0.3 + v[0] * h, u[1] * 0.3 + v[1] * h),
                        (u[0] * (r_in + 0.1) + v[0] * h,
                         u[1] * (r_in + 0.1) + v[1] * h),
                        (u[0] * (r_in + 0.1) - v[0] * h,
                         u[1] * (r_in + 0.1) - v[1] * h),
                        (u[0] * 0.3 - v[0] * h, u[1] * 0.3 - v[1] * h)]),
             BALANCE_D0, BALANCE_D0 + 0.20, "add", "arm")
    screws = [circ(pol(r_out - 0.20, 2 * math.pi * k / 12 + 0.26), 0.14)
              for k in range(12)]
    slab(app, screws, BALANCE_D1, BALANCE_D1 + 0.12, "add", "timing screws")
    slab(app, [circ((0, 0), 0.70)], 1.25, 1.35, "add", "roller")
    slab(app, [circ((0.55, 0.0), 0.06)], ESCAPE_WHEEL_D, 1.36, "new_body",
         "impulse pin")
    part.frame("axis", origin=(0.0, 0.0, -float(MAINPLATE_T)))
    material(app, 0, "bronze")
    look(app, 0, "glucydur")
    material(app, 1, "ruby")
    look(app, 1, "ruby")
    return part


def hairspring(app):
    """An Archimedean spiral in round wire, above the balance and under the
    cock. There is no spiral primitive: a fit spline through the spiral,
    piped — twelve turns in about a second."""
    part = app.part("Hairspring")
    r0, r1, turns, per = 0.60, 2.80, 12, 20
    z = zb(3.30)
    n = turns * per
    pts = [[(r0 + (r1 - r0) * i / n) * math.cos(2 * math.pi * turns * i / n),
            (r0 + (r1 - r0) * i / n) * math.sin(2 * math.pi * turns * i / n),
            z] for i in range(n + 1)]
    cv = app.call("add_feature", feature={"type": "Curve3D", "name": "spiral",
                                          "kind": "fit_spline", "points": pts})
    app.call("add_feature", feature={"type": "Pipe", "name": "spring",
                                     "path_id": cv["id"],
                                     "outer_diameter": 0.05,
                                     "mode": "new_body"})
    slab(app, [circ((0, 0), 0.50), circ((0, 0), 0.20)], 3.22, 3.40,
         "new_body", "collet")
    part.frame("axis", origin=(0.0, 0.0, -float(MAINPLATE_T)))
    material(app, 0, "steel")
    material(app, 1, "steel")
    look(app, 0, "gilt")
    look(app, 1, "polished")
    return part


# --- plates and bridges (world coordinates, "seat" frame at the origin) -------------

PIVOT_R = 0.25     # a hole a staff passes through without touching


def mainplate(app):
    part = app.part("Mainplate")
    R = float(MOVEMENT_D) / 2
    prism(app, [circ((0, 0), R)], -MAINPLATE_T, MAINPLATE_T, "new_body",
          "plate")
    holes = [circ(POS[k], PIVOT_R) for k in
             ("third", "escape", "pallet", "balance", "reduction",
              "crown_wheel")]
    holes += [circ(POS["center"], 0.30), circ(POS["fourth"], 0.30),
              circ(POS["barrel"], 0.55)]
    prism(app, holes, -MAINPLATE_T - 0.01, MAINPLATE_T + 0.02, "cut",
          "pivot holes")
    # Perlage on the dial side is hidden by the dial, so it goes where it
    # shows: a ring of overlapping shallow circles around the rim, under the
    # rotor, seen through the caseback.
    rings = [circ(pol(R - 0.9, 2 * math.pi * k / 72), 0.55) for k in range(72)]
    prism(app, rings[::2], -MAINPLATE_T - 0.001, 0.03, "cut", "perlage")
    # And a field of it across the plate inside that ring, where it shows
    # between the bridges once the rotor is off — no spot reaching a hole.
    keep = [(POS[k], PIVOT_R) for k in
            ("third", "escape", "pallet", "balance", "reduction",
             "crown_wheel")]
    keep += [(POS["center"], 0.30), (POS["fourth"], 0.30),
             (POS["barrel"], 0.55)]
    pitch, spot = 1.25, 0.50
    n = int(R / pitch) + 1
    field = []
    for j in range(-n, n + 1):
        for i in range(-n, n + 1):
            c = ((i + 0.5 * (j % 2)) * pitch, j * pitch * math.sqrt(3) / 2)
            if math.hypot(*c) + spot > R - 1.45 - 0.1:
                continue
            if any(math.dist(c, h) < rr + spot + 0.1 for h, rr in keep):
                continue
            field.append(circ(c, spot))
    prism(app, field, -MAINPLATE_T - 0.001, 0.03, "cut", "perlage field")
    prism(app, [circ(POS["minute"], 0.20)], 0.0, MW_Z + 0.65, "add",
          "minute stud")
    keyless_seat(app, R)
    part.frame("seat", origin=(0.0, 0.0, 0.0))
    for k, (x, y) in POS.items():
        if k in ("minute",):
            continue
        xax = (1.0, 0.0, 0.0)
        if k == "pallet":
            d = (POS["balance"][0] - x, POS["balance"][1] - y)
            L = math.hypot(*d)
            xax = (d[0] / L, d[1] / L, 0.0)
        if k == "balance":
            d = (POS["pallet"][0] - x, POS["pallet"][1] - y)
            L = math.hypot(*d)
            xax = (d[0] / L, d[1] / L, 0.0)
        part.frame(f"pivot {k}", origin=(x, y, -float(MAINPLATE_T)), x=xax)
    part.frame("pivot minute", origin=(POS["minute"][0], POS["minute"][1],
                                       -float(MAINPLATE_T)))
    part.frame("pivot hour", origin=(0.0, 0.0, -float(MAINPLATE_T)))
    # The stem's axis, along +X at STEM_Z; everything on the stem is drawn
    # in STEM coordinates (see keyless works) and mated to this frame.
    part.frame("stem", origin=(0.0, 0.0, float(STEM_Z)), z=(1.0, 0.0, 0.0),
               x=(0.0, 1.0, 0.0))
    for k, c in lever_pivots().items():
        part.frame(f"pivot {k}", origin=(c[0], c[1], -float(MAINPLATE_T)))
    material(app, 0, "brass")
    look(app, 0, "rhodium")
    return part


def cotes(app, outline_pts, d_top, pitch=1.1, width=0.30, depth=0.035,
          angle_deg=35.0):
    """Côtes de Genève: parallel stripes across a bridge's visible face,
    as shallow cuts clipped to its outline by construction (stripes are
    drawn wide and the cut only removes what the bridge has)."""
    a = math.radians(angle_deg)
    u, v = (math.cos(a), math.sin(a)), (-math.sin(a), math.cos(a))
    xs = [p[0] for p in outline_pts]
    ys = [p[1] for p in outline_pts]
    cx, cy = sum(xs) / len(xs), sum(ys) / len(ys)
    span = max(max(xs) - min(xs), max(ys) - min(ys)) + 2
    ents = []
    k = -span
    while k <= span:
        o = (cx + v[0] * k, cy + v[1] * k)
        pts = [(o[0] - u[0] * span + v[0] * s, o[1] - u[1] * span + v[1] * s)
               for s in (-width / 2, width / 2)]
        pts += [(o[0] + u[0] * span + v[0] * s, o[1] + u[1] * span + v[1] * s)
                for s in (width / 2, -width / 2)]
        ents += poly(pts)
        k += pitch
    slab(app, ents, d_top - depth, d_top + 0.001, "cut", "cotes de geneve")


def bridge(app, name, centres, r, holes, d0=None, d1=None, stripes=True,
           pillars=()):
    """A bridge over `centres` (one radius, or one per centre), standing on
    `pillars` down to the mainplate — each with a blued screw let in over it,
    as a bridge screw runs down through its pillar into the plate."""
    part = app.part(name)
    d0 = BRIDGE_D if d0 is None else d0
    d1 = BRIDGE_D + BRIDGE_T if d1 is None else d1
    rs = list(r) if isinstance(r, (list, tuple)) else [r] * len(centres)
    pad = float(PILLAR_R) + 0.35
    outline = pads(list(centres) + list(pillars), rs + [pad] * len(pillars))
    slab(app, outline, d0, d1, "new_body", "bridge")
    if pillars:
        slab(app, [circ(c, PILLAR_R) for c in pillars], 0.0, d0 + 0.01, "add",
             "pillars")
    screws = list(pillars)
    if holes:
        slab(app, [circ(h, rr) for h, rr in holes], d0 - 0.01, d1 + 0.01,
             "cut", "pivot holes")
    if stripes:
        pts = [e["a"] for e in outline]
        cotes(app, pts, d1)
    # A ruby jewel in every pivot hole, set a little below the face in a
    # polished sink. Each is its own body so it can look like a stone; its
    # bore is the hole's, so the staff that passes still touches nothing.
    jewels = [h for h, rr in holes if rr == PIVOT_R]
    if jewels:
        slab(app, [circ(h, PIVOT_R + 0.33) for h in jewels], d1 - 0.12,
             d1 + 0.001, "cut", "jewel sinks")
    for h in jewels:
        slab(app, [circ(h, PIVOT_R + 0.25), circ(h, PIVOT_R)], d1 - 0.12,
             d1 - 0.02, "new_body", "jewel")
    # Blued screws, their heads let in flush like the jewels: a head proud
    # of the face would have to be cleared from the rotor and from the
    # reduction wheel that runs over the barrel bridge.
    if screws:
        slab(app, [circ(c, 0.45) for c in screws], d1 - 0.15, d1 + 0.001,
             "cut", "screw sinks")
    for c in screws:
        slab(app, [circ(c, 0.42)], d1 - 0.15, d1 - 0.01, "new_body", "screw")
    if screws:
        a = math.radians(30.0)
        u, v = (math.cos(a), math.sin(a)), (-math.sin(a), math.cos(a))
        slots = [poly([(c[0] + u[0] * p + v[0] * q, c[1] + u[1] * p + v[1] * q)
                       for p, q in ((-0.36, -0.06), (0.36, -0.06),
                                    (0.36, 0.06), (-0.36, 0.06))])
                 for c in screws]
        slab(app, [e for sl in slots for e in sl], d1 - 0.07, d1 + 0.001,
             "cut", "screw slots")
    part.frame("seat", origin=(0.0, 0.0, 0.0))
    material(app, 0, "brass")
    look(app, 0, "rhodium")
    for b in range(1, len(jewels) + 1):
        material(app, b, "ruby")
        look(app, b, "ruby")
    for b in range(len(jewels) + 1, len(jewels) + len(screws) + 1):
        material(app, b, "steel")
        look(app, b, "blued")
    return part


def bridges(app):
    out = {}
    out["train"] = bridge(
        app, "Train bridge",
        [POS["center"], POS["third"], POS["fourth"], POS["escape"]], 1.10,
        [(POS[k], PIVOT_R) for k in ("center", "third", "fourth", "escape")],
        pillars=S.PILLARS["train"])
    barrel_holes = [(POS["barrel"], 0.55), (POS["crown_wheel"], PIVOT_R),
                    (POS["reduction"], PIVOT_R)]
    # It carries the crown wheel's arbor too, on a pad reaching out to the
    # stem's line.
    out["barrel"] = bridge(
        app, "Barrel bridge", [POS["barrel"], POS["crown_wheel"]],
        [4.10, 0.90], barrel_holes, pillars=S.PILLARS["barrel"])
    # The balance cock: a disc over the balance and an arm out to its one
    # pillar, which pillars() sites at the rim beside the balance. (Its first
    # foot stood at the rim straight out from the balance — over the balance
    # rim's own sweep, which a pillar there would have gone through.)
    b = POS["balance"]
    out["cock"] = bridge(app, "Balance cock", [b], 1.30, [(b, PIVOT_R)],
                         stripes=False, pillars=S.PILLARS["cock"])
    # The pallet bridge sits UNDER the balance, on two feet.
    p = POS["pallet"]
    d = (POS["balance"][0] - p[0], POS["balance"][1] - p[1])
    L = math.hypot(*d)
    n = (-d[1] / L, d[0] / L)
    fa = (p[0] + n[0] * 1.0, p[1] + n[1] * 1.0)
    fb = (p[0] - n[0] * 1.0, p[1] - n[1] * 1.0)
    part = bridge(app, "Pallet bridge", [fa, fb], 0.40, [(p, PIVOT_R)],
                  d0=PALLET_BRIDGE_D, d1=PALLET_BRIDGE_D + PALLET_BRIDGE_T,
                  stripes=False)
    slab(app, [circ(fa, 0.30), circ(fb, 0.30)], 0.0, PALLET_BRIDGE_D, "add",
         "feet")
    out["pallet"] = part
    return out


# --- motion works (dial side) ---------------------------------------------------------

def cannon_pinion(app):
    """Rides on the centre arbor and carries the minute hand."""
    part = app.part("Cannon pinion")
    prism(app, teeth("cannon"), MW_Z, 0.30, "new_body",
          "pinion")
    # The tube starts ABOVE the pinion: at its level it would be wider than
    # the pinion's root and fill the gaps the minute wheel's teeth run in.
    prism(app, [circ((0, 0), 0.40)], MW_Z + 0.30,
          MINUTE_HAND_Z + HAND_T - MW_Z - 0.30, "add", "tube")
    prism(app, [circ((0, 0), 0.17)], MW_Z - 0.01,
          MINUTE_HAND_Z + HAND_T - MW_Z + 0.02, "cut", "bore")
    part.frame("axis", origin=(0.0, 0.0, -float(MAINPLATE_T)))
    material(app, 0, "steel")
    look(app, 0, "polished")
    return part


def minute_wheel(app):
    part = app.part("Minute wheel")
    prism(app, teeth("minute_w"), MW_Z, MINUTE_WHEEL_T, "new_body", "wheel")
    prism(app, teeth("minute_p"), MW_Z + MINUTE_WHEEL_T, 0.40, "add",
          "pinion")
    prism(app, [circ((0, 0), 0.23)], MW_Z - 0.01, 0.62, "cut", "bore")
    part.frame("axis", origin=(0.0, 0.0, -float(MAINPLATE_T)))
    material(app, 0, "brass")
    look(app, 0, "gilt")
    return part


def hour_wheel(app):
    """Carries the hour hand, on a tube around the cannon pinion's."""
    part = app.part("Hour wheel")
    z0 = MW_Z + 0.40
    prism(app, teeth("hour"), z0, 0.20, "new_body", "wheel")
    prism(app, [circ((0, 0), 0.55)], z0, HOUR_HAND_Z + HAND_T - z0, "add",
          "tube")
    prism(app, [circ((0, 0), 0.42)], z0 - 0.01,
          HOUR_HAND_Z + HAND_T - z0 + 0.02, "cut", "bore")
    part.frame("axis", origin=(0.0, 0.0, -float(MAINPLATE_T)))
    material(app, 0, "brass")
    look(app, 0, "gilt")
    return part


# --- dial and hands -------------------------------------------------------------------

def dial(app):
    part = app.part("Dial")
    R = float(DIAL_D) / 2
    prism(app, [circ((0, 0), R)], DIAL_Z, DIAL_T, "new_body", "dial")
    prism(app, [circ((0, 0), 0.75), circ(POS["fourth"], 0.32)], DIAL_Z - 0.01,
          DIAL_T + 0.02, "cut", "hand holes")
    top = DIAL_Z + DIAL_T
    # The small-seconds register, sunk, with concentric grooves.
    prism(app, [circ(POS["fourth"], SUBDIAL_R)], top - SUBDIAL_SINK,
          SUBDIAL_SINK + 0.01, "cut", "register")
    for r in (0.9, 1.5, 2.1, 2.7):
        prism(app, [circ(POS["fourth"], r + 0.04), circ(POS["fourth"], r)],
              top - SUBDIAL_SINK - 0.03, 0.031, "cut", "groove")
    # A railway minute track: sixty short cuts at the edge.
    ticks = []
    for k in range(60):
        a = math.pi / 2 - 2 * math.pi * k / 60
        u, v = (math.cos(a), math.sin(a)), (-math.sin(a), math.cos(a))
        r0, r1, h = R - 1.35, R - 0.75, (0.06 if k % 5 else 0.10)
        ticks += poly([(u[0] * r0 + v[0] * h, u[1] * r0 + v[1] * h),
                       (u[0] * r1 + v[0] * h, u[1] * r1 + v[1] * h),
                       (u[0] * r1 - v[0] * h, u[1] * r1 - v[1] * h),
                       (u[0] * r0 - v[0] * h, u[1] * r0 - v[1] * h)])
    prism(app, ticks, top - 0.04, 0.041, "cut", "minute track")
    part.frame("seat", origin=(0.0, 0.0, 0.0))
    material(app, 0, "brass")
    look(app, 0, "cream")
    return part


def indices(app):
    """Applied gold batons, doubled at twelve, none at six (the register)."""
    part = app.part("Indices")
    top = DIAL_Z + DIAL_T
    R = float(DIAL_D) / 2
    ents = []
    for k in range(12):
        if k == 6:
            continue
        a = math.pi / 2 - 2 * math.pi * k / 12
        u, v = (math.cos(a), math.sin(a)), (-math.sin(a), math.cos(a))
        for off in ((-0.34, 0.34) if k == 0 else (0.0,)):
            c = (v[0] * off, v[1] * off)
            h = 0.22
            r0, r1 = R - 4.0, R - 1.7
            ents += poly([(c[0] + u[0] * r0 + v[0] * h,
                           c[1] + u[1] * r0 + v[1] * h),
                          (c[0] + u[0] * r1 + v[0] * h,
                           c[1] + u[1] * r1 + v[1] * h),
                          (c[0] + u[0] * r1 - v[0] * h,
                           c[1] + u[1] * r1 - v[1] * h),
                          (c[0] + u[0] * r0 - v[0] * h,
                           c[1] + u[1] * r0 - v[1] * h)])
    prism(app, ents, top, 0.20, "new_body", "batons")
    part.frame("seat", origin=(0.0, 0.0, 0.0))
    material(app, 0, "gold")
    look(app, 0, "gold")
    return part


def hand(app, name, length, tail, width, hub_r, hole_r, z, shape="leaf"):
    """A hand pointing +Y at dof 0 (12 o'clock). Leaf for hours and minutes,
    a needle for seconds."""
    part = app.part(name)
    if shape == "leaf":
        pts = [(0.0, length), (width / 2, length * 0.55), (width * 0.28, 0.0),
               (0.0, -tail), (-width * 0.28, 0.0), (-width / 2, length * 0.55)]
    else:
        pts = [(0.0, length), (width / 2, 0.0), (width * 0.9, -tail),
               (-width * 0.9, -tail), (-width / 2, 0.0)]
    prism(app, poly(pts), z, HAND_T, "new_body", "blade")
    prism(app, [circ((0, 0), hub_r)], z, HAND_T, "add", "hub")
    prism(app, [circ((0, 0), hole_r)], z - 0.01, HAND_T + 0.02, "cut", "hole")
    part.frame("axis", origin=(0.0, 0.0, -float(MAINPLATE_T)))
    material(app, 0, "steel")
    look(app, 0, "blued")
    return part


def hands(app):
    return {
        "hour": hand(app, "Hour hand", 8.2, 1.4, 1.30, 0.85, 0.55,
                     HOUR_HAND_Z),
        "minute": hand(app, "Minute hand", 13.2, 1.8, 1.00, 0.65, 0.40,
                       MINUTE_HAND_Z),
        "seconds": hand(app, "Seconds hand", 2.9, 0.9, 0.16, 0.30, 0.15,
                        SECONDS_HAND_Z, shape="needle"),
    }


# --- automatic winding ---------------------------------------------------------------

def rotor(app):
    """A skeletonised rotor with a gold weight, pivoting at the centre above
    the bridges. Its pinion drives the reduction wheel."""
    part = app.part("Rotor")
    top = CASEBACK_D - 0.30                      # never touches the caseback
    slab(app, [circ((0, 0), ROTOR_STAFF_R)], BRIDGE_D + BRIDGE_T + 0.10,
         ROTOR_D + ROTOR_T, "new_body", "staff")
    # The hub boss sits ABOVE the pinion's level, where nothing meshes.
    slab(app, [circ((0, 0), 0.80)], ROTOR_D, ROTOR_D + ROTOR_T + 0.15, "add",
         "hub")
    slab(app, teeth("rotor_pin"), REDUCTION_D,
         REDUCTION_D + 0.20, "add", "pinion")
    R = 12.4
    # The plate: a half disc with three windows.
    pts = [pol(1.2, 0.0)] + [pol(R - 1.8, math.pi * k / 32) for k in range(33)] \
        + [pol(1.2, math.pi)] + [pol(1.2, math.pi + math.pi * k / 8)
                                 for k in range(1, 8)]
    slab(app, poly(pts), ROTOR_D, ROTOR_D + ROTOR_T, "add", "plate")
    for a0, a1 in ((0.35, 0.95), (1.25, 1.90), (2.20, 2.80)):
        w = [pol(3.0, a0)] + [pol(R - 2.6, a0 + (a1 - a0) * k / 8)
                              for k in range(9)] + [pol(3.0, a1)]
        slab(app, poly(w), ROTOR_D - 0.01, ROTOR_D + ROTOR_T + 0.01, "cut",
             "window")
    # Stripe the plate NOW. The weight is a second, thicker body, and a
    # stripe cut made after it exists would carve thin slots through the gold.
    cotes(app, pts, ROTOR_D + ROTOR_T, pitch=1.3, angle_deg=90.0)
    # The weight: a heavy gold segment at the rim, abutting the plate.
    seg_pts = [pol(R, math.pi * k / 32) for k in range(33)] + \
        [pol(R - 1.8, math.pi - math.pi * k / 32) for k in range(33)]
    slab(app, poly(seg_pts), ROTOR_D, min(ROTOR_D + ROTOR_WEIGHT_T, top),
         "new_body", "weight")
    part.frame("axis", origin=(0.0, 0.0, -float(MAINPLATE_T)))
    material(app, 0, "brass")
    look(app, 0, "rhodium")
    material(app, 1, "gold")
    look(app, 1, "gold")
    return part


def reduction_wheel(app):
    return arbor(app, "Reduction wheel", staff=(BRIDGE_D + BRIDGE_T - 0.4,
                                                REDUCTION_D + 0.22),
                 wheel=("reduction_w", REDUCTION_D, 0.20),
                 pinion=("reduction_p", RATCHET_D - 0.05,
                         RATCHET_D + RATCHET_T + 0.05), spokes=4, hub=0.5)


def crown_wheel(app):
    """The crown wheel meshing the ratchet on the barrel bridge, and on the
    same arbor, down in a pocket in the mainplate, the winding wheel the
    stem's winding pinion drives. The arbor stops at the winding wheel's top
    face: it must never reach the level of the stem, which runs over it."""
    zc = float(STEM_Z) - S.GEAR["wp_rack"]["rp"]     # winding wheel mid-plane
    t = float(KEYLESS_WHEEL_T)
    d0 = -(zc + t / 2) - float(MAINPLATE_T)          # its dial-side face
    d1 = -(zc - t / 2) - float(MAINPLATE_T)
    return arbor(app, "Crown wheel", staff=(d0, RATCHET_D + RATCHET_T + 0.15),
                 wheel=("crown_wheel", RATCHET_D, RATCHET_T),
                 pinion=("winding_w", d0, d1), spokes=5, hub=0.7,
                 look_as="rhodium")


# --- keyless and case --------------------------------------------------------------------

# The stem and everything riding on it are drawn in STEM coordinates: X along
# the stem, its axis through the origin (world z = local z + STEM_Z), so a YZ
# sketch plane at x has u = world Y and v = local Z. Each part's "axis" frame
# sits on that axis and matches the mainplate's "stem" frame, so dof 0 leaves
# it where it was drawn: the WINDING position. Angles about the stem are
# measured from +Y towards +Z, and tooth 0 of every contrate ring is at +Y.

def contrate(app, key, x_face, sign, name):
    """A ring of contrate teeth on the end face at x_face, standing out
    towards sign * X. Past the contact a contrate tooth moves as a rack tooth
    does, so each is watch_gearing's cycloidal rack tooth, drawn in the plane
    through its radial line and extruded across the face width — one tooth,
    then a circular Pattern round the stem. (A Pattern of an extrude that
    ADDS used to fail, and this went in as one extrude per tooth.)"""
    g = S.GEAR[key]
    w = float(CONTRATE_W)
    ents = watch_gearing.rack_profile(float(MODULE), g["rho_add"],
                                      g["rho_ded"], g["frac"], g["h_a"],
                                      g["h_f"], sink=0.02)
    # Tooth 0 on +Y: its plane faces along +Y (the radial line), u runs
    # along +Z (the way the tooth moves), and v = normal x u = sign * X.
    sid = app.call("add_feature", feature={
        "type": "Sketch", "name": name, "entities": ents,
        "plane": {"origin": [float(x_face), g["rp"] - w / 2, 0.0],
                  "normal": [0, 1, 0], "uAxis": [0, 0, float(sign)]}})["id"]
    fid = app.call("add_feature", feature={
        "type": "Extrude", "name": name, "sketch_id": sid, "distance": w,
        "mode": "add"})["id"]
    app.call("add_feature", feature={
        "type": "Pattern", "name": name + " ring", "kind": "circular",
        "source_id": fid, "axis": {"kind": "x"}, "count": g["z"],
        "angle_deg": 360.0 / g["z"]})


def dogs(app, x0, length, phase_deg, name, n=4, r0=0.36, r1=0.62,
         span_deg=40.0):
    """n dog teeth, annular sectors from x0 along +X: the clutch's square
    drive into the winding pinion. (A real one is Breguet — sawteeth that
    slip one way; square dogs are the same engagement without the slip.)"""
    ents = []
    for k in range(n):
        c = math.radians(phase_deg + 360.0 * k / n)
        h = math.radians(span_deg) / 2
        a0, a1 = (c - h) % (2 * math.pi), (c + h) % (2 * math.pi)
        ents += [arc((0, 0), r1, a0, a1), seg(pol(r1, a1), pol(r0, a1)),
                 arc((0, 0), r0, a0, a1), seg(pol(r0, a0), pol(r1, a0))]
    prism(app, ents, 0, length, "add", name, pl=plane_x(x0))


def _stem_frame(part):
    part.frame("axis", origin=(0.0, 0.0, 0.0), z=(1.0, 0.0, 0.0),
               x=(0.0, 1.0, 0.0))


def _square(half):
    return poly([(-half, -half), (half, -half), (half, half), (-half, half)])


def stem(app):
    """The stem: a square where the clutch slides, round through the
    winding pinion and out through the case, a groove the setting lever's
    pin rides in, and the crown."""
    K = S.KEYLESS
    part = app.part("Stem and crown")
    R = float(CASE_D) / 2
    x_end, x_sq = K["stem_end"], K["square_to"]
    prism(app, _square(float(STEM_SQUARE) / 2), 0, x_sq - x_end, "new_body",
          "square", pl=plane_x(x_end))
    prism(app, [circ((0, 0), STEM_R)], 0, R + 1.2 - x_sq + 0.01, "add",
          "stem", pl=plane_x(x_sq - 0.01))
    prism(app, [circ((0, 0), STEM_R + 0.01), circ((0, 0), 0.20)], 0, 0.16,
          "cut", "lever groove", pl=plane_x(float(STEM_GROOVE_X) - 0.08))
    # The crown: fluted, with a polished cap.
    cr = float(CROWN_D) / 2
    flutes = []
    n = 20
    for k in range(n):
        a0 = 2 * math.pi * k / n
        a1 = a0 + math.pi / n
        a2 = a0 + 2 * math.pi / n
        flutes += [arc((0, 0), cr, a0, a1),
                   seg(pol(cr, a1), pol(cr - 0.18, a1)),
                   arc((0, 0), cr - 0.18, a1, a2 % (2 * math.pi)),
                   seg(pol(cr - 0.18, a2), pol(cr, a2))]
    prism(app, flutes, 0, CROWN_L, "add", "crown",
          pl=plane_x(R + 0.7))                 # 0.1 clear of the tube
    prism(app, [circ((0, 0), cr - 0.30)], 0, 0.35, "add", "cap",
          pl=plane_x(R + 0.7 + CROWN_L))
    _stem_frame(part)
    material(app, 0, "stainless_steel")
    look(app, 0, "polished")
    return part


def winding_pinion(app):
    """Free on the stem's round: dogs on its inner face, which the clutch
    engages crown-in, and contrate teeth on its outer face driving the
    winding wheel below the stem."""
    K = S.KEYLESS
    part = app.part("Winding pinion")
    xa, xe = K["wp_inner"], K["wp_face"]
    prism(app, [circ((0, 0), KEYLESS_R), circ((0, 0), STEM_R + 0.02)], 0,
          xe - xa, "new_body", "pinion", pl=plane_x(xa))
    dogs(app, xa - DOG_H, DOG_H + 0.02, 0.0, "dogs")
    contrate(app, "wp_rack", xe, +1, "contrate teeth")
    _stem_frame(part)
    material(app, 0, "steel")
    look(app, 0, "polished")
    return part


def clutch(app):
    """The sliding pinion, on the stem's square: dogs on its outer face into
    the winding pinion, contrate teeth on its inner face for the setting
    wheel, and a groove the yoke's pin rides in. Drawn crown-in."""
    K = S.KEYLESS
    part = app.part("Clutch")
    xi, xo = K["clutch_inner"], K["clutch_outer"]
    prism(app, [circ((0, 0), KEYLESS_R)]
          + _square(float(STEM_SQUARE) / 2 + 0.02), 0, xo - xi, "new_body",
          "clutch", pl=plane_x(xi))
    xm = (xi + xo) / 2
    prism(app, [circ((0, 0), KEYLESS_R + 0.01), circ((0, 0), 0.47)], 0, 0.16,
          "cut", "yoke groove", pl=plane_x(xm - 0.08))
    dogs(app, xo - 0.02, DOG_H + 0.02, 45.0, "dogs")
    contrate(app, "clutch_rack", xi, -1, "contrate teeth")
    _stem_frame(part)
    material(app, 0, "steel")
    look(app, 0, "polished")
    return part


def setting_wheel(app):
    """Always meshed with the minute wheel, at its level; the clutch's
    contrate teeth reach it only crown-out. It turns on a stud hanging from
    the keyless bridge, which stops short of the stem running under it."""
    part = app.part("Setting wheel")
    z0 = float(SETTING_MID_Z) - float(KEYLESS_WHEEL_T) / 2
    prism(app, teeth("setting_w"), z0, KEYLESS_WHEEL_T, "new_body", "wheel")
    prism(app, [circ((0, 0), 0.19)], z0 - 0.01, KEYLESS_WHEEL_T + 0.02,
          "cut", "bore")
    part.frame("axis", origin=(0.0, 0.0, -float(MAINPLATE_T)))
    material(app, 0, "steel")
    look(app, 0, "polished")
    return part


def lever_pivots():
    """Each lever turns on a stud ARM from where its pin meets its groove,
    square to the stem, so its pin moves along the stem as it turns."""
    K = S.KEYLESS
    return {"setting_lever": (float(STEM_GROOVE_X), float(SETTING_LEVER_ARM)),
            "yoke": ((K["clutch_inner"] + K["clutch_outer"]) / 2,
                     float(YOKE_ARM))}


def lever(app, name, arm, pin_tip_z):
    """A keyless lever: a plate turning on its stud at the origin, with a pin
    at (0, -arm) reaching down into the groove it drives."""
    part = app.part(name)
    z0, t = float(LEVER_Z), float(LEVER_T)
    prism(app, pads([(0.0, 0.0), (0.0, -arm), (0.7, 0.9)],
                    [0.45, 0.20, 0.25]), z0, t, "new_body", "lever")
    prism(app, [circ((0, 0), 0.22)], z0 - 0.01, t + 0.02, "cut", "pivot hole")
    prism(app, [circ((0.0, -arm), 0.06)], pin_tip_z, z0 - pin_tip_z + 0.01,
          "add", "pin")
    part.frame("axis", origin=(0.0, 0.0, -float(MAINPLATE_T)))
    material(app, 0, "steel")
    look(app, 0, "polished")
    return part


def keyless_bridge(app):
    """The plate over the keyless works, under the dial: it holds the
    setting wheel's stud and caps the levers' studs, on two feet."""
    part = app.part("Keyless bridge")
    z0, t = float(KEYLESS_BRIDGE_Z), float(KEYLESS_BRIDGE_T)
    sx = POS["setting"][0]
    lp = lever_pivots()
    feet = [(sx - 0.4, -1.5), (11.8, 1.6)]
    prism(app, pads([(sx, 0.0), lp["yoke"], lp["setting_lever"], (11.8, 0.0)]
                    + feet, [0.5, 0.45, 0.45, 0.7, 0.4, 0.4]), z0, t,
          "new_body", "bridge")
    prism(app, [circ(lp["yoke"], 0.19), circ(lp["setting_lever"], 0.19)],
          z0 - 0.01, t + 0.02, "cut", "stud holes")
    zs = float(SETTING_MID_Z) - float(KEYLESS_WHEEL_T) / 2 - 0.03
    prism(app, [circ((sx, 0.0), 0.15)], zs, z0 - zs + 0.01, "add",
          "setting wheel stud")
    prism(app, [circ(f, 0.30) for f in feet], 0.0, z0 + 0.01, "add", "feet")
    part.frame("seat", origin=(0.0, 0.0, 0.0))
    material(app, 0, "brass")
    look(app, 0, "rhodium")
    return part


def keyless_seat(app, R):
    """What the mainplate gives the keyless works: a slot for the stem, open
    to the dial face; pockets for the clutch and winding pinion (in both
    positions) and for the winding wheel below the stem; and the levers'
    studs."""
    K = S.KEYLESS
    zs = float(STEM_Z)
    w = float(STEM_R) + 0.03
    x0 = K["stem_end"] - 0.05
    prism(app, poly([(-w, zs - w), (w, zs - w), (w, 0.01), (-w, 0.01)]), 0,
          R + 0.1 - x0, "cut", "stem slot", pl=plane_x(x0))
    kr = float(KEYLESS_R) + 0.08
    # From the clutch's contrate TIPS crown-out (not its face: the tips stand
    # h_f + h_a proud of it, and struck the pocket's end 0.029 mm3 deep) to
    # the winding pinion's tips.
    cr = S.GEAR["clutch_rack"]
    xa = K["clutch_inner_set"] - cr["h_f"] - cr["h_a"] - 0.05
    xb = K["wp_tip"] + 0.05
    prism(app, poly([(xa, -kr), (xb, -kr), (xb, kr), (xa, kr)]), zs - kr,
          0.01 - (zs - kr), "cut", "keyless pocket")
    zw = zs - S.GEAR["wp_rack"]["rp"] - float(KEYLESS_WHEEL_T) / 2 - 0.08
    prism(app, [circ(POS["crown_wheel"], S.tip_r("winding_w") + 0.10)], zw,
          0.01 - zw, "cut", "winding wheel pocket")
    prism(app, [circ(c, 0.18) for c in lever_pivots().values()], 0.0,
          KEYLESS_BRIDGE_Z, "add", "lever studs")


def case(app):
    parts = {}
    R = float(CASE_D) / 2
    z0, z1 = float(CASE_Z0), float(CASE_Z1) - 0.7
    part = app.part("Case")
    prism(app, [circ((0, 0), R)], z0, z1 - z0, "new_body", "middle")
    prism(app, [circ((0, 0), float(MOVEMENT_D) / 2 + 0.05)], z0 - 0.01,
          z1 - z0 + 0.02, "cut", "movement seat")
    prism(app, [circ((0, 0), float(DIAL_D) / 2 + 0.30)], 0.0, z1 + 0.01,
          "cut", "dial seat")
    # Lugs at twelve and six, their inner faces LUG_W apart. Drawn in side
    # view — the top falls from under the bezel in an arc to a rounded tip
    # that carries the spring bar — and extruded to their width.
    lug_t, tip_r = float(LUG_T), 1.5
    x_out = float(LUG_W) / 2 + lug_t
    # The root starts inside the wall where the wall is nearest the axis —
    # at the lug's OUTER face. A literal R - 3 put it 1.3 mm outside the case
    # there, and each lug hung on by a 0.3 mm sliver at its inner edge.
    root = math.sqrt(R * R - x_out * x_out) - float(LUG_ROOT)
    zt, zl, zc = z1 - 0.4, z0 + 2.0, z0 + 5.0
    for sy in (1, -1):
        yc, y_in = sy * (R + 3.0), sy * root   # yc: the spring bar
        side = [seg((y_in, zl), (y_in, zt)),
                arc_through((y_in, zt), (sy * (R + 1.0), zt - 1.1),
                            (yc, zc + tip_r)),
                arc_through((yc, zc + tip_r), (yc + sy * tip_r, zc),
                            (yc, zc - tip_r)),
                seg((yc, zc - tip_r), (y_in, zl))]
        for sx in (1, -1):
            x = float(LUG_W) / 2 if sx > 0 else -(float(LUG_W) / 2 + lug_t)
            prism(app, side, 0, lug_t, "add", "lug", pl=plane_x(x))
        prism(app, [circ((yc, zc), 0.45)], 0, float(LUG_W) + 2 * lug_t + 1.0,
              "cut", "spring bar hole",
              pl=plane_x(-(float(LUG_W) / 2 + lug_t + 0.5)))
        # In plan each lug narrows by half a millimetre toward its tip, the
        # taper starting where its outer face leaves the case.
        y_leave = math.sqrt(R * R - x_out * x_out)
        y_end = R + 3.0 + tip_r + 0.2
        for sx in (1, -1):
            prism(app, poly([(sx * (x_out + 0.1), sy * y_leave),
                             (sx * (x_out + 0.1), sy * y_end),
                             (sx * (x_out - 0.5), sy * y_end)]),
                  z0 - 0.1, z1 - z0 + 0.2, "cut", "lug taper")
    # The stem's way through the wall at three o'clock.
    prism(app, [circ((0.0, float(STEM_Z)), STEM_R + 0.10)], 0,
          R - float(MOVEMENT_D) / 2 + 1.0, "cut", "stem hole",
          pl=plane_x(float(MOVEMENT_D) / 2 - 0.5))
    prism(app, [circ((0.0, float(STEM_Z)), 1.10),
                circ((0.0, float(STEM_Z)), STEM_R + 0.10)], 0, 0.6, "add",
          "crown tube", pl=plane_x(R))
    part.frame("seat", origin=(0.0, 0.0, 0.0))
    material(app, 0, "stainless_steel")
    look(app, 0, "brushed")
    parts["case"] = part

    # The bezel is turned: a rounded outer edge and a polished bevel down to
    # the crystal, revolved from its section.
    part = app.part("Bezel")
    ri, h, e = R - float(BEZEL_W) - 2.3, 1.4, 0.9
    section = [seg((ri, z1), (R, z1)), seg((R, z1), (R, z1 + h - e)),
               arc_through((R, z1 + h - e),
                           pol(e, math.pi / 4, (R - e, z1 + h - e)),
                           (R - e, z1 + h)),
               seg((R - e, z1 + h), (ri + 1.2, z1 + h)),
               seg((ri + 1.2, z1 + h), (ri, z1 + 0.75)),
               seg((ri, z1 + 0.75), (ri, z1))]
    sid = app.call("add_feature", feature={
        "type": "Sketch", "name": "bezel section", "entities": section,
        "plane": {"origin": [0.0, 0.0, 0.0], "normal": [0, -1, 0],
                  "uAxis": [1, 0, 0]}})["id"]    # u = world X, v = world Z
    app.call("add_feature", feature={
        "type": "Revolve", "name": "bezel", "sketch_id": sid,
        "axis": {"kind": "z"}, "angle_deg": 360.0, "mode": "new_body"})
    part.frame("seat", origin=(0.0, 0.0, 0.0))
    material(app, 0, "stainless_steel")
    look(app, 0, "polished")
    parts["bezel"] = part

    part = app.part("Crystal")
    prism(app, [circ((0, 0), R - float(BEZEL_W) - 2.3)], CRYSTAL_Z,
          CRYSTAL_T, "new_body", "crystal")
    part.frame("seat", origin=(0.0, 0.0, 0.0))
    material(app, 0, "glass")
    parts["crystal"] = part

    part = app.part("Caseback")
    zc = z0 - 1.6
    prism(app, [circ((0, 0), R - 1.0), circ((0, 0), 12.0)], zc, 1.6,
          "new_body", "caseback")
    part.frame("seat", origin=(0.0, 0.0, 0.0))
    material(app, 0, "stainless_steel")
    look(app, 0, "polished")
    parts["caseback"] = part

    part = app.part("Caseback window")
    prism(app, [circ((0, 0), 12.0)], zc + 0.3, 1.1, "new_body", "window")
    part.frame("seat", origin=(0.0, 0.0, 0.0))
    material(app, 0, "glass")
    parts["window"] = part
    return parts
