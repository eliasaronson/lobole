"""The parts of the 5" FPV quad: carbon plates, hardware, electronics.

Every builder takes the connected app and returns the Part. Nothing here
places anything — the assembly (fpv_drone.py) does that, and mates through
the named frames each builder leaves behind.

Carbon is `carbon_fibre` (1550 kg/m3), which the material table grew for
this: modelled as black plastic a plate read a third light, which is a
third of an airframe's structural mass.
"""
import math

from designer_sdk import Plane

from fpv_drone_spec import *  # noqa: F401,F403  — the dimensions, by name

CF = "carbon_fibre"


# --- profile helpers --------------------------------------------------------

def _unit(v):
    n = math.hypot(v[0], v[1])
    return (v[0] / n, v[1] / n)


def corner(a, b, c, r):
    """The fillet at vertex b between edges b->a and b->c.

    Returns (tangent_in, arc_mid, tangent_out). Works for a reflex corner
    too — the bisector points into the material either way, which is what
    the camera plate's tab shoulders need.
    """
    u1, u2 = _unit((a[0] - b[0], a[1] - b[1])), _unit((c[0] - b[0], c[1] - b[1]))
    dot = max(-1.0, min(1.0, u1[0] * u2[0] + u1[1] * u2[1]))
    ang = math.acos(dot)
    if ang < 1e-6 or abs(ang - math.pi) < 1e-6:
        return b, None, b
    t = r / math.tan(ang / 2.0)
    t1 = (b[0] + u1[0] * t, b[1] + u1[1] * t)
    t2 = (b[0] + u2[0] * t, b[1] + u2[1] * t)
    bis = _unit((u1[0] + u2[0], u1[1] + u2[1]))
    d = r / math.sin(ang / 2.0)
    ctr = (b[0] + bis[0] * d, b[1] + bis[1] * d)
    back = _unit((b[0] - ctr[0], b[1] - ctr[1]))
    return t1, (ctr[0] + back[0] * r, ctr[1] + back[1] * r), t2


def rounded_poly(sk, verts, arcs=None):
    """A closed profile of straight edges with filleted corners.

    `verts` is [(x, y, radius), ...]; radius 0 leaves the corner sharp.
    `arcs` maps a vertex index to a point the edge LEAVING it should bow
    through, which is how the arm's straight taper meets its round motor
    pad without a transcribed end point.
    """
    arcs = arcs or {}
    n = len(verts)
    ins, outs, mids = [], [], []
    for i, (x, y, r) in enumerate(verts):
        b = (x, y)
        if r <= 0:
            ins.append(b), outs.append(b), mids.append(None)
            continue
        t1, m, t2 = corner(verts[i - 1][:2], b, verts[(i + 1) % n][:2], r)
        ins.append(t1), outs.append(t2), mids.append(m)
    for i in range(n):
        if mids[i] is not None:
            sk.arc_through(ins[i], mids[i], outs[i])
        j = (i + 1) % n
        if i in arcs:
            sk.arc_through(outs[i], arcs[i], ins[j])
        else:
            sk.line(outs[i], ins[j])
    return sk


def plate_outline(sk, length, width, radius):
    return rounded_poly(sk, [(length / 2, -width / 2, radius),
                             (length / 2, width / 2, radius),
                             (-length / 2, width / 2, radius),
                             (-length / 2, -width / 2, radius)])


def arm_bolt_points():
    """The eight arm-bolt centres, on the plates, in world XY."""
    return [diag(r, k) for k in range(4) for r in (ARM_BOLT_IN, ARM_BOLT_OUT)]


def stack_points():
    h = STACK_PATTERN / 2.0
    return [(sx * h, sy * h) for sx in (1, -1) for sy in (1, -1)]


def _slots(p, name, rects, z_from, depth):
    """Cut rectangular slots (each (cx, cy, w, h)) straight down."""
    sk = p.sketch(Plane.xy(z_from), name)
    for cx, cy, w, h in rects:
        sk.centered_rect((cx, cy), w, h)
    return p.cut(sk.add(), depth, name=name)


# --- carbon plates ----------------------------------------------------------

def bottom_plate(app):
    """3 mm: the structural plate. Arms bolt on top of it, stack on top of
    that, and it is what hits the ground."""
    p = app.part("Bottom plate")
    sk = p.sketch(Plane.xy(0), "Outline")
    plate_outline(sk, PLATE_L, PLATE_W, PLATE_R)
    p.extrude(sk.add(), T_BOTTOM, name="Bottom plate")

    sk = p.sketch(Plane.xy(T_BOTTOM), "M3 hole centres")
    for x, y in arm_bolt_points() + stack_points():
        sk.point((x, y))
    p.hole(sk.add(), d=M3_CLEAR, through=True,
           fit={"thread": "M3", "kind": "close"}, name="M3 clearance holes")

    # Zip-tie slots for the motor leads and the battery lead's strain relief.
    _slots(p, "Zip-tie slots",
           [(x, y, 2.5, 9.0) for x in (30.0, -30.0) for y in (38.0, -38.0)]
           + [(-49.0, 0.0, 4.0, 14.0)],
           -1.0, T_BOTTOM + 2.0)

    p.frame("Origin", origin=(0, 0, 0))
    p.frame("Top face", origin=(0, 0, T_BOTTOM))
    return p.material(CF).check(solids=1)


def mid_plate(app):
    """2 mm: clamps the arms, carries the camera cage and the VTX. Its
    central opening is what the stack stands up through."""
    p = app.part("Mid plate")
    sk = p.sketch(Plane.xy(0), "Outline")
    plate_outline(sk, PLATE_L, PLATE_W, PLATE_R)
    p.extrude(sk.add(), T_MID, name="Mid plate")

    sk = p.sketch(Plane.xy(-1.0), "Stack opening")
    h = MID_OPENING / 2.0
    rounded_poly(sk, [(h, -h, MID_OPENING_R), (h, h, MID_OPENING_R),
                      (-h, h, MID_OPENING_R), (-h, -h, MID_OPENING_R)])
    p.cut(sk.add(), T_MID + 2.0, name="Stack opening")

    sk = p.sketch(Plane.xy(T_MID), "M3 hole centres")
    for x, y in arm_bolt_points():
        sk.point((x, y))
    p.hole(sk.add(), d=M3_CLEAR, through=True,
           fit={"thread": "M3", "kind": "close"}, name="Arm bolt holes")

    sk = p.sketch(Plane.xy(T_MID), "Antenna mount screw centres")
    for sy in (1, -1):
        sk.point((ANT_MOUNT_X, sy * ANT_MOUNT_PITCH / 2))
    p.hole(sk.add(), d=M3_CLEAR, through=True,
           fit={"thread": "M3", "kind": "close"}, name="Antenna mount holes")

    sk = p.sketch(Plane.xy(T_MID), "M2 hole centres")
    for sx in (1, -1):
        for sy in (1, -1):
            sk.point((VTX_AT[0] + sx * VTX_PATTERN / 2,
                      VTX_AT[1] + sy * VTX_PATTERN / 2))
    p.hole(sk.add(), d=M2_CLEAR, through=True,
           fit={"thread": "M2", "kind": "close"}, name="VTX holes")

    # The camera plates drop through these. 0.2 mm of fit across the plate;
    # along it, the slot is 1.2 mm longer than the tab at each end, which is
    # where the tab's root radius (and a cutter's) has to go.
    y = CAM_MOUNT_GAP / 2.0 + T_SIDE / 2.0
    cx = (CAGE_TAB[0] + CAGE_TAB[1]) / 2.0
    length = CAGE_TAB[1] - CAGE_TAB[0] + 2 * CAGE_TAB_RELIEF
    _slots(p, "Camera plate slots",
           [(cx, sy * y, length, T_SIDE + 0.2) for sy in (1, -1)],
           -1.0, T_MID + 2.0)
    _slots(p, "Zip-tie slots",
           [(x, y2, 2.5, 9.0) for x in (-30.0,) for y2 in (38.0, -38.0)],
           -1.0, T_MID + 2.0)

    p.frame("Origin", origin=(0, 0, 0))
    p.frame("RX seat", origin=(RX_AT[0], RX_AT[1], T_MID))
    p.frame("Cap seat", origin=(CAP_AT[0], CAP_AT[1], T_MID + CAP_D / 2))
    p.frame("Buzzer seat", origin=(BUZZER_AT[0], BUZZER_AT[1], T_MID))
    return p.material(CF).check(solids=1)


def top_plate(app):
    """2 mm: the battery deck. Forward-open forks let it drop over the
    camera plates; it stays under the propeller disc."""
    p = app.part("Top plate")
    sk = p.sketch(Plane.xy(0), "Outline")
    plate_outline(sk, TOP_L, TOP_W, PLATE_R)
    p.extrude(sk.add(), T_TOP, name="Top plate")

    sk = p.sketch(Plane.xy(T_TOP), "M3 hole centres")
    for k in range(4):
        sk.point(diag(ARM_BOLT_OUT, k))
    p.hole(sk.add(), d=M3_CLEAR, through=True,
           fit={"thread": "M3", "kind": "close"}, name="Standoff holes")

    # The camera and its plates rise through this notch; the notch is what
    # lets the camera be taller than the 15 mm between the decks.
    notch_front = TOP_L / 2.0 + 4.0
    _slots(p, "Camera notch",
           [((TOP_NOTCH_BACK + notch_front) / 2, 0.0,
             notch_front - TOP_NOTCH_BACK, 2 * TOP_NOTCH_HALF)],
           -1.0, T_TOP + 2.0)
    _slots(p, "Battery strap slots",
           [(sx * STRAP_AT[0], sy * STRAP_AT[1], STRAP_SLOT_L, STRAP_SLOT_W)
            for sx in (1, -1) for sy in (1, -1)],
           -1.0, T_TOP + 2.0)

    p.frame("Origin", origin=(0, 0, 0))
    p.frame("Battery seat", origin=(BATT_X, 0, T_TOP))
    return p.material(CF).check(solids=1)


def arm(app):
    """5 mm, one part, four placements. Modelled in the frame's own polar
    space: +X is the arm's own axis, so the occurrence is a rotation about
    Z and the motor lands at the wheelbase with no coordinate typed twice.
    """
    p = app.part("Arm")
    pad = (MOTOR_R, 0.0)
    waist_hi = (ARM_WAIST_R, ARM_WAIST_HW)
    t_hi = tangent_point(waist_hi, pad, ARM_PAD_R, +1.0)
    t_lo = (t_hi[0], -t_hi[1])
    sk = p.sketch(Plane.xy(0), "Arm outline")
    rounded_poly(sk, [
        (ARM_ROOT_R, -ARM_ROOT_HW, 3.0),
        (ARM_WAIST_R, -ARM_WAIST_HW, 40.0),
        (t_lo[0], t_lo[1], 0.0),
        (t_hi[0], t_hi[1], 0.0),
        (ARM_WAIST_R, ARM_WAIST_HW, 40.0),
        (ARM_ROOT_R, ARM_ROOT_HW, 3.0),
    ], arcs={2: (MOTOR_R + ARM_PAD_R, 0.0)})
    p.extrude(sk.add(), T_ARM, name="Arm")

    sk = p.sketch(Plane.xy(T_ARM), "M3 hole centres")
    sk.point((ARM_BOLT_IN, 0.0))
    sk.point((ARM_BOLT_OUT, 0.0))
    for sx in (1, -1):
        for sy in (1, -1):
            sk.point((MOTOR_R + sx * MOTOR_BOLT_SQ / 2, sy * MOTOR_BOLT_SQ / 2))
    p.hole(sk.add(), d=M3_CLEAR, through=True,
           fit={"thread": "M3", "kind": "close"}, name="M3 clearance holes")

    sk = p.sketch(Plane.xy(T_ARM), "Shaft clearance")
    sk.point(pad)
    p.hole(sk.add(), d=ARM_SHAFT_CLEAR_D, through=True, name="Shaft clearance")

    p.frame("Root", origin=(0, 0, 0))
    p.frame("Motor seat", origin=(MOTOR_R, 0, T_ARM))
    return p.material(CF).check(solids=1)


def camera_plate(app):
    """2 mm, vertical, two of them. A flat plate mirrored through a plane
    parallel to itself is the SAME plate, so left and right are one part
    and the difference is a translation.
    """
    p = app.part("Camera plate")
    x0, x1 = CAGE_TAB
    verts = [
        (CAGE_BACK_X, Z_MID_TOP, 2.0),
        (x0, Z_MID_TOP, 1.0), (x0, Z_ARM, 1.0),      # down through the slot
        (x1, Z_ARM, 1.0), (x1, Z_MID_TOP, 1.0),      # ... and back up
        (x1 + 5.0, Z_MID_TOP, 2.0),
        (CAGE_FRONT_X, 16.0, 3.0), (CAGE_FRONT_X, 28.0, 3.0),
        (50.0, CAGE_TOP_Z, 3.0), (32.0, CAGE_TOP_Z, 3.0),
        (CAGE_BACK_X, 28.0, 3.0),
    ]
    sk = p.sketch(Plane.xz(0.0), "Camera plate outline")
    rounded_poly(sk, verts)
    p.extrude(sk.add(), T_SIDE, name="Camera plate")

    sk = p.sketch(Plane.xz(-T_SIDE), "Hole centres")
    sk.point(CAM_PIVOT)
    p.hole(sk.add(), d=M2_CLEAR, through=True,
           fit={"thread": "M2", "kind": "close"}, name="Camera pivot hole")
    sk = p.sketch(Plane.xz(-T_SIDE), "Lock hole centre")
    sk.point(CAGE_LOCK)
    p.hole(sk.add(), d=M2_CLEAR, through=True,
           fit={"thread": "M2", "kind": "close"}, name="Lock screw hole")

    p.frame("Origin", origin=(0, 0, 0))
    return p.material(CF).check(solids=1)


# --- hardware ---------------------------------------------------------------

def standoff(app):
    """M3 x 15 aluminium hex standoff: the arm bolt threads into its bottom,
    the top plate screw into its top."""
    p = app.part(f"Standoff M3x{STANDOFF_LEN:.0f}")
    sk = p.sketch(Plane.xy(0), "Hex")
    sk.polygon((0, 0), 6, across_flats=STANDOFF_AF)
    p.extrude(sk.add(), STANDOFF_LEN, name="Standoff")
    for z, flip in ((STANDOFF_LEN, False), (0.0, True)):
        sk = p.sketch(Plane.xy(z, flip=flip), f"Tap centre {z:.0f}")
        sk.point((0, 0))
        p.hole(sk.add(), d=2.5, depth=STANDOFF_THREAD_DEPTH,
               fit={"thread": "M3", "kind": "tapped"},
               name=f"M3 tapped {'top' if z else 'bottom'}")
    for face in p.faces(kind="cylinder"):
        p.thread(face, "M3", allow_engagement=True, minimum_engagement=4.0)
    p.frame("Bottom", origin=(0, 0, 0))
    p.frame("Top", origin=(0, 0, STANDOFF_LEN))
    return p.material("aluminum").check(solids=1)


def tapped_spacer(app, name, d, length, thread, tap_d, depth,
                  material="aluminum"):
    """A female-female standoff: a screw into each end, no nut anywhere."""
    p = app.part(name)
    sk = p.sketch(Plane.xy(0), "Outline")
    sk.circle((0, 0), d=d)
    p.extrude(sk.add(), length, name=name)
    for z, flip in ((length, False), (0.0, True)):
        sk = p.sketch(Plane.xy(z, flip=flip), f"Tap {z:.0f}")
        sk.point((0, 0))
        p.hole(sk.add(), d=tap_d, depth=depth,
               fit={"thread": thread, "kind": "tapped"},
               name=f"{thread} tapped {'top' if z else 'bottom'}")
    for face in p.faces(kind="cylinder", radius=tap_d / 2):
        p.thread(face, thread, allow_engagement=True, minimum_engagement=2.0)
    p.frame("Bottom", origin=(0, 0, 0))
    p.frame("Top", origin=(0, 0, length))
    return p.material(material).check(solids=1)


def spacer(app, name, d, length, bore, material="plastic_white"):
    p = app.part(name)
    sk = p.sketch(Plane.xy(0), "Outline")
    sk.circle((0, 0), d=d)
    p.extrude(sk.add(), length, name=name)
    sk = p.sketch(Plane.xy(length), "Bore")
    sk.point((0, 0))
    p.hole(sk.add(), d=bore, through=True, name="Bore")
    p.frame("Bottom", origin=(0, 0, 0))
    return p.material(material).check(solids=1)


# --- printed parts ----------------------------------------------------------

def antenna_mount(app):
    """TPU, bolted UNDER the top plate's tail so the battery sits on the
    plate undisturbed, with the stem leaning back out behind the frame.
    Modelled with the base's underside at z = 0.

    On the centreline on purpose: an antenna set out to one side at the
    rear is inside the rear propeller's disc — a shredded antenna and a
    lost video feed, not a styling choice.
    """
    p = app.part("Antenna mount")
    back, front, t = ANT_BASE_BACK, ANT_BASE_FRONT, ANT_BASE_T
    sk = p.sketch(Plane.xy(0), "Base")
    rounded_poly(sk, [(front, -ANT_MOUNT_W / 2, 3.0),
                      (front, ANT_MOUNT_W / 2, 3.0),
                      (back, ANT_MOUNT_W / 2, 3.0),
                      (back, -ANT_MOUNT_W / 2, 3.0)])
    p.extrude(sk.add(), t, name="Base")
    # A U, not a plate: the video transmitter's spacers stand where the
    # middle would be, so the base reaches round them to its two screws.
    x0, x1, half = ANT_NOTCH
    _slots(p, "VTX bay", [((x0 + x1) / 2, 0.0, x1 - x0, 2 * half)],
           -1.0, t + 2.0)

    bb, bf, tf, tb = ant_stem_points()
    sk = p.sketch(Plane.xz(ANT_STEM_W_Y / 2), "Stem")
    rounded_poly(sk, [(bb[0], bb[1], 0.0), (bf[0], bf[1], 0.0),
                      (tf[0], tf[1], 2.0), (tb[0], tb[1], 2.0)])
    p.extrude(sk.add(), ANT_STEM_W_Y, mode="add", name="Stem")

    sk = p.sketch(Plane.xy(t), "Screw centres")
    for sy in (1, -1):
        sk.point((0.0, sy * ANT_MOUNT_PITCH / 2))
    p.hole(sk.add(), d=M3_CLEAR, through=True,
           fit={"thread": "M3", "kind": "normal"}, name="Mount screw holes")

    # The SMA bore runs up the stem, so it is an angled cylinder cut.
    axis = ant_sma_axis()
    o = ant_sma_origin()
    # The bore stands on a frame 20 down the SMA's own axis, so it starts
    # inside the mount and opens through the stem's face.
    p.frame("Origin", origin=(0, 0, 0))
    p.frame("SMA", origin=o, z=axis, x=(axis[2], 0, -axis[0]))
    start = p.frame("SMA bore start",
                    origin=(o[0] - axis[0] * 20, o[1], o[2] - axis[2] * 20),
                    z=axis, x=(axis[2], 0, -axis[0]))
    p.cylinder(start, d=ANT_BORE_D, height=26.0, mode="cut", name="SMA bore")
    return p.material("rubber").check(solids=1)


def battery_pad(app):
    """TPU: a grippy pad between the battery and the top plate, printed
    with the strap slots clear."""
    p = app.part("Battery pad")
    sk = p.sketch(Plane.xy(0), "Outline")
    plate_outline(sk, BATT_L, BATT_W, 6.0)
    p.extrude(sk.add(), 3.0, name="Battery pad")
    p.frame("Origin", origin=(0, 0, 0))
    p.frame("Pack seat", origin=(0, 0, 3.0))
    return p.material("rubber").check(solids=1)


# --- electronics ------------------------------------------------------------

def _pcb(app, name, board, thick, pattern, hole_d, top_h, bottom_h,
         top_w=None, bottom_w=None, corner=4.0, fit="M3"):
    """A board with its mounting pattern, and the component envelopes above
    and below it — which is what a clearance check actually needs."""
    p = app.part(name)
    sk = p.sketch(Plane.xy(0), "Board")
    plate_outline(sk, board, board, corner)
    p.extrude(sk.add(), thick, name="PCB")
    sk = p.sketch(Plane.xy(thick), "Mount holes")
    for sx in (1, -1):
        for sy in (1, -1):
            sk.point((sx * pattern / 2, sy * pattern / 2))
    # Recorded as the fit it IS, not just a diameter: 2.2 mm read backwards
    # is both an M1.8 loose and an M2 close clearance, and a screw fitted to
    # it is refused rather than guessed.
    p.hole(sk.add(), d=hole_d, through=True,
           fit={"thread": fit, "kind": "close"}, name="Mounting holes")
    if top_h:
        sk = p.sketch(Plane.xy(thick), "Components top")
        w = top_w or board - 8.0
        plate_outline(sk, w, w, 2.0)
        p.extrude(sk.add(), top_h, mode="add", name="Components top")
    if bottom_h:
        sk = p.sketch(Plane.xy(-bottom_h), "Components bottom")
        w = bottom_w or board - 12.0
        plate_outline(sk, w, w, 2.0)
        p.extrude(sk.add(), bottom_h, mode="add", name="Components bottom")
    p.frame("Origin", origin=(0, 0, 0))
    return p.material("plastic_black").check(solids=1)


def flight_controller(app):
    # 24 mm of components, not 28: the stack nuts land at 30.5/2 either way
    # and a 28 mm envelope reaches under them.
    p = _pcb(app, "Flight controller F722", STACK_BOARD, FC_PCB_T,
             STACK_PATTERN, STACK_HOLE_D, FC_TOP_H, FC_BOTTOM_H, top_w=24.0)
    sk = p.sketch(Plane.yz(-STACK_BOARD / 2 - 4.0), "USB-C")
    sk.centered_rect((0.0, FC_PCB_T + 1.75), 9.0, 3.5)
    p.extrude(sk.add(), 8.0, mode="add", name="USB-C")
    return p.check(solids=1)


def esc(app):
    return _pcb(app, "ESC 4-in-1 55A", STACK_BOARD, ESC_PCB_T, STACK_PATTERN,
                STACK_HOLE_D, ESC_TOP_H, ESC_BOTTOM_H, top_w=24.0)


def vtx(app):
    return _pcb(app, "VTX 5.8G 800mW", VTX_BOARD, VTX_PCB_T, VTX_PATTERN,
                VTX_HOLE_D, VTX_TOP_H, 0.0, top_w=18.0, corner=2.0, fit="M2")


def camera(app):
    """19 mm micro camera. Modelled about its TILT AXIS, so the occurrence
    is placed at the pivot and rotated by the tilt angle."""
    p = app.part("FPV camera 19mm")
    sk = p.sketch(Plane.yz(-CAM_DEPTH / 2 - 2.0), "Body")
    plate_outline(sk, CAM_BODY, CAM_BODY, 2.0)
    p.extrude(sk.add(), CAM_DEPTH, name="Body")
    sk = p.sketch(Plane.yz(CAM_DEPTH / 2 - 2.0), "Lens")
    sk.circle((0, 0), d=CAM_LENS_D)
    p.extrude(sk.add(), CAM_LENS_L, mode="add", name="Lens")
    # A 19 mm camera hangs on its two M2 tilt screws, one into each side,
    # and those holes are THREADED in the camera — which is also why the
    # screws are short: nothing spans the body. Modelling the body solid put
    # the screws through solid plastic; modelling one through-bolt instead
    # asked for an M2x25, which is not made.
    for sy in (-1, 1):
        plane = Plane((0.0, sy * CAM_BODY / 2, 0.0), (0.0, sy, 0.0),
                      (1.0, 0.0, 0.0))
        sk = p.sketch(plane, f"Tilt tap {sy}")
        sk.point((0, 0))
        p.hole(sk.add(), d=1.6, depth=4.0,
               fit={"thread": "M2", "kind": "tapped"},
               name=f"M2 tilt tap {'L' if sy > 0 else 'R'}")
    for face in p.faces(kind="cylinder", radius=0.8):
        p.thread(face, "M2", allow_engagement=True, minimum_engagement=2.0)
    p.frame("Pivot", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    return p.material("plastic_black").check(solids=1)


def receiver(app):
    p = app.part("Receiver ELRS 2.4G")
    p.box("Top", width=RX_L, depth=RX_W, height=RX_H, name="Receiver")
    p.frame("Origin", origin=(0, 0, 0))
    return p.material("plastic_black").check(solids=1)


def battery(app):
    p = app.part(f"Battery {CELLS}S 1300mAh")
    sk = p.sketch(Plane.xy(0), "Pack")
    plate_outline(sk, BATT_L, BATT_W, 4.0)
    p.extrude(sk.add(), BATT_H, name="Pack")
    # The lead comes out the BACK, which is where the frame's tail and the
    # antenna are; out the front it is 14 mm into the camera cage.
    sk = p.sketch(Plane.yz(-BATT_L / 2 - XT60_L + 2.0), "XT60")
    sk.centered_rect((0.0, 6.0 + XT60_H / 2), XT60_W, XT60_H)
    p.extrude(sk.add(), XT60_L, mode="add", name="XT60")
    p.frame("Origin", origin=(0, 0, 0))
    return p.material("plastic_black").check(solids=1)


def capacitor(app):
    p = app.part("Capacitor 35V 1000uF")
    p.cylinder("Right", d=CAP_D, height=CAP_H, name="Can")
    p.frame("Origin", origin=(0, 0, 0))
    return p.material("aluminum").check(solids=1)


def buzzer(app):
    p = app.part("Buzzer 5V")
    p.cylinder("Top", d=BUZZER_D, height=BUZZER_H, name="Buzzer")
    p.frame("Origin", origin=(0, 0, 0))
    return p.material("plastic_black").check(solids=1)


def antenna(app):
    """RHCP 5.8 GHz on an SMA bulkhead, modelled about the FACE it bolts
    to: the shank runs down into the mount's bore, the nut and the radome
    stand above it."""
    p = app.part("Antenna RHCP SMA")
    sk = p.sketch(Plane.xy(-10.0), "SMA shank")
    sk.circle((0, 0), d=ANT_CONNECTOR_D)
    p.extrude(sk.add(), 12.0, name="SMA shank")
    sk = p.sketch(Plane.xy(0.0), "SMA nut")
    sk.polygon((0, 0), 6, across_flats=8.0)
    p.extrude(sk.add(), 4.0, mode="add", name="SMA nut")
    sk = p.sketch(Plane.xy(4.0), "Radome")
    sk.circle((0, 0), d=ANT_TUBE_D)
    p.extrude(sk.add(), ANT_TUBE_L, mode="add", name="Radome")
    p.frame("Face", origin=(0, 0, 0))
    return p.material("plastic_black").check(solids=1)


def motor(app):
    """2207 outrunner, modelled about its MOUNT FACE at z = 0: base plate,
    stator, bell and the M5 prop shaft, with the four M3 tapped holes the
    arm screws actually thread into."""
    p = app.part("Motor 2207 1750KV")
    bell_z = MOTOR_PROP_SEAT_Z - MOTOR_BELL_H
    for z, d, h, mode, name in (
            (0.0, MOTOR_BASE_D, MOTOR_BASE_T, "new_body", "Base"),
            (MOTOR_BASE_T, MOTOR_STATOR_D, MOTOR_STATOR_H, "add", "Stator"),
            (bell_z, MOTOR_BELL_D, MOTOR_BELL_H, "add", "Bell"),
            (MOTOR_PROP_SEAT_Z, MOTOR_SHAFT_D,
             MOTOR_SHAFT_TOP - MOTOR_PROP_SEAT_Z, "add", "Shaft")):
        sk = p.sketch(Plane.xy(z), name)
        sk.circle((0, 0), d=d)
        p.extrude(sk.add(), h, mode=mode, name=name)

    sk = p.sketch(Plane.xy(0, flip=True), "Mount tap centres")
    for sx in (1, -1):
        for sy in (1, -1):
            sk.point((sx * MOTOR_BOLT_SQ / 2, sy * MOTOR_BOLT_SQ / 2))
    p.hole(sk.add(), d=2.5, depth=MOTOR_BOLT_DEPTH,
           fit={"thread": "M3", "kind": "tapped"}, name="M3 mounting taps")
    for face in p.faces(kind="cylinder", radius=1.25):
        p.thread(face, "M3", allow_engagement=True, minimum_engagement=3.0)
    for face in p.faces(kind="cylinder", radius=MOTOR_SHAFT_D / 2):
        p.thread(face, "M5", allow_engagement=True, minimum_engagement=3.0)
    p.frame("Mount face", origin=(0, 0, 0))
    p.frame("Prop seat", origin=(0, 0, MOTOR_PROP_SEAT_Z))
    return p.material("aluminum").check(solids=1)


def prop_nut(app):
    p = app.part("Prop nut M5")
    sk = p.sketch(Plane.xy(0), "Flange")
    sk.circle((0, 0), d=11.0)
    p.extrude(sk.add(), 1.5, name="Flange")
    sk = p.sketch(Plane.xy(1.5), "Hex")
    sk.polygon((0, 0), 6, across_flats=PROP_NUT_AF)
    p.extrude(sk.add(), PROP_NUT_H - 1.5, mode="add", name="Hex")
    sk = p.sketch(Plane.xy(PROP_NUT_H), "Bore")
    sk.point((0, 0))
    p.hole(sk.add(), d=4.2, through=True,
           fit={"thread": "M5", "kind": "tapped"}, name="M5 thread")
    for face in p.faces(kind="cylinder", radius=2.1):
        p.thread(face, "M5", allow_engagement=True, minimum_engagement=3.0)
    p.frame("Seat", origin=(0, 0, 0))
    return p.material("aluminum").check(solids=1)


# --- propeller --------------------------------------------------------------

# A Clark-Y-ish section, normalised: (chordwise 0..1, thickness as a
# fraction of the section's own thickness ratio). One shape, scaled per
# station, is what makes the blade a loft rather than a stack of guesses.
AIRFOIL = [(0.0, 0.0), (0.03, 0.35), (0.10, 0.72), (0.25, 1.0), (0.45, 0.92),
           (0.65, 0.68), (0.85, 0.36), (1.0, 0.0), (0.85, -0.06),
           (0.65, -0.12), (0.45, -0.15), (0.25, -0.15), (0.10, -0.10),
           (0.03, -0.05)]

# (radius, chord, twist deg, thickness ratio) — twist from the 4.3" pitch,
# capped inboard where a true helical angle would fold the section over.
STATIONS = [(5.5, 8.0, 42.0, 0.14), (12.0, 11.5, 42.0, 0.12),
            (20.0, 14.0, 41.0, 0.10), (30.0, 14.5, 30.1, 0.085),
            (42.0, 13.5, 22.5, 0.07), (54.0, 11.0, 17.8, 0.055),
            (62.0, 7.5, 15.6, 0.05), (64.5, 2.5, 15.0, 0.05)]


def propeller(app, hand):
    """5.1 x 4.3 x 3. Three lofted blades on planes whose normal is the
    blade's own radius, so the twist is the section's, not a shear.

    `hand` is "CW" or "CCW" seen from above with the shaft coming toward
    you — the two are mirror images and a quad needs both.
    """
    # The airfoil's leading edge sits at -u, so a blade built on the
    # counter-clockwise tangent moves CLOCKWISE seen from above. Mirroring
    # is done on the section's own chord axis (u), never on the plane: flip
    # the plane and its V axis flips with it, which pushed one prop's blades
    # below its hub and into the motor bell.
    sign = -1.0 if hand == "CCW" else 1.0
    p = app.part(f"Propeller 5.1x4.3x3 {hand}")
    sk = p.sketch(Plane.xy(0), "Hub")
    sk.circle((0, 0), d=PROP_HUB_D)
    hub = p.extrude(sk.add(), PROP_HUB_H, name="Hub")

    blades = []
    for b in range(PROP_BLADES):
        phi = math.radians(120.0 * b)
        cs, sn = math.cos(phi), math.sin(phi)
        sketches = []
        for radius, chord, twist, thick in STATIONS:
            plane = Plane((radius * cs, radius * sn, 0.0), (cs, sn, 0.0),
                          (-sn, cs, 0.0))
            s = p.sketch(plane, f"{hand} blade {b + 1} r{radius:g}")
            th = math.radians(twist)
            pts = []
            for a, t in AIRFOIL:
                u0 = (a - 0.25) * chord            # 0 = leading edge
                v0 = t * thick * chord
                u = u0 * math.cos(th) - v0 * math.sin(th)
                v = u0 * math.sin(th) + v0 * math.cos(th)
                # The mirror is on the chord axis alone: v (and so the
                # suction side, and so which way the thrust points) is the
                # same for both hands.
                pts.append((u * sign, v + PROP_HUB_H / 2 + 0.7))
            s.polyline(pts, close=True)
            sketches.append(s.add())
        loft = p._add({"type": "Loft", "name": f"{hand} blade {b + 1}",
                       "mode": "new_body",
                       "sketch_ids": [s.id for s in sketches]})
        blades.append(loft)

    target = hub.created_by
    for i, loft in enumerate(blades):
        r = p._add({"type": "Boolean", "name": f"Join blade {i + 1}",
                    "op": "union", "target_id": target, "tool_id": loft["id"]})
        target = r.get("id", target)
    sk = p.sketch(Plane.xy(PROP_HUB_H), "Bore")
    sk.point((0, 0))
    p.hole(sk.add(), d=PROP_BORE_D, through=True, name="M5 bore")
    p.frame("Seat", origin=(0, 0, 0))
    p.frame("Nut seat", origin=(0, 0, PROP_HUB_H))
    return p.material("plastic_black").check()
