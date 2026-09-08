#!/usr/bin/env python3
"""The bottom end of the diesel: block, crank, rods, pistons, and everything
that seals or bears on them.

Every part here is one function that takes the app and returns the Part, so
the assembly module can place them and nothing has to be built in an order
this file knows about. The numbers all come from om615_spec.
"""
import math
import struct
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from designer_sdk import Plane, SdkError, V  # noqa: E402
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from om615_spec import *  # noqa: E402,F401,F403


# --- the two shapes everything here is made of ------------------------------
#
# No Box, Sphere or Cylinder anywhere in this engine. A primitive is always a
# NEW BODY (they take no mode), so a part built from them is a bag of
# overlapping solids rather than one; and a drawn profile is what the part is
# actually described by, so changing a number re-draws it instead of moving a
# literal. These two helpers are the whole vocabulary.

def plane_span(z0, z1):
    """Span between the API's float sketch planes, not unsnapped doubles.

    Otherwise a slab ending at 242.9 passes about 0.000006 mm
    through a gasket whose starting plane is stored at 242.899993896.
    """
    snap = lambda z: struct.unpack("f", struct.pack("f", z))[0]
    return snap(z1) - snap(z0)


def casting_finish(p, material="aluminum", color=(0.48, 0.49, 0.50)):
    """A rough casting surface, separate from the physical material."""
    p.material(material)
    p.app.set_appearance(body=0, color=list(color), roughness=0.65, metallic=0.8)


def rounded_polygon(sk, points, radius):
    """Tangent circular corners, kept inside each corner's adjacent edges.

    Scalar or per-corner radii; zero preserves a machined/flange corner.
    The profiles remain analytic B-Reps rather than faceted approximations.
    """
    corners = []
    for i, pt in enumerate(points):
        v = V((*pt, 0))
        a = V((*points[i - 1], 0)) - v
        b = V((*points[(i + 1) % len(points)], 0)) - v
        u, w = a.unit(), b.unit()
        angle = math.acos(max(-1.0, min(1.0, sum(u[j]*w[j] for j in range(3)))))
        r = radius[i] if isinstance(radius, (list, tuple)) else radius
        t = min(r / math.tan(angle / 2), 0.45*a.length(), 0.45*b.length())
        if t < 1e-8:
            corners.append((pt, None, pt))
            continue
        r = t * math.tan(angle / 2)
        centre = v + (u + w).unit() * (r / math.sin(angle / 2))
        mid = centre + (v - centre).unit() * r
        corners.append((tuple(v[j]+u[j]*t for j in range(2)),
                        tuple(mid[j] for j in range(2)),
                        tuple(v[j]+w[j]*t for j in range(2))))
    for i, (a, mid, b) in enumerate(corners):
        sk.line(corners[i-1][2], a)
        if mid is not None:
            sk.arc_through(a, mid, b)


def slab(p, x0, y0, z0, x1, y1, z1, name, mode="new_body"):
    """A rectangular block, drawn on its BOTTOM face and extruded up. Drawn
    on the top and reversed instead, the plane's V axis flips with it and
    every y in the rectangle is mirrored — which is invisible on a symmetric
    part and wrong on every other."""
    sk = p.sketch(Plane.xy(z0), name=name)
    sk.rect((x0, y0), (x1, y1))
    return p.extrude(sk.add(), plane_span(z0, z1), mode=mode, name=name)


def disc(p, centre, d, z0, z1, name, mode="add"):
    """A round column between two heights, on the bore's own axis."""
    sk = p.sketch(Plane.xy(z0), name=name)
    sk.circle(centre, d=d)
    return p.extrude(sk.add(), plane_span(z0, z1), mode=mode, name=name)


def axis_solid(p, start, target, d, name, mode="add", over=0.0):
    """A round column along ANY axis — a port sleeve, an inclined boss."""
    direction = V(target) - V(start)
    sk = p.sketch(axis_plane(start, direction), name=name)
    sk.circle((0, 0), d=d)
    return p.extrude(sk.add(), direction.length() + over, mode=mode,
                     name=name)


def axis_plane(start, direction):
    """A sketch plane at `start` facing along `direction`."""
    dd = V(direction).unit()
    u = V((0, 1, 0)) if abs(dd.dot(V((0, 1, 0)))) < 0.9 else V((1, 0, 0))
    u = (u - dd * u.dot(dd)).unit()
    return Plane(start, tuple(dd), tuple(u))


def spline_tube(p, points, od, name, mode="add", wall=0.0):
    """A pipe swept along a spline fitted through `points`.

    This is how a manifold runner is shaped, and the reason it matters is
    that a straight cylinder between two flanges is not one: a runner leaves
    the port on the port's own axis and arrives at the log on the log's, and
    everything in between is a curve. Build the branches SOLID and subtract
    their bores afterwards — two hollow tubes unioned leave each one's wall
    across the other's throat, which is a manifold that does not flow.
    """
    p.app.add_feature(feature={
        "type": "Curve3D", "name": f"{name} path", "kind": "fit_spline",
        "points": [list(V(q)) for q in points]})
    path = p.state()["features"][-1]["id"]
    f = {"type": "Pipe", "name": name, "path_id": path,
         "outer_diameter": od, "mode": mode}
    if wall:
        f["wall_thickness"] = wall
    p.app.add_feature(feature=f)


def bore(p, start, target, d, name, over=6.0):
    """Cut a round hole from `start` toward `target`, running past it by
    `over` so it always breaks through into what it is aimed at."""
    return axis_solid(p, start, target, d, name, mode="cut", over=over)

# Numbers only this module uses. Anything the head or the assembly also needs
# lives in om615_spec.
# How far down the webs are cut away for the pistons: below the lowest the
# skirt reaches at BDC, and clear of the main tunnel's own 32 mm radius.
CAP_CLEARANCE_D = 13.0            # M12 Close. A hole that is not one of the
                                  # standard's own clearances is refused, and
                                  # 12.5 (the bolt plus a half) is not.
BDC_RELIEF_Z = 62.0
BELT_GROOVE_D = 12.0              # how deep a pulley's vee is...
BELT_GROOVE_H = 6.0               # ...and half how wide at its mouth
BELT_W = 5.0                      # the belt fits INSIDE it at its own radius
THRUST_OD = MAIN_D + 32.0
THRUST_T = 3.0
REAR_CARRIER_BOLTS = [(-72.0, 56.0), (72.0, 56.0), (-72.0, -20.0),
                      (72.0, -20.0), (-72.0, -78.0), (72.0, -78.0),
                      (0.0, -78.0), (0.0, 62.0)]


# ---------------------------------------------------------------- the block
def block(app):
    """The cylinder block: four bores in a water jacket over a five-bearing
    crankcase, the deck the head bolts to, and the pan rail.

    The crankcase is WIDER than the deck and that is not styling: the big end
    sweeps CRANK_R + BIG_END_D/2 = 80.8 mm from the crank axis, so a skirt at
    the deck's own 70 would be inside the rod's circle at every angle.
    """
    p = app.part("block")
    (p.var("bore", BORE).var("stroke", STROKE).var("deck", DECK)
      .var("bore_pitch", BORE_PITCH))
    length = BLOCK_Y1 - BLOCK_Y0
    # The crankcase is a SECTION with its lower outer edges chamfered, not a
    # box. A casting has draft where a box has a corner, and the sump rail is
    # narrower than the skirt for the same reason a real one is.
    c = CASE_CHAMFER
    sec = p.sketch(Plane.xz(BLOCK_Y1), name="Crankcase")
    rounded_polygon(sec,
                    [(-CASE_X + c, PAN_RAIL), (CASE_X - c, PAN_RAIL),
                     (CASE_X, PAN_RAIL + c), (CASE_X, BLOCK_STEP - 16.0),
                     (DECK_X, BLOCK_STEP + 16.0),
                     (-DECK_X, BLOCK_STEP + 16.0),
                     (-CASE_X, BLOCK_STEP - 16.0), (-CASE_X, PAN_RAIL + c)],
                    [0, 0, 0, 10, 10, 10, 10, 0])
    p.extrude(sec.add(), BLOCK_Y1 - BLOCK_Y0, mode="new_body", name="Crankcase")
    upper = p.sketch(Plane.xz(BLOCK_Y1), name="Cylinder casting draft")
    upper.polyline([(-DECK_X-4, BLOCK_STEP), (DECK_X+4, BLOCK_STEP),
                    (DECK_X, DECK), (-DECK_X, DECK)])
    p.extrude(upper.add(), length, mode="add", name="Cylinder block")
    return _block_rest(p, length)


def _block_rest(p, length):
    for i in range(len(MAIN_Y) - 1):
        y0 = MAIN_Y[i] + BULKHEAD_T / 2.0
        y1 = MAIN_Y[i + 1] - BULKHEAD_T / 2.0
        # Plane.xz(y)'s normal is -Y and an extrude follows the normal, so
        # the sketch goes at the void's AFT face and the cut travels FORWARD.
        # Placed at y0 instead, all four voids landed a full bay ahead of
        # where they belong — the middle one straight through the centre main
        # web, which then had no material for its thrust recesses to cut.
        # Nothing outside the block changed, so the render looked right.
        sk = p.sketch(Plane.xz(y1), name=f"Crankcase void {i + 1}")
        sk.rect((-(CASE_X - 14.0), PAN_RAIL - 1.0),
                (CASE_X - 14.0, CRANKCASE_DECK + 1.0))
        p.extrude(sk.add(), y1 - y0, mode="cut", name=f"Crankcase void {i + 1}")

    # The water jacket: one void round all four bores, then the liners added
    # back inside it. At the bore pitch the 99 mm liners overlap by 2, which
    # is what siamesed bores are.
    jacket = p.sketch(Plane.xy(JACKET_Z0), name="Water jacket")
    # Stops short of both end faces: at the block's own length the jacket
    # broke out of the front face and the water had nowhere to be.
    jacket.rect((-JACKET_X, BLOCK_Y0 + 15.0), (JACKET_X, BLOCK_Y1 - 15.0))
    p.extrude(jacket.add(), JACKET_Z1 - JACKET_Z0, mode="cut",
              name="Water jacket")
    # One sketch each: at the bore pitch the 99 mm liners overlap by 2, and
    # overlapping loops in ONE profile are refused rather than unioned.
    for i, y in enumerate(CYL_Y):
        disc(p, (0, y), LINER_OD, JACKET_Z0, JACKET_Z1, f"Liner {i + 1}")

    # The bores, last, so they cut the liners as well as the deck.
    bores = p.sketch(Plane.xy(CRANKCASE_DECK - 6.0), name="Bores")
    for y in CYL_Y:
        bores.circle((0, y), d="bore")
    p.extrude(bores.add(), DECK - CRANKCASE_DECK + 8.0, mode="cut",
              name="Bores")

    # The piston drops out of its bore at BDC — its skirt reaches z = 73.2
    # while the bore ends at 89 — and the bay it drops into is 75 mm wide
    # between the webs against a piston 85.8 across. So each bore is carried
    # on down through the webs beside it, which is what a real block's
    # crankcase does.
    relief = p.sketch(Plane.xy(BDC_RELIEF_Z), name="Skirt relief")
    for y in CYL_Y:
        relief.circle((0, y), d=BORE + 2.0)
    p.extrude(relief.add(), CRANKCASE_DECK - BDC_RELIEF_Z + 2.0, mode="cut",
              name="Skirt relief")

    # The main bearing tunnel, and the register the caps drop into. The bore
    # is cut full round; the cap slot then takes the bottom of each web away,
    # so what is left is the upper half-shell seat.
    tunnel = p.sketch(Plane.xz(BLOCK_Y1 + 1.0), name="Main tunnel")
    tunnel.circle((0, 0), d=MAIN_HOUSING_D)
    p.extrude(tunnel.add(), length + 2.0, mode="cut", name="Main tunnel")
    for i, y in enumerate(MAIN_Y):
        sk = p.sketch(Plane.xz(y + BULKHEAD_T / 2.0 + 1.0),
                      name=f"Cap register {i + 1}")
        # The register and the relief under it are ONE closed outline, not
        # two rectangles sharing an edge: two of those cost 30 s of boolean
        # on this block for a shape a six-point polyline states directly.
        sk.polyline([(-CAP_X, 0.0), (CAP_X, 0.0), (CAP_X, -CAP_DEPTH),
                     (CASE_X - 14.0, -CAP_DEPTH),
                     (CASE_X - 14.0, PAN_RAIL - 1.0),
                     (-(CASE_X - 14.0), PAN_RAIL - 1.0),
                     (-(CASE_X - 14.0), -CAP_DEPTH), (-CAP_X, -CAP_DEPTH)])
        p.extrude(sk.add(), BULKHEAD_T + 2.0, mode="cut",
                  name=f"Cap register {i + 1}")

    # Main cap bolts: blind, up into each web from the parting line.
    # UP into the web from the parting line, not down: below z = 0 the cap
    # register has already taken the material away, so a downward cut is
    # 46 mm of fresh air and the app says so.
    caps = p.sketch(Plane.xy(0.0), name="Main cap bolt holes")
    for y in MAIN_Y:
        for sx in (-1, 1):
            # Nominal plus TAP_FIT — see the spec: at exactly nominal the
            # bolt and the hole are the same cylinder, which OCC's boolean
            # does not survive once the part is off its own axes.
            caps.circle((sx * CAP_BOLT_X, y), d=CAP_BOLT_D + TAP_FIT)
    p.extrude(caps.add(), 46.0, mode="cut", name="Main cap bolt holes")

    # Head bolts: blind in the deck, ten of them, on the list the HEAD drills
    # through — one list, not two that agree until one is edited.
    # Start outside the casting. Sketch planes store float coordinates while
    # the slab's extrusion distance is double: at deck=242.9 an exactly
    # coplanar cut started 0.000006 mm below the face, leaving a membrane
    # that the fastener's axial probe correctly detected as a blocked hole.
    deck = p.sketch(Plane.xy(DECK + 1.0, flip=True), name="Head bolt holes")
    for x, y in HEAD_BOLT:
        deck.circle((x, y), d=HEAD_BOLT_D + TAP_FIT)
    p.extrude(deck.add(), 61.0, mode="cut", name="Head bolt holes")

    # The centre main carries the thrust: a recess each side of its web for
    # the washers that take the clutch's push.
    for sy in (-1, 1):
        t = p.sketch(Plane.xz(MAIN_Y[2] + sy * BULKHEAD_T / 2.0
                              + (0.0 if sy > 0 else THRUST_T)),
                     name=f"Thrust recess {sy}")  # noqa: E501
        t.circle((0, 0), d=THRUST_OD)
        p.extrude(t.add(), THRUST_T, mode="cut", name=f"Thrust recess {sy}")

    # The front cover bolts into the block for every hole below the deck;
    # the ones above it are the HEAD's, and both come off COVER_BOLTS.
    fr = p.sketch(Plane.xz(BLOCK_Y0 + 22.0), name="Cover bolt holes")
    for x, z in COVER_BOLTS:
        fr.circle((x, z), d=7.5)
    p.extrude(fr.add(), 22.0, mode="cut", name="Cover bolt holes")

    # ...and the rear seal carrier's, in the rear face.
    rear = p.sketch(Plane.xz(BLOCK_Y1), name="Carrier bolt holes")
    for x, z in REAR_CARRIER_BOLTS:
        rear.circle((x, z), d=7.5)
    p.extrude(rear.add(), 20.0, mode="cut", name="Carrier bolt holes")

    # Sump rail: a bolt every ~70 mm round the pan face.
    rail = p.sketch(Plane.xy(PAN_RAIL), name="Sump bolt holes")
    for y in _pan_bolt_y():
        for sx in (-1, 1):
            rail.circle((sx * PAN_BOLT_X, y), d=PAN_BOLT_D + TAP_FIT)
    p.extrude(rail.add(), 20.0, mode="cut", name="Sump bolt holes")

    # Ribs down the crankcase's flanks, over each main web — which is where
    # a real block puts them, and it is most of what makes a casting read as
    # a casting rather than as a box.
    for i, y in enumerate(MAIN_Y):
        for sx in (-1, 1):
            rib = p.sketch(Plane.xz(y + 8.0), name=f"Rib {i}{sx}")
            rib.polyline([(sx * CASE_X, PAN_RAIL + 10.0),
                          (sx * (CASE_X + 7.0), PAN_RAIL + 24.0),
                          (sx * (CASE_X + 7.0), BLOCK_STEP - 16.0),
                          (sx * CASE_X, BLOCK_STEP - 2.0)])
            p.extrude(rib.add(), 16.0, mode="add", name=f"Rib {i}{sx}")

    # The engine's own mounting pads, one each side between the ribs.
    for sx in (-1, 1):
        for dy, z in MOUNT_BOLTS:
            bore(p, (sx * CASE_X, MOUNT_Y + dy, z),
                 (sx * (CASE_X - MOUNT_BOLT_DEPTH), MOUNT_Y + dy, z),
                 MOUNT_BOLT_D - 1.5, f"Mount bolt {sx}{dy}", over=0.0)

    # ---- the galleries --------------------------------------------------
    # Oil: one gallery the length of the skirt wall, a cross-drilling down to
    # every main, and a riser up the deck to the head. The riser is in TWO
    # legs because the block steps in at BLOCK_STEP: one straight line from
    # the gallery to the deck leaves the block at x = -82 and finishes the
    # last 120 mm in fresh air.
    g = p.sketch(Plane.xz(BLOCK_Y1 + 1.0), name="Oil gallery")
    g.circle((GALLERY_X, GALLERY_Z), d=GALLERY_D)
    p.extrude(g.add(), length + 2.0, mode="cut", name="Oil gallery")
    for i, y in enumerate(MAIN_Y):
        bore(p, (GALLERY_X, y, GALLERY_Z), (0, y, 0), 6.0,
             f"Main oil feed {i + 1}", over=0.0)
    bore(p, (GALLERY_X, CYL_Y[0], GALLERY_Z),
         (OIL_RISER_X, CYL_Y[0], BLOCK_STEP + 5.0), OIL_RISER_D,
         "Oil riser lower", over=0.0)
    r2 = p.sketch(Plane.xy(BLOCK_STEP), name="Oil riser upper")
    r2.circle((OIL_RISER_X, CYL_Y[0]), d=OIL_RISER_D)
    p.extrude(r2.add(), DECK - BLOCK_STEP, mode="cut", name="Oil riser upper")

    # Coolant: the jacket already surrounds the liners; these are the ten
    # holes it crosses the gasket through, and the way in from the pump.
    ct = p.sketch(Plane.xy(JACKET_Z1 - 2.0), name="Coolant transfers")
    for x, y in COOLANT_HOLES:
        ct.circle((x, y), d=COOLANT_D)
    p.extrude(ct.add(), DECK - JACKET_Z1 + 2.0, mode="cut",
              name="Coolant transfers")
    wp = p.sketch(Plane.xz(BLOCK_Y0 + 20.0), name="Water pump inlet")
    wp.circle((0, WATER_PUMP_Z), d=50.0)
    p.extrude(wp.add(), 20.0, mode="cut", name="Water pump inlet")

    casting_finish(p, "steel", (0.32, 0.34, 0.35))
    # Faces the covers bolt to, and the frames the assembly mates by.
    p.frame("crank_axis", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.frame("deck", origin=(0, 0, DECK), z=(0, 0, 1), x=(1, 0, 0))
    # Every MOUNTING frame is world-aligned and only translated. The parts
    # that bolt to these faces — pan, covers, carriers — are drawn in engine
    # coordinates, so a frame that also rotates would place them by a second
    # transform nobody wrote down. Only the frames something TURNS or SLIDES
    # on carry an axis: crank_axis, bore_i, main_i.
    p.frame("front_face", origin=(0, BLOCK_Y0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("rear_face", origin=(0, BLOCK_Y1, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("pan_rail", origin=(0, 0, PAN_RAIL), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("pan_face", origin=(0, 0, PAN_RAIL - SUMP_GASKET_T), z=(0, 0, 1),
            x=(1, 0, 0))
    p.frame("head_face", origin=(0, 0, FIRE_FACE), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("cover_face", origin=(0, BLOCK_Y0 - COVER_GASKET_T, 0),
            z=(0, 0, 1), x=(1, 0, 0))
    p.frame("oil_pump", origin=(0, BLOCK_Y0, OIL_PUMP_Z), z=(0, 0, 1),
            x=(1, 0, 0))
    p.frame("timing_chain", origin=(0, DRIVE_Y, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.frame("oil_chain", origin=(0, OIL_DRIVE_Y, OIL_PUMP_Z), z=(0, 1, 0),
            x=(1, 0, 0))
    p.frame("belt", origin=(0, BELT_Y, 0), z=(0, 1, 0), x=(1, 0, 0))
    for sx, nm in ((-1, "mount_left"), (1, "mount_right")):
        p.frame(nm, origin=(sx * CASE_X, MOUNT_Y, 0), z=(0, 0, 1),
                x=(sx, 0, 0))
    for sy, nm in ((-1, "thrust_f"), (1, "thrust_r")):
        p.frame(nm, origin=(0, MAIN_Y[2] + sy * (BULKHEAD_T / 2.0
                                                 - THRUST_T / 2.0), 0),
                z=(0, 1, 0), x=(-1, 0, 0))
    for i, y in enumerate(CYL_Y):
        p.frame(f"bore_{i}", origin=(0, y, 0), z=(0, 0, 1), x=(1, 0, 0))
    for i, y in enumerate(MAIN_Y):
        # x = -X on purpose. A half shell is drawn as its +Y half, and the
        # frame's own Y is z x x — so (0,1,0) crossed with (-1,0,0) is +Z and
        # the shell lands in the UPPER half of the bore, which is the block's.
        # The cap's matching frame uses +X and gets the lower half.
        p.frame(f"main_{i}", origin=(0, y, 0), z=(0, 1, 0), x=(-1, 0, 0))
        # The cap's own seat: the parting plane, world-aligned like every
        # other mounting frame. It was z = -Z and x = +Y once, on the
        # reasoning that the face "points down" — and a mate aligns axes, so
        # every cap arrived rotated a half-turn about (1,1,0): 120 mm long
        # ALONG THE CRANK, standing above the parting line instead of under
        # it, and dragging its shells and thrust washers with it. That one
        # frame was 88 of the assembly's 88 clashes.
        p.frame(f"cap_{i}", origin=(0, y, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def _pan_bolt_y():
    """Sump bolt centres along the rail, ~70 mm apart, symmetric."""
    n = 7
    span = (BLOCK_Y1 - 12.0) - (BLOCK_Y0 + 12.0)
    return [BLOCK_Y0 + 12.0 + span * i / (n - 1) for i in range(n)]


# ----------------------------------------------------------- the crankshaft
def crankshaft(app):
    """Five mains, four throws at 0/180/180/0, eight webs with the
    counterweight opposite each pin, a nose carrying two sprockets and the
    damper, and a flywheel flange.

    ONE body, not eighteen. A primitive is always a new body — Box, Cylinder
    and Sphere take no mode — so a crank built from cylinders is a bag of
    overlapping solids that reports the overlaps twice in its own volume and
    has no single outside for anything to bear on. Every round section here
    is a circle extruded with mode="add", which is the same drawing and one
    solid at the end of it.

    The web thicknesses are DERIVED from the gap each one has to fill — half
    a bore pitch less half a main and half a rod journal — so moving a main
    or widening a rod re-fits every web instead of leaving a slot of air or
    an overlap the boolean quietly absorbs.
    """
    p = app.part("crankshaft")
    p.var("crank_r", CRANK_R).var("stroke", STROKE)
    first = ["new_body"]

    def round_section(y0, y1, d, name, at=(0.0, 0.0)):
        """A cylinder from y0 to y1: a circle on the plane at y1 (whose
        normal is -Y) extruded back toward the front of the engine."""
        sk = p.sketch(Plane.xz(y1), name=name)
        sk.circle(at, d=d)
        mode, first[0] = first[0], "add"
        p.extrude(sk.add(), y1 - y0, mode=mode, name=name)

    for i, y in enumerate(MAIN_Y):
        round_section(y - MAIN_W / 2.0, y + MAIN_W / 2.0, MAIN_D,
                      f"Main journal {i + 1}")
    for k, y in enumerate(CYL_Y):
        sgn = 1.0 if THROW_DEG[k] == 0.0 else -1.0
        round_section(y - ROD_JW / 2.0, y + ROD_JW / 2.0, ROD_JD,
                      f"Rod journal {k + 1}", at=(0.0, sgn * CRANK_R))
        # A web each side, filling exactly the gap to the main beside it.
        _web(p, MAIN_Y[k] + MAIN_W / 2.0, y - ROD_JW / 2.0, sgn,
             f"Web {k + 1}F")
        _web(p, y + ROD_JW / 2.0, MAIN_Y[k + 1] - MAIN_W / 2.0, sgn,
             f"Web {k + 1}R")
    # The nose: shoulder, the oil pump's sprocket seat, the timing sprocket's,
    # the seal land the front seal runs on, the damper's seat, and the stub.
    for (a, b), d, nm in ((NOSE_SHOULDER, MAIN_D, "Nose shoulder"),
                          (OIL_SPROCKET, NOSE_D, "Oil sprocket seat"),
                          (TIMING_SPROCKET, NOSE_D, "Timing sprocket seat"),
                          (FRONT_SEAL_LAND, FRONT_SEAL_D, "Front seal land"),
                          (DAMPER_SEAT, NOSE_D, "Damper seat"),
                          ((NOSE_Y, DAMPER_SEAT[0]), NOSE_D, "Nose end")):
        round_section(a, b, d, nm)
    # Rear: the seal land, the flywheel flange and the spigot it centres on.
    # Through the block's rear wall at the TUNNEL's diameter, and only then
    # out to the seal land: the wall runs to BLOCK_Y1 and a 75 mm land
    # started at the journal put 7.2 cm3 of crank inside it.
    round_section(MAIN_Y[-1] + MAIN_W / 2.0, BLOCK_Y1, MAIN_D,
                  "Rear journal extension")
    round_section(BLOCK_Y1, FLANGE_Y, REAR_SEAL_D, "Rear seal land")
    round_section(FLANGE_Y, FLANGE_Y + FLANGE_T, FLANGE_D, "Flywheel flange")
    round_section(FLANGE_Y + FLANGE_T, FLANGE_Y + FLANGE_T + 8.0,
                  FLYWHEEL_SPIGOT_D, "Flywheel spigot")

    bolts = p.sketch(Plane.xz(FLANGE_Y + FLANGE_T + 9.0), name="Flywheel bolts")
    for i in range(FLYWHEEL_BOLTS):
        a = 2 * math.pi * i / FLYWHEEL_BOLTS
        bolts.circle((FLYWHEEL_BOLT_PCD / 2.0 * math.cos(a),
                      FLYWHEEL_BOLT_PCD / 2.0 * math.sin(a)),
                     d=FLYWHEEL_BOLT_D + TAP_FIT)
    p.extrude(bolts.add(), 40.0, mode="cut", name="Flywheel bolt holes")
    # The nose bolt that holds the damper on.
    nb = p.sketch(Plane.xz(NOSE_Y + 40.0), name="Nose bolt hole")
    nb.circle((0, 0), d=18.0)
    p.extrude(nb.add(), 41.0, mode="cut", name="Nose bolt hole")

    # The oil ways: one drilling per throw, from the main journal it is fed
    # by to the rod journal it feeds. This is how a plain-bearing crank gets
    # oil to its big ends, and it is why a crank is drilled rather than cast
    # hollow.
    for k, y in enumerate(CYL_Y):
        sgn = 1.0 if THROW_DEG[k] == 0.0 else -1.0
        bore(p, (0, MAIN_Y[k], sgn * (MAIN_D / 2.0 + 2.0)),
             (0, y, sgn * (CRANK_R - ROD_JD / 2.0)), 6.0,
             f"Oil way {k + 1}", over=3.0)
    p.material("steel")
    p.frame("main", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    for k, y in enumerate(CYL_Y):
        sgn = 1.0 if THROW_DEG[k] == 0.0 else -1.0
        p.frame(f"pin_{k}", origin=(0, y, sgn * CRANK_R), z=(0, 1, 0),
                x=(1, 0, 0))
    p.frame("timing_drive", origin=(0, DRIVE_Y, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.frame("oil_drive", origin=(0, OIL_DRIVE_Y, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.frame("damper", origin=(0, DAMPER_Y, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.frame("flywheel", origin=(0, FLANGE_Y + FLANGE_T, 0), z=(0, 1, 0),
            x=(1, 0, 0))
    p.frame("front_seal", origin=(0, FRONT_SEAL_Y, 0), z=(0, 1, 0),
            x=(1, 0, 0))
    p.frame("rear_seal", origin=(0, REAR_SEAL_Y, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.check()
    return p


def _web(p, y0, y1, sgn, name):
    """One crank web: a disc of WEB_R with the cheek either side of the pin
    cut back, which is what makes the mass sit OPPOSITE the pin."""
    sk = p.sketch(Plane.xz(y1), name=name)
    sk.circle((0, 0), r=WEB_R)
    p.extrude(sk.add(), y1 - y0, mode="add", name=name)
    cut = p.sketch(Plane.xz(y1 + 0.5), name=f"{name} cheek")
    for side in (-1, 1):
        x0, x1 = sorted((side * 36.0, side * (WEB_R + 4.0)))
        z0, z1 = sorted((sgn * 12.0, sgn * (WEB_R + 4.0)))
        cut.rect((x0, z0), (x1, z1))
    p.extrude(cut.add(), y1 - y0 + 1.0, mode="cut", name=f"{name} cheek")


# ------------------------------------------------- rods, pistons and pins
def con_rod(app):
    """The rod: an I-section shank between a big end split on the crank
    centreline and a small end that takes a bush.

    Its origin is the BIG END, and the small end is ROD_L up +Z — so the
    assembly mates it to the crank pin and to the wrist pin by name and
    nothing here is repeated as a coordinate over there.
    """
    p = app.part("con rod")
    p.var("rod_l", ROD_L).var("big_end", ROD_HOUSING_D)
    w = BIG_END_W                 # at the big end, where the shells are
    ws = BIG_END_W - 4.0          # ...and through the shank and small end
    # The small end is NARROWER than the big end, and it has to be: it lives
    # between the piston's two pin bosses, and a rod as wide there as at its
    # big end is 1.3 cm3 of steel inside the piston at every crank angle.
    shank = p.sketch(Plane.xz(ws / 2.0), name="Shank")
    shank.polyline([(-14, 0), (14, 0), (10, ROD_L), (-10, ROD_L)])
    p.extrude(shank.add(), ws, mode="new_body", name="Shank")
    big = p.sketch(Plane.xz(w / 2.0), name="Big end")
    big.circle((0, 0), d=BIG_END_D)
    p.extrude(big.add(), w, mode="add", name="Big end")
    small = p.sketch(Plane.xz(ws / 2.0), name="Small end")
    small.circle((0, ROD_L), d=SMALL_END_D)
    p.extrude(small.add(), ws, mode="add", name="Small end")
    # The I-section: a pocket in each cheek, leaving a web between them.
    for side, y in ((-1, -4.0), (1, ws / 2.0)):
        sk = p.sketch(Plane.xz(y), name=f"Web relief {side}")
        sk.polyline([(-8, 26), (8, 26), (5, ROD_L - 30), (-5, ROD_L - 30)])
        p.extrude(sk.add(), 8.0, mode="cut", name=f"Web relief {side}")
    bores = p.sketch(Plane.xz(w), name="Bores")
    bores.circle((0, 0), d="big_end")
    bores.circle((0, ROD_L), d=BUSH_OD)
    p.extrude(bores.add(), 2 * w, mode="cut", name="Bores")
    # The bolt holes, through the big end along the rod's own axis: the cap
    # is pulled up onto the rod, so both halves share these centres.
    bolts = p.sketch(Plane.xy(-BIG_END_D), name="Rod bolt holes")
    for sx in (-1, 1):
        bolts.circle((sx * ROD_BOLT_X, 0), d=ROD_BOLT_D)
    p.extrude(bolts.add(), 2 * BIG_END_D, mode="cut", name="Rod bolt holes")
    # ...and the rod is the half ABOVE the split. Everything below the crank
    # centreline is the cap, which is a part of its own.
    split = p.sketch(Plane.xz(w + 1.0), name="Split")
    split.rect((-BIG_END_D, -BIG_END_D), (BIG_END_D, 0.0))
    p.extrude(split.add(), 2 * w + 2.0, mode="cut", name="Split")

    p.frame("big_end", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.frame("small_end", origin=(0, 0, ROD_L), z=(0, 1, 0), x=(1, 0, 0))
    p.frame("cap", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    # The upper shell's seat: local +y of the shell has to come out along
    # world +Z, so x is -X and z is the journal axis.
    p.frame("shell", origin=(0, 0, 0), z=(0, 1, 0), x=(-1, 0, 0))
    p.material("steel")
    p.check()
    return p


def rod_cap(app):
    """The other half of the big end. Same bore, same bolt centres, the same
    two numbers — because it reads them from the same place the rod does."""
    p = app.part("rod cap")
    w = BIG_END_W
    sk = p.sketch(Plane.xz(w / 2.0), name="Cap")
    sk.circle((0, 0), d=BIG_END_D)
    p.extrude(sk.add(), w, mode="new_body", name="Cap")
    top = p.sketch(Plane.xz(w + 1.0), name="Split")
    top.rect((-BIG_END_D, 0.0), (BIG_END_D, BIG_END_D))
    p.extrude(top.add(), 2 * w + 2.0, mode="cut", name="Split")
    bore = p.sketch(Plane.xz(w), name="Bore")
    bore.circle((0, 0), d=ROD_HOUSING_D)
    p.extrude(bore.add(), 2 * w, mode="cut", name="Bore")
    bolts = p.sketch(Plane.xy(-BIG_END_D), name="Bolt holes")
    for sx in (-1, 1):
        bolts.circle((sx * ROD_BOLT_X, 0), d=ROD_BOLT_D + 0.5)
    p.extrude(bolts.add(), 2 * BIG_END_D, mode="cut", name="Bolt holes")
    p.frame("cap", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("shell", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.material("steel")
    p.check()
    return p


def piston(app):
    """A turned piston: bowl, three ring grooves, a hollow underside with the
    pin bosses left standing, and valve pockets in the crown.

    Its origin is the WRIST PIN, so the rod mates to it with no offset and
    the piston's z IS the slider-crank's own displacement.
    """
    p = app.part("piston")
    p.var("bore", BORE).var("pin_h", PIN_H)
    r = (BORE - PISTON_CLEARANCE) / 2.0
    p.revolve_profile(
        [(0, -SKIRT), (r, -SKIRT), (r, PIN_H), (BOWL_D / 2.0, PIN_H),
         (BOWL_D / 2.0 - 5.0, PIN_H - BOWL_DEPTH), (0, PIN_H - BOWL_DEPTH)],
        plane=Plane.xz(), axis="z", name="Piston")
    for i, (t, dz) in enumerate(zip(RING_T, RING_Z)):
        z = PIN_H + dz
        groove_width = t + 2 * RING_SIDE_CLEARANCE
        g = p.sketch(Plane.xz(), name=f"Ring groove {i + 1}")
        g.polyline([(r - RING_GROOVE_DEPTH, z), (r + 1.0, z),
                    (r + 1.0, z - groove_width),
                    (r - RING_GROOVE_DEPTH, z - groove_width)])
        p.revolve(g.add(), axis="z", mode="cut", name=f"Ring groove {i + 1}")
    # Hollow underside, then the two pin bosses put back inside it.
    under = p.sketch(Plane.xz(), name="Underside")
    under.polyline([(0, -SKIRT - 1.0), (r - 7.0, -SKIRT - 1.0),
                    (r - 7.0, PIN_H - 14.0), (0, PIN_H - 14.0)])
    p.revolve(under.add(), axis="z", mode="cut", name="Underside")
    # TWO bosses, with the rod's small end between them. One boss across
    # the whole pin was 36 cm3 of piston in the same place as the rod at
    # every crank angle — and the rod is inside the piston, so nothing but a
    # clash check ever sees it.
    half = BIG_END_W / 2.0
    for sy in (-1, 1):
        boss = p.sketch(Plane.xz(sy * GUDGEON_L / 2.0 if sy > 0 else -half),
                        name=f"Pin boss {sy}")
        boss.circle((0, 0), d=GUDGEON_D + 16.0)
        p.extrude(boss.add(), GUDGEON_L / 2.0 - half, mode="add",
                  name=f"Pin boss {sy}")
    pin = p.sketch(Plane.xz(GUDGEON_L), name="Pin bore")
    pin.circle((0, 0), d=GUDGEON_D + FIT)
    p.extrude(pin.add(), 2 * GUDGEON_L, mode="cut", name="Pin bore")
    # Circlip grooves, one at each end of the pin bore.
    for sy, off in ((1, GUDGEON_L / 2.0 - 1.5), (-1, -(GUDGEON_L / 2.0 - 4.5))):
        clip = p.sketch(Plane.xz(off), name=f"Circlip groove {sy}")
        clip.circle((0, 0), d=GUDGEON_D + 5.0)
        p.extrude(clip.add(), 3.0, mode="cut", name=f"Circlip groove {sy}")
    # Valve pockets: a diesel at 21:1 has the valves in the crown's way.
    # On the VALVES' own centres, stagger and all — put both on y = 0 and
    # the two pockets overlap, which a profile refuses outright.
    pockets = p.sketch(Plane.xy(PIN_H + 1.0, flip=True), name="Valve pockets")
    for kind, (vx, dy) in VALVE.items():
        pockets.circle((vx, dy), d=VALVE_HEAD_D[kind] + 3.0)
    p.extrude(pockets.add(), 3.5, mode="cut", name="Valve pockets")

    # The catalog-height skirt needs clearance for the counterweights at
    # BDC. Relieve its fore/aft edges where the webs run, retaining full
    # skirt length on the thrust faces. This relief is a derived modeling
    # shape, not a measured factory piston profile.
    relief_top = WEB_R + FIT - (ROD_L - CRANK_R)
    if relief_top > -SKIRT:
        inner = ROD_JW / 2.0 - FIT
        for side in (-1, 1):
            y0, y1 = sorted((side * inner, side * BORE))
            slab(p, -BORE, y0, -SKIRT - 1.0,
                 BORE, y1, relief_top, f"Counterweight relief {side}",
                 mode="cut")

    p.frame("wrist_pin", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.frame("slide", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    for i, dz in enumerate(RING_Z):
        p.frame(f"ring_{i}", origin=(0, 0, PIN_H + dz - RING_T[i] / 2.0
                                    - RING_SIDE_CLEARANCE),
                z=(0, 0, 1), x=(1, 0, 0))
    for sy, nm in ((1, "clip_r"), (-1, "clip_f")):
        p.frame(nm, origin=(0, sy * (GUDGEON_L / 2.0 - 3.0), 0), z=(0, 1, 0),
                x=(1, 0, 0))
    p.material("aluminum")
    p.check()
    return p


def ring(app, name, od, id_, width, angle=360.0, material=None):
    """A ring: the shape a bush, a shell, a ring and a washer all are, turned
    about its own axis with its origin in the middle of it."""
    p = app.part(name)
    p.revolve_profile([(id_ / 2.0, -width / 2.0), (od / 2.0, -width / 2.0),
                       (od / 2.0, width / 2.0), (id_ / 2.0, width / 2.0)],
                      plane=Plane.xz(), axis="z", angle=angle, name=name)
    if material:
        p.material(material)
    p.frame("seat", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def gudgeon_pin(app):
    p = ring(app, "gudgeon pin", GUDGEON_D, 18.0, GUDGEON_L,
              material="steel")
    p.frame("axis", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    return p


def circlip(app):
    """A retaining ring: an open C, because that is what makes it fit."""
    return ring(app, "gudgeon circlip", GUDGEON_D + 4.4, GUDGEON_D + FIT,
                 2.0, angle=290.0, material="steel")


def rings(app):
    """Top compression, second compression, oil control — each an open ring
    of its own section, sized to the groove it sits in."""
    made = []
    for i, (t, nm) in enumerate(zip(RING_T, ("top ring", "second ring",
                                             "oil ring"))):
        made.append(ring(app, nm, BORE - FIT,
                          BORE - 2 * (RING_GROOVE_DEPTH - 0.6),
                          t, angle=352.0, material="steel"))
    return made


def small_end_bush(app):
    return ring(app, "small end bush", BUSH_OD - FIT, GUDGEON_D + FIT, 22.0,
                material="bronze")


def main_shell(app):
    # 178 degrees, not 180: two half shells that butt on the parting line
    # share a face, and a mesh contact check reads a shared face as contact.
    # OD a hair under the housing for the same reason — a press fit cannot
    # be drawn as one without every check calling it a clash.
    return ring(app, "main bearing shell", MAIN_HOUSING_D - FIT,
                MAIN_D + FIT, SHELL_W, angle=178.0, material="bronze")


def rod_shell(app):
    return ring(app, "rod bearing shell", ROD_HOUSING_D - FIT, ROD_JD + FIT,
                BIG_END_W - 4.0, angle=178.0, material="bronze")


def thrust_washer(app):
    return ring(app, "thrust washer", THRUST_OD - FIT, MAIN_D + FIT,
                THRUST_T - 0.3, angle=178.0, material="bronze")


def main_cap(app, name="main cap", thrust=False):
    """A main bearing cap: the lower half of one main bore, in the register
    the block's web leaves for it, held by two bolts.

    `thrust=True` is the centre one, which is recessed each side for the
    washers that locate the crank fore and aft — the same recess the block's
    own centre web has, because the washer is two halves and the split is
    the same split.
    """
    p = app.part(name)
    t = BULKHEAD_T
    sk = p.sketch(Plane.xz(t / 2.0), name="Cap")
    sk.rect((-CAP_X, -CAP_DEPTH), (CAP_X, 0.0))
    p.extrude(sk.add(), t, mode="new_body", name="Cap")
    bore = p.sketch(Plane.xz(t), name="Bore")
    bore.circle((0, 0), d=MAIN_HOUSING_D)
    p.extrude(bore.add(), 2 * t, mode="cut", name="Bore")
    bolts = p.sketch(Plane.xy(-CAP_DEPTH - 1.0), name="Bolt holes")
    for sx in (-1, 1):
        bolts.circle((sx * CAP_BOLT_X, 0), d=CAP_CLEARANCE_D)  # M12 Close
    p.extrude(bolts.add(), CAP_DEPTH + 2.0, mode="cut", name="Bolt holes")
    if thrust:
        for sy in (-1, 1):
            r = p.sketch(Plane.xz(sy * t / 2.0
                                  + (0.0 if sy > 0 else THRUST_T)),
                         name=f"Thrust recess {sy}")
            r.circle((0, 0), d=THRUST_OD)
            p.extrude(r.add(), THRUST_T, mode="cut",
                      name=f"Thrust recess {sy}")
    p.frame("seat", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("shell", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    for sy, nm in ((-1, "thrust_f"), (1, "thrust_r")):
        p.frame(nm, origin=(0, sy * (t / 2.0 - THRUST_T / 2.0), 0),
                z=(0, 1, 0), x=(1, 0, 0))
    p.material("steel")
    p.check()
    return p


# ------------------------------------------------ flywheel, damper, seals
FLYWHEEL_D = 280.0
FLYWHEEL_T = 32.0
RING_TEETH = 118
RING_ROOT = 143.0
RING_TIP = 148.5
RING_W = 14.0


def flywheel(app):
    """A dished flywheel: a heavy rim, a clutch face, and the six bolts that
    hold it to the crank flange. Its origin is the face it bolts to."""
    p = app.part("flywheel")
    p.revolve_profile([(FLYWHEEL_SPIGOT_D / 2.0 + 0.2, 0),
                       (FLYWHEEL_D / 2.0, 0),
                       (FLYWHEEL_D / 2.0, 16.0), (95.0, 16.0),
                       (95.0, FLYWHEEL_T),
                       (FLYWHEEL_SPIGOT_D / 2.0 + 0.2, FLYWHEEL_T)],
                      plane=Plane.xz(), axis="z", name="Flywheel")
    bolts = p.sketch(Plane.xy(-1.0), name="Flywheel bolt holes")
    for i in range(FLYWHEEL_BOLTS):
        a = 2 * math.pi * i / FLYWHEEL_BOLTS
        bolts.circle((FLYWHEEL_BOLT_PCD / 2.0 * math.cos(a),
                      FLYWHEEL_BOLT_PCD / 2.0 * math.sin(a)), d=11.0)
    p.extrude(bolts.add(), FLYWHEEL_T + 2.0, mode="cut",
              name="Flywheel bolt holes")
    p.material("steel")
    p.frame("flange", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("ring_gear", origin=(0, 0, 1.0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def ring_gear(app):
    """The starter ring: shrunk on the flywheel's rim, and with real teeth —
    118 of them, drawn from the count, because a starter ring with a smooth
    rim is a spacer."""
    p = app.part("ring gear")
    body = p.sketch(Plane.xy(0.0), name="Ring")
    body.circle((0, 0), r=RING_ROOT)
    body.circle((0, 0), d=FLYWHEEL_D + 0.2)
    p.extrude(body.add(), RING_W, mode="new_body", name="Ring")
    teeth = p.sketch(Plane.xy(0.0), name="Teeth")
    half = math.pi / RING_TEETH * 0.42
    for i in range(RING_TEETH):
        a = 2 * math.pi * i / RING_TEETH
        pts = []
        for r, w in ((RING_ROOT - 1.0, half * 1.35), (RING_TIP, half * 0.6)):
            pts.append((r * math.cos(a - w), r * math.sin(a - w)))
        for r, w in ((RING_TIP, half * 0.6), (RING_ROOT - 1.0, half * 1.35)):
            pts.append((r * math.cos(a + w), r * math.sin(a + w)))
        teeth.polyline(pts)
    p.extrude(teeth.add(), RING_W, mode="add", name="Teeth")
    p.material("steel")
    p.frame("seat", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def damper(app):
    """Crank pulley and torsional damper in one: a hub on the nose, a rubber
    ring, and two V-grooves for the water pump belt."""
    p = app.part("crank damper")
    p.revolve_profile([(NOSE_D / 2.0 + 0.2, 0), (46.0, 0), (46.0, DAMPER_T),
                       (NOSE_D / 2.0 + 0.2, DAMPER_T)],
                      plane=Plane.xz(), axis="z", name="Damper hub")
    p.revolve_profile([(46.0, 2.0), (PULLEY_D / 2.0, 2.0),
                       (PULLEY_D / 2.0, DAMPER_T - 2.0),
                       (46.0, DAMPER_T - 2.0)],
                      plane=Plane.xz(), axis="z", mode="add", name="Rim")
    # ONE groove, and a deep one: a belt has to sit INSIDE it. At 8 mm deep
    # and 10 wide the drawn belt was wider than the vee at the radius it
    # rides, so it stood on the pulley's rims instead of in the groove.
    z = BELT_Y - DAMPER_Y
    g = p.sketch(Plane.xz(), name="V groove")
    g.polyline([(PULLEY_D / 2.0 + 1.0, z - BELT_GROOVE_H),
                (PULLEY_D / 2.0 - BELT_GROOVE_D, z),
                (PULLEY_D / 2.0 + 1.0, z + BELT_GROOVE_H)])
    p.revolve(g.add(), axis="z", mode="cut", name="V groove")
    p.material("steel")
    p.frame("hub", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("belt", origin=(0, 0, BELT_Y - DAMPER_Y), z=(0, 0, 1),
            x=(1, 0, 0))
    p.check()
    return p


def lip_seal(app, name, shaft_d, od, width):
    """A radial lip seal: a steel case, an elastomer body and the lip that
    runs on the shaft.

    Drawn with half a millimetre of radial gap at the lip. A real one is an
    interference fit — it has to be, that is the seal — but the collision
    mesh deflects ~0.4 mm on parts this size, so a lip drawn where it
    actually runs is reported as steel inside steel at every sample. The
    gap is a MODELLING clearance and it is the only one in the engine that
    is not a running fit.
    """
    p = app.part(name)
    p.revolve_profile([(shaft_d / 2.0 + 0.5, -width / 2.0),
                       (od / 2.0, -width / 2.0), (od / 2.0, width / 2.0),
                       (shaft_d / 2.0 + 3.5, width / 2.0),
                       (shaft_d / 2.0 + 0.5, width / 2.0 - 3.0)],
                      plane=Plane.xz(), axis="z", name=name)
    p.material("rubber")
    p.frame("seat", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def rear_seal_carrier(app):
    """The housing the rear main seal sits in, bolted to the block's rear
    face — which is why the crank's flange stands proud of that face."""
    p = app.part("rear seal carrier")
    # Plane.xz's normal is -Y and an extrude follows it, so the whole part
    # is drawn from its AFT end forward — the carrier occupies local y 0..18
    # with its mounting face at y = 0, which is the block's rear face.
    sk = p.sketch(Plane.xz(18.0), name="Carrier")
    sk.rect((-85.0, PAN_RAIL), (85.0, 72.0))
    p.extrude(sk.add(), 18.0, mode="new_body", name="Carrier")
    bore = p.sketch(Plane.xz(19.0), name="Seal bore")
    bore.circle((0, 0), d=100.0)
    p.extrude(bore.add(), 20.0, mode="cut", name="Seal bore")
    holes = p.sketch(Plane.xz(19.0), name="Bolt holes")
    for x, z in REAR_CARRIER_BOLTS:
        holes.circle((x, z), d=9.0)
    p.extrude(holes.add(), 20.0, mode="cut", name="Bolt holes")
    p.frame("face", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("seal", origin=(0, 9.0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.material("aluminum")
    p.check()
    return p



# --------------------------------------------------- the sump and the oiling
SUMP_WALL = 4.0
DRAIN_D = 18.0


def pan_inner_half_width(z):
    """How wide the pan's CAVITY is at height z — the same tapering section
    `sump()` draws, offset by its wall. The strainer lies in the oil, so it
    is this that says how long it can be; drawn 40 mm each side of the tube
    it stood 20 mm out through the pan's flank."""
    bx, w = CASE_X - PAN_BOX_INSET, SUMP_WALL
    floor = bx - 26.0
    lo, hi = SUMP_Z + w, SUMP_Z + 44.0
    if z >= hi:
        return bx - w
    f = max(0.0, (z - lo) / (hi - lo))
    return (floor - w) + (bx - floor) * f


def sump(app):
    """A pressed steel pan, drawn as a SECTION and extruded along the engine:
    wide at the rail, tapering to a narrower floor. A box is quicker and
    looks like a box.

    The pan is narrower than the block and the flange reaches back out to it
    — draw the box at the block's own width and the bolt centres fall in the
    4 mm wall's void, which is 14 bolts through nothing.
    """
    p = app.part("sump")
    bx, w = CASE_X - PAN_BOX_INSET, SUMP_WALL
    floor = bx - 26.0

    def section(sk, o):
        rounded_polygon(sk,
                        [(-bx + o, PAN_RAIL), (bx - o, PAN_RAIL),
                         (bx - o, SUMP_Z + 44.0), (floor - o, SUMP_Z + o),
                         (-floor + o, SUMP_Z + o),
                         (-bx + o, SUMP_Z + 44.0)],
                        [0, 0, 18-o, 18-o, 18-o, 18-o])

    outer = p.sketch(Plane.xz(SUMP_Y1), name="Pan")
    section(outer, 0.0)
    p.extrude(outer.add(), SUMP_Y1 - SUMP_Y0, mode="new_body", name="Pan")
    inner = p.sketch(Plane.xz(SUMP_Y1 - w), name="Pan inside")
    section(inner, w)
    p.extrude(inner.add(), SUMP_Y1 - SUMP_Y0 - 2 * w, mode="cut",
              name="Pan inside")
    # The chain case's floor is OPEN — it drains into the sump — so the pan's
    # flange has to run forward under it. Stopped at the block's front face,
    # the two left a slot straight into the crankcase.
    # The flange it bolts up with, on the block's own bolt centres. Nested
    # rectangles, so it is a rim and not a lid.
    flange = p.sketch(Plane.xy(PAN_RAIL - 6.0), name="Flange")
    flange.rect((-CASE_X, COVER_Y0), (CASE_X, BLOCK_Y1))
    flange.rect((-bx + w, COVER_Y0 + 14.0), (bx - w, BLOCK_Y1 - 14.0))  # rim
    p.extrude(flange.add(), 6.0, mode="add", name="Flange")
    holes = p.sketch(Plane.xy(PAN_RAIL - 7.0), name="Bolt holes")
    for y in _pan_bolt_y():
        for sx in (-1, 1):
            holes.circle((sx * PAN_BOLT_X, y), d=9.0)  # M8 Normal
    p.extrude(holes.add(), 8.0, mode="cut", name="Bolt holes")
    # A drain boss at the deepest, rearmost corner, standing PROUD of the
    # floor — inside it would be a lump in the oil.
    boss = p.sketch(Plane.xy(SUMP_Z - 8.0), name="Drain boss")
    boss.circle((0, SUMP_Y1 - 40.0), d=34.0)
    p.extrude(boss.add(), 8.0 + w, mode="add", name="Drain boss")
    drain = p.sketch(Plane.xy(SUMP_Z - 9.0), name="Drain")
    drain.circle((0, SUMP_Y1 - 40.0), d=DRAIN_D)
    p.extrude(drain.add(), 16.0, mode="cut", name="Drain")
    casting_finish(p, "steel", (0.22, 0.23, 0.24))
    p.frame("rail", origin=(0, 0, PAN_RAIL), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("drain", origin=(0, SUMP_Y1 - 40.0, SUMP_Z - 8.0), z=(0, 0, -1),
            x=(1, 0, 0))
    p.check()
    return p


def sump_gasket(app):
    """The pan gasket: the rail's own outline, 2 mm of cork."""
    p = app.part("sump gasket")
    sk = p.sketch(Plane.xy(-SUMP_GASKET_T), name="Gasket")
    sk.rect((-CASE_X, BLOCK_Y0), (CASE_X, BLOCK_Y1))
    sk.rect((-(CASE_X - 18.0), BLOCK_Y0 + 18.0),
            (CASE_X - 18.0, BLOCK_Y1 - 18.0))
    p.extrude(sk.add(), SUMP_GASKET_T, mode="new_body", name="Gasket")
    holes = p.sketch(Plane.xy(-3.0), name="Bolt holes")
    for y in _pan_bolt_y():
        for sx in (-1, 1):
            holes.circle((sx * PAN_BOLT_X, y), d=9.0)  # M8 Normal
    p.extrude(holes.add(), 4.0, mode="cut", name="Bolt holes")
    p.material("rubber")
    # Its origin is its TOP face, the one against the block's rail.
    p.frame("rail", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def drain_plug(app):
    p = app.part("drain plug")
    p.revolve_profile([(0, 0), (DRAIN_D / 2.0, 0), (DRAIN_D / 2.0, 12.0),
                       (14.0, 12.0), (14.0, 20.0), (0, 20.0)],
                      plane=Plane.xz(), axis="z", name="Plug")
    p.material("steel")
    p.frame("head", origin=(0, 0, 20.0), z=(0, 0, -1), x=(1, 0, 0))
    p.check()
    return p


# ------------------------------------------------------- the timing drive
def sprocket(app, name, radius, bore_d, teeth, width, hub_d=None,
             phase_deg=0.0):
    """A chain sprocket: a rim, drawn teeth, and a bore.

    The teeth are drawn because the ratio is the RELATION's job, not this
    geometry's — but a sprocket with no teeth beside a chain is a disc, and
    the point of drawing the drive is that you can see it is a drive.
    """
    p = app.part(name)
    body = p.sketch(Plane.xz(width / 2.0), name="Rim")
    body.circle((0, 0), d=2 * radius)
    p.extrude(body.add(), width, mode="new_body", name="Rim")
    tooth_d = 2 * (tooth_tip(radius, teeth) - radius)
    ring = p.sketch(Plane.xz(width / 2.0), name="Teeth")
    for i in range(teeth):
        a = 2 * math.pi * i / teeth + math.radians(phase_deg)
        ring.circle((radius * math.sin(a), radius * math.cos(a)), d=tooth_d)
    p.extrude(ring.add(), width, mode="add", name="Teeth")
    if hub_d:
        hub = p.sketch(Plane.xz(width / 2.0 + 10.0), name="Hub")
        hub.circle((0, 0), d=hub_d)
        p.extrude(hub.add(), width / 2.0 + 10.0, mode="add", name="Hub")
    bore = p.sketch(Plane.xz(width + 12.0), name="Bore")
    bore.circle((0, 0), d=bore_d)
    p.extrude(bore.add(), 2 * width + 24.0, mode="cut", name="Bore")
    p.material("steel")
    p.frame("hub", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.check()
    return p


def chain(app, name, span, tip_a, tip_b, width, thickness=CHAIN_T):
    """A roller chain, as the band that wraps both sprockets.

    Two arcs and two tangents, which is what a chain path IS — and the
    tangents are computed rather than eyeballed: for circles of radius a and
    b whose centres are `span` apart, the external tangent's normal makes
    asin((b - a) / span) with the centre line and touches each circle there.

    FROZEN geometry: the RELATION is what makes the camshaft turn at half
    crank speed, and a chain whose links follow their sprockets is out of
    scope. So it is drawn with running clearance on the tooth tips rather
    than sitting on them — drawn where it really rides, it would be reported
    as a clash at every sample of the study, which would be a true statement
    about a deliberate simplification.
    """
    p = app.part(name)
    r1, r2 = tip_a + CHAIN_CLEAR, tip_b + CHAIN_CLEAR

    def loop(sk, grow):
        a, b = r1 + grow, r2 + grow
        beta = math.asin((b - a) / span)
        n = (math.cos(beta), math.sin(beta))
        deg = math.degrees(beta)
        sk.arc((0.0, 0.0), a, 180.0 - deg, 360.0 + deg)
        sk.line((a * n[0], a * n[1]), (b * n[0], span + b * n[1]))
        sk.arc((0.0, span), b, deg, 180.0 - deg)
        sk.line((-b * n[0], span + b * n[1]), (-a * n[0], a * n[1]))

    band = p.sketch(Plane.xz(width / 2.0), name="Chain")
    loop(band, thickness)   # outer
    loop(band, 0.0)         # inner: NESTED, which a sketch allows — it is a
                            # hole, not an overlap
    p.extrude(band.add(), width, mode="new_body", name="Chain")
    p.material("steel")
    p.frame("crank_end", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.check()
    return p


def cover_inner_x(z):
    """Half-width of the chain case's CAVITY at height z — the same outline
    `_cover_outline` draws, offset by its own wall. Anything that lives
    inside the case measures itself against this."""
    k = COVER_WALL
    rows = [(PAN_RAIL - 1.0, 100.0 - k), (110.0, 100.0 - k),
            (130.0, 88.0 - k), (COVER_TOP - 30.0, 88.0 - k),
            (COVER_TOP - k, 40.0)]
    if z <= rows[0][0]:
        return rows[0][1]
    for (z0, x0), (z1, x1) in zip(rows, rows[1:]):
        if z <= z1:
            return x0 + (x1 - x0) * (z - z0) / (z1 - z0)
    return rows[-1][1]


RAIL_GAP = 1.5      # running clearance between a rail's shoe and the chain
RAIL_RIB = 9.0      # how thick the rail is at its ends, before the rib


def chain_run_x(z, span, tip_a, tip_b, thickness=CHAIN_T):
    """Where the drawn chain's OUTER surface is at height z, on the +x run.

    The same tangent `chain()` draws, solved for x — so a rail placed against
    it cannot be inside the chain it guides. Placed by eye, both rails were:
    17 cm^3 of guide rail in the chain, each.
    """
    a, b = tip_a + CHAIN_CLEAR + thickness, tip_b + CHAIN_CLEAR + thickness
    beta = math.asin((b - a) / span)
    c, sn = math.cos(beta), math.sin(beta)
    p1, p2 = (a * c, a * sn), (b * c, span + b * sn)
    return p1[0] + (p2[0] - p1[0]) * (z - p1[1]) / (p2[1] - p1[1])


def chain_rail(app, name, z0, z1, side, span, tip_a, tip_b, bow, width):
    """A chain guide: a plastic-faced rail along one run of the chain, on a
    pivot at its lower end.

    Its SHOE is the chain's own tangent line stood off by RAIL_GAP — it has
    to be straight, because the chain here is frozen geometry and a shoe
    bowed into it is a shoe through it. The `bow` is on the BACK instead,
    which is where a real rail carries its stiffening rib anyway: thick in
    the middle, thin at the ends where it is held.
    """
    p = app.part(name)
    sk = p.sketch(Plane.xz(width / 2.0), name="Rail")
    n = 9
    face, back = [], []
    for i in range(n):
        f = i / (n - 1.0)
        z = z0 + (z1 - z0) * f
        shoe = chain_run_x(z, span, tip_a, tip_b) + RAIL_GAP
        rib = RAIL_RIB + bow * math.sin(math.pi * f)
        # ...and the rib stops at the chain case's own wall. It is the only
        # thing out there, and a rail bowed 14 mm proud of a shoe already
        # 63 mm out is 4 mm into the casting for most of its length.
        room = cover_inner_x(z) - RAIL_GAP
        face.append((side * shoe, z))
        back.append((side * min(shoe + rib, room), z))
    sk.polyline(face + list(reversed(back)))
    p.extrude(sk.add(), width, mode="new_body", name="Rail")
    p.material("rubber")
    # Drawn in ENGINE coordinates like the chain it guides, so a frame that
    # only translates puts it where it was drawn.
    p.frame("plane", origin=(0, 0, 0), z=(0, 1, 0), x=(1, 0, 0))
    p.frame("pivot", origin=(side * (chain_run_x(z0, span, tip_a, tip_b)
                                     + RAIL_GAP + RAIL_RIB / 2.0), 0, z0),
            z=(0, 1, 0), x=(1, 0, 0))

    p.check()
    return p


# ------------------------------------------------------------- the oil pump
# The gear housing, front face first. Both ends are constrained: its front
# has to stay inside the chain case's cavity, which begins 10 mm behind the
# cover's front face, and its rear has to be clear of the driven sprocket at
# OIL_DRIVE_Y. At (-252, -228) the pump's own cover was inside the cover's
# front wall and the sprocket inside the pump.
OIL_BODY_Y = (-246.0, -224.0)
# Two gears in mesh, the driven one on the chain's own centreline. Their
# centre distance is twice a tooth's TIP radius, so they mesh rather than
# merely sit near each other — derived, because the tooth height is.
OIL_GEAR_R = OIL_GEAR_D / 2.0 - 3.0
OIL_GEAR_TIP = OIL_GEAR_R * (1.0 + math.pi * 0.62 / 10.0)
OIL_GEAR_X = (0.0, 2.0 * OIL_GEAR_TIP)
OIL_BRACKET_Z = (OIL_PUMP_Z + 6.0, OIL_PUMP_Z + 18.0)
# The two arms straddle the drive chain, so where they START is the chain's
# own wrap round the pump sprocket — not a number. At 26 they were 6 mm
# inside it, and only at the arms' own height, which is why nothing showed.
OIL_ARM_X = (chain_outer(OIL_PCD, OIL_PUMP_TEETH, OIL_CHAIN_T) + 2.0,
             chain_outer(OIL_PCD, OIL_PUMP_TEETH, OIL_CHAIN_T) + 18.0)
PICKUP_Y = -230.0                 # inside the sump's front wall, not in it
# ...and OUTBOARD of the driven sprocket, whose teeth reach 24 mm from the
# pump's axis: down the middle it was a tube through a sprocket.
PICKUP_X = OIL_GEAR_X[1] + OIL_GEAR_TIP + 2.0


def oil_pump(app):
    """A gear pump hung off the block's front face, inside the chain case,
    driven by its own chain from the crank.

    The bracket that carries it is TWO arms out at |x| 30..46 rather than one
    plate: the drive chain runs down the middle of that span, and a plate
    across it is a bracket through a chain.
    """
    p = app.part("oil pump")
    y0, y1 = OIL_BODY_Y
    body = p.sketch(Plane.xz(y1), name="Body")
    body.rect((-OIL_GEAR_TIP - 8.0, OIL_PUMP_Z - OIL_BODY_H),
              (OIL_GEAR_X[1] + OIL_GEAR_TIP + 8.0, OIL_PUMP_Z + OIL_BODY_H))
    p.extrude(body.add(), y1 - y0, mode="new_body", name="Body")
    for sx in (-1, 1):
        arm = p.sketch(Plane.xz(BLOCK_Y0), name="Bracket arm")
        arm.rect((sx * OIL_ARM_X[0], OIL_BRACKET_Z[0]),
                 (sx * OIL_ARM_X[1], OIL_BRACKET_Z[1]))
        p.extrude(arm.add(), BLOCK_Y0 - y1, mode="add", name="Bracket arm")
    # Two cuts, not one sketch with two circles: the pockets OVERLAP, which
    # is what makes it a gear pump, and overlapping loops in one profile are
    # refused ("loops overlap or nest inconsistently") rather than unioned.
    for i, x in enumerate(OIL_GEAR_X):
        # THROUGH the body, from its own rear face: cut 2 mm in and 0.4
        # proud and the 20 mm gears stood 1 mm out of the pocket into the
        # back wall. The pump's cover is what closes the front.
        pk = p.sketch(Plane.xz(y1), name=f"Gear pocket {i + 1}")
        pk.circle((x, OIL_PUMP_Z), d=2 * OIL_GEAR_TIP + 2 * FIT)
        p.extrude(pk.add(), y1 - y0, mode="cut", name=f"Gear pocket {i + 1}")
    shafts = p.sketch(Plane.xz(y1 + 20.0), name="Shaft bores")
    for x in OIL_GEAR_X:
        shafts.circle((x, OIL_PUMP_Z), d=16.4)
    p.extrude(shafts.add(), 60.0, mode="cut", name="Shaft bores")
    holes = p.sketch(Plane.xz(BLOCK_Y0), name="Mount holes")
    for sx in (-1, 1):
        holes.circle((sx * sum(OIL_ARM_X) / 2.0,
                      OIL_BRACKET_Z[0] + 6.0), d=9.0)
    p.extrude(holes.add(), BLOCK_Y0 - y1, mode="cut", name="Mount holes")
    # The pickup's mouth, in the bottom of the body.
    inlet = p.sketch(Plane.xz(y1 - 6.0), name="Inlet")
    inlet.circle((PICKUP_X, OIL_PUMP_Z - OIL_BODY_H), d=18.0)
    p.extrude(inlet.add(), 12.0, mode="cut", name="Inlet")
    p.material("aluminum")
    p.frame("mount", origin=(0, BLOCK_Y0, OIL_PUMP_Z), z=(0, 0, 1),
            x=(1, 0, 0))
    p.frame("drive", origin=(0, OIL_DRIVE_Y, OIL_PUMP_Z), z=(0, 1, 0),
            x=(1, 0, 0))
    p.frame("idler", origin=(OIL_GEAR_X[1], (y0 + y1) / 2.0, OIL_PUMP_Z),
            z=(0, 1, 0), x=(1, 0, 0))
    p.frame("driver", origin=(0, (y0 + y1) / 2.0, OIL_PUMP_Z), z=(0, 1, 0),
            x=(1, 0, 0))
    p.frame("pickup", origin=(PICKUP_X, PICKUP_Y,
                              OIL_PUMP_Z - OIL_BODY_H), z=(0, 0, -1),
            x=(1, 0, 0))
    p.frame("cover", origin=(0, y0, OIL_PUMP_Z), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


OIL_GEAR_TEETH = 10


def oil_pump_gear(app, name, phase_deg=0.0):
    """One of the pump's two gears. Ten teeth, drawn — a gear pump with
    smooth rotors is a pair of discs that pumps nothing.

    The pair runs at a centre distance of OIL_GEAR_X, which is twice their
    TIP radius: they mesh, so the second one is drawn half a tooth out of
    phase and kept there by the gear relation. In phase they would be two
    solids in the same place at every angle, which the study would report
    and be right about.
    """
    return sprocket(app, name, OIL_GEAR_R, 12.0, OIL_GEAR_TEETH,
                    OIL_GEAR_W, phase_deg=phase_deg)


def oil_pump_cover(app):
    p = app.part("oil pump cover")
    sk = p.sketch(Plane.xz(OIL_BODY_Y[0]), name="Cover")
    sk.rect((-OIL_GEAR_TIP - 8.0, OIL_PUMP_Z - OIL_BODY_H),
            (OIL_GEAR_X[1] + OIL_GEAR_TIP + 8.0, OIL_PUMP_Z + OIL_BODY_H))
    p.extrude(sk.add(), 4.0, mode="new_body", name="Cover")
    p.material("aluminum")
    p.frame("face", origin=(0, OIL_BODY_Y[0], OIL_PUMP_Z), z=(0, 0, 1),
            x=(1, 0, 0))
    p.check()
    return p


def oil_pickup(app):
    """The strainer and the tube up to the pump: drawn in engine coordinates,
    because both of its ends are fixed to things that are."""
    p = app.part("oil pickup")
    x = PICKUP_X
    top = OIL_PUMP_Z - OIL_BODY_H
    bottom = SUMP_Z + 28.0
    disc(p, (x, PICKUP_Y), 18.0, bottom, top, "Tube", mode="new_body")
    # The strainer lies in the OIL, which means inside the pan's own walls —
    # hung off the pump's front face it was through the pan's nose.
    strain = p.sketch(Plane.xy(bottom - 10.0), name="Strainer")
    hw = pan_inner_half_width(bottom - 10.0) - 4.0
    strain.rect((-hw, PICKUP_Y - 12.0), (hw, PICKUP_Y + 22.0))
    p.extrude(strain.add(), 10.0, mode="add", name="Strainer")
    p.material("steel")
    p.frame("flange", origin=(x, PICKUP_Y, top), z=(0, 0, -1), x=(1, 0, 0))
    p.check()
    return p


# ------------------------------------------- the chain case and the cooling
def _cover_outline(sk, inset=0.0, open_floor=False):
    """The front cover's silhouette: crankcase width at the bottom, deck
    width up the middle, and tapering in over the cam sprocket.

    The CAVITY is open at the bottom — inset everywhere but there. A chain
    case drains into the sump, which is directly under it; and a floor there
    is 6 mm of aluminium exactly where the oil pump's driven sprocket has to
    hang.
    """
    k = inset
    floor = PAN_RAIL - 1.0 if open_floor else PAN_RAIL + k
    radius = COVER_TOP - CAM_Z - k
    sk.polyline([(-100+k, floor), (100-k, floor), (100-k, 110),
                 (88-k, 130), (88-k, CAM_Z-20), (radius, CAM_Z)],
                close=False)
    # A concentric crown clears the actual chain envelope and has a constant
    # radial wall, instead of the old house-shaped polygon across the top.
    sk.arc((0, CAM_Z), radius, 0, 180)
    sk.polyline([(-radius, CAM_Z), (-88+k, CAM_Z-20), (-88+k, 130),
                 (-100+k, 110), (-100+k, floor)], close=False)


def timing_cover(app):
    """The chain case: a one-piece cover from the pan rail to above the cam
    sprocket, carrying the front crank seal in its front wall."""
    p = app.part("timing cover")
    back = BLOCK_Y0 - COVER_GASKET_T
    sk = p.sketch(Plane.xz(back), name="Cover")
    _cover_outline(sk)
    p.extrude(sk.add(), back - COVER_Y0, mode="new_body", name="Cover")
    hollow = p.sketch(Plane.xz(back), name="Cavity")
    _cover_outline(hollow, inset=COVER_WALL, open_floor=True)
    p.extrude(hollow.add(), back - COVER_Y0 - COVER_FRONT_T, mode="cut",
              name="Cavity")
    nose = p.sketch(Plane.xz(COVER_Y0 + COVER_FRONT_T + 1.0), name="Nose bore")
    nose.circle((0, 0), d=70.0)
    p.extrude(nose.add(), COVER_FRONT_T + 2.0, mode="cut", name="Nose bore")
    wp = p.sketch(Plane.xz(COVER_Y0 + COVER_FRONT_T + 1.0), name="Water pump")
    wp.circle((0, WATER_PUMP_Z), d=60.0)
    p.extrude(wp.add(), COVER_FRONT_T + 2.0, mode="cut", name="Water pump")
    holes = p.sketch(Plane.xz(back), name="Bolt holes")
    for x, z in COVER_BOLTS + COVER_BOLTS_HEAD:
        holes.circle((x, z), d=9.0)
    p.extrude(holes.add(), back - COVER_Y0, mode="cut", name="Bolt holes")
    casting_finish(p)
    p.frame("face", origin=(0, back, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("front_seal", origin=(0, FRONT_SEAL_Y, 0), z=(0, 1, 0),
            x=(1, 0, 0))
    p.frame("water_pump", origin=(0, COVER_Y0, WATER_PUMP_Z), z=(0, 0, 1),
            x=(1, 0, 0))
    p.check()
    return p


def timing_cover_gasket(app):
    p = app.part("timing cover gasket")
    sk = p.sketch(Plane.xz(BLOCK_Y0), name="Gasket")
    _cover_outline(sk)
    _cover_outline(sk, inset=16.0)
    p.extrude(sk.add(), COVER_GASKET_T, mode="new_body", name="Gasket")
    holes = p.sketch(Plane.xz(BLOCK_Y0 + 1.0), name="Bolt holes")
    for x, z in COVER_BOLTS + COVER_BOLTS_HEAD:
        holes.circle((x, z), d=9.0)
    p.extrude(holes.add(), COVER_GASKET_T + 2.0, mode="cut", name="Bolt holes")
    p.material("rubber")
    p.frame("face", origin=(0, BLOCK_Y0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


WP_BODY_Y = (-274.0, COVER_Y0)
WP_SHAFT_Y = (-292.0, -262.0)
WP_SHAFT_D = 18.0


def water_pump(app):
    """Housing, impeller, shaft and pulley — belt driven off the crank
    damper, which is why its pulley shares the damper's own belt plane."""
    p = app.part("water pump")
    y0, y1 = WP_BODY_Y
    b = p.sketch(Plane.xz(y1), name="Housing")
    b.circle((0, WATER_PUMP_Z), d=110.0)
    p.extrude(b.add(), y1 - y0, mode="new_body", name="Housing")
    snout = p.sketch(Plane.xz(y1), name="Outlet")
    snout.rect((-24.0, WATER_PUMP_Z - 50.0), (24.0, WATER_PUMP_Z))
    p.extrude(snout.add(), y1 - y0, mode="add", name="Outlet")
    vol = p.sketch(Plane.xz(y1 - 1.0), name="Volute")
    vol.circle((0, WATER_PUMP_Z), d=IMPELLER_D + 8.0)
    p.extrude(vol.add(), y1 - y0 - 2.0, mode="cut", name="Volute")
    bore = p.sketch(Plane.xz(y1 + 1.0), name="Shaft bore")
    bore.circle((0, WATER_PUMP_Z), d=WP_SHAFT_D + FIT)
    p.extrude(bore.add(), y1 - y0 + 2.0, mode="cut", name="Shaft bore")
    p.material("aluminum")
    p.frame("face", origin=(0, y1, WATER_PUMP_Z), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("shaft", origin=(0, sum(WP_SHAFT_Y) / 2.0, WATER_PUMP_Z),
            z=(0, 1, 0), x=(1, 0, 0))
    p.check()
    return p


def water_pump_shaft(app):
    """The pump shaft, with a frame where each thing on it goes: the impeller
    inside the volute, the pulley out in the belt's own plane."""
    y0, y1 = WP_SHAFT_Y
    mid = (y0 + y1) / 2.0
    p = ring(app, "water pump shaft", WP_SHAFT_D, 0.0, y1 - y0,
              material="steel")
    p.frame("impeller", origin=(0, 0, IMPELLER_Y - mid), z=(0, 0, 1),
            x=(1, 0, 0))
    p.frame("pulley", origin=(0, 0, BELT_Y - mid), z=(0, 0, 1), x=(1, 0, 0))
    return p


IMPELLER_Y = -267.0


def impeller(app):
    """A six-vane impeller. The vanes are what make it a pump, so they are
    drawn rather than implied by a disc."""
    p = app.part("impeller")
    hub = p.sketch(Plane.xy(-5.0), name="Back plate")
    hub.circle((0, 0), d=IMPELLER_D)
    p.extrude(hub.add(), 4.0, mode="new_body", name="Back plate")
    vanes = p.sketch(Plane.xy(-1.0), name="Vanes")
    for i in range(6):
        a = 2 * math.pi * i / 6
        c, s = math.cos(a), math.sin(a)
        pts = [(10 * c - 2.5 * s, 10 * s + 2.5 * c),
               (IMPELLER_D / 2.0 * c - 2.5 * s, IMPELLER_D / 2.0 * s + 2.5 * c),
               (IMPELLER_D / 2.0 * c + 2.5 * s, IMPELLER_D / 2.0 * s - 2.5 * c),
               (10 * c + 2.5 * s, 10 * s - 2.5 * c)]
        vanes.polyline(pts)
    p.extrude(vanes.add(), 6.0, mode="add", name="Vanes")
    boss = p.sketch(Plane.xy(-5.0), name="Boss")
    boss.circle((0, 0), d=32.0)
    p.extrude(boss.add(), 10.0, mode="add", name="Boss")
    bore = p.sketch(Plane.xy(-6.0), name="Bore")
    bore.circle((0, 0), d=WP_SHAFT_D)
    p.extrude(bore.add(), 20.0, mode="cut", name="Bore")
    p.material("brass")
    p.frame("hub", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


def vee_pulley(app, name, d, bore_d, width=16.0):
    """A single-groove V pulley, turned."""
    p = app.part(name)
    p.revolve_profile([(bore_d / 2.0, 0), (d / 2.0, 0), (d / 2.0, width),
                       (bore_d / 2.0, width)], plane=Plane.xz(), axis="z",
                      name=name)
    g = p.sketch(Plane.xz(), name="V groove")
    g.polyline([(d / 2.0 + 1.0, width / 2.0 - BELT_GROOVE_H),
                (d / 2.0 - BELT_GROOVE_D, width / 2.0),
                (d / 2.0 + 1.0, width / 2.0 + BELT_GROOVE_H)])
    p.revolve(g.add(), axis="z", mode="cut", name="V groove")
    p.material("steel")
    p.frame("hub", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("belt", origin=(0, 0, width / 2.0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p


# --------------------------------------------------------- how it is held
MOUNT_W = 52.0            # the bracket's width along the engine
MOUNT_REACH = 52.0        # how far its foot sticks out past the block


def engine_mount(app, side):
    """A cast mounting bracket: a plate against the crankcase's own pad and a
    foot standing out from it with the rubber mount's bolt through it.

    Drawn once and placed twice — the frame it mates to is the mirror, which
    is why its own x runs OUTWARD from the block rather than along +X.
    """
    p = app.part(f"engine mount {side}")
    x0 = CASE_X
    sec = p.sketch(Plane.xz(MOUNT_Y + MOUNT_W / 2.0), name="Bracket")
    sec.polyline([(x0, 62.0), (x0 + 9.0, 62.0), (x0 + 9.0, 8.0),
                  (x0 + MOUNT_REACH, 8.0), (x0 + MOUNT_REACH, -14.0),
                  (x0, -14.0)])
    p.extrude(sec.add(), MOUNT_W, mode="new_body", name="Bracket")
    for dy, z in MOUNT_BOLTS:
        bore(p, (x0 + 10.0, MOUNT_Y + dy, z), (x0 - 1.0, MOUNT_Y + dy, z),
             MOUNT_BOLT_D, f"Bolt hole {dy}", over=0.0)
    m = p.sketch(Plane.xy(-16.0), name="Mount hole")
    m.circle((x0 + MOUNT_REACH - 18.0, MOUNT_Y), d=20.0)
    p.extrude(m.add(), 28.0, mode="cut", name="Mount hole")
    p.material("steel")
    p.frame("pad", origin=(x0, MOUNT_Y, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.check()
    return p
