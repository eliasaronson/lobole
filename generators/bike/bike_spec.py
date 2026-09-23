"""The bicycle's dimensions, and the layout derived from them.

The model is of a real bicycle that was photographed, not measured
(generators/bike/README.md). So this file keeps two kinds of number strictly
apart, and says which is which on every line:

  READ      what the photographs establish: a gents' diamond frame, 28"
            wheels, a 3-speed trigger shifter (so an internal-gear rear hub),
            one chainring, a full teardrop chainguard, alloy fenders, a
            tubular rear rack, north-road bars.
  ASSUMED   every length, angle and tooth count. These are standard
            Scandinavian roadster numbers. Each is declared as a project
            variable, so correcting one is an edit and not a rebuild.

Nothing here is a literal in a part file: the parts import this, and
`declare(app)` rebinds every name to the document variable that drives it.

Coordinates: world Z up, X forward (toward the front wheel), Y left. The
ORIGIN IS THE BOTTOM BRACKET — a bicycle's one unambiguous point, and the
only one every other dimension is quoted from in the trade. The ground is
therefore at z = BB_DROP - WHEEL_R, which is negative; nothing is built
"standing on" z = 0.
"""
import math

# --- the wheels ---------------------------------------------------------------
# READ: 28" wheels, chrome rims, 36 spokes, black tyres of about this section.
RIM_BED_D = 622.0          # ASSUMED 622 (ISO 28"/700C) — the common size
TYRE_SECTION = 37.0        # ASSUMED 37-622, the roadster's usual
RIM_DEPTH = 13.0           # ASSUMED — a box-section steel/alloy rim
RIM_WIDTH = 22.0           # ASSUMED, outside
RIM_WALL = 1.1             # ASSUMED
RIM_BED_T = 3.0            # ASSUMED: the floor the spokes pull on is thicker
                           # than the rest of the section, on a real rim and
                           # here — a spoke ending inside a 1.1 mm wall is a
                           # cap wedged between two surfaces, and five of the
                           # thirty-six unions failed on it
SPOKE_D = 2.0              # ASSUMED 14g plain gauge
SPOKE_COUNT = 36           # READ: 36 spokes, counted off the photographs
SPOKE_CROSS = 3            # ASSUMED — three-cross is what 36h roadsters get
TYRE_TREAD_D = 3.0         # ASSUMED depth of the tread band's relief

# --- the hubs -----------------------------------------------------------------
# READ: the rear hub is a wide, smooth, shiny shell and the right bar carries a
# trigger shifter: a 3-speed internal-gear hub. Its shell is the give-away, and
# it is why the rear wheel is not just the front wheel mirrored.
FRONT_HUB_D = 38.0         # ASSUMED shell
FRONT_HUB_W = 92.0         # ASSUMED shell length
FRONT_OLD = 100.0          # ASSUMED over-locknut (fork spacing)
REAR_HUB_D = 72.0          # ASSUMED — a 3-speed shell
REAR_HUB_W = 84.0          # ASSUMED shell, between the flange faces — short,
                           # because the cog's driver has to fit between the
                           # shell's right end and the dropout
REAR_OLD = 126.0           # ASSUMED (roadster 3-speed)
FLANGE_D = 58.0            # ASSUMED spoke flange, front
FLANGE_SPACING = 66.0      # ASSUMED between flanges, front
REAR_FLANGE_D = 70.0       # ASSUMED — an internal-gear hub's flanges sit on
                           # its big shell
REAR_FLANGE_SPACING = 74.0 # ASSUMED
AXLE_D = 9.5               # ASSUMED 3/8" axle
AXLE_FIT = -0.4            # ASSUMED: the dropout's hole, RELATIVE to the
                           # axle. Negative — the axle is clamped in it, and
                           # a 0.3 mm gap is not a fit, it is a near miss the
                           # assembly check rightly complains about

# --- the frame ----------------------------------------------------------------
# READ: diamond frame, HORIZONTAL top tube, lugged, and a tall one — the
# saddle sits well above the bars in every photograph.
SEAT_TUBE_LEN = 580.0      # ASSUMED centre-to-top: a 58 frame
TOP_TUBE_LEN = 580.0       # ASSUMED horizontal, seat-tube axis to head-tube
HEAD_TUBE_LEN = 160.0      # ASSUMED
HEAD_ANGLE_DEG = 71.0      # ASSUMED — roadsters are slacker than road bikes
SEAT_ANGLE_DEG = 72.0      # ASSUMED
BB_DROP = 65.0             # ASSUMED — a low bottom bracket, upright riding
CHAINSTAY_LEN = 465.0      # ASSUMED — long stays, to clear the fender
FORK_RAKE = 55.0           # ASSUMED offset, giving the trail checked below
TOP_TUBE_DROP = 6.0        # ASSUMED: top-tube axis below the seat-tube top
HEAD_ABOVE_TT = 25.0       # ASSUMED: head tube standing proud of the top tube
DOWN_TUBE_UP_HEAD = 22.0   # ASSUMED: down tube meets the head tube this far
                           # above its bottom

# Tube diameters. ASSUMED throughout — plain-gauge roadster tubing.
HEAD_TUBE_D = 32.0
SEAT_TUBE_D = 28.6
DOWN_TUBE_D = 28.6
TOP_TUBE_D = 25.4
SEAT_STAY_D = 16.0
CHAIN_STAY_D = 16.0
BB_SHELL_D = 40.0
BB_SHELL_W = 68.0          # ASSUMED BSA 68
TUBE_WALL = 1.2            # ASSUMED — and the reason the frame is built as
                           # PIPES: a solid one would weigh three times what
                           # this bicycle weighs, and the mass is a check.
LUG_T = 1.6                # ASSUMED lug wall over the tube it wraps
LUG_LEN = 36.0             # ASSUMED lug length along each tube
STAY_SPLAY = 22.0          # ASSUMED half-spacing of the stays at the frame end
CHAINSTAY_FOOT_Y = 22.0    # ASSUMED where a chainstay leaves the bottom
CHAINSTAY_FOOT_Z = 12.0    # bracket shell: on its UPPER rear quadrant, and
                           # close in. Both numbers are set by the crank,
                           # which sweeps a 170 mm circle in the plane of its
                           # arm: run off the shell's centre line the stay
                           # passed through that circle and the left arm cut
                           # 4.7 cm3 out of it every revolution
DROPOUT_T = 5.0            # ASSUMED plate thickness
DROPOUT_R = 26.0           # ASSUMED plate radius about the axle

# --- the drivetrain -----------------------------------------------------------
# READ: one chainring, one cog, a full teardrop alloy chainguard, and the
# 3-speed hub above. Tooth counts are INTEGERS and stay out of the document —
# a count is not a dimension.
CHAINRING_T = 46
COG_T = 18
CHAIN_PITCH = 12.7         # ASSUMED 1/2" — the only pitch a bicycle uses
CHAIN_WIDTH = 8.0          # ASSUMED 1/8" single-speed chain, over the plates
CHAIN_ROLLER_D = 7.8       # ASSUMED
CHAINRING_T_MM = 2.0       # ASSUMED ring thickness
CHAINRING_ARMS = 5         # READ: a five-arm crank, visible through the guard
CRANK_LEN = 170.0          # ASSUMED
CRANK_W = 28.0             # ASSUMED arm width
CRANK_OFFSET = 64.0        # ASSUMED: the arm's centre from the bicycle's
                           # centre line, so a Q-factor of 138 over the two
                           # faces. It is not a free number: the arm has to
                           # pass OUTSIDE the chainguard, which passes
                           # outside the chain
CRANK_T = 13.0             # ASSUMED arm thickness
SPINDLE_D = 17.0           # ASSUMED
CHAINLINE = 47.0           # ASSUMED: the chain's plane, right of centre (-Y).
                           # It is set by where the cog can sit on the hub,
                           # and the chainring is then built to match it
PEDAL_L = 92.0             # ASSUMED — rubber-block pedals
PEDAL_W = 62.0
PEDAL_H = 26.0
COG_T_MM = 3.2             # ASSUMED cog thickness

# The chainguard: READ as a teardrop — a disc over the chainring with a hole
# showing the crank, and a tapering arm reaching back over the chain.
GUARD_T = 1.5              # ASSUMED alloy sheet
GUARD_CLEAR = 6.5          # ASSUMED: the guard's plate, outboard of the
                           # chainring. It sits in the gap between the chain
                           # and the CRANK ARM, which sweeps outside it —
                           # which is what you see on the bicycle, and what
                           # makes the layout close (generators/bike/README.md#what-a-run-checks)
GUARD_DISC_MARGIN = 14.0   # ASSUMED beyond the chainring's teeth
GUARD_EYE_D = 140.0        # ASSUMED: the hole the crank spider shows
                           # through — and it must CLEAR the spider, which
                           # reaches from the ring out to the arm's plane
GUARD_TAIL_R = 26.0        # ASSUMED radius of the tail
GUARD_TAIL_SHORT = 120.0    # ASSUMED: how far short of the rear axle the tail
                           # stops. Drawn ON the axle it swallowed it
GUARD_TAIL_W = 20.0        # ASSUMED depth of the tail's strip below the
                           # chain's top run — the chainstay comes up to meet
                           # it, and at 40 they touch near the dropout

# --- the cockpit --------------------------------------------------------------
# READ: swept-back north-road bars, white grips, one brake lever, a bell, a
# trigger shifter, a quill stem, a chrome seatpost well out of the frame.
BAR_D = 22.2               # ASSUMED
BAR_WIDTH = 560.0          # ASSUMED, tip to tip
BAR_RISE = 60.0            # ASSUMED: how far the grips sit above the clamp
BAR_SWEEP = 150.0          # ASSUMED: how far back the grips sit
BAR_CENTRE = 120.0         # ASSUMED: the straight run the stem clamps
BAR_BEND_R = 55.0          # ASSUMED: the radius the tube is BENT to. A
                           # handlebar is one pipe with two bends in it, and
                           # this is what makes it read as one
STEM_QUILL_UP = 95.0       # ASSUMED: stem clamp above the head tube's top
STEM_REACH = 60.0          # ASSUMED
STEM_D = 22.6              # ASSUMED quill. A TENTH over the steerer's bore,
                           # because a quill stem is wedged in: drawn a
                           # tenth UNDER it, the stem hung in the steerer
                           # touching nothing and the whole front end came
                           # back as a separate floating group
GRIP_LEN = 115.0           # ASSUMED
GRIP_D = 32.0              # ASSUMED
LEVER_LEN = 105.0          # ASSUMED
SEATPOST_D = 26.4          # ASSUMED — a tenth over what a 28.6 seat tube of
                           # this wall bores to, because it is CLAMPED in it
SEATPOST_OUT = 185.0       # ASSUMED exposed length — the photographs show a
                           # lot of post, which is what makes the frame read
                           # as ridden by someone tall
SADDLE_LEN = 265.0         # ASSUMED
SADDLE_W = 196.0           # ASSUMED — READ: the owner's is a wide padded
                           # city saddle, round at the back and long-nosed
SADDLE_H = 55.0            # ASSUMED shell + padding
BELL_D = 54.0              # ASSUMED

# --- the fittings -------------------------------------------------------------
# READ: full-length alloy fenders front and rear, a tubular rear rack with a
# slatted top, a red rear reflector, a front caliper brake.
FENDER_GAP = 14.0          # ASSUMED between tyre and fender
FENDER_W = 52.0            # ASSUMED
FENDER_T = 1.2             # ASSUMED
FENDER_CROWN = 9.0         # ASSUMED height of the fender's peak
# How far round each mudguard wraps, in the bicycle's own plane: 0 deg is
# straight FORWARD from that wheel's axle and the angle runs UP over the top,
# so 90 is the crown of the guard and 180 is level with the axle behind it.
REAR_FENDER_FROM_DEG = 25.0   # ASSUMED: it starts where the seat tube allows
REAR_FENDER_TO_DEG = 215.0    # ASSUMED: and ends below the axle at the back
FRONT_FENDER_FROM_DEG = -25.0  # ASSUMED: the long front section the
FRONT_FENDER_TO_DEG = 200.0    # photographs show, and back past the axle
STAY_D = 4.5               # ASSUMED fender stay rod
RACK_LEN = 320.0           # ASSUMED platform
RACK_W = 130.0             # ASSUMED
RACK_TUBE_D = 10.0         # ASSUMED
RACK_ABOVE_TYRE = 62.0     # ASSUMED: platform above the tyre's top
REFLECTOR_W = 62.0         # ASSUMED
REFLECTOR_H = 30.0
BRAKE_REACH = 60.0         # ASSUMED caliper drop

# --- clearances the build must keep -------------------------------------------
TYRE_FRAME_CLEAR = 8.0     # ASSUMED minimum tyre-to-tube
GROUND_PEDAL_CLEAR = 60.0  # ASSUMED minimum pedal-to-ground at the bottom

_INT = {"CHAINRING_T", "COG_T", "SPOKE_COUNT", "SPOKE_CROSS", "CHAINRING_ARMS"}


def _is_dimension(name, value):
    return (name.isupper() and not name.startswith("_")
            and isinstance(value, (int, float))
            and name not in _INT)


# --- the layout, derived ------------------------------------------------------
# Nothing below is typed in: it follows from the numbers above. A part asks
# for a point by name and never reconstructs the trigonometry itself, which is
# how the frame, the fork, the fenders and the chain stay on one geometry.

def _derive(ns):
    g = ns.__setitem__ if isinstance(ns, dict) else None
    d = ns if isinstance(ns, dict) else vars(ns)

    def V(*xyz):
        return tuple(float(c) for c in xyz)

    head = math.radians(float(d["HEAD_ANGLE_DEG"]))
    seat = math.radians(float(d["SEAT_ANGLE_DEG"]))
    wheel_r = (float(d["RIM_BED_D"]) + 2 * float(d["TYRE_SECTION"])) / 2.0
    drop = float(d["BB_DROP"])

    # The bottom bracket is the origin; the axles sit BB_DROP above it.
    bb = V(0, 0, 0)
    rear_axle = V(-math.sqrt(float(d["CHAINSTAY_LEN"]) ** 2 - drop ** 2), 0,
                  drop)

    # The seat tube, and the top tube across from it.
    seat_dir = V(-math.cos(seat), 0, math.sin(seat))       # BB -> saddle
    st_top = V(*[bb[i] + float(d["SEAT_TUBE_LEN"]) * seat_dir[i]
                 for i in range(3)])
    tt_z = st_top[2] - float(d["TOP_TUBE_DROP"])
    tt_rear = V(-tt_z / math.tan(seat), 0, tt_z)
    tt_front = V(tt_rear[0] + float(d["TOP_TUBE_LEN"]), 0, tt_z)

    # The head tube stands proud of the top tube and runs down from there.
    head_up = V(-math.cos(head), 0, math.sin(head))         # bottom -> top
    ht_top = V(*[tt_front[i] + float(d["HEAD_ABOVE_TT"]) * head_up[i]
                 for i in range(3)])
    ht_bot = V(*[ht_top[i] - float(d["HEAD_TUBE_LEN"]) * head_up[i]
                 for i in range(3)])
    dt_head = V(*[ht_bot[i] + float(d["DOWN_TUBE_UP_HEAD"]) * head_up[i]
                  for i in range(3)])

    # The front axle: down the steering axis, then offset by the rake
    # PERPENDICULAR to it, and it must land at the same height as the rear
    # one — a bicycle whose axles are at different heights does not stand on
    # flat ground. Solving for the height is what fixes how far down it is.
    steer_down = V(math.cos(head), 0, -math.sin(head))
    rake_dir = V(math.sin(head), 0, math.cos(head))         # forward, normal
    rake = float(d["FORK_RAKE"])
    along = (ht_bot[2] + rake * rake_dir[2] - drop) / math.sin(head)
    front_axle = V(ht_bot[0] + along * steer_down[0] + rake * rake_dir[0], 0,
                   drop)
    fork_crown = V(*[ht_bot[i] + 18.0 * steer_down[i] for i in range(3)])

    out = {
        "HEAD": head, "SEAT": seat,
        "WHEEL_R": wheel_r,
        "TYRE_R": wheel_r,
        "RIM_OUTER_R": float(d["RIM_BED_D"]) / 2.0 + 1.0,
        "RIM_INNER_R": float(d["RIM_BED_D"]) / 2.0 - float(d["RIM_DEPTH"]),
        "GROUND_Z": drop - wheel_r,
        "BB": bb,
        "REAR_AXLE": rear_axle,
        "FRONT_AXLE": front_axle,
        "SEAT_DIR": seat_dir,
        "ST_TOP": st_top,
        "TT_REAR": tt_rear,
        "TT_FRONT": tt_front,
        "HEAD_UP": head_up,
        "STEER_DOWN": steer_down,
        "HT_TOP": ht_top,
        "HT_BOT": ht_bot,
        "DT_HEAD": dt_head,
        "FORK_CROWN": fork_crown,
        "WHEELBASE": front_axle[0] - rear_axle[0],
        # Trail is the one number that decides whether a bicycle steers at
        # all: how far behind the steering axis's ground point the tyre's
        # contact patch sits. Negative trail is a bicycle that falls over.
        "TRAIL": (wheel_r * math.cos(head) - rake) / math.sin(head),
        # The chainring and cog, from their tooth counts: a sprocket's pitch
        # radius is p / (2 sin(pi/z)), and everything about the chain, the
        # guard and the chainline follows from these two.
        "CHAINRING_R": float(d["CHAIN_PITCH"]) / (2 * math.sin(
            math.pi / int(d["CHAINRING_T"]))),
        "COG_R": float(d["CHAIN_PITCH"]) / (2 * math.sin(
            math.pi / int(d["COG_T"]))),
        "GEAR_RATIO": int(d["CHAINRING_T"]) / int(d["COG_T"]),
    }
    # Development: how far the bicycle rolls per crank revolution, in metres.
    # It is what the gearing is FOR, and it is the sanity check on the two
    # tooth counts — 5.9 m is a roadster's middle gear; 12 m would mean the
    # counts are the wrong way round.
    out["DEVELOPMENT_M"] = (out["GEAR_RATIO"] * 2 * math.pi * wheel_r) / 1000.0
    if g:
        for k, v in out.items():
            g(k, v)
    else:
        for k, v in out.items():
            setattr(ns, k, v)
    _refuse(d, out)
    return out


def _refuse(d, L):
    """A layout that cannot be a bicycle is REFUSED here, at import, naming
    what is wrong — not drawn and left for somebody to notice in a render.

    Every one of these caught something while this model was being built.
    """
    def no(msg):
        raise SystemExit(f"bike_spec: {msg}")

    if L["TRAIL"] <= 10.0:
        no(f"trail is {L['TRAIL']:.1f} mm — a head angle of "
           f"{d['HEAD_ANGLE_DEG']} deg with {d['FORK_RAKE']} mm of rake does "
           "not steer. Slacken the head angle or cut the rake.")
    if L["GROUND_Z"] >= 0:
        no("the bottom bracket is below the axles' height: BB_DROP must be "
           "less than the wheel radius")
    if d["BB_DROP"] <= 0:
        no("BB_DROP must be positive — the axles sit ABOVE the bottom bracket")
    # The crank must not strike the ground when it is straight down.
    ground_clear = -L["GROUND_Z"] - float(d["CRANK_LEN"])
    if ground_clear < float(d["GROUND_PEDAL_CLEAR"]):
        no(f"a {d['CRANK_LEN']} mm crank leaves {ground_clear:.0f} mm under "
           f"the pedal, less than the {d['GROUND_PEDAL_CLEAR']} mm asked for")
    # The rear tyre must clear the seat tube, whose axis passes within
    # this distance of the axle.
    sd, ra = L["SEAT_DIR"], L["REAR_AXLE"]
    t = ra[0] * sd[0] + ra[2] * sd[2]
    gap = math.hypot(ra[0] - t * sd[0], ra[2] - t * sd[2]) - L["WHEEL_R"] \
        - float(d["SEAT_TUBE_D"]) / 2.0
    if gap < float(d["TYRE_FRAME_CLEAR"]):
        no(f"the rear tyre clears the seat tube by {gap:.1f} mm; "
           f"{d['TYRE_FRAME_CLEAR']} mm is the minimum. Lengthen "
           "CHAINSTAY_LEN.")
    # The chainring must clear the chainstay's line and the ground.
    if L["CHAINRING_R"] + 10.0 > -L["GROUND_Z"]:
        no("the chainring is bigger than the bottom bracket is high")
    if L["DEVELOPMENT_M"] < 2.0 or L["DEVELOPMENT_M"] > 10.0:
        no(f"{d['CHAINRING_T']}/{d['COG_T']} gives {L['DEVELOPMENT_M']:.1f} m "
           "per crank revolution, which is not a bicycle gear")


def declare(app):
    """Declare every dimension as a project variable and return the namespace
    rebound to the Params that drive the document."""
    base = {k: v for k, v in globals().items() if _is_dimension(k, v)}
    declared = app.params({k.lower(): v for k, v in sorted(base.items())},
                          comment="bike_spec")
    ns = {k: declared[k.lower()] for k in base}
    ns.update({k: v for k, v in globals().items()
               if k.isupper() and k in _INT})
    _derive(ns)
    return ns


# Derived names exist at module scope too, so `from bike_spec import *` in a
# part file has the layout before `declare` has run.
_derive(globals())
