"""The numbers the LFS19 chassis is built from.

This is a RECREATION of the car Lund Formula Student built in 2019, not a
generic Formula Student frame. The shape comes from a photograph of the car
on its weld jig; the sizes come from the rulebook; the lengths are the part
nobody has yet, because the team's CAD is SolidWorks and nothing in this
toolchain can read it (see docs/chassis-lfs-plan.md).

So every number below carries where it came from:

    # READ    — measured off the photograph, and against what
    # RULE    — fixed by the FS rules, with the rule's own number
    # ASSUMED — nobody has it yet. This is the list to walk the day
                19031101_Frame_19_SES.SLDPRT is exported as STEP.

Axes are the app's: X runs REARWARD from the front bulkhead, Y is to the
LEFT, Z is up from the table the jig sits on.
"""

# --- the stock ------------------------------------------------------------
# RULE: the main hoop and the front hoop are 25.4 x 2.4 steel; the bracing,
# the side impact structure and the bulkhead support may drop to 1.65.
MATERIAL = "steel"
HOOP = "25.4x2.4"       # main hoop, front hoop
STRUCT = "25.4x1.65"    # side impact, bracing, bulkhead support
SMALL = "19.05x1.65"    # tertiary: floor, engine bay, tab backing

OD = {HOOP: 25.4, STRUCT: 25.4, SMALL: 19.05}
WALL = {HOOP: 2.4, STRUCT: 1.65, SMALL: 1.65}

# The bender's dies. A shop owns a handful of formers, not a continuum.
FORMERS = [76.2, 101.6, 152.4]
STOCK_LENGTH = 6000.0   # a six-metre stick, which is how tube is sold
KERF = 2.0              # the saw

# --- the envelope ---------------------------------------------------------
# ASSUMED, all four. A Formula Student car of this class sits here, and the
# photograph agrees with the PROPORTIONS, but a photograph taken down the
# length of a car cannot give a length.
WHEELBASE = 1550.0
FRONT_OVERHANG = 700.0
REAR_OVERHANG = 500.0

# Longitudinal stations, from the front bulkhead at x = 0.
X_BULKHEAD = 0.0                # ASSUMED (datum)
X_BULKHEAD_SUPPORT = 350.0      # READ: the ring the nose braces land on
X_TAPER_FRONT = 700.0           # where the rails stop spreading
X_FRONT_HOOP = 1000.0           # READ: over the driver's knees
X_MAIN_HOOP = 1750.0            # READ: behind the driver's shoulders
X_ENGINE_FRONT = 2050.0         # READ: the first rear-truss station
X_REAR = 2400.0                 # READ: the rear node cluster


def _through(x, x0, y0, x1, y1):
    """y on the line through two stations, extended past both."""
    return y0 + (y1 - y0) * (x - x0) / (x1 - x0)


# Half-widths. READ off the photograph as RATIOS between stations; the
# absolute values are ASSUMED with the envelope.
#
# Only THREE of them are free. The lower rails run straight from the front
# taper all the way to the engine bay, so both hoops' feet sit ON that
# straight — which is the whole reason nothing in this frame is welded to a
# bend. Derive them, and moving a station moves its hoop with it.
W_BULKHEAD = 170.0              # READ: the bulkhead is narrow
W_TAPER_FRONT = 230.0           # READ: where the rails stop spreading
W_ENGINE = 250.0                # READ: the hips, at the engine bay's front
W_REAR = 175.0                  # READ: the rear truss CONVERGES

W_FRONT_HOOP = _through(X_FRONT_HOOP, X_TAPER_FRONT, W_TAPER_FRONT,
                        X_ENGINE_FRONT, W_ENGINE)
W_MAIN_HOOP = _through(X_MAIN_HOOP, X_TAPER_FRONT, W_TAPER_FRONT,
                       X_ENGINE_FRONT, W_ENGINE)

# Heights above the table.
Z_FLOOR = 60.0                  # the lower rails, on the jig plates
Z_BULKHEAD_TOP = 360.0          # READ: the bulkhead is very nearly square
Z_FRONT_HOOP_TOP = 780.0        # READ against the main hoop; ASSUMED value
Z_MAIN_HOOP_TOP = 1180.0        # RULE: above the tall driver's helmet
Z_SIDE_UPPER = 540.0            # READ: the upper side impact member
Z_SIDE_MID = 300.0              # READ: the middle one
Z_REAR_TOP = 700.0              # READ: the rear upper node cluster
Z_HARNESS = 620.0               # ASSUMED: the shoulder belts' own member

# --- the hoops --------------------------------------------------------
#
# THE THING THE PHOTOGRAPH IS MOST EMPHATIC ABOUT. Neither hoop is a
# rectangle with its corners bent. Each one rises vertically off its feet,
# then LEANS INWARD to a narrow crown: the main hoop is markedly narrower
# across the top than across its feet, which is what makes it look like an
# arch rather than a roll bar. A hoop is still ONE length of tube — it is
# the node list that changes, not the code.
#
# Four bends per hoop: a knee on each side where the leg stops being
# vertical, and a crown corner on each side where the lean becomes the top.
# The knee is LOW: the photograph's main hoop starts leaning not far above
# the driver's shoulders, and a high knee also puts the bend exactly where
# the rear rails want to land on the hoop.
Z_HOOP_KNEE = 0.45              # READ: fraction of the hoop's height
W_MAIN_HOOP_CROWN = 130.0       # READ: half width across the top
W_FRONT_HOOP_CROWN = 150.0      # READ: the front hoop leans less
# 101.6, not the biggest former the shop owns: a crown 260 mm across cannot
# absorb two 152.4 mm bends. At 152.4 the top straight came out 17 mm long
# and the bender's clamp die needs 50.8 mm to hold the tube at all.
HOOP_FORMER = 101.6             # the radius at knee and crown
BRACE_FORMER = 101.6

# Where a brace may land on a hoop: clear of the bend, because a hoop turns
# SHORT of its corner and there is no metal at the corner to weld to. A weld
# is a tube's radius wide, so a joint pressed up to the tangent puts half its
# bead on the bend — clear by a radius, plus a little for the welder.
BRACE_CLEARANCE = 30.0


def hoop(x, w_foot, w_crown, z_top):
    """The node list of one hoop, foot to foot, as the tube is bent.

    Vertical off the feet to the knee, leaning in to the crown, across the
    top, and back down the mirror of itself.
    """
    z_knee = Z_FLOOR + (z_top - Z_FLOOR) * Z_HOOP_KNEE
    return [
        (x, w_foot, Z_FLOOR),
        (x, w_foot, z_knee),
        (x, w_crown, z_top),
        (x, -w_crown, z_top),
        (x, -w_foot, z_knee),
        (x, -w_foot, Z_FLOOR),
    ]


MAIN_HOOP = hoop(X_MAIN_HOOP, W_MAIN_HOOP, W_MAIN_HOOP_CROWN, Z_MAIN_HOOP_TOP)
FRONT_HOOP = hoop(X_FRONT_HOOP, W_FRONT_HOOP, W_FRONT_HOOP_CROWN,
                  Z_FRONT_HOOP_TOP)

# A brace lands on the LEANING stretch, below the crown bend and above the
# knee bend. Derived, so changing a former moves every brace with it.
Z_MH_BRACE = Z_MAIN_HOOP_TOP - HOOP_FORMER - BRACE_CLEARANCE
Z_FH_BRACE = Z_FRONT_HOOP_TOP - HOOP_FORMER - BRACE_CLEARANCE


def lean(nodes, z):
    """Half width of a hoop at height z.

    Below the knee a hoop's leg is vertical; above it the leg leans in to
    the crown. Anything that lands on a hoop has to ask, because a hoop that
    leans is the whole difference between this frame and a roll bar.
    """
    (_, w_foot, z_knee) = nodes[1]
    if z <= z_knee:
        return w_foot
    (_, w_crown, z_crown) = nodes[2]
    t = (z - z_knee) / (z_crown - z_knee)
    return w_foot + (w_crown - w_foot) * t


W_MH_BRACE = lean(MAIN_HOOP, Z_MH_BRACE)
W_FH_BRACE = lean(FRONT_HOOP, Z_FH_BRACE)


# WHERE A BEND MAY BE: nowhere a joint lands. A station is where five or six
# members converge, and a bend is the one stretch of a tube with no straight
# to fit them against. So the rails taper BETWEEN stations and run straight
# through every station where something is welded.
X_SIDE_KNEE = 800.0             # READ: where the upper side member levels
W_SIDE_KNEE = _through(X_SIDE_KNEE, X_FRONT_HOOP, W_FRONT_HOOP,
                       X_MAIN_HOOP, W_MAIN_HOOP)

# Where the front upper rail meets the front hoop. The hoop is LEANING by
# then, so the rail's far end is asked of the hoop, not assumed.
W_FH_UPPER = lean(FRONT_HOOP, Z_SIDE_UPPER)

# The bulkhead support ring sits ON the rails rather than kinking them —
# both rails. The lower rail and the upper rail are spreading at different
# rates, so the ring's uprights LEAN: its top is 19 mm narrower than its
# foot. Derived, because a ring that misses a rail by 20 mm is a cope that
# cuts nothing and a member welded to air.
W_SUPPORT = _through(X_BULKHEAD_SUPPORT, X_BULKHEAD, W_BULKHEAD,
                     X_TAPER_FRONT, W_TAPER_FRONT)
W_SUPPORT_TOP = _through(X_BULKHEAD_SUPPORT, X_BULKHEAD, W_BULKHEAD,
                         X_FRONT_HOOP, W_FH_UPPER)
Z_SUPPORT_TOP = _through(X_BULKHEAD_SUPPORT, X_BULKHEAD, Z_BULKHEAD_TOP,
                         X_FRONT_HOOP, Z_SIDE_UPPER)

# --- the panels -----------------------------------------------------------
# READ off the CAD's FILE NAMES, which is all the CAD would give up: the
# floor is `FloorPanelTrimmedWithHoles` plus a separate `RearFloor`, the
# firewall is four pieces (`Upperfirewall`, `Firewall_seat`,
# `FirewallShoulder`, `Firewall_frame`), and the side panels are LOFTED and
# bent, cut as left/right front and rear `PreCut` blanks.
PANEL_MATERIAL = "steel"
PANEL_GAUGE = "1.5mm"

# --- the tabs -------------------------------------------------------------
# READ: every suspension pickup in the photograph is a PAIR of plates in
# double shear, straddling the rod end. The CAD names agree — the front
# lower A-arm has a `...MountFront` and a `...MountRear`, not one mount.
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
