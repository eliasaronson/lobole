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
