# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Dimensions for the 5-inch FPV freestyle quadcopter.

Everything the model is built from, in one place, in millimetres and
degrees. Three kinds of number live here and they are kept apart:

  CLASS     what a 5" freestyle quad IS — 220 mm wheelbase, 5.1x4.3x3
            props, 2207 motors, 6S. Chosen once (the build class), and
            every other number below either follows from it or is a
            catalogue dimension of a part that fits it.

  CATALOGUE dimensions of bought parts, from their manufacturers' drawings:
            the 30.5 mm stack pattern, the 16x16 motor bolt pattern, the
            19 mm micro camera, the 20x20 VTX. These are NOT free — a
            frame that gets one wrong cannot be built with real parts.

  DESIGN    this frame's own choices: plate sizes, arm shape, where the
            bolts go. Free, but constrained by the two above and by the
            clearances asserted in fpv_drone.py's checks.

The derived layout at the bottom is arithmetic, never transcription: the
motor positions come from the wheelbase, the plate z-levels stack from the
thicknesses, and the standoff length is what puts the top plate under the
propeller disc.
"""
import math

# --- CLASS ------------------------------------------------------------------
WHEELBASE = 220.0          # motor axis to motor axis, diagonal (true X)
PROP_DIAMETER = 129.5      # 5.1 in
PROP_PITCH = 109.2         # 4.3 in, used for the blade twist
PROP_BLADES = 3
CELLS = 6                  # 6S lipo

# --- CATALOGUE (bought parts) ----------------------------------------------
# Motor: 2207 outrunner (EMAX ECO II 2207 / iFlight XING2 2207 class).
MOTOR_BELL_D = 27.9
MOTOR_BASE_D = 28.0
MOTOR_BASE_T = 6.0         # base plate the mounting holes are tapped into
MOTOR_BOLT_SQ = 16.0       # M3 mounting holes on a 16 x 16 square
MOTOR_BOLT_DEPTH = 5.0     # usable tapped depth — a longer screw bottoms out
MOTOR_STATOR_D = 22.0
MOTOR_STATOR_H = 8.0
MOTOR_BELL_H = 16.0
MOTOR_PROP_SEAT_Z = 24.0   # prop mounting face above the motor's mount face
MOTOR_SHAFT_D = 5.0        # M5 prop shaft
MOTOR_SHAFT_TOP = 36.0     # 12 mm of M5 above the seat: 7 of prop, 5 of nut
MOTOR_MASS_G = 32.0        # datasheet, one motor with wires

# Stack: 30.5 x 30.5 M3 pattern, 36 x 36 boards (the universal FPV stack).
STACK_PATTERN = 30.5
STACK_BOARD = 36.0
STACK_HOLE_D = 3.2         # boards ship with 3.2 mm holes / M3 grommets
FC_PCB_T = 1.6
FC_TOP_H = 5.0             # tallest components on top (USB, gyro, pads)
FC_BOTTOM_H = 2.5
ESC_PCB_T = 1.6
ESC_TOP_H = 4.0            # FETs and the input capacitor pads
ESC_BOTTOM_H = 3.0
FC_MASS_G = 9.0
ESC_MASS_G = 16.0

# Video: analog. 19 mm "micro" camera, 20 x 20 VTX.
CAM_BODY = 19.0            # the 19 x 19 square the mount width is set by
CAM_DEPTH = 14.0           # body depth behind the lens
CAM_LENS_D = 14.0
CAM_LENS_L = 11.0
CAM_MOUNT_GAP = 19.5       # between the side plates: body + assembly clearance
CAM_SCREW = "M2"
CAM_MASS_G = 8.5
VTX_PATTERN = 20.0
VTX_BOARD = 25.0
VTX_PCB_T = 1.6
VTX_TOP_H = 4.0
VTX_HOLE_D = 2.2           # M2 clearance
VTX_MASS_G = 8.0
RX_L, RX_W, RX_H = 22.0, 12.0, 4.0
RX_MASS_G = 1.5

# Battery: 6S 1300 mAh, a common freestyle pack.
BATT_L, BATT_W, BATT_H = 72.0, 36.0, 42.0
BATT_MASS_G = 210.0
XT60_L, XT60_W, XT60_H = 16.0, 8.0, 8.0

# Odds and ends.
CAP_D, CAP_H = 10.0, 20.0      # 35 V 470 uF low-ESR, across the ESC input
CAP_MASS_G = 8.0
BUZZER_D, BUZZER_H = 12.0, 6.0
BUZZER_MASS_G = 3.0
ANT_CONNECTOR_D = 6.2          # SMA bulkhead thread
ANT_BORE_D = 6.6               # ... and the printed mount's clearance bore
ANT_TUBE_D, ANT_TUBE_L = 10.0, 62.0
ANT_MASS_G = 9.0
PROP_MASS_G = 4.5              # one 5.1x4.3x3 tri-blade
PROP_HUB_D = 12.5
PROP_HUB_H = 7.0
PROP_BORE_D = 5.0
PROP_NUT_AF = 8.0              # M5 prop nut, across flats
PROP_NUT_H = 5.0

# --- DESIGN (this frame) ----------------------------------------------------
T_BOTTOM = 3.0             # 3K carbon plate thicknesses
T_ARM = 5.0
T_MID = 2.0
T_TOP = 2.0
T_SIDE = 2.0
CF_DENSITY = 1.55e-3       # g/mm^3, 3K twill carbon plate

PLATE_L = 108.0            # bottom and mid plate: X
PLATE_W = 88.0             # ... and Y
PLATE_R = 16.0             # corner radius
TOP_L = 96.0               # top plate is shorter: the camera cage is in front
TOP_W = 88.0

ARM_BOLT_IN = 36.0         # arm bolt radii, measured along the arm axis
ARM_BOLT_OUT = 52.0        # ... and where the standoffs stand
ARM_ROOT_R = 30.0          # root end: clear of the stack and of the
                           # video transmitter's nuts under the mid plate
ARM_ROOT_HW = 14.0         # half width there (28 mm root)
ARM_WAIST_R = 66.0
ARM_WAIST_HW = 10.5        # 21 mm waist
ARM_PAD_R = 16.0           # round motor pad, radius
M3_CLEAR = 3.2             # close fit in carbon; the Hole feature derives it
M2_CLEAR = 2.2
ARM_SHAFT_CLEAR_D = 9.0    # clears the motor's shaft end and circlip

STANDOFF_LEN = 15.0        # sets the top plate height — see the prop check
STANDOFF_AF = 7.0          # hex across flats, 7075 aluminium
STANDOFF_THREAD_DEPTH = 6.0

# The camera cage: two vertical plates, tabs down through the mid plate,
# locked by one transverse screw under it. Everything assembles straight down.
CAGE_TAB = (34.0, 48.0)    # the tab that drops through the mid plate
CAGE_TAB_RELIEF = 1.2      # slot relief past the tab, for its root radius
CAGE_FRONT_X = 57.0
CAGE_BACK_X = 28.0
CAGE_TOP_Z = 34.0          # above the top plate: the camera looks over it
CAM_PIVOT = (44.0, 23.0)   # (x, z) of the camera tilt screw
CAM_TILT_DEG = 20.0
CAGE_LOCK = (42.0, 5.5)    # (x, z) of the transverse lock screw, under the mid plate
# The cage's lock screws are M2 for a reason worth writing down: the space
# between the bottom and mid plates is the arm's own 5 mm, and an M3 cap
# head is 5.5 across. Two M2s into a tapped spacer also beat one long screw
# and a nut, which needs the same room on the far side.
CAGE_SPACER_D = 4.5        # it lives in the 5 mm between bottom and mid plate
CAGE_SCREW = "M2"
CAGE_TAP_DEPTH = 6.0

STRAP_SLOT_L, STRAP_SLOT_W = 21.0, 3.4  # for a 20 mm battery strap
STRAP_AT = (20.0, 20.0)                 # |x|, |y| of the four slots
# The antenna mount bolts to the MID plate's tail, not the top deck: the
# battery covers the whole top plate, and a countersink deep enough for an
# M3 head is 1.86 mm of a 2 mm plate. From there the stem leans out behind
# the frame, clear of the battery and of both rear propellers.
ANT_MOUNT_X = -47.0
ANT_MOUNT_PITCH = 32.0     # its two screws, across
ANT_MOUNT_W = 40.0         # printed base, in Y
ANT_STEM_W_Y = 16.0        # the leaning stem is narrower than the base
ANT_MOUNT_ANGLE = 45.0     # antenna leans back this far from vertical
ANT_BASE_BACK = -6.0       # the printed base, in its own X
ANT_BASE_FRONT = 6.0
ANT_NOTCH = (-2.0, 10.0, 13.0)  # x from, x to, |y|: the bay the VTX stands in
ANT_BASE_T = 3.0
ANT_STEM_LEN = 24.0
ANT_STEM_W = 6.0

VTX_AT = (-36.0, 0.0)      # VTX centre on the mid plate
RX_AT = (0.0, -30.0)       # beside the stack: the tail is full and the
                           # arm bolts' nuts own the mid plate's corners
BUZZER_AT = (-18.0, 35.0)
CAP_AT = (-49.0, 25.0)     # soldered to the ESC input, lying on the tail
STACK_SPACER_D = 6.0       # nylon spacers in the stack
STACK_LOWER_SPACER = 4.0   # bottom plate -> ESC
STACK_UPPER_SPACER = 8.0   # ESC -> FC: 4 mm of ESC caps under 2.5 of FC

# These are choices, not consequences — they sit here because they describe
# the layout, but nothing computes them, so they are declared like any other
# design dimension.
MID_OPENING = 42.0         # square hole the stack passes through
MID_OPENING_R = 8.0
BATT_X = -11.0             # pack centre: forward of the props, behind the cage
TOP_NOTCH_BACK = 26.0      # the top plate's U-notch, so the camera sees over it
TOP_NOTCH_HALF = 13.0

# --- DERIVED LAYOUT ---------------------------------------------------------
# Written once, evaluated twice. At import it runs over the plain floats above,
# so this module stays importable with no app anywhere. `declare(app)` runs it
# again over Params, so the SAME arithmetic reaches the document as formulas —
# Z_TOP becomes an expression in the plate thicknesses, and thickening the arm
# slides every level above it. vcad's loon bridge does the same thing in two
# passes (crates/vcad-loon/src/params.rs: scan, resolve, rewrite); Python's
# operator overloading lets the second pass BE the first one, run again.
#
# Trig and tuples deliberately fall back to plain numbers: the document's
# expression language is not asked to hold a cosine, and a name that is a
# choice of four (ARM_DIRS) is not a dimension.

def _derive(g):
    g["MOTOR_R"] = g["WHEELBASE"] / 2.0
    g["PROP_R"] = g["PROP_DIAMETER"] / 2.0
    g["ARM_DIRS"] = (45.0, 135.0, 225.0, 315.0)      # front-left is +X+Y
    g["ARM_NAMES"] = ("Arm FL", "Arm RL", "Arm RR", "Arm FR")
    g["MOTOR_NAMES"] = ("Motor FL", "Motor RL", "Motor RR", "Motor FR")
    # Diagonal pairs spin the same way; front-left and rear-right are "props in".
    g["PROP_HAND"] = ("CW", "CCW", "CW", "CCW")

    g["Z_BOTTOM"] = 0.0
    g["Z_ARM"] = g["Z_BOTTOM"] + g["T_BOTTOM"]                     # 3
    g["Z_MID"] = g["Z_ARM"] + g["T_ARM"]                           # 8
    g["Z_MID_TOP"] = g["Z_MID"] + g["T_MID"]                       # 10
    g["Z_TOP"] = g["Z_MID_TOP"] + g["STANDOFF_LEN"]                # 25
    g["Z_TOP_TOP"] = g["Z_TOP"] + g["T_TOP"]                       # 27
    g["Z_MOTOR"] = g["Z_ARM"] + g["T_ARM"]                         # motors sit on the arm top
    g["Z_PROP"] = g["Z_MOTOR"] + g["MOTOR_PROP_SEAT_Z"]            # prop seat, world

    g["Z_ESC"] = g["Z_ARM"] + g["STACK_LOWER_SPACER"]              # ESC PCB underside
    g["Z_FC"] = g["Z_ESC"] + g["ESC_PCB_T"] + g["STACK_UPPER_SPACER"]   # FC PCB underside


    return g


_DERIVED_NAMES = tuple(_derive({k: v for k, v in globals().items()
                                if k.isupper()}).keys() - set(
    k for k in list(globals()) if k.isupper()))
_derive(globals())


def _is_dimension(name, value):
    """A base dimension: NAMED, a length or an angle, and not derived.

    `float`, deliberately, not `(int, float)`. This module already draws the
    line and every one of its 129 dimensions is written with a decimal point;
    the only two integers in it are the two things that are NOT dimensions —
    PROP_BLADES (a count, which `range()` needs as an int) and CELLS (a label,
    which prints as "6S"). Declaring those as variables makes the document
    claim you can have 3.5 blades, and breaks both call sites.
    """
    return (name.isupper() and not name.startswith("_")
            and isinstance(value, float)
            and name not in _DERIVED_NAMES)


def declare(app):
    """Declare every dimension here as a project variable; return the
    namespace rebound to Params.

    The 103 constants in this module are a dimension table — classified,
    commented, and derived rather than transcribed — that reached the document
    only as literals, so the drone opened with nothing to turn. This binds
    them:

        globals().update(spec.declare(app))     # at the top of a builder

    Every use site is unchanged: MOTOR_BOLT_SQ is still a number, it just also
    knows it is called `motor_bolt_sq`. Variable names are the lowercase of
    the constant, which is what a reader of the GUI's variable table expects.
    """
    base = {k: v for k, v in globals().items() if _is_dimension(k, v)}
    declared = app.params({k.lower(): v for k, v in sorted(base.items())},
                          comment="fpv_drone_spec")
    ns = {k: declared[k.lower()] for k in base}
    _derive(ns)
    return ns


def diag(radius, k):
    """The point `radius` out along arm k's axis."""
    a = math.radians(ARM_DIRS[k])
    return (radius * math.cos(a), radius * math.sin(a))


def ant_stem_points():
    """The leaning stem's profile in the mount's XZ: bottom back, bottom
    front, top front, top back. Horizontal where it meets the base, square
    to the lean at the top, which is the face the SMA comes out of."""
    lean = math.radians(ANT_MOUNT_ANGLE)
    s, c = math.sin(lean), math.cos(lean)
    b, w, length = ANT_BASE_BACK, ANT_STEM_W, ANT_STEM_LEN
    front = (b + w - length * s, length * c)
    return ((b, 0.0), (b + w, 0.0), front,
            (front[0] - w * c, front[1] - w * s))


def ant_sma_origin():
    """Where the SMA sits on the antenna mount, in the mount's own space.

    Both the bore and the antenna's placement read this, so the antenna
    cannot end up beside the hole it goes through.
    """
    _, _, front, back = ant_stem_points()
    return ((front[0] + back[0]) / 2, 0.0, (front[1] + back[1]) / 2)


def ant_sma_axis():
    lean = math.radians(ANT_MOUNT_ANGLE)
    return (-math.sin(lean), 0.0, math.cos(lean))


def tangent_point(p, centre, r, sign=1.0):
    """Where a line from `p` touches the circle (centre, r).

    `sign` picks which of the two tangents. Used for the arm: its straight
    edge has to MEET the round motor pad tangentially, and a transcribed
    end point leaves either a step or a sliver at the join.
    """
    dx, dy = centre[0] - p[0], centre[1] - p[1]
    length = math.hypot(dx, dy)
    if length <= r:
        raise ValueError("tangent_point: the point is inside the circle")
    ang = math.atan2(dy, dx) + sign * math.asin(r / length)
    reach = math.sqrt(length * length - r * r)
    return (p[0] + reach * math.cos(ang), p[1] + reach * math.sin(ang))
