#!/usr/bin/env python3
"""One builder per part of the PES4M pump. Every builder takes the SDK app
and returns the finished Part; every number comes from pes_spec."""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "tools"))
from designer_sdk import Plane, V  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gear import involute_profile  # noqa: E402
from pes_spec import *  # noqa: E402,F401,F403


# --- vocabulary ---------------------------------------------------------------

def _snap(z):
    return struct.unpack("f", struct.pack("f", z))[0]


def slab(p, x0, y0, z0, x1, y1, z1, name, mode="add"):
    """A block drawn on its BOTTOM face and extruded up."""
    sk = p.sketch(Plane.xy(z0), name=name)
    sk.rect((x0, y0), (x1, y1))
    return p.extrude(sk.add(), _snap(z1) - _snap(z0), mode=mode, name=name)


def rounded_rect(sk, x0, y0, x1, y1, r):
    """A rectangle with its four corners rounded to radius r."""
    sk.line((x0 + r, y0), (x1 - r, y0))
    sk.arc((x1 - r, y0 + r), r, 270.0, 360.0)
    sk.line((x1, y0 + r), (x1, y1 - r))
    sk.arc((x1 - r, y1 - r), r, 0.0, 90.0)
    sk.line((x1 - r, y1), (x0 + r, y1))
    sk.arc((x0 + r, y1 - r), r, 90.0, 180.0)
    sk.line((x0, y1 - r), (x0, y0 + r))
    sk.arc((x0 + r, y0 + r), r, 180.0, 270.0)


def round_slab(p, x0, y0, z0, x1, y1, z1, r, name, mode="add"):
    """`slab` with its vertical edges rounded: how a casting is shaped."""
    sk = p.sketch(Plane.xy(z0), name=name)
    rounded_rect(sk, x0, y0, x1, y1, r)
    return p.extrude(sk.add(), _snap(z1) - _snap(z0), mode=mode, name=name)


def axis_plane(start, direction):
    dd = V(direction).unit()
    u = V((0, 1, 0)) if abs(dd.dot(V((0, 1, 0)))) < 0.9 else V((1, 0, 0))
    u = (u - dd * u.dot(dd)).unit()
    return Plane(start, tuple(dd), tuple(u))


def rod(p, start, end, d, name, mode="add"):
    """A round column from `start` to `end`, any direction."""
    direction = V(end) - V(start)
    sk = p.sketch(axis_plane(start, direction), name=name)
    sk.circle((0, 0), d=d)
    return p.extrude(sk.add(), direction.length(), mode=mode, name=name)


def turned(p, profile, name, mode="new_body"):
    """A turned part about the pump-vertical Z axis: (r, z) points."""
    return p.revolve_profile(profile, plane=Plane.xz(), axis="z", name=name,
                             mode=mode)


def turned_x(p, profile, name, mode="new_body"):
    """A turned part about the X axis: (x, r) points."""
    return p.revolve_profile(profile, plane=Plane.xz(), axis="x", name=name,
                             mode=mode)


def coil(p, z0, z1, mean_d, wire, turns, name, origin_xy=(0.0, 0.0)):
    """A closed-end-free helical spring standing on z0, top wire at z1."""
    pitch = (z1 - z0 - wire) / turns
    p.app.add_feature(feature={
        "type": "Curve3D", "name": f"{name} path", "kind": "helix",
        "origin": [origin_xy[0], origin_xy[1], z0 + wire / 2.0],
        "axis": [0, 0, 1], "radius": mean_d / 2.0, "pitch": pitch,
        "turns": turns})
    path = p.state()["features"][-1]["id"]
    p.app.add_feature(feature={"type": "Pipe", "name": name, "path_id": path,
                               "outer_diameter": wire, "mode": "new_body"})


def coil_x(p, x0, x1, mean_d, wire, turns, name):
    """The same, along +X about the local origin."""
    pitch = (x1 - x0 - wire) / turns
    p.app.add_feature(feature={
        "type": "Curve3D", "name": f"{name} path", "kind": "helix",
        "origin": [x0 + wire / 2.0, 0, 0], "axis": [1, 0, 0],
        "radius": mean_d / 2.0, "pitch": pitch, "turns": turns})
    path = p.state()["features"][-1]["id"]
    p.app.add_feature(feature={"type": "Pipe", "name": name, "path_id": path,
                               "outer_diameter": wire, "mode": "new_body"})


def finish(p, material, color, roughness=0.45, metallic=0.9):
    p.material(material)
    p.app.set_appearance(body=0, color=list(color), roughness=roughness,
                         metallic=metallic)


# Linear, not sRGB: the app's colours are linear, so 0.6 renders near white.
CASTING = (0.17, 0.175, 0.17)
STEEL = (0.22, 0.23, 0.25)
BLACK = (0.012, 0.012, 0.014)
BRASS = (0.45, 0.28, 0.07)
RUBBER = (0.02, 0.02, 0.02)


def hexagon(sk, af, centre=(0.0, 0.0), flats_on_u=True):
    """A hexagon `af` across flats; flats face +-U when flats_on_u."""
    r = af / math.sqrt(3.0)
    off = 30.0 if flats_on_u else 0.0
    sk.polyline([(centre[0] + r * math.cos(math.radians(off + 60 * k)),
                  centre[1] + r * math.sin(math.radians(off + 60 * k)))
                 for k in range(6)])


# --- the housing ----------------------------------------------------------------

GOV_Y = (RACK_Y - MODULE + RACK_BACK) / 2.0
GOV_Z = (GEAR_Z0 + GEAR_Z1) / 2.0
UP_X0, UP_X1 = H_X0 + 6.0, H_X1           # the element block
RACK_SLOT = (RACK_Y - MODULE - 0.5, RACK_BACK + 0.5,
             GEAR_Z0 - 0.4, GEAR_Z1 + 0.4)  # y0, y1, z0, z1
CAP_X0 = UP_X0 - 20.0                     # the rack's closed end cap
FEED_BORE_D = 14.2
PRIMER_Y = -50.0


def housing(app):
    p = app.part("pump housing")
    # Cam case, element block, drive flange with its register, rear cover.
    round_slab(p, H_X0, H_Y0, H_Z0, H_X1, H_Y1, 40.0, 6.0, "Cam case",
               mode="new_body")
    round_slab(p, UP_X0, UP_Y0, 30.0, UP_X1, UP_Y1, H_TOP, 5.0,
               "Element block")
    # Mounting foot along the bottom, and ribs from it up the cam case.
    round_slab(p, H_X0 + 4.0, H_Y0 - 10.0, H_Z0, H_X1 - 4.0, H_Y1 + 10.0,
               H_Z0 + 7.0, 4.0, "Mounting foot")
    for x in (-24.0, 24.0):
        slab(p, x - 3.0, H_Y1 - 1.0, H_Z0 + 7.0, x + 3.0, H_Y1 + 6.0,
             H_Z0 + 32.0, f"Rib {x:+.0f}")
    # The deck is machined down between the elements, leaving a boss round
    # every delivery-valve holder.
    slab(p, UP_X0 - 1.0, UP_Y0 - 1.0, H_TOP - 6.0, UP_X1 + 1.0, UP_Y1 + 1.0,
         H_TOP + 1.0, "Deck", mode="cut")
    sk = p.sketch(Plane.xy(H_TOP - 6.5), name="Holder bosses")
    for x in EL_X:
        sk.circle((x, 0), d=23.5)
    p.extrude(sk.add(), 6.5, mode="add", name="Holder bosses")
    # The fuel inlet boss on -Y, cross-drilled into the gallery.
    rod(p, (UP_X1 - 12.0, UP_Y0 + 1.0, PORT_Z), (UP_X1 - 12.0, UP_Y0 - 5.0,
        PORT_Z), 15.0, "Inlet boss")
    turned_x(p, [(H_X0 - 10.0, 0), (H_X0 - 10.0, 48.0), (H_X0, 48.0),
                 (H_X0, 0)], "Drive flange", mode="add")
    turned_x(p, [(H_X0 - 13.0, 0), (H_X0 - 13.0, 30.0), (H_X0 - 10.0, 30.0),
                 (H_X0 - 10.0, 0)], "Register", mode="add")
    turned_x(p, [(H_X1, 0), (H_X1, 20.0), (H_X1 + 12.0, 20.0),
                 (H_X1 + 12.0, 0)], "Rear bearing cover", mode="add")
    y0, y1, z0, z1 = RACK_SLOT
    slab(p, CAP_X0, y0 - 3.0, z0 - 3.0, UP_X0 + 1.0, y1 + 3.0, z1 + 3.0,
         "Rack end cap")
    # Cavities.
    slab(p, -50.0, -GALLERY, -GALLERY, 50.0, GALLERY, GALLERY, "Cam gallery",
         mode="cut")
    for x0, x1 in ((H_X0 - 1.0, -50.0), (50.0, H_X1)):
        turned_x(p, [(x0, 0), (x0, BEARING_OD / 2.0), (x1, BEARING_OD / 2.0),
                     (x1, 0)], f"Bearing bore {x0:+.0f}", mode="cut")
    turned_x(p, [(H_X0 - 14.0, 0), (H_X0 - 14.0, 13.0), (H_X0, 13.0),
                 (H_X0, 0)], "Seal bore", mode="cut")
    turned_x(p, [(H_X1 - 1.0, 0), (H_X1 - 1.0, 6.8), (H_X1 + 9.0, 6.8),
                 (H_X1 + 9.0, 0)], "Stub pocket", mode="cut")
    # The element bores: one stage per sketch, all four elements at once.
    stages = [(10.0, TAPPET_TOP, TAPPET_BORE_D, "Tappet bores"),
              (TAPPET_TOP, WEB_Z0, SPRING_CHAMBER_D, "Spring chambers"),
              (WEB_Z0 - 1.0, RACK_GALLERY_Z0 + 1.0, 15.6, "Web bores"),
              (RACK_GALLERY_Z1 - 1.0, BARREL_COLLAR_Z, 12.3, "Barrel bores"),
              (BARREL_COLLAR_Z, BARREL_TOP, 17.2, "Collar bores"),
              (BARREL_TOP, H_TOP + 1.0, 18.2, "Holder bores")]
    for z0_, z1_, d, name in stages:
        sk = p.sketch(Plane.xy(z0_), name=name)
        for x in EL_X:
            sk.circle((x, 0), d=d)
        p.extrude(sk.add(), _snap(z1_) - _snap(z0_), mode="cut", name=name)
    # The rack gallery, open on +Y: the inspection window.
    slab(p, -49.0, -12.0, RACK_GALLERY_Z0, 49.0, UP_Y1 + 1.0, RACK_GALLERY_Z1,
         "Rack gallery", mode="cut")
    slab(p, CAP_X0 + 2.0, y0, z0, H_X1 + 1.0, y1, z1, "Rack slot", mode="cut")
    # The fuel gallery, drilled from the governor end, and its inlet.
    gy, gz = FUEL_GALLERY
    rod(p, (UP_X1 + 1.0, gy, gz), (UP_X0 + 6.0, gy, gz), 6.0, "Fuel gallery",
        mode="cut")
    rod(p, (UP_X1 - 12.0, UP_Y0 - 6.0, gz), (UP_X1 - 12.0, gy, gz), 5.0,
        "Inlet drilling", mode="cut")
    sk = p.sketch(Plane.xy(H_Z0 - 1.0), name="Foot holes")
    for x in (H_X0 + 12.0, H_X1 - 12.0):
        for y in (H_Y0 - 5.0, H_Y1 + 5.0):
            sk.circle((x, y), d=9.0)
    p.extrude(sk.add(), 9.0, mode="cut", name="Foot holes")
    # The feed pump's tappet bore through the -Y wall.
    rod(p, (ECC_X, H_Y0 - 1.0, 0), (ECC_X, -GALLERY + 1.0, 0), FEED_BORE_D,
        "Feed tappet bore", mode="cut")
    # Drive-flange bolt holes, and the bosses the window cover screws into.
    sk = p.sketch(Plane((H_X0, 0, 0), (-1, 0, 0), (0, 1, 0)),
                  name="Flange holes")
    for a in (90.0, 210.0, 330.0):
        sk.circle((39.0 * math.cos(math.radians(a)),
                   39.0 * math.sin(math.radians(a))), d=9.0)
    p.extrude(sk.add(), 10.0, mode="cut", name="Flange holes")
    p.frame("cam_axis", origin=(0, 0, 0), z=(1, 0, 0), x=(0, 1, 0))
    p.frame("bearing_drive", origin=(H_X0, 0, 0), z=(1, 0, 0), x=(0, 1, 0))
    p.frame("bearing_governor", origin=(50.0, 0, 0), z=(1, 0, 0), x=(0, 1, 0))
    p.frame("origin", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    for i, x in enumerate(EL_X):
        p.frame(f"el{i + 1}", origin=(x, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("rack", origin=(0, 0, 0), z=(-1, 0, 0), x=(0, 1, 0))
    p.frame("feed", origin=(ECC_X, 0, 0), z=(0, -1, 0), x=(1, 0, 0))
    p.frame("governor", origin=(0, GOV_Y, GOV_Z), z=(1, 0, 0), x=(0, 1, 0))
    finish(p, "aluminum", CASTING, roughness=0.7, metallic=0.6)
    p.check(solids=1)
    return p


def window_cover(app):
    p = app.part("inspection cover")
    slab(p, -51.0, UP_Y1, RACK_GALLERY_Z0 - 3.0, 51.0, UP_Y1 + 3.0,
         RACK_GALLERY_Z1 + 3.0, "Plate", mode="new_body")
    for x in (-44.0, 44.0):
        rod(p, (x, UP_Y1 + 3.0, (RACK_GALLERY_Z0 + RACK_GALLERY_Z1) / 2.0),
            (x, UP_Y1 + 6.0, (RACK_GALLERY_Z0 + RACK_GALLERY_Z1) / 2.0),
            8.0, f"Screw head {x:+.0f}")
    p.frame("origin", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    finish(p, "steel", CASTING, roughness=0.6, metallic=0.6)
    p.check(solids=1)
    return p


def bearing(app):
    p = app.part("camshaft bearing")
    turned_x(p, [(0, JOURNAL_D / 2.0 + 0.05), (0, BEARING_OD / 2.0 - 0.05),
                 (WALL, BEARING_OD / 2.0 - 0.05), (WALL, JOURNAL_D / 2.0 + 0.05)],
             "Bearing")
    p.frame("seat", origin=(0, 0, 0), z=(1, 0, 0), x=(0, 1, 0))
    finish(p, "steel", STEEL)
    p.check(solids=1)
    return p


# --- camshaft -------------------------------------------------------------------

def camshaft(app):
    """Journals and lobes; each lobe is the roller envelope of the SAME lift
    law its tappet's cam relation is given."""
    p = app.part("camshaft")
    r_j, r_s = JOURNAL_D / 2.0, SHAFT_D / 2.0
    turned_x(p, [(CAM_X0, 0), (CAM_X0, 5.0), (-84.0, 5.0), (-84.0, 6.5),
                 (-70.0, 8.5), (-50.0, r_j), (-49.0, r_j), (-49.0, r_s),
                 (49.0, r_s), (49.0, r_j), (H_X1, r_j), (H_X1, 6.0),
                 (CAM_X1, 6.0), (CAM_X1, 0)], "Shaft")
    for i, x in enumerate(EL_X):
        sk = p.sketch(Plane.yz(x - LOBE_W / 2.0), name=f"Lobe {i + 1}")
        # One closed cubic B-spline on the envelope samples: the curve lies
        # inside its (convex) control polygon, i.e. inside the law, by
        # microns — and the lobe is one smooth face, not 180 facets.
        pts = lobe_profile(i + 1)
        k = min(range(len(pts)), key=lambda j: pts[j][1])   # start on the base
        pts = pts[k:] + pts[:k]
        sk.entities.append({"type": "spline", "degree": 3,
                            "poles": [list(q) for q in pts + [pts[0]]]})
        p.extrude(sk.add(), LOBE_W, mode="add", name=f"Lobe {i + 1}")
    sk = p.sketch(Plane.yz(ECC_X - ECC_W / 2.0), name="Feed-pump eccentric")
    sk.circle(ECC_C0, r=ECC_R - CAM_ALLOW)
    p.extrude(sk.add(), ECC_W, mode="add", name="Feed-pump eccentric")
    # Key for the timing device.
    slab(p, -84.0, -1.5, 5.5, -72.0, 1.5, 8.0, "Key")
    p.frame("axle", origin=(0, 0, 0), z=(1, 0, 0), x=(0, 1, 0))
    finish(p, "steel", STEEL)
    p.check(solids=1)
    return p


# --- one element ------------------------------------------------------------------

def tappet(app):
    p = app.part("roller tappet")
    r = TAPPET_D / 2.0 - 0.05
    turned(p, [(0, TAPPET_BOT), (r, TAPPET_BOT), (r, TAPPET_TOP - 1.0),
               (r - 1.0, TAPPET_TOP), (0, TAPPET_TOP)], "Body")
    # The roller's fork, open at the bottom, and the pin bore.
    slab(p, -ROLLER_W / 2.0 - 0.3, -r - 1, TAPPET_BOT - 1.0,
         ROLLER_W / 2.0 + 0.3, r + 1, PITCH_BASE + ROLLER_R + 1.0, "Fork",
         mode="cut")
    rod(p, (-r - 1, 0, PITCH_BASE), (r + 1, 0, PITCH_BASE), 4.0, "Pin bore",
        mode="cut")
    # The adjusting screw's head, standing proud of the crown.
    p.frame("axis", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("pin", origin=(0, 0, PITCH_BASE), z=(1, 0, 0), x=(0, 1, 0))
    finish(p, "steel", STEEL)
    p.check(solids=1)
    return p


def tappet_pin(app):
    p = app.part("tappet pin")
    r = TAPPET_D / 2.0 - 0.3
    rod(p, (-r, 0, 0), (r, 0, 0), 4.0, "Pin", mode="new_body")
    p.frame("pin", origin=(0, 0, 0), z=(1, 0, 0), x=(0, 1, 0))
    finish(p, "steel", STEEL)
    p.check(solids=1)
    return p


def roller(app):
    p = app.part("tappet roller")
    turned_x(p, [(-ROLLER_W / 2.0, 2.05), (-ROLLER_W / 2.0, ROLLER_R),
                 (ROLLER_W / 2.0, ROLLER_R), (ROLLER_W / 2.0, 2.05)], "Roller")
    p.frame("axle", origin=(0, 0, 0), z=(1, 0, 0), x=(0, 1, 0))
    finish(p, "steel", (0.7, 0.71, 0.73), roughness=0.25)
    p.check(solids=1)
    return p


def spring_seat(app):
    p = app.part("lower spring seat")
    turned(p, [(4.2, TAPPET_TOP), (9.5, TAPPET_TOP), (9.5, SEAT_TOP),
               (4.2, SEAT_TOP)], "Seat")
    p.frame("axis", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    finish(p, "steel", STEEL)
    p.check(solids=1)
    return p


def plunger_spring(app):
    """Drawn at its SHORTEST, top dead centre, and carried by the tappet —
    a rigid part cannot be both lengths."""
    p = app.part("plunger spring")
    coil(p, SEAT_TOP + 0.02, WEB_Z0 - STROKE - 0.1,
         SPRING_OD - SPRING_WIRE, SPRING_WIRE, 4.5, "Spring")
    p.frame("axis", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    finish(p, "steel", (0.25, 0.26, 0.28))
    p.check(solids=1)
    return p


# The helix law (HELIX_SPAN, land_height, effective_stroke) lives in pes_spec.
HELIX_GROOVE_D = 2.4
PLUNGER_FRAME_DEG = HELIX_SPAN + 90.0


def plunger(app):
    p = app.part("plunger")
    r = PLUNGER_D / 2.0 - 0.01
    turned(p, [(0, PLUNGER_FOOT_Z), (4.0, PLUNGER_FOOT_Z), (4.0, 46.0),
               (r, 46.0), (r, PLUNGER_TOP), (0, PLUNGER_TOP)], "Plunger")
    # The drive vanes, on the frame's X axis.
    a = math.radians(PLUNGER_FRAME_DEG)
    ux, uy = math.cos(a), math.sin(a)
    vx, vy = -uy, ux
    sk = p.sketch(Plane.xy(VANE_Z0), name="Vanes")
    for s in (-1, 1):
        pts = []
        for (t, w) in ((2.5, -1), (VANE_R, -1), (VANE_R, 1), (2.5, 1)):
            pts.append((s * t * ux + w * VANE_W / 2.0 * vx,
                        s * t * uy + w * VANE_W / 2.0 * vy))
        sk.polyline(pts)
    p.extrude(sk.add(), VANE_Z1 - VANE_Z0, mode="add", name="Vanes")
    # The control helix: a helical groove rising counter-clockwise from local
    # angle 0 (the full-load end) to HELIX_SPAN (next to the vertical groove).
    z_top = PLUNGER_TOP - LAND_MIN - HELIX_GROOVE_D / 2.0
    z_bot = PLUNGER_TOP - LAND_MAX - HELIX_GROOVE_D / 2.0
    pitch = (z_top - z_bot) * 360.0 / HELIX_SPAN
    p.app.add_feature(feature={
        "type": "Curve3D", "name": "Helix path", "kind": "helix",
        "origin": [0, 0, z_bot], "axis": [0, 0, 1], "radius": r,
        "pitch": pitch, "turns": HELIX_SPAN / 360.0})
    path = p.state()["features"][-1]["id"]
    p.app.add_feature(feature={"type": "Pipe", "name": "Control helix",
                               "path_id": path,
                               "outer_diameter": HELIX_GROOVE_D,
                               "mode": "cut"})
    # The vertical groove from the top face down to the helix's upper end.
    g = math.radians(HELIX_SPAN)          # on the port at the stop position
    gx, gy = math.cos(g), math.sin(g)
    sk = p.sketch(Plane.xy(z_top - 0.6), name="Vertical groove")
    w = 0.7
    sk.polyline([(1.8 * gx - w * gy, 1.8 * gy + w * gx),
                 (3.5 * gx - w * gy, 3.5 * gy + w * gx),
                 (3.5 * gx + w * gy, 3.5 * gy - w * gx),
                 (1.8 * gx + w * gy, 1.8 * gy - w * gx)])
    p.extrude(sk.add(), PLUNGER_TOP - z_top + 1.6, mode="cut",
              name="Vertical groove")
    p.frame("axis", origin=(0, 0, 0), z=(0, 0, 1), x=(ux, uy, 0))
    finish(p, "steel", (0.78, 0.79, 0.80), roughness=0.2)
    p.check(solids=1)
    return p


def barrel(app):
    p = app.part("pump barrel")
    ri = PLUNGER_D / 2.0 + 0.01
    turned(p, [(ri, BARREL_Z0), (BARREL_OD / 2.0, BARREL_Z0),
               (BARREL_OD / 2.0, BARREL_COLLAR_Z),
               (BARREL_COLLAR_D / 2.0, BARREL_COLLAR_Z),
               (BARREL_COLLAR_D / 2.0, BARREL_TOP), (ri, BARREL_TOP)],
           "Barrel")
    rod(p, (0, -BARREL_COLLAR_D / 2.0 - 1.0, PORT_Z), (0, -1.0, PORT_Z),
        PORT_D, "Inlet port", mode="cut")
    p.frame("axis", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    finish(p, "steel", STEEL)
    p.check(solids=1)
    return p


def control_sleeve(app):
    p = app.part("control sleeve")
    turned(p, [(SLEEVE_ID / 2.0, SLEEVE_Z0), (SLEEVE_OD / 2.0, SLEEVE_Z0),
               (SLEEVE_OD / 2.0, SLEEVE_Z1), (SLEEVE_ID / 2.0, SLEEVE_Z1)],
           "Sleeve")
    ents, _ = involute_profile(MODULE, GEAR_TEETH, 20.0, backlash=BACKLASH)
    sk = p.sketch(Plane.xy(GEAR_Z0), name="Toothed quadrant")
    sk.entities.extend(ents)
    p.extrude(sk.add(), GEAR_Z1 - GEAR_Z0, mode="add", name="Toothed quadrant")
    sk = p.sketch(Plane.xy(GEAR_Z0 - 1.0), name="Quadrant bore")
    sk.circle((0, 0), d=SLEEVE_OD - 0.01)
    p.extrude(sk.add(), GEAR_Z1 - GEAR_Z0 + 2.0, mode="cut",
              name="Quadrant bore")
    # The sleeve is re-added through the quadrant so it stays one body.
    turned(p, [(SLEEVE_ID / 2.0, GEAR_Z0 - 0.5), (SLEEVE_OD / 2.0, GEAR_Z0 - 0.5),
               (SLEEVE_OD / 2.0, GEAR_Z1 + 0.5), (SLEEVE_ID / 2.0, GEAR_Z1 + 0.5)],
           "Clamp", mode="add")
    # The slots the plunger's vanes ride in, open at the bottom.
    slab(p, -SLEEVE_OD, -SLOT_W / 2.0, SLEEVE_Z0 - 1.0, SLEEVE_OD,
         SLOT_W / 2.0, SLOT_TOP, "Vane slots", mode="cut")
    p.frame("axis", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    finish(p, "steel", BRASS, roughness=0.35)
    p.check(solids=1)
    return p


SEAT_BODY_D = 18.0
VALVE_BORE_R = 2.25


def valve_seat(app):
    p = app.part("delivery valve seat")
    turned(p, [(VALVE_BORE_R, BARREL_TOP), (SEAT_BODY_D / 2.0, BARREL_TOP),
               (SEAT_BODY_D / 2.0, SEAT_BODY_Z1), (3.2, SEAT_BODY_Z1),
               (VALVE_BORE_R, SEAT_BODY_Z1 - 2.0)], "Seat body")
    p.frame("axis", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    finish(p, "steel", STEEL)
    p.check(solids=1)
    return p


VALVE_HEAD_TOP = SEAT_BODY_Z1 + 1.5
HOLDER_CAVITY_TOP = 121.5


def delivery_valve(app):
    """Guide stem, retraction collar, cone, and a spigot for its spring."""
    p = app.part("delivery valve")
    c = 0.05
    turned(p, [(0, BARREL_TOP + 2.0), (VALVE_BORE_R - 0.05, BARREL_TOP + 2.0),
               (VALVE_BORE_R - 0.05, SEAT_BODY_Z1 - 2.0 + c),
               (3.2, SEAT_BODY_Z1 + c), (3.2, VALVE_HEAD_TOP),
               (1.4, VALVE_HEAD_TOP), (1.4, VALVE_HEAD_TOP + 1.5),
               (0, VALVE_HEAD_TOP + 1.5)], "Valve")
    # Three flats on the guide stem: the fuel's way past it.
    sk = p.sketch(Plane.xy(BARREL_TOP + 1.0), name="Flutes")
    for k in range(3):
        a = math.radians(120.0 * k)
        cx, cy = 2.9 * math.cos(a), 2.9 * math.sin(a)
        sk.circle((cx, cy), d=2.0)
    p.extrude(sk.add(), SEAT_BODY_Z1 - 3.5 - BARREL_TOP, mode="cut",
              name="Flutes")
    p.frame("axis", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    finish(p, "steel", (0.8, 0.8, 0.82), roughness=0.2)
    p.check(solids=1)
    return p


def valve_spring(app):
    p = app.part("delivery valve spring")
    coil(p, VALVE_HEAD_TOP + VALVE_LIFT + 0.05, HOLDER_CAVITY_TOP - 0.05,
         4.4, 0.8, 4.0, "Spring")
    p.frame("axis", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    finish(p, "steel", (0.25, 0.26, 0.28))
    p.check(solids=1)
    return p


def valve_holder(app):
    p = app.part("delivery valve holder")
    turned(p, [(0, SEAT_BODY_Z1), (9.0, SEAT_BODY_Z1), (9.0, HOLDER_HEX_Z0),
               (0, HOLDER_HEX_Z0)], "Thread")
    sk = p.sketch(Plane.xy(HOLDER_HEX_Z0), name="Hex")
    hexagon(sk, 22.0)
    p.extrude(sk.add(), HOLDER_HEX_Z1 - HOLDER_HEX_Z0, mode="add", name="Hex")
    turned(p, [(0, HOLDER_HEX_Z1), (6.0, HOLDER_HEX_Z1),
               (6.0, HOLDER_TOP - 1.0), (5.0, HOLDER_TOP), (0, HOLDER_TOP)],
           "Union spigot", mode="add")
    turned(p, [(0, SEAT_BODY_Z1 - 1.0), (3.6, SEAT_BODY_Z1 - 1.0),
               (3.6, HOLDER_CAVITY_TOP), (1.0, HOLDER_CAVITY_TOP),
               (1.0, HOLDER_TOP + 1.0), (0, HOLDER_TOP + 1.0)], "Bore",
           mode="cut")
    # Thread grooves on the union spigot.
    for k in range(5):
        z = HOLDER_HEX_Z1 + 2.5 + 2.0 * k
        turned(p, [(5.4, z), (6.1, z - 0.6), (6.1, z + 0.6)],
               f"Thread groove {k + 1}", mode="cut")
    p.frame("axis", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    finish(p, "steel", (0.5, 0.5, 0.52))
    p.check(solids=1)
    return p


# --- control rack -------------------------------------------------------------

RACK_TIP = RACK_Y - MODULE
RACK_ROOT = RACK_Y + 1.25 * MODULE


def rack_profile():
    """(x, y) outline of the rack bar at the stop position: straight 20-degree
    flanks (an involute rack), a tooth centred on every element axis."""
    t = math.tan(math.radians(20.0))
    half_p = RACK_PITCH / 4.0 - BACKLASH / 4.0
    tip = half_p - MODULE * t
    root = half_p + 1.25 * MODULE * t
    k0 = int(math.ceil((EL_X[0] - 10.0) / RACK_PITCH))
    k1 = int(math.floor((EL_X[-1] + 10.0 + RACK_TRAVEL) / RACK_PITCH))
    pts = [(RACK_X0, RACK_BACK), (RACK_X0, RACK_ROOT)]
    for k in range(k0, k1 + 1):
        xc = k * RACK_PITCH
        pts += [(xc - root, RACK_ROOT), (xc - tip, RACK_TIP),
                (xc + tip, RACK_TIP), (xc + root, RACK_ROOT)]
    pts += [(RACK_X1, RACK_ROOT), (RACK_X1, RACK_BACK)]
    return pts


def control_rack(app):
    p = app.part("control rack")
    sk = p.sketch(Plane.xy(GEAR_Z0), name="Rack")
    sk.polyline(rack_profile())
    p.extrude(sk.add(), GEAR_Z1 - GEAR_Z0, mode="new_body", name="Rack")
    p.frame("slide", origin=(0, 0, 0), z=(-1, 0, 0), x=(0, 1, 0))
    p.frame("governor", origin=(0, GOV_Y, GOV_Z), z=(1, 0, 0), x=(0, 1, 0))
    finish(p, "steel", STEEL)
    p.check(solids=1)
    return p


# --- feed pump --------------------------------------------------------------------

FEED_ROLLER_W = 5.8
FEED_TAPPET_R = FEED_BORE_D / 2.0 - 0.1
FEED_Y0 = -(ECC_R + FEED_ROLLER_R - ECC_E)       # roller centre, zero lift
FEED_BODY_Y = (-68.0, H_Y0)


def feed_tappet(app):
    """Roller, cheeks and the piston it pushes, one rigid part."""
    p = app.part("feed pump tappet")
    rod(p, (0, FEED_Y0 - 5.0, 0), (0, -55.0, 0), 2 * FEED_TAPPET_R, "Body",
        mode="new_body")
    for s in (-1, 1):
        x0 = s * (FEED_ROLLER_W / 2.0 + 0.4)
        x1 = s * (FEED_ROLLER_W / 2.0 + 2.6)
        slab(p, min(x0, x1), FEED_Y0 - 5.5, -4.0, max(x0, x1), FEED_Y0 + 4.0,
             4.0, f"Cheek {s:+d}")
    rod(p, (-FEED_ROLLER_W / 2.0 - 2.6, FEED_Y0, 0),
        (FEED_ROLLER_W / 2.0 + 2.6, FEED_Y0, 0), 3.0, "Pin")
    sk = p.sketch(Plane.yz(-FEED_ROLLER_W / 2.0), name="Roller")
    sk.circle((FEED_Y0, 0), r=FEED_ROLLER_R)
    p.extrude(sk.add(), FEED_ROLLER_W, mode="add", name="Roller")
    p.frame("slide", origin=(0, 0, 0), z=(0, -1, 0), x=(1, 0, 0))
    finish(p, "steel", STEEL)
    p.check(solids=1)
    return p


def feed_pump_body(app):
    p = app.part("feed pump body")
    y0, y1 = FEED_BODY_Y
    slab(p, -16.0, y0, -18.0, 16.0, y1, 16.0, "Body", mode="new_body")
    rod(p, (0, PRIMER_Y, 16.0), (0, PRIMER_Y, 40.0), 16.0, "Primer barrel")
    for s in (-1, 1):
        rod(p, (s * 16.0, -45.0, -6.0), (s * 24.0, -45.0, -6.0), 14.0,
            f"Banjo boss {s:+d}")
    # Mounting flange ears.
    slab(p, -26.0, y1 - 6.0, -12.0, 26.0, y1, 10.0, "Flange")
    rod(p, (0, y1 + 0.5, 0), (0, y0 + 4.0, 0), FEED_BORE_D, "Piston bore",
        mode="cut")
    rod(p, (0, PRIMER_Y, 41.0), (0, PRIMER_Y, 4.0), 10.4, "Primer bore",
        mode="cut")
    p.frame("mount", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("primer", origin=(0, PRIMER_Y, 0), z=(0, 0, 1), x=(1, 0, 0))
    finish(p, "steel", BLACK, roughness=0.6,
           metallic=0.3)
    p.check(solids=1)
    return p


PRIMER_STROKE = 15.0


def primer(app):
    p = app.part("hand primer")
    rod(p, (0, PRIMER_Y, 20.0), (0, PRIMER_Y, 41.0), 10.0, "Stem",
        mode="new_body")
    rod(p, (0, PRIMER_Y, 41.0), (0, PRIMER_Y, 53.0), 24.0, "Knob")
    sk = p.sketch(Plane.xy(49.0), name="Grip")
    for k in range(12):
        a = math.radians(30.0 * k)
        sk.circle((12.3 * math.cos(a), PRIMER_Y + 12.3 * math.sin(a)), d=2.4)
    p.extrude(sk.add(), 5.0, mode="cut", name="Grip")
    p.frame("slide", origin=(0, PRIMER_Y, 0), z=(0, 0, 1), x=(1, 0, 0))
    finish(p, "rubber", BLACK, roughness=0.8, metallic=0.0)
    p.check(solids=1)
    return p


# --- pneumatic governor (local: X = pump X, axis through the rack) -------------

GOV_R = GOV_D / 2.0
GOV_X_MEMBRANE = 82.0
PLATE_T = 3.0


def governor_housing(app):
    p = app.part("governor housing")
    turned_x(p, [(H_X1, 0), (H_X1, GOV_R), (GOV_X_MEMBRANE, GOV_R),
                 (GOV_X_MEMBRANE, 0)], "Bowl")
    turned_x(p, [(H_X1 + 4.0, 0), (H_X1 + 4.0, 40.0), (GOV_X_MEMBRANE + 1, 40.0),
                 (GOV_X_MEMBRANE + 1, 0)], "Chamber", mode="cut")
    ry = RACK_SLOT
    slab(p, H_X1 - 1.0, ry[0] - GOV_Y, ry[2] - GOV_Z, H_X1 + 5.0,
         ry[1] - GOV_Y, ry[3] - GOV_Z, "Rack slot", mode="cut")
    p.frame("mount", origin=(0, 0, 0), z=(1, 0, 0), x=(0, 1, 0))
    finish(p, "aluminum", CASTING, roughness=0.7, metallic=0.6)
    p.check(solids=1)
    return p


def governor_cover(app):
    p = app.part("governor cover")
    turned_x(p, [(GOV_X_MEMBRANE + 1.0, 0), (GOV_X_MEMBRANE + 1.0, GOV_R),
                 (100.0, GOV_R), (106.0, GOV_R - 10.0), (106.0, 0)], "Cover")
    turned_x(p, [(GOV_X_MEMBRANE, 0), (GOV_X_MEMBRANE, 40.0), (102.0, 40.0),
                 (102.0, 0)], "Vacuum chamber", mode="cut")
    rod(p, (104.0, 22.0, 0), (118.0, 22.0, 0), 10.0, "Vacuum spigot")
    rod(p, (100.0, 22.0, 0), (119.0, 22.0, 0), 5.0, "Vacuum port",
        mode="cut")
    p.frame("mount", origin=(0, 0, 0), z=(1, 0, 0), x=(0, 1, 0))
    finish(p, "aluminum", CASTING, roughness=0.7, metallic=0.6)
    p.check(solids=1)
    return p


def membrane(app):
    p = app.part("governor diaphragm rim")
    turned_x(p, [(GOV_X_MEMBRANE, 36.0), (GOV_X_MEMBRANE, GOV_R - 0.5),
                 (GOV_X_MEMBRANE + 1.0, GOV_R - 0.5),
                 (GOV_X_MEMBRANE + 1.0, 36.0)], "Rim")
    p.frame("mount", origin=(0, 0, 0), z=(1, 0, 0), x=(0, 1, 0))
    finish(p, "rubber", RUBBER, roughness=0.9, metallic=0.0)
    p.check(solids=1)
    return p


def diaphragm_plate(app):
    p = app.part("governor diaphragm")
    turned_x(p, [(RACK_X1, 0), (RACK_X1, 30.0), (RACK_X1 + PLATE_T, 30.0),
                 (RACK_X1 + PLATE_T, 0)], "Plate")
    turned_x(p, [(RACK_X1 + PLATE_T, 0), (RACK_X1 + PLATE_T, 8.0),
                 (RACK_X1 + PLATE_T + 2.0, 8.0), (RACK_X1 + PLATE_T + 2.0, 0)],
             "Spring boss", mode="add")
    p.frame("mount", origin=(0, 0, 0), z=(1, 0, 0), x=(0, 1, 0))
    finish(p, "rubber", RUBBER, roughness=0.9, metallic=0.0)
    p.check(solids=1)
    return p


def governor_spring(app):
    """Drawn at its shortest (the stop position) and seated in the cover."""
    p = app.part("governor spring")
    coil_x(p, RACK_X1 + PLATE_T + 0.1, 102.0 - 0.05, 36.0, 2.5, 4.0, "Spring")
    p.frame("mount", origin=(0, 0, 0), z=(1, 0, 0), x=(0, 1, 0))
    finish(p, "steel", (0.25, 0.26, 0.28))
    p.check(solids=1)
    return p


# --- timing device (on the camshaft's drive taper) -----------------------------

HUB_X0, HUB_X1 = -84.0, -72.0
WEIGHT_X0, WEIGHT_X1 = -93.0, -85.0
PIVOT_R = 21.0
FLANGE_X0, FLANGE_X1 = -106.0, -95.0


def timing_hub(app):
    p = app.part("timing device hub")
    turned_x(p, [(HUB_X0, 8.7), (HUB_X0, 34.0), (HUB_X1, 34.0),
                 (HUB_X1, 8.7)], "Hub")
    for s in (1, -1):
        rod(p, (HUB_X0 + 0.5, 0, s * PIVOT_R), (WEIGHT_X0 - 1.5, 0, s * PIVOT_R),
            6.0, f"Pivot pin {s:+d}")
    p.frame("axle", origin=(0, 0, 0), z=(1, 0, 0), x=(0, 1, 0))
    p.frame("pin_a", origin=(WEIGHT_X0, 0, PIVOT_R), z=(1, 0, 0), x=(0, 1, 0))
    p.frame("pin_b", origin=(WEIGHT_X0, 0, -PIVOT_R), z=(1, 0, 0),
            x=(0, -1, 0))
    finish(p, "steel", STEEL)
    p.check(solids=1)
    return p


def camshaft_nut(app):
    p = app.part("camshaft nut")
    sk = p.sketch(Plane.yz(-91.0), name="Hex")
    hexagon(sk, 17.0)
    p.extrude(sk.add(), 91.0 + HUB_X0 - 0.05, mode="new_body", name="Hex")
    rod(p, (-92.0, 0, 0), (-80.0, 0, 0), 10.05, "Thread", mode="cut")
    p.frame("axle", origin=(0, 0, 0), z=(1, 0, 0), x=(0, 1, 0))
    finish(p, "steel", STEEL)
    p.check(solids=1)
    return p


def flyweight(app):
    """A crescent round the hub, pivoting at its top end; turning NEGATIVE
    about +X swings its free end outward."""
    p = app.part("timing flyweight")
    r0, r1 = 13.5, 29.0
    a0, a1 = 90.0, 225.0
    def at(r, a):
        return (r * math.cos(math.radians(a)), r * math.sin(math.radians(a)))
    sk = p.sketch(Plane.yz(WEIGHT_X0), name="Weight")
    sk.arc((0, 0), r1, a0, a1)
    sk.line(at(r1, a1), at(r0, a1))
    sk.arc((0, 0), r0, a0, a1)
    sk.line(at(r0, a0), at(r1, a0))
    p.extrude(sk.add(), WEIGHT_X1 - WEIGHT_X0, mode="new_body", name="Weight")
    # A boss round the pivot so the pin is carried in metal.
    sk = p.sketch(Plane.yz(WEIGHT_X0), name="Pivot boss")
    sk.circle((0.0, PIVOT_R), r=7.0)
    p.extrude(sk.add(), WEIGHT_X1 - WEIGHT_X0, mode="add", name="Pivot boss")
    sk = p.sketch(Plane.yz(WEIGHT_X0 - 1.0), name="Pivot bore")
    sk.circle((0.0, PIVOT_R), d=6.1)
    p.extrude(sk.add(), WEIGHT_X1 - WEIGHT_X0 + 2.0, mode="cut",
              name="Pivot bore")
    p.frame("pivot", origin=(WEIGHT_X0, 0, PIVOT_R), z=(1, 0, 0),
            x=(0, 1, 0))
    finish(p, "steel", (0.4, 0.41, 0.43))
    p.check(solids=1)
    return p


def drive_flange(app):
    """The chain-driven half: the duplex sprocket and the flange that the
    flyweights turn against."""
    p = app.part("timing device sprocket")
    teeth, pitch = 26, 9.525
    rp = pitch / (2.0 * math.sin(math.pi / teeth))
    turned_x(p, [(FLANGE_X0, 0), (FLANGE_X0, rp - 3.5), (FLANGE_X1, rp - 3.5),
                 (FLANGE_X1, 0)], "Flange")
    # Two rows of teeth: a disc at the tip circle with a roller seat cut on
    # the pitch circle between every pair of teeth (3/8" duplex chain).
    for row, x0 in enumerate((FLANGE_X0 + 0.5, FLANGE_X0 + 5.9)):
        sk = p.sketch(Plane.yz(x0), name=f"Sprocket row {row + 1}")
        sk.circle((0, 0), r=rp + 2.6)
        p.extrude(sk.add(), 4.6, mode="add", name=f"Sprocket row {row + 1}")
    sk = p.sketch(Plane.yz(FLANGE_X0 - 1.0), name="Roller seats")
    for k in range(teeth):
        c = 2 * math.pi * (k + 0.5) / teeth
        sk.circle((rp * math.cos(c), rp * math.sin(c)), r=3.35)
    p.extrude(sk.add(), FLANGE_X1 - FLANGE_X0 + 2.0, mode="cut",
              name="Roller seats")
    p.frame("axle", origin=(0, 0, 0), z=(1, 0, 0), x=(0, 1, 0))
    finish(p, "steel", (0.35, 0.35, 0.37))
    p.check(solids=1)
    return p
