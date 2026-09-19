"""The numbers a Formula Student chassis is built from.

Every one of them is a decision, and the ones a rulebook makes are marked as
such. The frame is a steel space frame in the shape the 2019 car had: front
bulkhead, front hoop, main hoop, the side impact structure between them, and
a triangulated rear bay carrying the engine.

Axes are the app's: X runs REARWARD from the front bulkhead, Y is to the
LEFT, Z is up from the ground plane the frame sits on in its jig.
"""

# --- the stock ------------------------------------------------------------
# A rulebook sets these two: the main hoop and the front hoop are 25.4 x 2.4
# steel, and the bracing and the side impact structure may drop to 1.65.
MATERIAL = "steel"
HOOP = "25.4x2.4"       # main hoop, front hoop
STRUCT = "25.4x1.65"    # side impact, bracing, bulkhead support
SMALL = "19.05x1.65"    # tertiary members: floor, engine bay, tabs

OD = {HOOP: 25.4, STRUCT: 25.4, SMALL: 19.05}
WALL = {HOOP: 2.4, STRUCT: 1.65, SMALL: 1.65}

# The bender's dies. A shop owns a handful of formers, not a continuum, and
# asking for a radius nobody owns is the refusal check_tube exists to make.
FORMERS = [76.2, 101.6, 152.4]
STOCK_LENGTH = 6000.0   # a six-metre stick, which is how tube is sold
KERF = 2.0              # the saw

# --- the envelope ---------------------------------------------------------
WHEELBASE = 1550.0
FRONT_OVERHANG = 700.0          # bulkhead ahead of the front axle
REAR_OVERHANG = 500.0

# Longitudinal stations, from the front bulkhead at x = 0.
X_BULKHEAD = 0.0
X_BULKHEAD_SUPPORT = 380.0      # where the bulkhead support ties back
X_FRONT_HOOP = 950.0            # over the driver's knees
X_MAIN_HOOP = 1720.0            # behind the driver's shoulders
X_MH_BRACE_FOOT = 2320.0        # where the main hoop bracing lands
X_REAR = 2620.0                 # the back of the engine bay

# Half-widths. A frame is narrow at the nose and wide at the hips.
W_BULKHEAD = 150.0
W_FRONT_HOOP = 230.0
W_MAIN_HOOP = 260.0
W_REAR = 230.0

# Heights above the jig plate.
Z_FLOOR = 60.0                  # the lower rails
Z_BULKHEAD_TOP = 330.0
Z_FRONT_HOOP_TOP = 620.0
Z_MAIN_HOOP_TOP = 1180.0        # a rulebook: above the tall driver's helmet
Z_SIDE_UPPER = 520.0            # the upper side impact member
Z_SIDE_MID = 290.0              # the middle one
Z_REAR_TOP = 640.0

# The hoops are bent from ONE length of tube, on a former the shop owns.
HOOP_FORMER = 152.4             # the big radius at a hoop's top corners
BRACE_FORMER = 101.6

# Where a brace may land on a hoop: clear of the bend, because a hoop turns
# SHORT of its corner and there is no metal at the corner to weld to. With a
# 152 mm former on a right angle the tube leaves the straight 152 mm below
# the corner, so an attachment has to be below that and not at the corner it
# was drawn from. Derived, so a change of former moves it.
BRACE_CLEARANCE = 40.0
Z_MH_BRACE = Z_MAIN_HOOP_TOP - HOOP_FORMER - BRACE_CLEARANCE
Z_FH_BRACE = Z_FRONT_HOOP_TOP - HOOP_FORMER - BRACE_CLEARANCE

# --- the panels -----------------------------------------------------------
PANEL_MATERIAL = "steel"
PANEL_GAUGE = "1.5mm"           # the floor and the firewall

# --- what it is welded with ----------------------------------------------
# A cope is cut a little open: a root gap is what lets the weld reach the
# bottom of the joint. It is also what keeps the cut off a TANGENCY — two
# tubes of one diameter whose axes meet exactly have surfaces that touch
# along a curve rather than crossing it, and a boolean asked to cut one with
# the other hands back the argument unchanged. Every equal-diameter joint in
# this frame is drawn to its node, so every one of them would be that case.
WELD_GAP = 0.25
