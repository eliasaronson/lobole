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
    +X   across the engine: the car's LEFT, with the front at -Y and up +Z.
         The PUMP SIDE (injection pump, injectors, filters, dipstick,
         vacuum pump) is PUMP_SIDE, +X; both manifolds and the alternator
         are on the other side, MANIFOLD_SIDE. Standing on the pump side,
         the fan is on your left, as on the real engine.

Everything with a unit is derived from the four numbers a person would
actually quote for an engine (bore, stroke, rod, bore pitch) wherever the
derivation is real. Other values include design estimates; sourced dimensions
are identified separately in generators/om615/README.md#sources.
"""
import math

# Which side of the engine the injection equipment is on, as the sign of x.
# +1 is +X, the car's LEFT (front -Y, up +Z), where the OM615 has its pump,
# injectors, filters, dipstick and vacuum pump; the manifolds and the
# alternator are on MANIFOLD_SIDE. Every side-specific number and frame below
# and in the builders is written with one of these two, so the side is one
# choice and cannot drift piece by piece. It is a RE-LAYOUT, not a mirror:
# threads, standard parts, frames and the direction the crank turns (+Y,
# clockwise seen from the front) do not change with it.
PUMP_SIDE = 1
MANIFOLD_SIDE = -PUMP_SIDE

# Reference target: early 200 D/8, OM615.913 (21:1), not the later 615.940.
# Verified dimensions and remaining approximations: generators/om615/README.md#sources.
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
PIN_H = 52.9              # Proxmann 71160800, OM615.913, catalog p.67
PISTON_HEIGHT = 91.4      # same drawing: crown to skirt bottom
SQUISH = 3.2              # crown to deck at TDC
DECK = CRANK_R + ROD_L + PIN_H + SQUISH
SKIRT = PISTON_HEIGHT - PIN_H
# Running fits are drawn at 0.5 mm RADIAL, everywhere, and that is a
# modelling number rather than an engineering one: a piston runs at 0.1 mm
# and a bearing shell at 0.05, but the collision mesh deflects ~0.4 mm on
# parts this size, so anything tighter is reported as metal inside metal at
# every sample of the motion study. FIT is the one place that decision is
# written down.
FIT = 1.0                 # diametral
PISTON_CLEARANCE = FIT
GUDGEON_D = 26.0          # Proxmann 71160800: 26 x 72 mm pin
GUDGEON_L = 72.0
BUSH_OD = 36.0            # small-end bush outside diameter
SMALL_END_D = 46.0
RING_T = [3.0, 2.0, 4.0]  # Proxmann 71160800 ring stack
RING_SIDE_CLEARANCE = 0.05  # modeled axial clearance per groove flank
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
GALLERY_X = PUMP_SIDE * 93.0
GALLERY_Z = 60.0
GALLERY_D = 10.0
OIL_RISER_X = PUMP_SIDE * 62.0
OIL_RISER_D = 8.0
COVER_GASKET_T = 2.0
CAM_GASKET_T = 3.0
COVER_LIP = 5.0           # how far the cam cover overhangs the head
HEAD_CHAMFER = 10.0       # the head's lower outer edges

# ---- the head --------------------------------------------------------------
FIRE_FACE = DECK + GASKET_T          # the head's underside
HEAD_X = 86.0             # Wider than the deck to enclose the rocker shafts.
CAM_COVER_BOLT_X = HEAD_X + 15.0
CAM_COVER_BOSS_D = 16.0
CAM_COVER_BOSS_H = 14.0   # Low mounting ears, accessible beside the cover.
HEAD_H = 85.0             # Workshop 05.4-118/1: machined height 84.8–85.0.
HEAD_TOP = FIRE_FACE + HEAD_H
# Both valves are on the manifold side; their order alternates by cylinder.
# Each valve, rocker and cam lobe shares one longitudinal station.
VALVE_INSET = 8.0         # how far the valves stand off the centreline
VALVE_X = MANIFOLD_SIDE * VALVE_INSET
VALVE_DY = 20.0
VALVE = {"intake": (VALVE_X, -VALVE_DY), "exhaust": (VALVE_X, VALVE_DY)}
STEM_D = 9.92            # within both diesel stem ranges, manual p.24
STEM_TIP_CHAMFER = 0.5   # Modeled edge break; not a factory dimension.
VALVE_HEAD_D = {"intake": 38.8, "exhaust": 33.2}  # manual p.24 midpoints
SEAT_ANGLE = 45.0
# Insert outside diameters are modeling dimensions. Leave 2 mm radial wall
# around the corrected heads, bounded by the bore and the adjacent insert.
SEAT_OD = {k: d + 4.0 for k, d in VALVE_HEAD_D.items()}
SEAT_T = 6.0
GUIDE_OD = 15.0
GUIDE_LEN = 35.0          # ends BELOW where the retainer comes down to
GUIDE_Z = FIRE_FACE + 16.0           # where the guide's bore starts
VALVE_LIFT = 8.0
PORT_D = {"intake": 30.0, "exhaust": 28.0}
PORT_Z = FIRE_FACE + 39.0            # where a port leaves the side face
# Modeled studs above and below each external port. The pads stop below
# the cover's mounting ears; none of these coordinates is a factory pattern.
MANIFOLD_BOLT_DZ = 24.0
MANIFOLD_BOLT_D = 9.0
MANIFOLD_PAD = 12.0                  # how thick a manifold's flange is
MANIFOLD_PAD_HY = 30.0               # ...and how far it reaches in y and z
MANIFOLD_PAD_HZ = 30.0
MANIFOLD_GASKET_T = 2.0
MANIFOLD_STUD_DEPTH = 15.0           # shallow ON PURPOSE: 26 deep and the
                                     # stud runs into the head bolt's own
                                     # boss, which is 12.5 mm away in y and
                                     # the full height of the head in z
def valve_dy(k, kind):
    # E I | I E | E I | I E. Paired intake valves share two external ports.
    return (1 if (k % 2 == 0) == (kind == "intake") else -1) * VALVE_DY


def port_centers(kind):
    return [-BORE_PITCH, BORE_PITCH] if kind == "intake" else [
        cy + valve_dy(k,kind) for k,cy in enumerate(CYL_Y)]


def port_y(k,kind):
    return (-BORE_PITCH if k<2 else BORE_PITCH) if kind=="intake" else CYL_Y[k]+valve_dy(k,kind)


# Both manifolds have analytic circular elbows into a longitudinal collector.
# The bend radius is computed from each port and collector position, with a
# horizontal start tangent so the gasket face remains planar.
# (x, z, outer, bore) for each log, and the outer/bore of its branches.
INT_LOG = (MANIFOLD_SIDE * 145.0, FIRE_FACE + 135.0, 70.0, 56.0)   # high, like the real one
INT_RUNNER = (54.0, 44.0)
EXH_RUNNER = (44.0, 34.0)
MANIFOLD_END_ALLOWANCE = 8.0


def collector_span(kind, runner):
    # Enclose the whole outer tube, with an allowance beyond its envelope.
    margin = runner[0] / 2.0 + MANIFOLD_END_ALLOWANCE
    return (port_centers(kind)[0] - margin,
            port_centers(kind)[-1] + margin)


INT_LOG_Y = collector_span("intake", INT_RUNNER)
EXH_LOG_Y = collector_span("exhaust", EXH_RUNNER)


# Collector standoff leaves room for the elbow and a core around its bore.
EXH_LOG = (MANIFOLD_SIDE * (HEAD_X + MANIFOLD_GASKET_T + MANIFOLD_PAD
                           + 1.5 * EXH_RUNNER[0]),
           FIRE_FACE - 5.0, 52.0, 40.0)
# The spring sits in a POCKET sunk below the rocker chamber's floor. On the
# floor itself there are only 20 mm between the seat and the retainer, which
# is a spring whose coils are closer together than its wire is thick.
SPRING_SEAT_Z = FIRE_FACE + 28.0
SPRING_POCKET_D = 38.0
SPRING_D = 30.0           # mean coil diameter
SPRING_WIRE = 3.8         # diesel spring wire, manual p.24
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
# Inclined prechambers and injectors are opposite the manifolds. The manual
# establishes the architecture; angle and stepped envelopes remain estimates.
PRE_X = PUMP_SIDE * 22.0
PRE_DY = 0.0
INJ_D = 24.0
INJ_REACH = 25.0
INJ_HEIGHT = 70.0
INJ_BODY_D = 30.0
INJ_HEX_AF = 27.0          # Workshop 07.1.4-230/1 specifies a 27 mm socket.
GLOW_D = 10.0
INJ_FACE = (PUMP_SIDE * 62.0, PRE_DY, HEAD_TOP)
INJ_TARGET = (PRE_X, PRE_DY, FIRE_FACE + 13.0)
GLOW_FACE = (PUMP_SIDE * HEAD_X, PRE_DY, FIRE_FACE + 20.0)
GLOW_TARGET = (PUMP_SIDE * 31.0, PRE_DY, FIRE_FACE + 38.0)

# ---- the valve gear --------------------------------------------------------
CAM_Z = FIRE_FACE + 155.0   # Estimated cam datum, independent of machined head height.
CAM_SHAFT_D = 32.0
CAM_JOURNAL_D = 44.0
CAM_BEARING_W = 16.0
# Over the webs like the mains, but the outer pair pulled IN: a saddle on
# the outermost web puts its cap and its rocker post through the cam cover's
# own end wall, and through its gasket's rim. Same bound as the head bolts.
CAM_OUTER_Y = min(-BLOCK_Y0, BLOCK_Y1) - CAM_BEARING_W / 2.0 - 2.0
CAM_BEARING_Y = [-CAM_OUTER_Y, -97.0, 0.0, 97.0, CAM_OUTER_Y]
CAM_BASE_R = 20.0
CAM_LOBE_W = 16.0
# The rocker shafts, outboard of both valves — but INBOARD enough that a
# rocker's pivot boss stays inside the chamber wall. The boss is 16 mm in
# radius and the chamber ends at 78, so 72 put every one of the eight into
# the head's own side wall. A distance from the centreline: the shaft is at
# MANIFOLD_SIDE * ROCKER_PIVOT_X.
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
ARM_VALVE = ROCKER_PIVOT_X - VALVE_INSET  # pivot to the valve pad
ROCKER_RATIO = ARM_VALVE / ARM_CAM
CAM_LIFT = VALVE_LIFT / ROCKER_RATIO
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
# The pan is DEEP only under the front cylinders, where the pickup is, and
# shallow behind — the factory section (KTG 9810) shows the well ending under
# the middle of the engine and a ramp up to a floor just below the rail. The
# shallow floor still clears the crank: the rod caps reach 83.8 below the
# axis and the rail is at 92.
SUMP_WELL_Y = MAIN_Y[2]           # where the well's flat floor ends
SUMP_SHALLOW_DEPTH = 45.0         # the rear floor, below the pan rail
SUMP_RAMP_DEG = 40.0              # the ramp between them, from horizontal
SUMP_LIP = 4.0                    # the pressed lip round the flange...
SUMP_LIP_H = 7.0                  # ...and how far it turns down
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
MOUNT_ARM_T = 7.0         # the cast arm's two side webs

# ---- the outside of the block ------------------------------------------------
# Casting form read off the photographs (scaled against the 87 mm bore), not
# a drawing. The ribs are the ones over each main web.
BLOCK_RIB_W = 16.0
JACKET_PANEL_T = 3.0      # the raised panel over the water jacket
CORE_PLUG_D = 34.0        # three a side, between the bores
CORE_PLUG_Z = (JACKET_Z0 + JACKET_Z1) / 2.0
FLANGE_BAND_H = 12.0      # the ledge along the pan rail
FLANGE_BAND_T = 3.0
# Where the oil filter housing bolts on: a flat round pad on the pump side
# (PUMP_SIDE), low and toward the rear, in the bay between the ribs over mains 3
# and 4, at the main gallery's own height so its port drills straight in.
OIL_FILTER_BOSS_D = 70.0
OIL_FILTER_BOSS_H = 12.0  # proud of the crankcase wall; clears the ribs
OIL_FILTER_BOSS = (PUMP_SIDE * (CASE_X + OIL_FILTER_BOSS_H),
                   (MAIN_Y[2] + MAIN_Y[3]) / 2.0, GALLERY_Z)
OIL_FILTER_AXIS = (PUMP_SIDE, 0.0, 0.0)      # outward, the pad's normal
OIL_FILTER_BOLT_D = 8.0                     # four M8, tapped
OIL_FILTER_BOLT_PCD = 52.0
OIL_FILTER_PORT_D = 12.0                    # into the main gallery
# The dipstick enters the crankcase through the same wall, in the next bay
# aft, leaning outward so its tube clears the pad. Its socket's top face and
# axis are the tube's seat.
DIPSTICK_TILT_DEG = 15.0
DIPSTICK_AXIS = (PUMP_SIDE * math.sin(math.radians(DIPSTICK_TILT_DEG)), 0.0,
                 math.cos(math.radians(DIPSTICK_TILT_DEG)))
DIPSTICK_BOSS_D = 22.0
DIPSTICK_BORE_D = 10.0
# High enough that the bore breaks into the crankcase 20 mm above the rail,
# not through the rail's gasket face.
DIPSTICK_BOSS = (PUMP_SIDE * (CASE_X + OIL_FILTER_BOSS_H + 2.0),
                 (MAIN_Y[3] + MAIN_Y[4]) / 2.0,
                 PAN_RAIL + 20.0 + (OIL_FILTER_BOSS_H + 2.0 + 14.0)
                 / math.tan(math.radians(DIPSTICK_TILT_DEG)))

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

# Visual-reference mounting envelope. Not a sourced pump bolt pattern.
PUMP_MOUNT = (PUMP_SIDE * 108.0, BLOCK_Y0 + 80.0, 174.0)
PUMP_REGISTER_D = 60.0
PUMP_BOLT_PCD = 78.0
PUMP_MOUNT_D = 96.0

COVER_BASE = HEAD_TOP
COVER_LEFT = 42.0         # the cam cover's half-width on the PUMP side
COVER_TRANSITION_Y = -175.0
COVER_DOME_END_Y = DRIVE_Y + CHAIN_W/2 + 8.0

def cover_bolt_x(side):
    """The cam cover bolts' x on the side whose sign is `side`: close in on
    the pump side, where the cover is narrow, out past the head's edge on
    the manifold side."""
    return side * (54.0 if side == PUMP_SIDE else CAM_COVER_BOLT_X)


# ---- the top end's castings: what they look like -------------------------
# Shape only: nothing below moves anything, and every interface the
# mechanism or a mate uses is named above. Read off the photographs (top,
# manifold side, the KTG 9810 sectional drawing) against the 87 mm bore and
# the 97 mm bore pitch, so each is said in those terms where it can be.
CAM_COVER_WALL = 6.0                 # the cavity is the outside, inset by it
CAM_COVER_CROWN = CAM_COVER_WALL     # the top rises this much between its
                                     # shoulders: a dome, not a flat roof
CAM_COVER_RIM = (CAM_COVER_WALL / 2.0, CAM_COVER_BOSS_H * 0.6)
                                     # (proud, tall): the lip at the gasket
CAM_COVER_FRONT_R = 2.0 * CAM_COVER_WALL   # the chain dome's front corners
STAR_D = 0.7 * BORE                  # the three-pointed star's medallion
STAR_RELIEF = 2.0                    # how far the star stands off its disc
FILLER_BOSS_D = 56.0                 # the cap is 52 across; its neck stands
FILLER_Y = COVER_TRANSITION_Y + FILLER_BOSS_D / 2.0 + 2.0 * CAM_COVER_WALL
                                     # near the FRONT, just behind the chain
                                     # dome, where the photographs have it
BREATHER = (18.0, 11.0)              # breather stub outside / bore
BREATHER_Y = CAM_BEARING_Y[1] + CAM_BEARING_W   # clear of a rocker post

# The intake: a cast box on the INT_LOG axis, not a tube, with two branches
# per paired port — four runners, as the real one has.
PLENUM = (INT_LOG[2] + 6.0, INT_LOG[2] - 6.0, INT_LOG[2] / 4.0)
                                     # (width x, height z, corner radius)
PLENUM_HALF_L = 1.5 * BORE_PITCH     # it spans the outer cylinders' centres
INT_BRANCH = (PORT_D["intake"] + 4.0, PORT_D["intake"] - 4.0)
INT_BRANCH_DY = PORT_D["intake"] / 2.0     # each branch off its port's axis
THROTTLE_BORE = 1.5 * PORT_D["intake"]
THROTTLE_OD = THROTTLE_BORE + 16.0
THROTTLE_NECK = 14.0                 # the neck between plenum and body
THROTTLE_L = 2.0 * THROTTLE_OD / 3.0

# The exhaust: one outlet, straight DOWN from the middle of the log, on a
# three-stud flange.
EXH_OUTLET_DROP = 16.0               # tube below the log before the flange
EXH_OUTLET_T = MANIFOLD_PAD - 2.0
EXH_OUTLET_STUD = (8.0, 36.0, 18.0)  # (diameter, radius from the axis, length)

# The head's side: a rib under the cover rail, proud of the drafted wall.
HEAD_RIB = (4.0, 12.0)               # (proud, tall), up to HEAD_TOP

# The thermostat housing, on a cast ear at the head's front corner on the
# manifold side, above the first exhaust flange and ahead of the plenum.
TSTAT_X = MANIFOLD_SIDE * (HEAD_X + 34.0)
TSTAT_Y = BLOCK_Y0 + 22.0
TSTAT_BOLT_DY = 28.0                 # two M8, fore and aft of the body
TSTAT_BOLT_D = PAN_BOLT_D
TSTAT_BODY_D = 36.0
TSTAT_FLANGE_T = 10.0
TSTAT_TAP_DEPTH = 12.0
TSTAT_OUTLET = (30.0, 22.0)          # the upper-hose stub outside / bore
TSTAT_EAR_Z = PORT_Z + MANIFOLD_PAD_HZ + 3.0   # the ear's underside, clear
                                     # above the first exhaust flange
TSTAT_WEB_T = 6.0                    # the web under the ear's front edge


# ---- what hangs on it: the static parts outside ------------------------------
# Read off the pump-side, manifold-side and front photographs against the
# 87 mm bore and the 97 mm bore pitch. None of it moves anything. The
# interfaces the parts bolt or push onto are said here; each part's own shape
# is in om615_accessories.
#
# The oil filter housing bolts to the block's pad (OIL_FILTER_BOSS) through a
# flange as thick as the pad stands proud; the pad's four M8 are blind.
OIL_FILTER_FLANGE_T = OIL_FILTER_BOSS_H
OIL_FILTER_TAP_DEPTH = 16.0
# The fuel filter hangs from a bracket on a boss on the head's pump side,
# between cylinders 1 and 2 (the glow plugs and injectors are ON the
# cylinders' centres), high enough that its canister clears the injection
# pump's flange below it.
FUEL_FILTER_Y = MAIN_Y[1]
FUEL_FILTER_BOSS_H = 14.0                    # proud of the head's side wall
FUEL_FILTER_BOSS = (PUMP_SIDE * (HEAD_X + FUEL_FILTER_BOSS_H), FUEL_FILTER_Y,
                    HEAD_TOP - 24.0)         # the boss's FACE centre
FUEL_FILTER_BOSS_HALF = (14.0, 22.0)         # (y, z) half-size of the pad
FUEL_FILTER_BOLT_DZ = 12.0                   # two M8, above and below centre
FUEL_FILTER_BOLT_D = PAN_BOLT_D
FUEL_FILTER_TAP_DEPTH = 16.0                 # ends inside the boss's root
# Coolant hose: the thermostat's outlet stub is the hose's bore, and the
# water pump's inlet neck is the same hose at the radiator's other end.
COOLANT_HOSE_WALL = 5.0
COOLANT_HOSE_OD = TSTAT_OUTLET[0] + 2.0 * COOLANT_HOSE_WALL
WP_INLET = TSTAT_OUTLET                      # (outside, bore) of the neck
# The neck stands on top of the pump housing, ahead of the timing cover by
# the hose's own radius, so the hose leaving it runs clear of the cover.
WP_INLET_Y = COVER_Y0 - COOLANT_HOSE_OD / 2.0 - 5.0
# The heater pipe: a steel tube off the thermostat housing's outboard side,
# back along the manifold side between the exhaust flanges and the plenum.
HEATER_PIPE = (16.0, 1.5)                    # (outside diameter, wall)
HEATER_STUB = 12.0                           # the spigot it pushes onto
