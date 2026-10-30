#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""One builder per part of the PES4M pump. Every builder takes the SDK app
and returns the finished Part; every number comes from pes_spec."""
import math
import os
import sys

from lobole.sdk import Plane, V  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gear import involute_profile  # noqa: E402
from pes_spec import *  # noqa: E402,F401,F403


# --- vocabulary ---------------------------------------------------------------

def slab(p, x0, y0, z0, x1, y1, z1, name, mode="add"):
    """A block drawn on its BOTTOM face and extruded up."""
    sk = p.sketch(Plane.xy(z0), name=name)
    sk.rect((x0, y0), (x1, y1))
    return p.extrude(sk.add(), z1 - z0, mode=mode, name=name)


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


def bottom_rounded(sk, u0, v0, u1, v1, r):
    """A rectangle with only its two -V corners rounded: a casting's
    section, flat on top where the next body stands on it."""
    sk.line((u0 + r, v0), (u1 - r, v0))
    sk.arc((u1 - r, v0 + r), r, 270.0, 360.0)
    sk.line((u1, v0 + r), (u1, v1))
    sk.line((u1, v1), (u0, v1))
    sk.line((u0, v1), (u0, v0 + r))
    sk.arc((u0 + r, v0 + r), r, 180.0, 270.0)


def stadium(sk, u0, u1, v, r):
    """Two semicircles of radius r at u0 and u1 on the line v, joined."""
    sk.line((u0, v - r), (u1, v - r))
    sk.arc((u1, v), r, -90.0, 90.0)
    sk.line((u1, v + r), (u0, v + r))
    sk.arc((u0, v), r, 90.0, 270.0)


def round_slab(p, x0, y0, z0, x1, y1, z1, r, name, mode="add"):
    """`slab` with its vertical edges rounded: how a casting is shaped."""
    sk = p.sketch(Plane.xy(z0), name=name)
    rounded_rect(sk, x0, y0, x1, y1, r)
    return p.extrude(sk.add(), z1 - z0, mode=mode, name=name)


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


# Linear, not sRGB: the app's colours are linear, so 0.6 renders near white
# and a grey that reads 0.5 in sRGB is about 0.2. Paint is a dielectric
# (metallic 0). From the owner's photographs of the pump on the engine and
# on the bench.
CASTING = (0.35, 0.355, 0.35)          # satin die-cast aluminium
CASTING_FINISH = dict(roughness=0.5, metallic=0.8)
STEEL = (0.22, 0.23, 0.25)
BLACK = (0.012, 0.012, 0.014)
BRASS = (0.45, 0.28, 0.07)
RUBBER = (0.02, 0.02, 0.02)
FEED_PAINT = (0.04, 0.04, 0.042)       # the feed pump's dark paint
Z_OLIVE = (0.25, 0.2, 0.08)            # the "Z" cover's brass-olive
CAP_RED = (0.5, 0.02, 0.02)            # the oil filler cap


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
# From the photographs, scaled on the 25 mm element pitch: two lugs under
# the ends carry the foot holes, a gusset rises from each lug, two
# ribs stand on the +Y wall, and each clamp plate's screw goes into a pillar
# midway between its pair of holders.
LUG_X = 33.0                  # the lugs' inner ends, +-X
RIB_X = 38.5                  # the gussets, +-X
SIDE_RIB_X = 14.0
CLAMP_X = ((EL_X[0] + EL_X[1]) / 2.0, (EL_X[2] + EL_X[3]) / 2.0)


def _toes(p):
    """The side-view prism: the cam case's bottom rounded into its ends."""
    sk = p.sketch(Plane.xz(H_Y1 + 1.0), name="Case toes")
    bottom_rounded(sk, H_X0, H_Z0, H_X1, 41.0, CASE_TOE_R)
    p.extrude(sk.add(), H_Y1 - H_Y0 + 2.0, mode="new_body", name="Case toes")


def housing(app):
    p = app.part("pump housing")
    # The cam case is the intersection of three prisms: its section, rounded
    # along the bottom; its plan, rounded at the ends; its side view, rounded
    # where the bottom meets the end walls.
    sk = p.sketch(Plane.yz(H_X0), name="Cam case")
    bottom_rounded(sk, H_Y0, H_Z0, H_Y1, 40.0, CASE_R)
    p.extrude(sk.add(), H_X1 - H_X0, mode="new_body", name="Cam case")
    case = p.state()["features"][-1]["id"]
    for name, draw in (
            ("Case ends", lambda: round_slab(
                p, H_X0, H_Y0 - 1.0, H_Z0 - 1.0, H_X1, H_Y1 + 1.0, 41.0,
                CASE_END_R, "Case ends", mode="new_body")),
            ("Case toes", lambda: _toes(p))):
        draw()
        tool = p.state()["features"][-1]["id"]
        p.app.add_feature(feature={"type": "Boolean", "name": f"{name} trim",
                                   "target_id": case, "tool_id": tool,
                                   "op": "intersect"})
        case = p.state()["features"][-1]["id"]     # the trimmed body's maker
    round_slab(p, UP_X0, UP_Y0, 30.0, UP_X1, UP_Y1, H_TOP, BLOCK_R,
               "Element block")
    # Two mounting lugs under the ends, and a gusset from each up the side.
    for x0, x1 in ((H_X0 + 2.0, -LUG_X), (LUG_X, H_X1 - 2.0)):
        round_slab(p, x0, H_Y0 - 10.0, H_Z0, x1, H_Y1 + 10.0, H_Z0 + 7.0,
                   4.0, f"Mounting lug {x0:+.0f}")
    for x in (-RIB_X, RIB_X):
        for s in (-1, 1):
            sk = p.sketch(Plane.yz(x - 2.5), name=f"Gusset {x:+.0f} {s:+d}")
            sk.polyline([(s * 38.0, H_Z0 + 7.0), (s * 22.0, H_Z0 + 7.0),
                         (s * 30.0, 30.0)])
            p.extrude(sk.add(), 5.0, mode="add", name=f"Gusset {x:+.0f} {s:+d}")
    # Side ribs on the +Y wall between the gussets (the feed pump's flange
    # has the -Y wall).
    for x in (-SIDE_RIB_X, SIDE_RIB_X):
        slab(p, x - 2.0, H_Y1 - 2.0, -8.0, x + 2.0, H_Y1 + 3.0, 38.0,
             f"Rib {x:+.0f}")
    # The deck is machined down between the elements, leaving a boss round
    # every delivery-valve holder and a pillar between each pair for the
    # clamp plate's screw.
    slab(p, UP_X0 - 1.0, UP_Y0 - 1.0, H_TOP - 6.0, UP_X1 + 1.0, UP_Y1 + 1.0,
         H_TOP + 1.0, "Deck", mode="cut")
    sk = p.sketch(Plane.xy(H_TOP - 6.5), name="Holder bosses")
    for x in EL_X:
        sk.circle((x, 0), d=23.5)
    p.extrude(sk.add(), 6.5, mode="add", name="Holder bosses")
    sk = p.sketch(Plane.xy(H_TOP - 6.5), name="Clamp pillars")
    for x in CLAMP_X:
        sk.circle((x, 0), d=10.0)
    p.extrude(sk.add(), 6.5, mode="add", name="Clamp pillars")
    # The fuel inlet boss on -Y, cross-drilled into the gallery.
    rod(p, (UP_X1 - 12.0, UP_Y0 + 1.0, PORT_Z), (UP_X1 - 12.0, UP_Y0 - 5.0,
        PORT_Z), 15.0, "Inlet boss")
    turned_x(p, [(FLANGE_FACE_X, 0), (FLANGE_FACE_X, FLANGE_D / 2.0),
                 (H_X0, FLANGE_D / 2.0), (H_X0, 0)], "Drive flange", mode="add")
    turned_x(p, [(FLANGE_FACE_X - REGISTER_H, 0),
                 (FLANGE_FACE_X - REGISTER_H, REGISTER_D / 2.0),
                 (FLANGE_FACE_X, REGISTER_D / 2.0), (FLANGE_FACE_X, 0)],
             "Register", mode="add")
    turned_x(p, [(H_X1, 0), (H_X1, 20.0), (H_X1 + 12.0, 20.0),
                 (H_X1 + 12.0, 0)], "Rear bearing cover", mode="add")
    y0, y1, z0, z1 = RACK_SLOT
    slab(p, CAP_X0, y0 - 3.0, z0 - 3.0, UP_X0 + 1.0, y1 + 3.0, z1 + 3.0,
         "Rack end cap")
    # Cavities.
    sk = p.sketch(Plane.yz(-50.0), name="Cam gallery")
    bottom_rounded(sk, -GALLERY, -GALLERY, GALLERY, GALLERY, GALLERY_R)
    p.extrude(sk.add(), 100.0, mode="cut", name="Cam gallery")
    for x0, x1 in ((H_X0 - 1.0, -50.0), (50.0, H_X1)):
        turned_x(p, [(x0, 0), (x0, BEARING_OD / 2.0), (x1, BEARING_OD / 2.0),
                     (x1, 0)], f"Bearing bore {x0:+.0f}", mode="cut")
    turned_x(p, [(H_X0 - 14.0, 0), (H_X0 - 14.0, 13.0), (H_X0, 13.0),
                 (H_X0, 0)], "Seal bore", mode="cut")
    turned_x(p, [(H_X1 - 1.0, 0), (H_X1 - 1.0, 6.8), (H_X1 + 9.0, 6.8),
                 (H_X1 + 9.0, 0)], "Stub pocket", mode="cut")
    # Each element's stepped bore, turned about its own axis: tappet bore,
    # spring chamber, web, barrel, collar and holder seat in one outline.
    steps = [(TAPPET_BORE_D / 2.0, 10.0), (SPRING_CHAMBER_D / 2.0, TAPPET_TOP),
             (7.8, WEB_Z0), (6.15, RACK_GALLERY_Z0 + 1.0),
             (8.6, BARREL_COLLAR_Z), (9.1, BARREL_TOP)]
    for i, x in enumerate(EL_X):
        pts = [(x, 10.0)]
        for k, (r, z0_) in enumerate(steps):
            z1_ = steps[k + 1][1] if k + 1 < len(steps) else H_TOP + 1.0
            pts += [(x + r, z0_), (x + r, z1_)]
        pts.append((x, H_TOP + 1.0))
        sk = p.sketch(Plane.xz(0.0), name=f"Element {i + 1} bore")
        sk.polyline(pts)
        p.revolve(sk.add(), axis={"kind": "custom", "origin": [x, 0, 0],
                                  "direction": [0, 0, 1]},
                  mode="cut", name=f"Element {i + 1} bore")
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
    # The sketch's V is normal x U = -Z, so a bolt at +Z is drawn at -v.
    sk = p.sketch(Plane((H_X0, 0, 0), (-1, 0, 0), (0, 1, 0)),
                  name="Flange holes")
    r = FLANGE_BOLT_PCD / 2.0
    for a in FLANGE_BOLT_ANGLES:
        sk.circle((r * math.cos(math.radians(a)),
                   -r * math.sin(math.radians(a))), d=FLANGE_BOLT_D)
    p.extrude(sk.add(), H_X0 - FLANGE_FACE_X, mode="cut", name="Flange holes")
    # ...and room behind each for its bolt's head: a spot-faced pocket into
    # the end wall, down to the flange's own back face. Without it the top
    # bolt's head stands in the cam case and the lower two in the lugs.
    hd, hh = FLANGE_BOLT_POCKET
    for a in FLANGE_BOLT_ANGLES:
        y, z = r * math.cos(math.radians(a)), r * math.sin(math.radians(a))
        rod(p, (FLANGE_BOLT_SEAT_X, y, z), (H_X0 + hh, y, z), hd,
            f"Flange bolt pocket {a:.0f}", mode="cut")
    p.frame("cam_axis", origin=(0, 0, 0), z=(1, 0, 0), x=(0, 1, 0))
    p.frame("bearing_drive", origin=(H_X0, 0, 0), z=(1, 0, 0), x=(0, 1, 0))
    p.frame("bearing_governor", origin=(50.0, 0, 0), z=(1, 0, 0), x=(0, 1, 0))
    p.frame("origin", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    for i, x in enumerate(EL_X):
        p.frame(f"el{i + 1}", origin=(x, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("rack", origin=(0, 0, 0), z=(-1, 0, 0), x=(0, 1, 0))
    p.frame("feed", origin=(ECC_X, 0, 0), z=(0, -1, 0), x=(1, 0, 0))
    p.frame("governor", origin=(0, GOV_Y, GOV_Z), z=(1, 0, 0), x=(0, 1, 0))
    # What an engine mounts it by, and where its fuel comes in.
    p.frame("flange", origin=(FLANGE_FACE_X, 0, 0), z=(1, 0, 0), x=(0, 0, 1))
    p.frame("fuel_inlet", origin=(UP_X1 - 12.0, UP_Y0 - 5.0, PORT_Z),
            z=(0, -1, 0), x=(1, 0, 0))
    for i, x in enumerate(CLAMP_X):
        p.frame(f"clamp{i + 1}", origin=(x, 0, H_TOP), z=(0, 0, 1),
                x=(1, 0, 0))
    finish(p, "aluminum_6061_t6", CASTING, **CASTING_FINISH)
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
    finish(p, "aluminum_6061_t6", CASTING, **CASTING_FINISH)
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
    p = app.part("pump camshaft")
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
        sk.spline(pts[k:] + pts[:k], closed=True)
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
# The port is on -Y, so at the stop position the vertical groove faces it.
GROOVE_DEG = -90.0


def plunger(app):
    p = app.part("plunger")
    r = PLUNGER_D / 2.0 - 0.01
    turned(p, [(0, PLUNGER_FOOT_Z), (4.0, PLUNGER_FOOT_Z), (4.0, 46.0),
               (r, 46.0), (r, PLUNGER_TOP), (0, PLUNGER_TOP)], "Plunger")
    # The drive vanes, along X: the sleeve's slots.
    sk = p.sketch(Plane.xy(VANE_Z0), name="Vanes")
    for s in (-1, 1):
        sk.rect((s * 2.5, -VANE_W / 2.0), (s * VANE_R, VANE_W / 2.0))
    p.extrude(sk.add(), VANE_Z1 - VANE_Z0, mode="add", name="Vanes")
    # The control helix: a helical groove rising counter-clockwise from its
    # full-load end, HELIX_SPAN short of the vertical groove, up to it.
    z_top = PLUNGER_TOP - LAND_MIN - HELIX_GROOVE_D / 2.0
    z_bot = PLUNGER_TOP - LAND_MAX - HELIX_GROOVE_D / 2.0
    pitch = (z_top - z_bot) * 360.0 / HELIX_SPAN
    p.app.add_feature(feature={
        "type": "Curve3D", "name": "Helix path", "kind": "helix",
        "origin": [0, 0, z_bot], "axis": [0, 0, 1], "radius": r,
        "pitch": pitch, "turns": HELIX_SPAN / 360.0,
        "start_deg": GROOVE_DEG - HELIX_SPAN})
    path = p.state()["features"][-1]["id"]
    p.app.add_feature(feature={"type": "Pipe", "name": "Control helix",
                               "path_id": path,
                               "outer_diameter": HELIX_GROOVE_D,
                               "mode": "cut"})
    # The vertical groove from the top face down to the helix's upper end.
    g = math.radians(GROOVE_DEG)          # on the port at the stop position
    gx, gy = math.cos(g), math.sin(g)
    sk = p.sketch(Plane.xy(z_top - 0.6), name="Vertical groove")
    w = 0.7
    sk.polyline([(1.8 * gx - w * gy, 1.8 * gy + w * gx),
                 (3.5 * gx - w * gy, 3.5 * gy + w * gx),
                 (3.5 * gx + w * gy, 3.5 * gy - w * gx),
                 (1.8 * gx + w * gy, 1.8 * gy - w * gx)])
    p.extrude(sk.add(), PLUNGER_TOP - z_top + 1.6, mode="cut",
              name="Vertical groove")
    p.frame("axis", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
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
    """A plain shank above the deck (the clamp plate's), the hex body, a
    neck, and the larger union nut whose top the injection line seats on."""
    p = app.part("delivery valve holder")
    turned(p, [(0, SEAT_BODY_Z1), (9.0, SEAT_BODY_Z1), (9.0, HOLDER_HEX_Z0),
               (0, HOLDER_HEX_Z0)], "Shank")
    sk = p.sketch(Plane.xy(HOLDER_HEX_Z0), name="Hex")
    hexagon(sk, HOLDER_HEX_AF)
    p.extrude(sk.add(), HOLDER_HEX_Z1 - HOLDER_HEX_Z0, mode="add", name="Hex")
    turned(p, [(0, HOLDER_HEX_Z1 - 0.5), (7.5, HOLDER_HEX_Z1 - 0.5),
               (7.5, UNION_NUT_Z0 + 0.5), (0, UNION_NUT_Z0 + 0.5)], "Neck",
           mode="add")
    sk = p.sketch(Plane.xy(UNION_NUT_Z0), name="Union nut")
    hexagon(sk, UNION_NUT_AF)
    p.extrude(sk.add(), HOLDER_TOP - UNION_NUT_Z0, mode="add",
              name="Union nut")
    # The nut's corners chamfered at both faces, as a turned nut is.
    rc = UNION_NUT_AF / math.sqrt(3.0) + 0.5
    for z, dz in ((HOLDER_TOP, -1.0), (UNION_NUT_Z0, 1.0)):
        turned(p, [(UNION_NUT_AF / 2.0 - 0.3, z - 0.1 * dz), (rc, z - 0.1 * dz),
                   (rc, z + dz * (rc - UNION_NUT_AF / 2.0 + 0.3))],
               f"Nut chamfer {z:.0f}", mode="cut")
    turned(p, [(0, SEAT_BODY_Z1 - 1.0), (3.6, SEAT_BODY_Z1 - 1.0),
               (3.6, HOLDER_CAVITY_TOP), (1.0, HOLDER_CAVITY_TOP),
               (1.0, HOLDER_TOP - 3.0), (3.0, HOLDER_TOP - 1.0),
               (3.0, HOLDER_TOP + 1.0), (0, HOLDER_TOP + 1.0)], "Bore",
           mode="cut")
    p.frame("axis", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    # The injection line's nipple seats on the union's top.
    p.frame("outlet", origin=(0, 0, HOLDER_TOP), z=(0, 0, 1), x=(1, 0, 0))
    finish(p, "steel", (0.5, 0.5, 0.52))
    p.check(solids=1)
    return p


# The clamp plates, from the photographs: flat steel, one over each pair of
# holders, on the deck under the hex bodies, a slotted screw between.
CLAMP_T = 3.5
# Narrow enough that the two plates, a holder pitch apart end to end, do not
# meet in the middle: at 26 their round ends overlapped by a millimetre.
CLAMP_W = 24.0
CLAMP_HOLE_D = 18.6


def clamp_plate(app):
    """Drawn about its screw at the local origin, on the deck (z = 0)."""
    p = app.part("holder clamp plate")
    half = EL_PITCH / 2.0
    sk = p.sketch(Plane.xy(0.0), name="Plate")
    stadium(sk, -half, half, 0.0, CLAMP_W / 2.0)
    p.extrude(sk.add(), CLAMP_T, mode="new_body", name="Plate")
    sk = p.sketch(Plane.xy(-1.0), name="Holder holes")
    for s in (-1, 1):
        sk.circle((s * half, 0), d=CLAMP_HOLE_D)
    p.extrude(sk.add(), CLAMP_T + 2.0, mode="cut", name="Holder holes")
    rod(p, (0, 0, -1.0), (0, 0, CLAMP_T + 1.0), 5.5, "Screw hole", mode="cut")
    p.frame("seat", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("screw", origin=(0, 0, CLAMP_T), z=(0, 0, 1), x=(1, 0, 0))
    finish(p, "steel", (0.3, 0.31, 0.32), roughness=0.4)
    p.check(solids=1)
    return p


# Small enough to sit between two holders' shanks (half a pitch from each,
# shank radius 9): at 8 across the clamp plates' screws were in them.
SCREW_HEAD_D, SCREW_HEAD_H = 6.5, 3.0


def slotted_screw(app):
    """A cheese-head slotted screw's head, seated at its local origin; the
    shank is left out, since it would have to be drilled for in every part
    it holds."""
    p = app.part("slotted screw")
    turned(p, [(0, 0), (SCREW_HEAD_D / 2.0, 0),
               (SCREW_HEAD_D / 2.0, SCREW_HEAD_H - 0.6),
               (SCREW_HEAD_D / 2.0 - 0.6, SCREW_HEAD_H), (0, SCREW_HEAD_H)],
           "Head")
    slab(p, -SCREW_HEAD_D, -0.6, SCREW_HEAD_H - 1.2, SCREW_HEAD_D, 0.6,
         SCREW_HEAD_H + 1.0, "Slot", mode="cut")
    p.frame("seat", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    finish(p, "steel", (0.35, 0.36, 0.37), roughness=0.35)
    p.check(solids=1)
    return p


# --- the "Z" cover on the element block's -Y face -----------------------------
# From the photographs: a large rectangular plate, most of the element
# block's -Y face, held by a slotted screw at each corner, with a raised Z.
# It stops short of the fuel inlet boss at the governor end.
ZC_X0, ZC_X1 = UP_X0 + 5.0, UP_X1 - 23.0
ZC_Z0, ZC_Z1 = 46.0, H_TOP - 9.0
ZC_T, ZC_R = 3.0, 6.0
ZC_SCREW_IN = 6.5
Z_W, Z_H, Z_BAND, Z_T = 40.0, 44.0, 7.0, 1.5


def z_cover(app):
    p = app.part("Z cover")
    y0 = UP_Y0
    sk = p.sketch(Plane.xz(y0), name="Plate")
    rounded_rect(sk, ZC_X0, ZC_Z0, ZC_X1, ZC_Z1, ZC_R)
    p.extrude(sk.add(), ZC_T, mode="new_body", name="Plate")
    cx, cz = (ZC_X0 + ZC_X1) / 2.0, (ZC_Z0 + ZC_Z1) / 2.0
    x0, x1 = cx - Z_W / 2.0, cx + Z_W / 2.0
    z0, z1 = cz - Z_H / 2.0, cz + Z_H / 2.0
    w = Z_BAND
    # The diagonal is offset so its band is as wide, square to itself, as
    # the bars: w / sin of its slope.
    d = w * math.hypot(x1 - x0, z1 - z0 - 2 * w) / (z1 - z0 - 2 * w)
    sk = p.sketch(Plane.xz(y0 - ZC_T), name="Z")
    sk.polyline([(x0, z1), (x1, z1), (x1, z1 - w), (x0 + d, z0 + w),
                 (x1, z0 + w), (x1, z0), (x0, z0), (x0, z0 + w),
                 (x1 - d, z1 - w), (x0, z1 - w)])
    p.extrude(sk.add(), Z_T, mode="add", name="Z")
    k = 0
    for x in (ZC_X0 + ZC_SCREW_IN, ZC_X1 - ZC_SCREW_IN):
        for z in (ZC_Z0 + ZC_SCREW_IN, ZC_Z1 - ZC_SCREW_IN):
            k += 1
            p.frame(f"screw{k}", origin=(x, y0 - ZC_T, z), z=(0, -1, 0),
                    x=(1, 0, 0))
    p.frame("origin", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    finish(p, "steel", Z_OLIVE, roughness=0.7, metallic=0.3)
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


# The Bosch FP/K 22M, from the photographs (scaled on the 25 mm element
# pitch): a dark cast body turned about the piston's axis, flanged to the
# cam case, a pillar carrying the hand primer, a big hex plug underneath, an
# oval nameplate boss on its outer end, and two banjo fittings: the inlet on
# top beside the primer, the outlet on the drive-end side.
FB_R = 18.0                   # body radius about the piston axis
FB_Y1 = -66.0                 # its outer end
FB_TOP = 26.0                 # the primer pillar's top
PLUG_AF = 27.0
# The inlet's eye stands clear of the primer's pillar beside it: at x = 19
# the two were 2 mm into each other.
INLET_BANJO = (22.0, -44.0, 18.0)      # its seat, on the boss's top
OUTLET_BANJO = (-26.0, -44.0, 0.0)     # its seat, on the -X boss's face


def feed_pump_body(app):
    p = app.part("feed pump body")
    sk = p.sketch(Plane.xz(H_Y0), name="Flange")
    stadium(sk, -15.0, 15.0, -1.0, 11.0)
    p.extrude(sk.add(), 6.0, mode="new_body", name="Flange")
    sk = p.sketch(Plane.xy(0.0), name="Body profile")
    sk.polyline([(0, H_Y0 - 5.0), (FB_R, H_Y0 - 5.0), (FB_R, FB_Y1 + 4.0),
                 (FB_R - 4.0, FB_Y1), (0, FB_Y1)])
    p.revolve(sk.add(), axis="y", mode="add", name="Body")
    rod(p, (0, PRIMER_Y, 0.0), (0, PRIMER_Y, FB_TOP - 2.0), 26.0,
        "Primer pillar")
    rod(p, (0, PRIMER_Y, FB_TOP - 2.0), (0, PRIMER_Y, FB_TOP), 22.0,
        "Pillar top")
    ix, iy, iz = INLET_BANJO
    round_slab(p, ix - 8.0, iy - 8.0, 0.0, ix + 8.0, iy + 8.0, iz, 5.0,
               "Inlet boss")
    ox, oy, oz = OUTLET_BANJO
    sk = p.sketch(Plane.yz(ox), name="Outlet boss")
    rounded_rect(sk, oy - 8.0, oz - 8.0, oy + 8.0, oz + 8.0, 5.0)
    p.extrude(sk.add(), -9.0 - ox, mode="add", name="Outlet boss")
    # The flange's two nuts.
    for s in (-1, 1):
        sk = p.sketch(Plane.xz(H_Y0 - 6.0), name=f"Flange nut {s:+d}")
        hexagon(sk, 10.0, centre=(s * 20.0, -1.0))
        p.extrude(sk.add(), 5.0, mode="add", name=f"Flange nut {s:+d}")
    # The big hex plug underneath, flats across the pump.
    sk = p.sketch(Plane.xy(-FB_R - 8.0), name="Plug")
    hexagon(sk, PLUG_AF, centre=(0.0, PRIMER_Y), flats_on_u=False)
    p.extrude(sk.add(), 10.0, mode="add", name="Plug")
    rod(p, (0, PRIMER_Y, -FB_R - 8.0), (0, PRIMER_Y, -FB_R - 10.0), 20.0,
        "Plug spigot")
    # The oval nameplate boss on the outer end.
    sk = p.sketch(Plane.xz(FB_Y1), name="Nameplate boss")
    stadium(sk, -5.0, 5.0, 0.0, 7.0)
    p.extrude(sk.add(), 1.5, mode="add", name="Nameplate boss")
    rod(p, (0, H_Y0 + 0.5, 0), (0, FB_Y1 + 3.0, 0), FEED_BORE_D, "Piston bore",
        mode="cut")
    rod(p, (0, PRIMER_Y, FB_TOP + 1.0), (0, PRIMER_Y, 4.0), 10.4,
        "Primer bore", mode="cut")
    p.frame("mount", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("primer", origin=(0, PRIMER_Y, 0), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("inlet", origin=INLET_BANJO, z=(0, 0, 1), x=(0, -1, 0))
    p.frame("outlet", origin=OUTLET_BANJO, z=(-1, 0, 0), x=(0, 0, 1))
    finish(p, "steel", FEED_PAINT, roughness=0.55, metallic=0.0)
    p.check(solids=1)
    return p


PRIMER_STROKE = 15.0
PRIMER_TUBE_TOP = 64.0        # the tube's top: the wheel's seat at rest
PRIMER_WHEEL_D = 45.0         # from the photographs
PRIMER_SPOKES = 6


def primer_tube(app):
    """The hand primer's steel cylinder, screwed into the pillar."""
    p = app.part("hand primer tube")
    sk = p.sketch(Plane.xy(FB_TOP), name="Hex")
    hexagon(sk, 17.0, centre=(0.0, PRIMER_Y))
    p.extrude(sk.add(), 5.0, mode="new_body", name="Hex")
    rod(p, (0, PRIMER_Y, FB_TOP + 5.0), (0, PRIMER_Y, PRIMER_TUBE_TOP), 14.0,
        "Tube")
    rod(p, (0, PRIMER_Y, FB_TOP - 1.0), (0, PRIMER_Y, PRIMER_TUBE_TOP + 1.0),
        10.4, "Bore", mode="cut")
    p.frame("mount", origin=(0, PRIMER_Y, 0), z=(0, 0, 1), x=(1, 0, 0))
    finish(p, "steel", (0.45, 0.46, 0.48), roughness=0.3)
    p.check(solids=1)
    return p


def primer(app):
    """The plunger and its wheel: a thin knurled rim on six spokes."""
    p = app.part("hand primer")
    z0 = PRIMER_TUBE_TOP + 0.5            # the hub's underside at rest
    rod(p, (0, PRIMER_Y, 20.0), (0, PRIMER_Y, z0), 10.0, "Stem",
        mode="new_body")
    rod(p, (0, PRIMER_Y, z0), (0, PRIMER_Y, z0 + 11.0), 15.0, "Hub")
    r_out = PRIMER_WHEEL_D / 2.0
    sk = p.sketch(Plane.xy(z0 + 1.5), name="Rim")
    sk.circle((0, PRIMER_Y), r=r_out)
    sk.circle((0, PRIMER_Y), r=r_out - 3.0)
    p.extrude(sk.add(), 8.0, mode="add", name="Rim")
    for k in range(PRIMER_SPOKES):
        a = math.radians(360.0 * k / PRIMER_SPOKES)
        c, s_ = math.cos(a), math.sin(a)
        sk = p.sketch(Plane.xy(z0 + 3.0), name=f"Spoke {k + 1}")
        pts = [(5.0 * c - 2.0 * s_, PRIMER_Y + 5.0 * s_ + 2.0 * c),
               ((r_out - 1.5) * c - 2.0 * s_,
                PRIMER_Y + (r_out - 1.5) * s_ + 2.0 * c),
               ((r_out - 1.5) * c + 2.0 * s_,
                PRIMER_Y + (r_out - 1.5) * s_ - 2.0 * c),
               (5.0 * c + 2.0 * s_, PRIMER_Y + 5.0 * s_ - 2.0 * c)]
        sk.polyline(pts)
        p.extrude(sk.add(), 5.0, mode="add", name=f"Spoke {k + 1}")
    sk = p.sketch(Plane.xy(z0), name="Knurl")
    for k in range(36):
        a = math.radians(10.0 * k)
        sk.circle((r_out * math.cos(a), PRIMER_Y + r_out * math.sin(a)),
                  d=1.6)
    p.extrude(sk.add(), 12.0, mode="cut", name="Knurl")
    p.frame("slide", origin=(0, PRIMER_Y, 0), z=(0, 0, 1), x=(1, 0, 0))
    finish(p, "plastic_black", BLACK, roughness=0.6, metallic=0.0)
    p.check(solids=1)
    return p


BANJO_EYE_D, BANJO_EYE_H = 16.0, 10.0


def banjo(app):
    """A banjo fitting: the eye on its seat at the local origin, the bolt's
    hex head above it, and the pipe stub along local +X."""
    p = app.part("banjo fitting")
    turned(p, [(4.5, 0), (BANJO_EYE_D / 2.0, 0),
               (BANJO_EYE_D / 2.0, BANJO_EYE_H), (4.5, BANJO_EYE_H)], "Eye")
    rod(p, (0, 0, 0.0), (0, 0, BANJO_EYE_H + 0.5), 9.0, "Bolt shank")
    sk = p.sketch(Plane.xy(BANJO_EYE_H), name="Bolt head")
    hexagon(sk, 12.0)
    p.extrude(sk.add(), 5.0, mode="add", name="Bolt head")
    rod(p, (BANJO_EYE_D / 2.0 - 1.0, 0, BANJO_EYE_H / 2.0),
        (24.0, 0, BANJO_EYE_H / 2.0), 8.0, "Pipe")
    rod(p, (BANJO_EYE_D / 2.0, 0, BANJO_EYE_H / 2.0),
        (25.0, 0, BANJO_EYE_H / 2.0), 5.0, "Pipe bore", mode="cut")
    p.frame("seat", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    finish(p, "steel", (0.4, 0.41, 0.42), roughness=0.35)
    p.check(solids=1)
    return p


# --- pneumatic governor (local: X = pump X, axis through the rack) -------------
# The governor parts are drawn in the governor's frame: pump X, but Y and Z
# measured from the governor's axis at (GOV_Y, GOV_Z).

GOV_R = GOV_D / 2.0
GOV_X_MEMBRANE = 82.0
PLATE_T = 3.0
# From the photographs: the housing flares out of the pump body to the
# diaphragm chamber, whose rim the cover is screwed to with a ring of small
# screws. The oil filler (a red cap) stands on a boss on top, a nameplate
# on a pad on the -Y side, and the stop lever's bracket below it.
GOV_NECK_R = 38.0             # where it leaves the pump body
GOV_BODY_R = 46.0             # the chamber's outside, inside the rims
GOV_RIM_X = 77.0              # the housing's rim, to the membrane
COVER_RIM_X = 87.0            # the cover's rim
GOV_SCREWS, GOV_SCREW_R, GOV_SCREW_D = 12, 46.2, 3.6
FILLER_X, FILLER_Z = 70.0, 126.0      # the cap's seat, pump coordinates
FILLER_BOSS_D = 24.0
PLATE_PAD_Y = -47.0           # the nameplate pad's face, governor frame
LEVER_PAD_Y = -44.0           # the stop-lever bracket's face
LEVER_AT = (69.0, -31.0)      # its pivot, governor frame (x, z)


def governor_housing(app):
    p = app.part("governor housing")
    turned_x(p, [(H_X1, 0), (H_X1, GOV_NECK_R), (H_X1 + 4.0, GOV_BODY_R - 2.5),
                 (H_X1 + 8.0, GOV_BODY_R - 0.5), (GOV_RIM_X, GOV_BODY_R),
                 (GOV_RIM_X, GOV_R), (GOV_X_MEMBRANE, GOV_R),
                 (GOV_X_MEMBRANE, 0)], "Bowl")
    # The pads go on before the chamber is cut, so the cut clears them out
    # of the chamber.
    rod(p, (FILLER_X, -GOV_Y, 20.0), (FILLER_X, -GOV_Y, FILLER_Z - GOV_Z),
        FILLER_BOSS_D, "Filler boss")
    slab(p, H_X1 + 4.0, PLATE_PAD_Y, -14.0, GOV_RIM_X, -30.0, 14.0,
         "Nameplate pad")
    lx, lz = LEVER_AT
    slab(p, lx - 5.0, LEVER_PAD_Y, lz - 5.0, lx + 5.0, -26.0, lz + 5.0,
         "Stop lever bracket")
    turned_x(p, [(H_X1 + 4.0, 0), (H_X1 + 4.0, 40.0), (GOV_X_MEMBRANE + 1, 40.0),
                 (GOV_X_MEMBRANE + 1, 0)], "Chamber", mode="cut")
    ry = RACK_SLOT
    slab(p, H_X1 - 1.0, ry[0] - GOV_Y, ry[2] - GOV_Z, H_X1 + 5.0,
         ry[1] - GOV_Y, ry[3] - GOV_Z, "Rack slot", mode="cut")
    p.frame("mount", origin=(0, 0, 0), z=(1, 0, 0), x=(0, 1, 0))
    p.frame("filler", origin=(FILLER_X, -GOV_Y, FILLER_Z - GOV_Z),
            z=(0, 0, 1), x=(1, 0, 0))
    finish(p, "aluminum_6061_t6", CASTING, **CASTING_FINISH)
    p.check(solids=1)
    return p


def governor_cover(app):
    p = app.part("governor cover")
    turned_x(p, [(GOV_X_MEMBRANE + 1.0, 0), (GOV_X_MEMBRANE + 1.0, GOV_R),
                 (COVER_RIM_X, GOV_R), (COVER_RIM_X, GOV_BODY_R - 1.0),
                 (100.0, GOV_BODY_R - 1.0), (106.0, GOV_R - 10.0), (106.0, 0)],
             "Cover")
    turned_x(p, [(GOV_X_MEMBRANE, 0), (GOV_X_MEMBRANE, 40.0), (102.0, 40.0),
                 (102.0, 0)], "Vacuum chamber", mode="cut")
    rod(p, (104.0, 22.0, 0), (118.0, 22.0, 0), 10.0, "Vacuum spigot")
    rod(p, (100.0, 22.0, 0), (119.0, 22.0, 0), 5.0, "Vacuum port",
        mode="cut")
    # The ring of screws round the rim.
    sk = p.sketch(Plane.yz(COVER_RIM_X), name="Rim screws")
    for k in range(GOV_SCREWS):
        a = 2.0 * math.pi * (k + 0.5) / GOV_SCREWS
        sk.circle((GOV_SCREW_R * math.cos(a), GOV_SCREW_R * math.sin(a)),
                  d=GOV_SCREW_D)
    p.extrude(sk.add(), 2.0, mode="add", name="Rim screws")
    p.frame("mount", origin=(0, 0, 0), z=(1, 0, 0), x=(0, 1, 0))
    finish(p, "aluminum_6061_t6", CASTING, **CASTING_FINISH)
    p.check(solids=1)
    return p


# The red oil filler cap, from the photographs: about 38 across and 40
# tall, with ribbed sides. Drawn on its seat at the local origin.
CAP_D, CAP_H, CAP_RIBS = 38.0, 40.0, 20


def filler_cap(app):
    p = app.part("governor filler cap")
    r = CAP_D / 2.0
    turned(p, [(0, 0), (FILLER_BOSS_D / 2.0 - 1.0, 0),
               (FILLER_BOSS_D / 2.0 - 1.0, 2.0), (r, 2.0), (r, CAP_H - 4.0),
               (r - 2.5, CAP_H - 0.5), (r - 5.0, CAP_H), (0, CAP_H)], "Cap")
    sk = p.sketch(Plane.xy(5.0), name="Ribs")
    for k in range(CAP_RIBS):
        a = 2.0 * math.pi * k / CAP_RIBS
        sk.circle(((r + 0.6) * math.cos(a), (r + 0.6) * math.sin(a)), d=3.0)
    p.extrude(sk.add(), CAP_H - 11.0, mode="cut", name="Ribs")
    p.frame("seat", origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0))
    finish(p, "plastic_black", CAP_RED, roughness=0.45, metallic=0.0)
    p.check(solids=1)
    return p


# The Bosch nameplate: a small riveted plate on the governor housing's -Y
# pad, in the governor frame.
NAMEPLATE_W, NAMEPLATE_H, NAMEPLATE_T = 13.0, 20.0, 0.8


def nameplate(app):
    p = app.part("nameplate")
    cx = (H_X1 + 4.0 + GOV_RIM_X) / 2.0
    sk = p.sketch(Plane((0, PLATE_PAD_Y, 0), (0, -1, 0), (1, 0, 0)),
                  name="Plate")
    rounded_rect(sk, cx - NAMEPLATE_W / 2.0, -NAMEPLATE_H / 2.0,
                 cx + NAMEPLATE_W / 2.0, NAMEPLATE_H / 2.0, 1.0)
    p.extrude(sk.add(), NAMEPLATE_T, mode="new_body", name="Plate")
    for z in (-NAMEPLATE_H / 2.0 + 2.5, NAMEPLATE_H / 2.0 - 2.5):
        rod(p, (cx, PLATE_PAD_Y - NAMEPLATE_T, z),
            (cx, PLATE_PAD_Y - NAMEPLATE_T - 0.8, z), 2.2, f"Rivet {z:+.0f}")
    p.frame("mount", origin=(0, 0, 0), z=(1, 0, 0), x=(0, 1, 0))
    finish(p, "aluminum_6061_t6", (0.55, 0.55, 0.52), roughness=0.3, metallic=0.9)
    p.check(solids=1)
    return p


def stop_lever(app):
    """The stop lever on its bracket, in the governor frame: a pivot pin out
    of the bracket and a flat arm reaching aft and down, parked."""
    p = app.part("stop lever")
    lx, lz = LEVER_AT
    rod(p, (lx, LEVER_PAD_Y, lz), (lx, LEVER_PAD_Y - 9.0, lz), 8.0, "Pivot",
        mode="new_body")
    sk = p.sketch(Plane((0, LEVER_PAD_Y - 4.0, 0), (0, -1, 0), (1, 0, 0)),
                  name="Arm")
    sk.slot((lx, lz), (lx + 18.0, lz - 9.0), 9.0)
    p.extrude(sk.add(), 3.0, mode="add", name="Arm")
    rod(p, (lx + 18.0, LEVER_PAD_Y - 7.0, lz - 9.0),
        (lx + 18.0, LEVER_PAD_Y - 11.0, lz - 9.0), 6.0, "Eye boss")
    p.frame("mount", origin=(0, 0, 0), z=(1, 0, 0), x=(0, 1, 0))
    finish(p, "steel", (0.3, 0.31, 0.32), roughness=0.45)
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
FLANGE_X0, FLANGE_X1 = SPROCKET_X0, SPROCKET_X1


def timing_hub(app):
    p = app.part("timing device hub")
    turned_x(p, [(HUB_X0, 8.7), (HUB_X0, TIMING_DEVICE_R),
                 (HUB_X1, TIMING_DEVICE_R), (HUB_X1, 8.7)], "Hub")
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
    teeth, rp = SPROCKET_TEETH, SPROCKET_PITCH_R
    turned_x(p, [(FLANGE_X0, 0), (FLANGE_X0, rp - 3.5), (FLANGE_X1, rp - 3.5),
                 (FLANGE_X1, 0)], "Flange")
    # Two rows of teeth: a disc at the tip circle with a roller seat cut on
    # the pitch circle between every pair of teeth (3/8" duplex chain).
    for row, x0 in enumerate((FLANGE_X0 + 0.5, FLANGE_X0 + 5.9)):
        sk = p.sketch(Plane.yz(x0), name=f"Sprocket row {row + 1}")
        sk.circle((0, 0), r=SPROCKET_TIP_R)
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
