# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""SO-101 wrist gripper (PG-52) — the numbers, in one place.

Tool frame, and everything in this design is expressed in it:

    Z = the wrist-roll axis, +Z away from the arm (the approach direction).
        Z = 0 is the wrist-roll servo's HORN TOP FACE — the plane the
        gripper bolts down onto.
    X = the direction the jaws close along; the tool is mirror-symmetric
        about X = 0 and the two pads meet there.
    Y = the gripper servo's shaft axis. Its drive horn faces +Y.

Every interface number below was MEASURED off the SO-ARM100 repo's own
STEP files (STEP/SO100/STS3215_03a.step and the SO101 assembly), not
copied from a datasheet — see the project's README for how.
"""
import math

# --- the servo, in its own STEP frame (measured) ---------------------------
# Shaft axis at local (x=12.5, y=0), along local +z.
SERVO = dict(
    shaft_x=12.5,               # shaft axis, local x
    case_x=(-22.7, 22.7),       # long axis, 45.4
    case_y=(-12.4, 12.4),       # 24.8 wide
    case_z=(-16.3, 16.6),       # case, shaft-face at +16.6
    horn_face_z=18.3,           # drive horn top face (1.7 proud of the case)
    horn_dia=20.0,
    horn_hole_pitch=9.9,        # 4x M3 on a 9.9 x 9.9 square
    horn_hole_dia=2.5,          # tapped by the M3 that came with the servo
    horn_centre_dia=5.4,        # the shaft screw, recessed
    idler_face_z=-18.1,         # back (idler) horn face
    idler_boss_z=-18.7,         # small boss beyond it
    conn_z=-17.3,               # the two 3-pin JST connectors...
    conn_x=-1.55,               # ...sit here on the back face
    stall_Nm_6V=1.62,           # 16.5 kgf.cm, the 7.4 V / 1:345 part
)

# --- wrist-roll interface (the same horn, on the arm's wrist servo) --------
MOUNT = dict(
    hole_pitch=9.9,
    hole_dia=3.4,               # M3 clearance
    screw="M3x6 pan head",      # into the horn's 2.5 mm holes; stock part
    centre_relief_dia=6.5,      # over the horn's central screw
    plate_t=3.0,                # floor thickness = screw engagement 3 mm
    horn_clear_dia=21.4,        # the drive plates' bosses journal in this
    case_top_below=1.7,         # the wrist servo's case, below Z=0
    hub_dia=20.0,               # the pad that sits ON the horn
    # The arm's WRIST BRACKET reaches PAST the horn face, into the tool's
    # space: measured off wrist_roll_pitch_so101_v2 posed by the URDF, up
    # to 4.73 mm at radii 12-25 mm, 1.30 mm out to r = 40, and nothing
    # inside r = 10 or beyond r = 40. Since the roll joint turns +-160 deg,
    # every sector of that ring passes under every part of the tool, so
    # the tool's body has to START above it -- only a hub no wider than
    # the horn may come down to the mounting face.
    stack_z=6.0,                # everything but the hub starts here
    bracket_intrusion=4.73,
)

# --- the mechanism ---------------------------------------------------------
OPEN_MAX = 52.0   # pad-to-pad, fully open
PAD_OFFSET = 7.0  # pad face, inboard of the finger's drive pivot A2
A = PAD_OFFSET + OPEN_MAX / 4.0   # = 20: drive-pivot offset in X, and so
#   the gear pitch radius. Putting it at the MIDDLE of the stroke is what
#   makes the sweep symmetric about theta = 0, which is both the best
#   transmission angle and the least travel out of plane.
Z0 = MOUNT["stack_z"]
ZA = Z0 + 42.0  # drive-pivot height (Z): sets where the servo's back lands
D = 14.5        # parallelogram base: coupler pivot is D further out in X.
#   Bounded BELOW by three collisions, all of them checked by the sweep in
#   verify.py: the coupler's B1 pad against the drive plate's hub, the two
#   bars against each other at full sweep, and the B1 post against the hub.
L = 21.5        # link length (both links)
THETA_MAX = math.degrees(math.asin(OPEN_MAX / 4.0 / L))   # +-37.2 deg

# The gear pair: two identical involute sectors, 1:1, meshing on X = 0.
# `backlash_scale` shrinks both profiles a touch, which is the same thing as
# opening the centre distance: 0.15 mm of backlash, which a printed tooth
# needs and a nominal involute pair does not have.
# `phase_quarter_pitch` is what lets ONE plate geometry serve both sides:
# the right-hand plate is the left one MIRRORED, so its teeth land at the
# mirror image of the left's. Draw the teeth a quarter pitch off the line of
# centres and the two mirrored patterns end up exactly half a pitch apart —
# a tooth of one in a space of the other, which is what meshing is.
GEAR = dict(module=2.0, teeth=20, pressure_deg=20.0, face=4.5,
            sector_deg=58.0, backlash_scale=0.995, phase_quarter_pitch=True,
            hub_r=10.5, arm_half=4.5, a2_pad_r=5.5,
            coupler_half=5.2, coupler_pad_r=5.2)

# --- the Y stack (what sits at which Y) ------------------------------------
# Four bands each side. The drive plate and the coupler are on SEPARATE
# bands for a reason the clash sweep found: they are parallel bars 14.5 mm
# apart, and at full sweep that gap closes to 14.5*cos(37.2) = 11.5 mm --
# less than the drive plate's hub, which has to be 10.5 mm to hold the
# servo horn's bolt circle. In one band they would collide; in two they
# never can, and only the pivot post has to thread past the hub.
WALL = (17.0, 20.0)        # chassis side walls, |Y|
PLATE = (20.0, 24.5)       # drive plates, |Y| (bosses reach to 26.2)
COUPLER = (26.4, 28.9)     # coupler (parallel-keeper) plates, |Y|
EAR = (29.3, 33.8)         # finger ears, |Y|
PLATE_MID = (PLATE[0] + PLATE[1]) / 2.0
COUPLER_MID = (COUPLER[0] + COUPLER[1]) / 2.0
BOSS_H = 1.7               # link-plate boss, reaching in to the horn face
PLATE_T = 4.5
COUPLER_T = 2.5
EAR_T = 4.5

# --- the camera ------------------------------------------------------------
CAM = dict(
    board=32.0, hole_pitch=27.0, hole_dia=2.2,   # 32x32 UVC module, 4x M2
    pcb_t=1.6, back_t=5.0, lens_dia=14.0, lens_len=13.0,
    centre=(16.0, 0.0, 27.0),    # board centre in tool coords
    tilt_deg=14.0,               # toward the tool centreline
    pod_base_z=12.0,             # the pedestal it stands on
    pocket=24.0,                 # recess for the components behind the PCB
    fov_deg=70.0,
)

# --- derived ---------------------------------------------------------------
def theta_for_opening(opening_mm):
    """Link angle (deg, from +Z, positive = closing) for a pad gap."""
    a2x = -PAD_OFFSET - opening_mm / 2.0        # left finger's A2, in X
    s = (a2x + A) / L
    return math.degrees(math.asin(max(-1.0, min(1.0, s))))


def opening_for_theta(theta_deg):
    a2x = -A + L * math.sin(math.radians(theta_deg))
    return -2.0 * (a2x + PAD_OFFSET)


def grip_force_N(theta_deg, torque_Nm=None, efficiency=0.75):
    """Pad force, from servo torque through the parallelogram.

    Half the servo's torque reaches each finger (the gear pair splits it);
    the finger is a translating stage, so F * d(x)/d(theta) = T.
    """
    T = SERVO["stall_Nm_6V"] if torque_Nm is None else torque_Nm
    return efficiency * (T / 2.0) / (L * math.cos(math.radians(theta_deg))
                                     / 1000.0)


THETA_CLOSED = theta_for_opening(0.0)
THETA_OPEN = theta_for_opening(OPEN_MAX)

if __name__ == "__main__":
    print(f"theta closed {THETA_CLOSED:+.2f} deg, open {THETA_OPEN:+.2f} deg,"
          f" sweep {THETA_CLOSED - THETA_OPEN:.1f} deg")
    for o in (0, 13, 26, 39, 52):
        t = theta_for_opening(o)
        print(f"  opening {o:5.1f} mm  theta {t:+6.2f}  grip {grip_force_N(t):5.1f} N"
              f"  (stall)  {grip_force_N(t, 0.3*SERVO['stall_Nm_6V']):4.1f} N at 30%")
    print("Z drift over the stroke:",
          round(L * (1 - math.cos(math.radians(THETA_MAX))), 2), "mm")
