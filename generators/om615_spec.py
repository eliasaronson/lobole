#!/usr/bin/env python3
"""The four-cylinder diesel, on paper: every number the three build modules
share, and nothing else.

An OM615-shaped engine — 87 x 83.6 mm, five main bearings, a single overhead
camshaft on a duplex chain working the valves through rocker arms, and a
pre-chamber per cylinder, which is the thing that makes it that engine
rather than a petrol four with a diesel label on it.

**The frame of reference**, once, because every module below reads it:

    +Z   up, and the bore axis. The crank centreline is z = 0 and the deck
         is z = DECK, so a piston's height IS its z.
    +Y   the crank axis. FRONT (the timing chain) is -Y, REAR (the flywheel)
         is +Y. Cylinder 1 is the front one.
    +X   across the engine. The INTAKE side is -X, the EXHAUST side is +X.

Everything with a unit is derived from the four numbers a person would
actually quote for an engine (bore, stroke, rod, bore pitch) wherever the
derivation is real, and written down as a measurement where it is not.
"""
import math

# ---- the four numbers ------------------------------------------------------
BORE = 87.0
STROKE = 83.6
ROD_L = 145.0             # big-end centre to small-end centre
BORE_PITCH = 97.0

CRANK_R = STROKE / 2.0    # 41.8
CYL_Y = [(i - 1.5) * BORE_PITCH for i in range(4)]   # -145.5 .. +145.5
N_CYL = len(CYL_Y)
DISPLACEMENT = N_CYL * math.pi * (BORE / 2.0) ** 2 * STROKE / 1000.0  # cm3

# Firing order 1-3-4-2: cylinders 1 and 4 share a crank throw at 0 degrees,
# 2 and 3 share the throw at 180. FIRING_TDC[k] is the crank angle at which
# cylinder k fires, measured from cylinder 1's firing TDC.
FIRING_ORDER = (1, 3, 4, 2)
FIRING_TDC = [360.0 * FIRING_ORDER.index(k + 1) / 2.0 for k in range(N_CYL)]
THROW_DEG = [0.0, 180.0, 180.0, 0.0]   # which throw each cylinder is on

# ---- the reciprocating stack ----------------------------------------------
PIN_H = 47.0              # gudgeon-pin centre to crown: a diesel's is tall
SQUISH = 3.2              # crown to deck at TDC
DECK = CRANK_R + ROD_L + PIN_H + SQUISH            # 237.0
SKIRT = 30.0              # pin centre to the bottom of the skirt
# Running fits are drawn at 0.5 mm RADIAL, everywhere, and that is a
# modelling number rather than an engineering one: a piston runs at 0.1 mm
# and a bearing shell at 0.05, but the collision mesh deflects ~0.4 mm on
# parts this size, so anything tighter is reported as metal inside metal at
# every sample of the motion study. FIT is the one place that decision is
# written down.
FIT = 1.0                 # diametral
PISTON_CLEARANCE = FIT
GUDGEON_D = 30.0
GUDGEON_L = 72.0
BUSH_OD = 36.0            # small-end bush outside diameter
SMALL_END_D = 46.0
RING_T = [2.5, 2.5, 4.0]  # top compression, second, oil control
RING_GROOVE_DEPTH = 4.5
RING_Z = [-9.0, -16.0, -24.0]      # below the crown
BOWL_D = 46.0             # the shallow dish a pre-chamber engine's piston has
BOWL_DEPTH = 5.0

# ---- the crankshaft --------------------------------------------------------
MAIN_D = 60.0
MAIN_W = 30.0             # journal length
MAIN_Y = [-194.0, -97.0, 0.0, 97.0, 194.0]
MAIN_SHELL_T = 2.0
MAIN_HOUSING_D = MAIN_D + 2 * MAIN_SHELL_T         # 64.0
SHELL_W = 18.0            # narrower than the web, because the CENTRE web
                          # also has a thrust recess 3 mm deep in each face
ROD_JD = 52.0             # rod journal
BIG_END_W = 28.0          # the rod's width at the big end
ROD_JW = BIG_END_W + 4.0  # journal a shade wider than the rod on it
ROD_SHELL_T = 1.8
ROD_HOUSING_D = ROD_JD + 2 * ROD_SHELL_T           # 55.6
BIG_END_D = 84.0
ROD_BOLT_X = 35.0         # rod bolt centres: 3 mm of metal inboard to the
ROD_BOLT_D = 8.5          # housing bore and 2.75 outboard to the rim
WEB_R = 70.0              # counterweight radius — see the note in the block
WEB_T = 18.0
NOSE_D = 42.0             # crank nose, front of the front main
NOSE_Y = -293.0           # where the nose ends, ahead of the damper
FLANGE_D = 110.0          # flywheel flange
FLANGE_T = 12.0
FLANGE_Y = 240.0          # far enough aft of the rear main for the seal
# The bolt circle is bounded on BOTH sides: outside the spigot the flywheel
# centres on, and inside the crank's rear seal land — at 74 the bolts ran out
# through the land and into the seal that runs on it.
FLYWHEEL_BOLT_PCD = 58.0
FLYWHEEL_SPIGOT_D = 42.0
FLYWHEEL_BOLTS = 6

# ---- the block -------------------------------------------------------------
DECK_X = 70.0             # half-width at the deck
CASE_X = 100.0            # half-width of the crankcase skirt. NOT styling:
                          # the big end sweeps CRANK_R + BIG_END_D/2 = 83.8
                          # from the crank axis, and the void inside these
                          # walls is 14 mm narrower again
BLOCK_Y0 = -205.0         # front face = the front main web's own face
BLOCK_Y1 = 215.0          # rear face (the bellhousing flange)
# Sump bolt centres. Squeezed from both sides: outboard of the pan flange's
# inner edge (a hole that breaks out of the face it seats on has no rim a
# fastener can be fitted to) and inboard of the block's own chamfered rail.
PAN_BOLT_X = 88.0
PAN_BOX_INSET = 20.0      # how much narrower the pan's box is than the block
CASE_CHAMFER = 6.0        # the crankcase's lower outer edges
PAN_RAIL = -92.0          # the sump rail. Deep enough that the rod CAP,
                          # whose corner reaches 83.8 below the axis at BDC,
                          # is not scraping it
CRANKCASE_DECK = 95.0     # the roof of the crankcase's open bays
BLOCK_STEP = 120.0        # ...and where the block narrows from the skirt's
                          # width to the deck's. It is ABOVE that roof on
                          # purpose: the bays are 172 wide (the rod's big end
                          # sweeps 83.8 from the crank axis) and the deck is
                          # 140, so a block that stepped in at the roof left
                          # each bay open to the sky through a 16 mm slot
                          # along the ledge
BULKHEAD_T = 26.0         # main bearing web thickness. Bounded ABOVE by the
                          # main journal, which is 30 wide: the crank's own
                          # webs start where the journal ends, so a bearing
                          # web thicker than that is inside them.
CAP_BOLT_X = 46.0         # main cap bolts, either side of the journal
# A tapped hole is drawn at the bolt's NOMINAL diameter — a thread's major
# diameter is the bolt's own, and drawn at the tapping drill every bolt in
# the engine reads as steel inside steel for its whole engagement. Plus a
# TENTH, and that tenth is not cosmetic: at exactly nominal the bolt's shank
# and the hole's wall are the same cylinder, and OCC's boolean walks off the
# end of its own pave list on two coincident cylinders. It survives while
# the part sits on the axes and SEGFAULTS the moment the crankshaft turns
# 15 degrees and the coincidence becomes a near-coincidence.
TAP_FIT = 0.1
CAP_BOLT_D = 12.0
PAN_BOLT_D = 8.0
FLYWHEEL_BOLT_D = 10.0
CAP_DEPTH = 42.0          # the cap's own depth below the parting line
CAP_X = 60.0              # half-width of the cap and of its register
JACKET_T = 6.0            # cylinder wall between bore and water jacket
HEAD_BOLT_D = 12.0
# Ten head bolts: two either side of each bore's centreline at the bore
# pitch, five a side. They are what clamps the gasket, so they live in ONE
# list that the block drills blind and the head drills through.
HEAD_BOLT_X = 58.0
# 20, not 24: the outermost head bolts are close to the head's own end face,
# and a boss that runs past it comes back as a rim SPLIT into two arcs —
# which is half a circle, and a fastener cannot be fitted to one.
HEAD_BOLT_BOSS_D = 20.0
# Anything that stands INSIDE the rocker chamber is bounded by where that
# chamber ends — and the head is not symmetric about zero, so it is the
# SHORTER end that says where. A boss straddling the end wall splits its own
# rim in two the same way: the outer bolts sat on the main webs, and the two
# at the front had no whole rim at all.
CHAMBER_Y = min(BLOCK_Y1, -BLOCK_Y0) - 8.0
HEAD_BOLT_OUTER_Y = CHAMBER_Y - HEAD_BOLT_BOSS_D / 2.0 - 2.0
HEAD_BOLT = [(sx * HEAD_BOLT_X,
              max(-HEAD_BOLT_OUTER_Y, min(HEAD_BOLT_OUTER_Y, y)))
             for y in MAIN_Y for sx in (-1, 1)]
JACKET_X = 52.0           # the water jacket's outer wall, inboard of the
                          # head bolts and outboard of the liners
JACKET_Z0 = 112.0
JACKET_Z1 = DECK - 8.0
LINER_OD = BORE + 2 * 6.0            # 99: the jackets are siamesed at 97
GASKET_T = 1.5
SUMP_GASKET_T = 2.0
# Coolant crosses the joint at ten places, on the SAME list the block drills,
# the gasket is punched and the head drills — outboard of the bores, inboard
# of the jacket's own wall, and clear of the head bolts at 58.
COOLANT_HOLES = [(sx * 40.0, y) for y in (-185.0, -97.0, 0.0, 97.0, 185.0)
                 for sx in (-1, 1)]
COOLANT_D = 14.0
# The oil gallery runs the length of the block in the SKIRT wall, because
# the crankcase bays inboard of it are 172 mm wide and a drilling at 78
# would be in fresh air. Cross-drillings feed each main; one riser feeds the
# head, up the deck beside cylinder one.
GALLERY_X = -93.0
GALLERY_Z = 60.0
GALLERY_D = 10.0
OIL_RISER_X = -62.0
OIL_RISER_D = 8.0
COVER_GASKET_T = 2.0
CAM_GASKET_T = 3.0
COVER_LIP = 5.0           # how far the cam cover overhangs the head
HEAD_CHAMFER = 10.0       # the head's lower outer edges

# ---- the head --------------------------------------------------------------
FIRE_FACE = DECK + GASKET_T          # 238.5, the head's underside
HEAD_X = 86.0             # WIDER than the block's deck: the rocker shafts
                          # sit outboard of both valves and have to be in it
HEAD_H = 155.0            # up to the camshaft's own centreline, which is
                          # where an overhead cam's saddles are split.
                          # It is the VALVE's length that sets this: the
                          # rocker's own stack (lash, pad, arm) eats 60 mm
                          # between the cam and the valve's tip, and at 134
                          # the spring retainer came down past the chamber
                          # floor and onto the stem seal.
HEAD_TOP = FIRE_FACE + HEAD_H        # 372.5
# The two valves are staggered ACROSS the bore, not side by side on its
# centreline. Each is under its own cam lobe, and a lobe presses a rocker
# that lies in ONE plane of constant y — so the valve, its lobe and its
# rocker all share a y, and the two of them have to be different.
# The stagger is as small as it can be while the two lobes still sit side by
# side on the camshaft: the seat rings have to stay INSIDE the bore, and at
# (20, 12) the intake ring's rim stood 2 mm out over the gasket.
VALVE_X = 18.0
VALVE_DY = 10.0           # intake at (-X, -dy), exhaust at (+X, +dy)
VALVE = {"intake": (-VALVE_X, -VALVE_DY), "exhaust": (VALVE_X, VALVE_DY)}
STEM_D = 9.0
VALVE_HEAD_D = {"intake": 36.0, "exhaust": 32.0}
SEAT_ANGLE = 45.0
# +6, not +8: staggered across the bore the two rings are 41.2 mm apart and
# at +8 their rims are 42 — they overlapped each other by eight tenths.
SEAT_OD = {k: d + 6.0 for k, d in VALVE_HEAD_D.items()}
SEAT_T = 6.0
GUIDE_OD = 15.0
GUIDE_LEN = 42.0          # ends BELOW where the retainer comes down to
GUIDE_Z = FIRE_FACE + 16.0           # where the guide's bore starts
VALVE_LIFT = 8.0
PORT_D = {"intake": 30.0, "exhaust": 28.0}
PORT_Z = FIRE_FACE + 34.0            # where a port leaves the side face
# Two studs per port, ABOVE and BELOW it. Fore and aft was the obvious place
# and it is the injector's: an injector comes out of this same face 38 mm
# behind its port, and a flange wide enough to reach studs at 24 is a flange
# through the injector. Above and below, the upper stud stops short of the
# head's side coolant channel (x 48..70) and the lower one clears the seat
# counterbore, because both are only 15 mm deep.
MANIFOLD_BOLT_DZ = 26.0
MANIFOLD_BOLT_D = 9.0
MANIFOLD_PAD = 20.0                  # how thick a manifold's flange is
MANIFOLD_PAD_HY = 24.0               # ...and how far it reaches in y and z
MANIFOLD_PAD_HZ = 34.0
MANIFOLD_GASKET_T = 2.0
MANIFOLD_STUD_DEPTH = 15.0           # shallow ON PURPOSE: 26 deep and the
                                     # stud runs into the head bolt's own
                                     # boss, which is 12.5 mm away in y and
                                     # the full height of the head in z
MANIFOLD_CONVERGE = 0.75             # how far each runner draws in toward
                                     # the log — at 0.34 the intake runner's
                                     # bore swallowed its own outer stud
INT_CONVERGE = 0.90                  # the intake barely converges: it is a
                                     # plenum with four stubs, not a header
# Both manifolds are LOGS with curved branches — swept along fitted splines,
# not straight cylinders between two flanges. A runner leaves its port on the
# port's own axis and arrives at the log on the log's, and everything between
# is a curve; and the branches are built SOLID and bored afterwards, because
# two hollow tubes unioned leave each one's wall across the other's throat.
#
# (x, z, outer, bore) for each log, and the outer/bore of its branches.
INT_LOG = (-140.0, FIRE_FACE + 130.0, 78.0, 62.0)   # high, like the real one
INT_LOG_Y = (-172.0, 172.0)
INT_RUNNER = (42.0, 32.0)
EXH_LOG_Y = (-72.0, 162.0)
EXH_RUNNER = (44.0, 34.0)


def land_y(kind, cy):
    """Where a runner MEETS its log — drawn in toward the log's middle by
    MANIFOLD_CONVERGE, which is what makes the exhaust side a four-into-one
    instead of four stubs into a pipe."""
    span = INT_LOG_Y if kind == "intake" else EXH_LOG_Y
    mid = sum(span) / 2.0
    c = INT_CONVERGE if kind == "intake" else MANIFOLD_CONVERGE
    return mid + (cy + VALVE[kind][1] - mid) * c


# How far the longest runner has to swing sideways to reach the collector...
EXH_SWEEP = max(abs(land_y("exhaust", cy) - (cy + VALVE["exhaust"][1]))
                for cy in CYL_Y)
# ...and therefore how far OUT the collector stands. A tube swept round a
# bend tighter than its own radius crosses itself; OCC cuts with such a pipe
# without complaint and takes the rest of the manifold with it — 91% of this
# one, leaving a lump that no longer touched anything and so no longer
# clashed with anything either. So the standoff is DERIVED: about as much
# length as the runner swings sideways, and never less than the tube itself.
EXH_LOG = (HEAD_X + MANIFOLD_GASKET_T + MANIFOLD_PAD
           + max(1.4 * EXH_SWEEP, 1.5 * EXH_RUNNER[0]),
           FIRE_FACE + 14.0, 62.0, 50.0)
# The spring sits in a POCKET sunk below the rocker chamber's floor. On the
# floor itself there are only 20 mm between the seat and the retainer, which
# is a spring whose coils are closer together than its wire is thick.
SPRING_SEAT_Z = FIRE_FACE + 28.0
SPRING_POCKET_D = 38.0
SPRING_D = 30.0           # mean coil diameter
SPRING_WIRE = 4.0
RETAINER_D = 34.0
# How far below the stem's tip the collets grip. Not 16: the rocker's arm
# carries on past its valve pad toward the cam, and at full lift that part
# of it swings down past a retainer sitting at the very top of the stem.
RETAINER_DROP = 24.0
# The collet's outer CONE. The retainer's bore is the same taper — that
# wedge is the whole point of a split collet — so both are derived from one
# place: given a plain cylindrical bore the retainer sat over the entire
# cone, half a cubic centimetre of steel in the same place, sixteen times.
COLLET_H = 7.0
COLLET_R0 = STEM_D / 2.0 + 3.0     # at the collet's own bottom face
COLLET_R1 = STEM_D / 2.0 + 5.5     # ...and COLLET_H above it
COLLET_DROP = 0.5                  # how far the retainer seats BELOW it
COLLET_FIT = 0.2                   # drawn just clear, not wedged tight


def collet_r(z):
    """The collet cone's radius at height z above the RETAINER's own seat."""
    cz = z - COLLET_DROP
    return COLLET_R0 + (COLLET_R1 - COLLET_R0) * cz / COLLET_H
STEM_SEAL_D = 18.0
STEM_SEAL_H = 8.0
# The pre-chamber: what makes it this engine and not a direct-injection four.
# Offset toward the intake side and clear of both valve seats.
PRE_X = -18.0
PRE_DY = 28.0             # far enough off the intake port AND off the intake
                          # valve's spring pocket not to break into either
PRE_D = 32.0              # the chamber itself
PRE_THROAT_D = 8.0        # the hole it fires through
PRE_Z = (FIRE_FACE + 13.0, FIRE_FACE + 54.0)
# Injector and glow plug both come in from the INTAKE side on the chamber's
# own y, one above the other. Staggering them in y instead runs one of them
# either into the intake port (16 mm of radius, 10 mm off the bore's centre)
# or into a head bolt boss (13 mm of radius, 48.5 mm away) — there is no y
# left between the two, and z is free.
INJ_D = 12.0
GLOW_D = 10.0
INJ_FACE = (-HEAD_X, PRE_DY, FIRE_FACE + 40.0)
INJ_TARGET = (PRE_X - PRE_D / 2.0 - 2.0, PRE_DY, FIRE_FACE + 36.0)
GLOW_FACE = (-HEAD_X, PRE_DY, FIRE_FACE + 20.0)
GLOW_TARGET = (PRE_X - PRE_D / 2.0 - 2.0, PRE_DY, FIRE_FACE + 24.0)

# ---- the valve gear --------------------------------------------------------
CAM_Z = HEAD_TOP                     # 372.5: the saddles ARE the top face
CAM_SHAFT_D = 32.0
CAM_JOURNAL_D = 44.0
CAM_BEARING_W = 20.0
# Over the webs like the mains, but the outer pair pulled IN: a saddle on
# the outermost web puts its cap and its rocker post through the cam cover's
# own end wall, and through its gasket's rim. Same bound as the head bolts.
CAM_OUTER_Y = CHAMBER_Y - CAM_BEARING_W / 2.0 - 3.0
CAM_BEARING_Y = [-CAM_OUTER_Y, -97.0, 0.0, 97.0, CAM_OUTER_Y]
CAM_BASE_R = 20.0
CAM_LOBE_W = 16.0
# The rocker shafts, outboard of both valves — but INBOARD enough that a
# rocker's pivot boss stays inside the chamber wall. The boss is 16 mm in
# radius and the chamber ends at 78, so 72 put every one of the eight into
# the head's own side wall.
ROCKER_PIVOT_X = 60.0
ROCKER_SHAFT_D = 20.0
LASH = 5.0                # drawn clearance at the lobe. A real cold lash is
                          # 0.15 mm; the collision mesh deflects ~0.4 mm on
                          # parts this size, so 0.15 reads as contact at
                          # every sample and 3 reads as a valve gear.

# The rocker is a lever: the cam presses its far end directly under the cam
# axis, the valve pad is between that and the pivot, so the lobe's lift and
# the valve's are in the ratio of those two arms. Say the VALVE lift, which
# is what an engine is specified by, and derive the lobe from it.
ARM_CAM = ROCKER_PIVOT_X             # pivot to the pad under the cam
ARM_VALVE = ROCKER_PIVOT_X - VALVE_X # pivot to the valve pad
ROCKER_RATIO = ARM_VALVE / ARM_CAM   # 0.722
CAM_LIFT = VALVE_LIFT / ROCKER_RATIO # 11.08 at the lobe
# Where the rocker sits, and therefore how long a valve is.
PAD_Z = CAM_Z - CAM_BASE_R - LASH    # the cam pad's face, when shut
# The cam pad stands PROUD of the arm, and that is not styling. A rocker
# whose top edge runs straight to its contact point is very nearly tangent to
# the lobe all the way along — the lobe reaches 31 mm from its axis and the
# arm's top passes within 25 — so the flanks of the lobe cut into the arm
# beside the pad at every angle it lifts. Raise the pad and the arm's top
# drops away under it, which is what a real rocker's pad does.
PAD_PROUD = 18.0
ROCKER_Z = PAD_Z + 22.0              # the shaft, above both pads
# The arm's own depth. It is what finally cleared the last contact in the
# engine: the rocker's pad slides 8 mm sideways OFF the valve as the rocker
# turns, so at full lift it is the arm's UNDERSIDE, not the pad, that comes
# down over the stem — and a 12 mm arm caught the stem's outer edge by
# three tenths of a millimetre.
ARM_T = 8.0
ARM_RISE = 12.0                      # how far its top stands above the pivot
ARM_DROP = (ROCKER_Z + ARM_RISE) - (PAD_Z - PAD_PROUD)


def arm_top(x):
    """The rocker arm's top edge, in the rocker's own coordinates: x from the
    pivot toward the cam, z from the pivot."""
    return ARM_RISE - ARM_DROP * x / ARM_CAM


VALVE_PAD_Z = ROCKER_Z + arm_top(ARM_VALVE) - ARM_T - 4.0
STEM_TOP = VALVE_PAD_Z - 10.0        # running clearance at the valve pad.
                                     # Generous, because the pad TILTS with
                                     # the rocker and its far corner closes
                                     # on the stem faster than the valve
                                     # itself comes down.
VALVE_LEN = STEM_TOP - FIRE_FACE
# Where the rocker's pad sits when the valve is shut, and therefore how long
# a valve is: from its seat on the fire face to under its own rocker.
#
# The two pads do NOT have to be on a line through the pivot, and it matters
# that they need not: a point on a lever moves vertically by its own X offset
# from the pivot times the angle, whatever its height. So the arm can be a
# beam with a boss hanging under each end, which is what a rocker is, instead
# of a bar through its own contact points.
# High enough for the LOBES, which reach CAM_BASE_R + CAM_LIFT from the axis
# — not just for the shaft and its caps.
CAM_COVER_TOP = CAM_Z + 40.0

# Valve timing, in CAMSHAFT degrees (the cam turns once per 720 of crank).
# OM615 figures: inlet opens 12 BTDC and closes 32 ABDC, exhaust opens 32
# BBDC and closes 12 ATDC — 224 crank degrees each, so 112 of cam.
DURATION_CAM = 112.0
INLET_OPEN_CAM = (360.0 - 12.0) / 2.0        # 174
EXHAUST_OPEN_CAM = (180.0 - 32.0) / 2.0      # 74

# ---- the timing drive ------------------------------------------------------
CHAIN_PITCH = 9.525       # 3/8 inch, duplex
CRANK_TEETH = 22
CAM_TEETH = 44            # twice: the four-stroke's own ratio
OIL_CRANK_TEETH = 14      # the oil pump chain: the crank's sprocket...
OIL_PUMP_TEETH = 14       # ...and the pump's. 1:1, because the pump has to
                          # fit BETWEEN the crank's sprocket and the floor of
                          # the chain case, and a bigger driven sprocket puts
                          # its own teeth through that floor
DRIVE_Y = -232.5          # the plane the timing chain runs in
CHAIN_W = 17.0            # duplex, across the two strands
CHAIN_T = 6.0             # radial thickness of the drawn chain
CHAIN_CLEAR = 3.0         # ...and how far off the tooth tips it is drawn.
                          # Both feed COVER_TOP and the oil pump's headroom:
                          # the wrap round the crank sprocket is what the
                          # pump has to fit under.


def sprocket_pcd(teeth, pitch=CHAIN_PITCH):
    """A roller-chain sprocket's pitch diameter — p / sin(pi/z)."""
    return pitch / math.sin(math.pi / teeth)


CRANK_PCD = sprocket_pcd(CRANK_TEETH)        # 66.9
CAM_PCD = sprocket_pcd(CAM_TEETH)            # 133.5


def tooth_tip(radius, teeth):
    """How far a sprocket's teeth actually reach — shared with the chain,
    because a chain drawn to the PITCH radius has the teeth standing through
    it."""
    return radius + (2 * math.pi * radius / teeth * 0.62) / 2


def chain_outer(pcd, teeth, thickness=CHAIN_T):
    """How far the drawn chain reaches from a sprocket's axis: the tooth tip,
    the clearance it is drawn off the tips, and its own thickness. Anything
    that has to stay out of the drive measures itself against THIS."""
    return tooth_tip(pcd / 2.0, teeth) + CHAIN_CLEAR + thickness


OIL_PCD = sprocket_pcd(OIL_PUMP_TEETH)
OIL_CRANK_PCD = sprocket_pcd(OIL_CRANK_TEETH)
OIL_CHAIN_W = 9.0
OIL_CHAIN_T = 5.0         # the oil chain is a lighter one than the timing's

# The crank nose, laid out end to end so nothing on it overlaps anything
# else. Each is (start, end) in y, front main journal face first.
# The oil chain's plane sits AFT of the pump body's rear face: on the pump's
# own centreline it was half a millimetre into it. And the shoulder behind
# it is DERIVED from that plane, because the sprocket and the chain are both
# as wide as the chain — a shoulder placed by eye left both of them half a
# millimetre inside the crankshaft.
OIL_DRIVE_Y = -216.0      # the oil pump chain's plane
NOSE_SHOULDER = (OIL_DRIVE_Y + OIL_CHAIN_W / 2.0 + 1.0,
                 OIL_DRIVE_Y + OIL_CHAIN_W / 2.0 + 4.0)
OIL_SPROCKET = (OIL_DRIVE_Y - OIL_CHAIN_W / 2.0 - 3.5, NOSE_SHOULDER[0])
TIMING_SPROCKET = (DRIVE_Y - CHAIN_W / 2.0, DRIVE_Y + CHAIN_W / 2.0)
FRONT_SEAL_LAND = (-260.0, -241.0)
FRONT_SEAL_D = 50.0
FRONT_SEAL_Y = -255.0     # in the cover's front wall, ahead of the chain
DAMPER_SEAT = (-290.0, -260.0)
DAMPER_Y = -290.0         # where the damper's own rear face sits
COVER_Y0 = -260.0         # the timing cover's front face
COVER_WALL = 6.0
# The chain case has to clear the CHAIN, not the cam sprocket — the wrap
# stands a clearance and a chain thickness proud of the teeth, and a roof
# sized to the sprocket alone was 5 mm inside it. A real overhead-cam
# engine's front cover is taller than its head, for exactly this reason.
COVER_TOP = CAM_Z + chain_outer(CAM_PCD, CAM_TEETH) + COVER_WALL + 4.0
# Where the front cover bolts up: into the BLOCK below the deck and into the
# HEAD above it. Two lists, because the two faces have different things in
# the way — at x = 64 the upper pair went straight through the outermost head
# bolt's own boss, and a boss with a hole across its top face comes back as a
# rim SPLIT in two, which no fastener can be fitted to. The head's pair sits
# in its side wall instead, outboard of the rocker chamber at 78.
COVER_BOLTS = [(sx * x, z) for x, z in ((93.0, -80.0), (93.0, -20.0),
                                        (93.0, 40.0), (64.0, 110.0),
                                        (64.0, 180.0)) for sx in (-1, 1)]
COVER_BOLTS_HEAD = [(sx * 82.0, z) for z in (250.0, 320.0) for sx in (-1, 1)]
COVER_FRONT_T = 10.0      # thicker, because the front seal lives in it
# The chain case has to clear the CAM SPROCKET, which at 44 teeth of 3/8
# reaches 72 mm from the cam axis — above the head's own top face. A real
# OHC engine's front cover is taller than its head at the front for exactly
# this reason.
REAR_SEAL_D = 75.0
REAR_SEAL_Y = 224.0

# ---- oiling ----------------------------------------------------------------
SUMP_DEPTH = 105.0        # below the pan rail
SUMP_Z = PAN_RAIL - SUMP_DEPTH
SUMP_Y0 = -260.0          # the pan runs forward to the chain case's own
                          # front face, because the case's floor is open
SUMP_Y1 = 205.0
# The pump hangs off the block's front face inside the chain case. Its height
# is squeezed from both ends: the chain has to be longer than the two
# sprockets' tip radii together, and everything on it — body, gears, driven
# sprocket — has to clear the case's floor at the pan rail.
OIL_GEAR_D = 30.0
OIL_GEAR_W = 20.0
OIL_BODY_H = OIL_GEAR_D / 2.0 + 6.0     # half the pump body's height
# ...and its axis is DERIVED from the top of that body: what is directly
# above the pump is the timing chain's wrap round the crank sprocket, and a
# pump placed by eye had its front top corner 1.4 mm inside it.
OIL_PUMP_Z = -(chain_outer(CRANK_PCD, CRANK_TEETH) + 3.0 + OIL_BODY_H)

# ---- how it is held ---------------------------------------------------------
# A mount pad each side of the crankcase, between the ribs, and a bracket
# bolted to it. The bolts are SHALLOW on purpose: the wall they go into is
# only 14 mm thick (the bays inside reach x = 86) and a deeper one is a bolt
# into the crankcase.
MOUNT_Y = -60.0
MOUNT_BOLT_DEPTH = 12.0
MOUNT_BOLTS = [(-15.0, 10.0), (15.0, 10.0), (0.0, 48.0)]   # (dy, z)
MOUNT_BOLT_D = 11.0

# ---- cooling ---------------------------------------------------------------
WATER_PUMP_Y = -272.0
WATER_PUMP_Z = 150.0
BELT_Y = DAMPER_SEAT[0] + 6.0      # the plane both pulleys' grooves run
                                   # in — far enough forward that the water
                                   # pump's pulley clears its own housing
IMPELLER_D = 78.0
PULLEY_D = 120.0          # crank pulley / damper
DAMPER_T = 26.0
WP_PULLEY_D = 96.0


def piston_z(crank_deg, throw_deg=0.0):
    """Where the gudgeon pin is, for a crank angle. The exact slider-crank,
    not the two-term approximation — this is what the motion study is checked
    against, so it has to be the real thing."""
    a = math.radians(crank_deg + throw_deg)
    return CRANK_R * math.cos(a) + math.sqrt(
        ROD_L ** 2 - (CRANK_R * math.sin(a)) ** 2)


def lobe_lift(cam_deg, start_deg, rise=CAM_LIFT, duration=DURATION_CAM):
    """The same harmonic lobe the cam RELATION builds from {rise, duration,
    start} — so the drawn profile and the kinematics are one function and
    cannot drift apart."""
    f = ((cam_deg - start_deg) % 360.0) / duration
    if f > 1.0:
        return 0.0
    return rise * 0.5 * (1.0 - math.cos(2.0 * math.pi * f))


def cam_phase(cylinder):
    """How far this cylinder's lobes are round the camshaft from number 1's."""
    return FIRING_TDC[cylinder] / 2.0
