# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Theo Jansen's leg as numbers: its lengths, its joints at a crank angle,
its foot curve, and the layer every bar of a leg pair goes in.

Pure python, no app: build.py models the parts from it and then checks
the app's own motion study against it, so a mistake here shows up as a
disagreement there rather than as a walker that does not walk.

The lengths are Jansen's "holy numbers" for a crank of 15, scaled by
`size / 15` (`--size` is the crank radius). The frame is the leg's own:
O, the crank axis, at the origin; the frame pivot B at (-a, -l); y up; the
leg reaching to -x. The mirrored (back) leg is x -> -x.
"""
import math

# Jansen's eleven lengths and the frame's two (a, l), for m = 15.
HOLY = dict(a=38.0, b=41.5, c=39.3, d=40.1, e=55.8, f=39.4, g=36.7, h=65.7,
            i=49.0, j=50.0, k=61.9, l=7.8, m=15.0)

# The bodies of one leg and the joints each carries, in its own frame's
# terms: (first joint, second joint) sets the body's x axis; a triangle's
# third vertex is placed in it.
BARS = {
    "j": ("A", "C"),        # crank pin to the upper joint
    "k": ("A", "E"),        # crank pin to the lower joint
    "bcd": ("B", "C", "D"),  # the upper triangle, on the frame pivot
    "c": ("B", "E"),        # frame pivot to the lower joint
    "f": ("D", "F"),        # the upper triangle to the knee
    "efg": ("E", "F", "G"),  # the foot triangle
}
# The joints that are pins of the leg (A is the crank pin, part of the
# crankshaft; B is the frame's pin, part of a frame plate), and the bars on
# each.
JOINTS = {
    "A": ("j", "k"),
    "B": ("bcd", "c"),
    "C": ("j", "bcd"),
    "D": ("bcd", "f"),
    "E": ("k", "c", "efg"),
    "F": ("f", "efg"),
}
# The dyads with a mirror branch -- the knees -- as (vertex, p, q): the
# vertex's side of the line p -> q is what a sweep must keep.
KNEES = (("C", "A", "B"), ("E", "A", "B"), ("F", "D", "E"))


def lengths(size):
    s = size / HOLY["m"]
    return {k: v * s for k, v in HOLY.items()}


def dyad(p, r1, q, r2, side):
    """The point r1 from p and r2 from q, left of p -> q for side +1."""
    dx, dy = q[0] - p[0], q[1] - p[1]
    d = math.hypot(dx, dy)
    a = (r1 * r1 - r2 * r2 + d * d) / (2.0 * d)
    h2 = r1 * r1 - a * a
    if h2 < 0.0:
        raise ValueError(f"the dyad does not close ({r1:.3f}, {r2:.3f} over "
                         f"{d:.3f})")
    h = math.sqrt(h2)
    ux, uy = dx / d, dy / d
    return (p[0] + a * ux - side * h * uy, p[1] + a * uy + side * h * ux)


def pose(L, theta, mirror=False):
    """Every joint at crank angle theta (radians, the crank's own sense):
    the built branch, by circle intersections. A mirrored leg is the same
    leg reflected x -> -x, which turns its crank the other way: its joints at
    theta are the front leg's at pi - theta, reflected."""
    if mirror:
        p = pose(L, math.pi - theta)
        return {k: (-x, y) for k, (x, y) in p.items()}
    O = (0.0, 0.0)
    B = (-L["a"], -L["l"])
    A = (L["m"] * math.cos(theta), L["m"] * math.sin(theta))
    C = dyad(A, L["j"], B, L["b"], -1)
    E = dyad(A, L["k"], B, L["c"], +1)
    D = dyad(B, L["d"], C, L["e"], +1)
    F = dyad(D, L["f"], E, L["g"], -1)
    G = dyad(E, L["i"], F, L["h"], +1)
    return dict(O=O, A=A, B=B, C=C, D=D, E=E, F=F, G=G)


def cross(o, a, b):
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])


def knee_signs(p):
    """The three knees' signs: which side of p -> q each vertex is on."""
    return tuple(1 if cross(p[v], p[a], p[b]) > 0 else -1
                 for v, a, b in KNEES)


def local(origin, toward, point):
    """`point` in a frame at `origin` whose x points at `toward`."""
    ang = math.atan2(toward[1] - origin[1], toward[0] - origin[0])
    c, s = math.cos(ang), math.sin(ang)
    dx, dy = point[0] - origin[0], point[1] - origin[1]
    return (dx * c + dy * s, -dx * s + dy * c)


def bar_angle(p, bar):
    a, b = BARS[bar][0], BARS[bar][1]
    return math.atan2(p[b][1] - p[a][1], p[b][0] - p[a][0])


def foot_curve(L, samples, mirror=False, phase=0.0):
    """The foot's (x, y) at `samples` crank angles over a turn, from
    `phase`."""
    return [pose(L, phase + 2.0 * math.pi * i / samples, mirror)["G"]
            for i in range(samples)]


def stride(curve, band):
    """The foot's walk off a closed curve: the lowest point, the lift (how
    high it rises over that), and the STRIDE -- the longest run of samples
    within `band` of the lowest, as its length along x and its flatness
    (the most any of them stands over the lowest). A foot on the stride is
    on the ground; the rest of the turn it is in the air."""
    n = len(curve)
    low = min(y for _, y in curve)
    on = [y - low <= band for _, y in curve]
    best = (0, 0)
    if all(on):
        best = (0, n)
    else:
        start = on.index(False)
        run = 0
        for k in range(1, n + 1):
            i = (start + k) % n
            if on[i]:
                run += 1
                if run > best[1]:
                    best = ((i - run + 1) % n, run)
            else:
                run = 0
    first, count = best
    pts = [curve[(first + k) % n] for k in range(count)]
    xs = [x for x, _ in pts]
    return {"lowest_mm": low,
            "lift_mm": max(y for _, y in curve) - low,
            "stride_mm": max(xs) - min(xs) if xs else 0.0,
            "flatness_mm": max(y for _, y in pts) - low if pts else 0.0,
            "on_ground_samples": count, "first_sample": first,
            "fraction_on_ground": count / n}


# ---- layers ------------------------------------------------------------

def seg_dist(p, q, a, b):
    """The distance between segments p-q and a-b."""
    def pt_seg(o, s0, s1):
        vx, vy = s1[0] - s0[0], s1[1] - s0[1]
        L2 = vx * vx + vy * vy
        t = 0.0 if L2 == 0 else max(0.0, min(1.0, (
            (o[0] - s0[0]) * vx + (o[1] - s0[1]) * vy) / L2))
        return math.hypot(o[0] - s0[0] - t * vx, o[1] - s0[1] - t * vy)

    def inter(p1, p2, p3, p4):
        d1, d2 = cross(p3, p4, p1), cross(p3, p4, p2)
        d3, d4 = cross(p1, p2, p3), cross(p1, p2, p4)
        return (d1 * d2 < 0) and (d3 * d4 < 0)
    if inter(p, q, a, b):
        return 0.0
    return min(pt_seg(p, a, b), pt_seg(q, a, b), pt_seg(a, p, q),
               pt_seg(b, p, q))


def edges(points):
    """A body's skeleton: its bar, or its triangle's three sides."""
    if len(points) == 2:
        return [tuple(points)]
    return [(points[i], points[(i + 1) % len(points)])
            for i in range(len(points))]


def skel_dist(e1, e2):
    return min(seg_dist(a, b, c, d) for a, b in e1 for c, d in e2)


# ---- which bar goes in which layer ---------------------------------------
#
# A leg pair's bars, pins and heads share the space between two frame plates
# in LAYERS across the crank axis, layer 1 the plate's side. Two bodies in
# one layer must never come within `clear` of each other over the turn. A
# joint's pin passes through its bars' layers lo..hi, its HEAD sits in layer
# lo - 1 and its pressed CAP in layer hi + 1, so no other bar may come near
# the joint in any layer from lo - 1 to hi + 1. The frame pivot B's pin is
# printed on the plate: it stands from layer 0 to a cap over its bars. The
# crank pin A spans the whole slab, so nothing but its own bars may ever come
# near it.

def _sweep(L, samples):
    return {s: [pose(L, 2.0 * math.pi * i / samples, s == "back")
                for i in range(samples)] for s in SIDES}


SIDES = ("front", "back")
PINS = ("C", "D", "E", "F")  # pins with a head and a cap; B is the plate's


def proximity(L, width, samples):
    """The closest approach over a turn of every pair of bodies, and of
    every joint to every bar not on it, skeleton to skeleton (subtract the
    widths to get the air)."""
    poses = _sweep(L, samples)
    els = [(s, b) for s in SIDES for b in BARS]

    def skel(e, i):
        s, b = e
        p = poses[s][i]
        return edges([p[n] for n in BARS[b]])
    bars = {}
    for x in range(len(els)):
        for y in range(x + 1, len(els)):
            a, b = els[x], els[y]
            bars[(a, b)] = min(skel_dist(skel(a, i), skel(b, i))
                               for i in range(samples))
    joints = {}
    for s in SIDES:
        for J in "ABCDEF":
            for e in els:
                if e[0] == s and e[1] in JOINTS[J]:
                    continue
                if J == "A" and e[1] in JOINTS["A"]:
                    continue  # both legs' crank bars ride on the one pin
                joints[((s, J), e)] = min(
                    skel_dist([(poses[s][i][J], poses[s][i][J])], skel(e, i))
                    for i in range(samples))
    heads = {}
    pins = [(s, J) for s in SIDES for J in ("B",) + PINS]
    for x in range(len(pins)):
        for y in range(x + 1, len(pins)):
            a, b = pins[x], pins[y]
            heads[(a, b)] = min(math.hypot(
                poses[a[0]][i][a[1]][0] - poses[b[0]][i][b[1]][0],
                poses[a[0]][i][a[1]][1] - poses[b[0]][i][b[1]][1])
                for i in range(samples))
    return {"bars": bars, "joints": joints, "heads": heads}


def assign_layers(prox, width, head_r, crank_pin_r, clear, max_layers=10):
    """The layer of every bar of a leg pair: the fewest layers (caps
    included), then the shortest pins. Raises when no assignment exists,
    which means the bodies are too wide for the linkage."""
    els = [(s, b) for s in SIDES for b in BARS]
    near_bar = width + clear
    near_joint = head_r + width / 2.0 + clear
    conflict = {e: set() for e in els}
    for (a, b), dist in prox["bars"].items():
        if dist < near_bar:
            conflict[a].add(b)
            conflict[b].add(a)
    for ((s, J), e), dist in prox["joints"].items():
        if J == "A" and dist < crank_pin_r + width / 2.0 + clear:
            raise ValueError(f"the {e[0]} {e[1]} bar runs over the crank pin "
                             f"of the {s} leg ({dist:.2f} mm): no layer can "
                             f"hold it")
    near = {(s, J): [e for ((s2, J2), e), dist in prox["joints"].items()
                     if (s2, J2) == (s, J) and dist < near_joint]
            for s in SIDES for J in ("B",) + PINS}
    pins = [(s, J) for s in SIDES for J in ("B",) + PINS]
    head_clash = [(a, b) for (a, b), dist in prox["heads"].items()
                  if dist < 2.0 * head_r + clear]
    order = sorted(els, key=lambda e: -len(conflict[e]))

    def spans(layer):
        out = {}
        for s, J in pins:
            ls = [layer[(s, b)] for b in JOINTS[J]]
            lo = 0 if J == "B" else min(ls) - 1  # B stands on the plate
            out[(s, J)] = (lo, max(ls) + 1, min(ls) - 1)
        return out

    def ok(layer):
        sp = spans(layer)
        for (s, J), (lo, hi, head) in sp.items():
            if head < 0 and J != "B":
                return False
            for e in near[(s, J)]:
                if lo <= layer[e] <= hi:
                    return False
        for a, b in head_clash:
            la = {sp[a][0], sp[a][1]} if a[1] != "B" else {sp[a][1]}
            lb = {sp[b][0], sp[b][1]} if b[1] != "B" else {sp[b][1]}
            if la & lb:
                return False
        return True

    for top in range(2, max_layers + 2):
        best = None
        layer = {}

        def search(k):
            nonlocal best
            if k == len(order):
                if not ok(layer):
                    return
                sp = spans(layer)
                if max(hi for _, hi, _ in sp.values()) > top:
                    return
                cost = sum(hi - lo for lo, hi, _ in sp.values())
                if best is None or cost < best[0]:
                    best = (cost, dict(layer))
                return
            e = order[k]
            for ell in range(1, top):
                if any(layer.get(o) == ell for o in conflict[e]):
                    continue
                layer[e] = ell
                search(k + 1)
                del layer[e]
        search(0)
        if best is not None:
            return {"layers": best[1], "top": top,
                    "pins": {f"{s} {J}": {"from": lo, "to": hi}
                             for (s, J), (lo, hi, _) in
                             spans(best[1]).items()}}
    raise ValueError(f"no assignment of the bars to {max_layers} layers "
                     "keeps every crossing pair apart")
