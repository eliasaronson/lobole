"""The numbers the FS chassis is built from.

The FRAME is a team's own CAD: `fs_chassis_frame.json`, read out of its
SolidWorks frame part by `sldprt_frame.py` — every node, every
member, its tube and its bends, in the CAD's coordinates. Nothing about the
frame's shape is assumed here any more; what is left in this file is the
SHOP (the bender's dies, the stock, the saw, the weld gap, the gauges) and
the stations the jig and the panels are placed at, which are DERIVED from
the node table so they move when the frame does.

Axes are the CAD's: X runs REARWARD (the front bulkhead is the lowest x),
Y across the car (the frame is mirrored about y = 0), Z up from the table the
jig sits on (the lowest tube sits some 45 mm above it).
"""
import json
import os

FRAME = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "fs_chassis_frame.json")))
NODES = [tuple(p) for p in FRAME["nodes"]]

# --- the stock ------------------------------------------------------------
# READ from the CAD: each member's outer and inner radius. The team bought
# imperial and metric tube both, and the sizes say which is which.
#
#   30 x 2        both hoops. RULE (FSG 2019 T3.3): 25 x 2.5 or 25.4 x 2.4,
#                 or an alternative with a wall of at least 2.0 and the same
#                 bending stiffness — 30 x 2 has 1.5x the second moment.
#   28.57 x 1.47  1-1/8" x 0.058": side impact, hoop bracing.
#   25.4 x 1.24   1" x 0.049": the bulkhead support and the braces' supports.
#   25 x 1        the floor, the engine bay, the harness bar's struts.
#   15.88 x 1.24  5/8" x 0.049": the lightest bracing.
MATERIAL = "steel"


# The CAD's weldment profiles are the imperial tubes ROUNDED to 0.01 mm —
# its 1-1/8" tube has a 14.285 radius — and nobody sells a 28.57 x 1.47. So
# each rounded profile is mapped to the tube it stands for, at its exact
# size, and only a profile listed here is mapped: anything else is ordered as
# the CAD drew it, and check_tube says so if nobody stocks it.
NOMINAL = {
    (28.57, 1.47): (28.575, 1.473),    # 1-1/8" x 0.058"
    (25.4, 1.24): (25.4, 1.245),       # 1" x 0.049"
    (15.88, 1.24): (15.875, 1.245),    # 5/8" x 0.049"
}


def stocked(od, wall):
    return NOMINAL.get((round(od, 2), round(wall, 2)), (od, wall))


def size_name(od, wall):
    od, wall = stocked(od, wall)
    return f"{od:g}x{wall:g}"


OD, WALL = {}, {}
for _m in FRAME["members"] + FRAME["bent"]:
    _s = size_name(_m["od"], _m["wall"])
    OD[_s], WALL[_s] = stocked(_m["od"], _m["wall"])


# --- a joint is not made on a bend -------------------------------------------
# The CAD lands the side-impact rail and two engine-bay diagonals a side ON the
# main hoop's knee: 1.4 mm from the corner of a 24 degree bend whose arc runs
# 21 mm either side of it; and the upper side members 40 mm into the front
# hoop's top bends, whose arcs run 74 mm. A bend is a torus, thinned by the
# bending, with no straight for a cope to sit against, and the app refuses a
# cope there (docs/tube/README.md). A fabricator slides the node down the
# hoop's straight until every fishmouth and its fillet sit clear of the arc,
# and so does this: along the leg the members already run toward, by the arc's
# tangent length, plus the longest fishmouth's footprint along the leg (r / sin
# of the angle it meets at), plus a fillet as wide as the hoop's wall. MOVED
# says which nodes moved and how far, and the run prints it.
def _clear_of_bends(nodes):
    import math

    def sub(a, b):
        return [a[k] - b[k] for k in range(3)]

    def unit(a):
        n = math.sqrt(sum(c * c for c in a))
        return [c / n for c in a]

    def dot(a, b):
        return sum(a[k] * b[k] for k in range(3))

    moved = []
    for hoop in FRAME["bent"]:
        pts = [nodes[hoop["feet"][0]]] + [tuple(c) for c in hoop["corners"]] \
            + [nodes[hoop["feet"][1]]]
        for i in range(1, len(pts) - 1):
            corner = pts[i]
            legs = [unit(sub(pts[i - 1], corner)),
                    unit(sub(pts[i + 1], corner))]
            turn = math.pi - math.acos(max(-1.0, min(1.0, dot(*legs))))
            tangent = hoop["radius"] * math.tan(turn / 2.0)
            for n in hoop["through"]:
                p = nodes[n]
                ending = [m for m in FRAME["members"] if n in m["nodes"]]
                if not ending:
                    continue
                radius = max(stocked(m["od"], m["wall"])[0] / 2.0
                             for m in ending)
                if math.dist(p, corner) >= tangent + radius:
                    continue
                dirs = [unit(sub(nodes[m["nodes"][1] if m["nodes"][0] == n
                                       else m["nodes"][0]], p))
                        for m in ending]
                pull = [sum(d[k] for d in dirs) for k in range(3)]
                leg = max(legs, key=lambda u: dot(u, pull))
                footprint = max(
                    stocked(m["od"], m["wall"])[0] / 2.0
                    / max(1e-3, math.sqrt(1.0 - dot(d, leg) ** 2))
                    for m, d in zip(ending, dirs))
                slide = tangent + footprint + hoop["wall"]
                # Nothing else on that leg may be passed: a node slid into
                # its neighbour is a different frame, not a fabricator's fix.
                for q in hoop["through"] + hoop["feet"]:
                    v = sub(nodes[q], corner)
                    if q != n and 0.0 < dot(v, leg) <= slide + radius and \
                            math.dist(nodes[q], corner) <= slide + radius:
                        raise SystemExit(
                            f"node {n} on a bend of the hoop cannot slide "
                            f"{slide:.0f} mm clear: node {q} is in the way")
                new = tuple(corner[k] + leg[k] * slide for k in range(3))
                nodes[n] = new
                moved.append((n, math.dist(p, new), slide - tangent, tangent))
    return moved


MOVED = _clear_of_bends(NODES)

# The bender's dies. A shop owns a handful of formers, not a continuum; the
# CAD bends both hoops round 100 mm, so the shop that built it had one.
FORMERS = [76.2, 100.0, 101.6, 152.4]
BRACE_FORMER = 100.0    # nothing straight is bent; a Frame member needs one
STOCK_LENGTH = 6000.0   # a six-metre stick, which is how tube is sold
KERF = 2.0              # the saw

# --- the stations, derived ---------------------------------------------------
# Every one of these is READ off the node table, so the jig plates and the
# panels follow the frame. Named the way the old generator named them because
# the jig and the panels ask for them by these names.
_xs = sorted(p[0] for p in NODES)
X_BULKHEAD = _xs[0]
X_REAR = _xs[-1]
Z_FLOOR = min(p[2] for p in NODES)

_hoops = sorted(FRAME["bent"], key=lambda b: max(c[2] for c in b["corners"]))
FRONT_HOOP, MAIN_HOOP = _hoops[0], _hoops[-1]


def _feet(b):
    return [NODES[i] for i in b["feet"]]


X_FRONT_HOOP = sum(p[0] for p in _feet(FRONT_HOOP)) / 2.0
X_MAIN_HOOP = sum(p[0] for p in _feet(MAIN_HOOP)) / 2.0
W_MAIN_HOOP = max(abs(p[1]) for p in _feet(MAIN_HOOP))
W_BULKHEAD = max(abs(p[1]) for p in NODES if abs(p[0] - X_BULKHEAD) < 5.0)
# The main hoop's knee: where its feet stop splaying out and it leans in. The
# firewall is closed up to it.
Z_SIDE_UPPER = min(c[2] for c in MAIN_HOOP["corners"])

# The bulkhead support: the station between the bulkhead and the front hoop
# where most nodes sit — the ring the nose braces land on.
_mid = [round(p[0] / 10.0) * 10.0 for p in NODES
        if X_BULKHEAD + 50.0 < p[0] < X_FRONT_HOOP - 50.0]
X_BULKHEAD_SUPPORT = max(set(_mid), key=_mid.count)

# The finish: the frame is powder-coated a deep blue, as the car in the
# photographs was. LINEAR colour, as set_appearance takes it — this is
# about sRGB (0.10, 0.18, 0.45).
FINISH = (0.010, 0.027, 0.17)

# --- the panels -----------------------------------------------------------
PANEL_MATERIAL = "steel"
PANEL_GAUGE = "1.5mm"

# --- the tabs -------------------------------------------------------------
# READ off the photographs: every suspension pickup is a PAIR of plates in
# double shear, straddling the rod end.
TAB_GAUGE = "4mm"
TAB_GAP = 16.0                  # ASSUMED: the rod end that goes between them
TAB_BOLT = 8.0                  # ASSUMED: M8 through the pair

# --- what it is welded with ----------------------------------------------
# A cope is cut a little open: a root gap is what lets the weld reach the
# bottom of the joint. It also keeps the cut off a TANGENCY — two tubes of
# one diameter whose axes meet exactly touch along a curve rather than
# crossing it, and a boolean asked to cut one with the other hands back the
# argument unchanged.
WELD_GAP = 0.25
