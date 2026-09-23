#!/usr/bin/env python3
"""The Bosch PES4M inline injection pump, on paper: every number the build
modules share, and the motion laws the drawn cams and the assembly's
relations are BOTH made from.

A four-element M-size pump as fitted to the OM615 (Mercedes L306D / W115
200 D). None of the reference images is dimensioned, so every value here is
an engineering estimate consistent with an M-size element pump; see
generators/pes_pump/README.md.

**The frame of reference:**

    +X   the camshaft axis. The DRIVE end (timing device) is -X, the
         GOVERNOR end is +X. Element 1 is the drive-end one.
    +Z   up, and every element's axis. The camshaft centreline is z = 0.
    +Y   across the pump. The control rack is on +Y (the inspection-window
         side), the fuel gallery and the feed pump on -Y.

Per-element parts are drawn on their own axis at x = y = 0 with their
heights in pump coordinates, so a part's reported position IS its travel.
"""
import math

# ---- the numbers an M-size pump is quoted by -------------------------------
N_EL = 4
EL_PITCH = 25.0              # element spacing
EL_X = [(i - 1.5) * EL_PITCH for i in range(N_EL)]   # -37.5 .. +37.5
PLUNGER_D = 6.0
STROKE = 7.0                 # cam lift = plunger stroke
RACK_TRAVEL = 15.0           # stop -> full load

# Firing order 1-3-4-2 at the pump's 90-degree intervals: the angle round
# the camshaft at which each element's lobe starts to rise.
FIRING_ORDER = [1, 3, 4, 2]
LOBE_START = {c: 90.0 * FIRING_ORDER.index(c) for c in range(1, N_EL + 1)}
LOBE_DUR = 180.0             # harmonic rise + fall, cam degrees

# ---- camshaft ----------------------------------------------------------------
CAM_BASE_R = 14.0
ROLLER_R = 6.0               # tappet roller
ROLLER_W = 7.6
LOBE_W = 7.0
CAM_ALLOW = 0.05             # the drawn lobe is this far inside the law
PITCH_BASE = CAM_BASE_R + ROLLER_R          # roller centre at zero lift
JOURNAL_D = 17.0
SHAFT_D = 18.0
BEARING_OD = 35.0            # 6203-size ball bearings in the end walls
CAM_X0 = -92.0               # drive-end thread tip
CAM_X1 = 66.0                # governor-end stub

# ---- housing -------------------------------------------------------------
H_X0, H_X1 = -58.0, 58.0     # end walls' outer faces
WALL = 8.0
H_Y0, H_Y1 = -30.0, 30.0     # cam case
H_Z0 = -30.0
UP_Y0, UP_Y1 = -19.0, 24.0   # element block, narrower than the cam case
H_TOP = 122.0
GALLERY = 24.0               # half-size of the cam gallery (lobe tip 21)

# ---- element stack (heights at zero lift, pump coordinates) ----------------
TAPPET_D = 20.0
TAPPET_BORE_D = 20.2
TAPPET_BOT = PITCH_BASE - 4.0      # body bottom (roller hangs 2 below)
TAPPET_TOP = 42.0
SEAT_TOP = 44.0                    # lower spring seat, on the tappet
SPRING_CHAMBER_D = 21.0
SPRING_OD, SPRING_WIRE = 20.0, 2.0
WEB_Z0, WEB_Z1 = 68.0, 72.0        # upper spring seat: a web of the housing
SLEEVE_Z0, SLEEVE_Z1 = 52.0, 86.0
SLEEVE_ID, SLEEVE_OD = 12.2, 15.0
SLOT_TOP = 63.5
VANE_Z0, VANE_Z1 = 52.5, 55.0      # plunger drive vanes
VANE_R, VANE_W = 7.1, 3.0
SLOT_W = 3.6
GEAR_Z0, GEAR_Z1 = 74.0, 80.0      # toothed quadrant / rack face width
RACK_GALLERY_Z0, RACK_GALLERY_Z1 = 72.5, 86.5
BARREL_Z0, BARREL_COLLAR_Z, BARREL_TOP = 63.0, 92.0, 104.0
BARREL_OD, BARREL_COLLAR_D = 12.0, 17.0
PORT_Z, PORT_D = 96.0, 2.0         # inlet / spill port, on the -Y side
PLUNGER_FOOT_Z = TAPPET_TOP + 0.05
PLUNGER_TOP = 94.0                 # at bottom dead centre
SEAT_BODY_Z1 = 113.0               # delivery-valve seat body 104..113
HOLDER_HEX_Z0, HOLDER_HEX_Z1, HOLDER_TOP = H_TOP, 132.0, 146.0
VALVE_LIFT = 1.5
FUEL_GALLERY = (-9.0, PORT_Z)      # (y, z) of the drilled gallery

# ---- control rack & sleeves --------------------------------------------
# A module chosen so the element pitch is a whole number of rack pitches:
# every sleeve then meshes at the same phase and all four quadrants are the
# same part.
RACK_PITCH = EL_PITCH / 8.0                  # 3.125
MODULE = RACK_PITCH / math.pi
GEAR_TEETH = 18
PITCH_R = MODULE * GEAR_TEETH / 2.0          # 8.95
RACK_Y = PITCH_R                             # rack pitch line
RACK_BACK = RACK_Y + 1.25 * MODULE + 5.0     # back face of the rack bar
RACK_X0, RACK_X1 = -50.0, 78.0               # at the stop position
BACKLASH = 0.10

# ---- feed pump ---------------------------------------------------------------
ECC_X = 0.0
ECC_E = 2.5
ECC_R = 13.0                 # eccentric radius
ECC_W = 8.0
FEED_ROLLER_R = 5.0
FEED_STROKE = 2.0 * ECC_E

# ---- governor (pneumatic, diaphragm) ------------------------------------
GOV_X0 = H_X1                # its flange
GOV_D = 96.0

# ---- timing device -----------------------------------------------------------
ADVANCE_MAX = 8.0            # cam degrees


# ---- the laws the cams are cut to AND the relations follow ---------------------

def lobe_lift(cam_deg, start_deg, rise=STROKE, duration=LOBE_DUR):
    """The same harmonic lobe the cam RELATION is given as a table, so the
    drawn profile and the kinematics are one function."""
    f = ((cam_deg - start_deg) % 360.0) / duration
    if f > 1.0:
        return 0.0
    return rise * 0.5 * (1.0 - math.cos(2.0 * math.pi * f))


def plunger_lift(el, cam_deg):
    return lobe_lift(cam_deg, LOBE_START[el])


def rotate_x(y, z, deg):
    """A point of the camshaft, in pump space, after the shaft has turned
    `deg` about +X (right hand)."""
    a = math.radians(deg)
    return (y * math.cos(a) - z * math.sin(a), y * math.sin(a) + z * math.cos(a))


def lobe_profile(el, steps=180):
    """The drawn lobe as (y, z) points about the cam axis: the inner envelope
    of the roller circle carried along the pitch curve. The shaft turned by
    theta brings the point that sat at angle -theta round to the follower
    (straight up), so the pitch point for theta is drawn at (sin, cos)."""
    def pitch(t):
        r = PITCH_BASE + plunger_lift(el, t)
        a = math.radians(t)
        return (r * math.sin(a), r * math.cos(a))
    pts = []
    for i in range(steps):
        t = 360.0 * i / steps
        p0, p1 = pitch(t - 0.05), pitch(t + 0.05)
        ty, tz = p1[0] - p0[0], p1[1] - p0[1]
        n = math.hypot(ty, tz)
        ny, nz = -tz / n, ty / n                   # outward for this sense
        py, pz = pitch(t)
        off = ROLLER_R + CAM_ALLOW
        pts.append((py - ny * off, pz - nz * off))
    return pts


# The control helix: the land between the plunger's top edge and the helix
# grows from LAND_MIN beside the vertical groove to LAND_MAX HELIX_SPAN round.
# The plunger part places it with the helix's own start_deg.
HELIX_SPAN = 100.0                  # degrees of plunger turn the helix covers
LAND_MIN, LAND_MAX = 2.0, 4.5       # land height at the groove / far end


def land_height(load_deg):
    """Top edge to helix edge, as seen by the port with the plunger turned
    `load_deg` from the stop position."""
    f = max(0.0, min(1.0, load_deg / HELIX_SPAN))
    return LAND_MIN + (LAND_MAX - LAND_MIN) * f


def effective_stroke(load_deg):
    """Port closure to spill, mm of plunger travel."""
    closure = PORT_Z + PORT_D / 2.0 - PLUNGER_TOP
    spill = (PORT_Z - PORT_D / 2.0) - PLUNGER_TOP + land_height(load_deg)
    return max(0.0, min(STROKE, spill) - closure)


def valve_window(el):
    """Cam angles over which element `el`'s delivery valve is off its seat
    at FULL LOAD: from port closure (the plunger's top edge covers the port)
    to spill (the helix edge uncovers it). A cam relation cannot also read
    the rack, so the valve keeps this timing at every load."""
    closure = PORT_Z + PORT_D / 2.0 - PLUNGER_TOP
    spill = closure + effective_stroke(math.degrees(RACK_TRAVEL / PITCH_R))

    def cam_at(lift):          # the rising flank reaches `lift`
        f = math.acos(1.0 - 2.0 * lift / STROKE) / (2.0 * math.pi)
        return LOBE_START[el] + f * LOBE_DUR
    return cam_at(closure), cam_at(min(spill, STROKE))


def valve_lift(el, cam_deg):
    a, b = valve_window(el)
    t = ((cam_deg - a) % 360.0) / (b - a)
    if t >= 1.0:
        return 0.0
    return VALVE_LIFT * math.sin(math.pi * t) ** 2


# The eccentric's centre, in the shaft's own frame, before it turns.
ECC_C0 = (0.0, -ECC_E)


def feed_lift(cam_deg):
    """The feed pump's roller follows the eccentric along -Y; exact for a
    circle: its centre sits at the circle's centre projected on -Y plus the
    chord half-length."""
    cy, cz = rotate_x(*ECC_C0, cam_deg)
    rr = ECC_R + FEED_ROLLER_R
    d = -cy + math.sqrt(rr * rr - cz * cz)
    return d - (rr - ECC_E)


def table(fn, step=2.0):
    n = int(round(360.0 / step))
    return [[i * step, round(fn(i * step), 6)] for i in range(n)]
