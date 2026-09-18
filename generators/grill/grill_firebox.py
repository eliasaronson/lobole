"""The hot half: the firebox pan, the lid, and everything welded inside it.

Drawn WHERE IT LIVES. The firebox's own coordinates are the world's, so a
socket on it is the world position of the part it carries, and the ledge a
grate rests on is at the height the grate rests at rather than at an offset
from an offset.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from grill_lib import (bend_radius, blank, bracket_frame, corner_relief,  # noqa: E402
                       drill, edges_at, flange, flat_config, frame, hem, holes,
                       normal_cut, outer_edge, part, profile_blank, sheet_ok,
                       sheet_panel, sheet_plane, sketch, tapped, thickness)
from _lib import check, circle, line, rect  # noqa: E402

import grill_spec as S  # noqa: E402


def near(t):
    """The pick tolerance for a sheet of thickness `t` — a quarter of it.

    A fixed window catches the blank's own corner edges, whose midpoints sit
    at t/2, and the flange then refuses a corner as a panel edge.
    """
    return max(1e-4, 0.25 * t)


def ledge_socket(wall, out, seat_z, t=S.T_BRK, along=0.0, hanging=True):
    """Where a welded angle goes: the wall it lies on, and the seat height.

    `wall` is the coordinate of the wall's inner face and `out` the direction
    its flat leg reaches; the angle is placed by its HEEL — the corner where
    the two legs meet — which is at the seat, or a thickness below it when
    the angle hangs with its flat leg on top.
    """
    up = (0.0, 0.0, -1.0) if hanging else (0.0, 0.0, 1.0)
    heel_z = seat_z - t if hanging else seat_z
    axes = bracket_frame(out, up)
    if abs(out[1]) > 0.5:                      # a ledge on a front/back wall
        origin = (along, wall, heel_z)
    else:                                      # one on a side wall
        origin = (wall, along, heel_z)
    return origin, axes


# --- the pan ---------------------------------------------------------------

def firebox_pan(d, tent_drop):
    """The pan: base, four walls, a rim, and every hole that goes through it.

    `tent_drop` is how far a heat tent's legs hang below its own ridge once
    it is rolled into place — measured off the tent, because the arithmetic
    for it is a fold angle, a thickness and a flange reach, and being 0.09 mm
    out puts the tent INSIDE the ledge it rests on.
    """
    part(d, "Firebox", S.FIREBOX_STOCK)
    t = thickness(d)
    base = sheet_panel(d, "z", S.FB_Z, +1,
                       (-S.BOX_X / 2, S.FRONT_WALL_OUT, S.FB_Z),
                       (S.BOX_X / 2, S.BACK_WALL_OUT, S.FB_Z),
                       name="Firebox blank")

    # The four top edges of the base: picked on the TOP face, so the walls
    # fold up. material_inside — the blank IS the box's outer size, so the
    # bend lines run inside it and cross at the corners, which is what makes
    # the relief necessary and what a brake would tear without.
    top = S.FB_Z + t
    walls = edges_at(d, lambda m: abs(m[2] - top) < near(t) and (
        abs(abs(m[0]) - S.BOX_X / 2) < near(t)
        or abs(m[1] - S.FRONT_WALL_OUT) < near(t)
        or abs(m[1] - S.BACK_WALL_OUT) < near(t)), what="base panel edge")
    flange(d, base, walls, S.BOX_H, name="Walls", position="material_inside")
    corner_relief(d, base, name="Corner relief")

    # The rim: the outermost top edge of each wall, folded out to carry the
    # lid and stiffen the box. Inset at both ends by a radius and a
    # thickness, or it meets the wall beside it at the corner and fuses to
    # it — and a corner of continuous metal folds from no blank.
    rim = []
    for axis, sign, extent in ((0, +1, S.BOX_X / 2), (0, -1, S.BOX_X / 2),
                               (1, +1, S.BACK_WALL_OUT),
                               (1, -1, -S.FRONT_WALL_OUT)):
        rim.append(outer_edge(
            d, lambda m, a=axis, s=sign, e=extent: (
                abs(m[2] - S.WALL_TOP) < 3.0 and s * m[a] > e - 5.0),
            axis, sign, what="wall top edge"))
    inset = bend_radius(d) + t
    # material_inside, so the rim's TOP surface is the wall's top edge: the
    # metal turns over there. The default (bend_outside) stacks the bend
    # ABOVE the wall, and the lid then lands 3 mm inside the rim.
    flange(d, base, rim, S.BOX_RIM, name="Rim", position="material_inside",
           offset_start=inset, offset_end=inset)

    # Through the front wall: the burner ports, and the two holes the control
    # panel's top lip bolts to. Cut from INSIDE the box looking forward — a
    # plane outside it would meet the front wall and then the back one, and
    # a normal cut needs one sheet.
    ports = [(S.burner_x(i), 0.0, S.BURNER_Z) for i in range(S.BURNERS)]
    holes(d, "y", S.FB_Y, -1, ports, S.BURNER_PORT_D, name="Burner ports")
    holes(d, "y", S.FB_Y, -1,
          [(x, 0.0, S.PANEL_LIP_Z) for x in S.PANEL_LIP_BOLTS],
          S.CLEAR[S.PANEL_LIP_BOLT], name="Panel lip holes")

    # Through the back wall: a screw into each burner's tail.
    holes(d, "y", S.FB_Y, +1,
          [(S.burner_x(i), 0.0, S.BURNER_Z) for i in range(S.BURNERS)],
          S.CLEAR[S.BURNER_BOLT], name="Burner screw holes")

    # Through the base: the grease drain, and the four bolts that hold the
    # firebox down on the cart's top flanges.
    base_cuts = [(0.0, S.DRAIN_Y, 0.0)]
    holes(d, "z", S.FB_Z + 40.0, -1, base_cuts, S.DRAIN_D, name="Grease drain")
    holes(d, "z", S.FB_Z + 40.0, -1,
          [(x, y, 0.0) for x, y in S.FIREBOX_BOLTS],
          S.CLEAR[S.FIREBOX_BOLT], name="Cart bolt holes")

    # --- sockets: everything the firebox carries hangs off one of these ----
    frame(d, "origin", (0.0, 0.0, 0.0))
    for name, wall, out, seat, hanging, along in (
            ("grate_ledge_front", S.FRONT_WALL_IN, (0, 1, 0),
             S.GRATE_LEDGE_Z, True, 0.0),
            ("grate_ledge_back", S.BACK_WALL_IN, (0, -1, 0),
             S.GRATE_LEDGE_Z, True, 0.0),
            ("tent_ledge_front", S.FRONT_WALL_IN, (0, 1, 0),
             S.TENT_LEDGE_Z, False, 0.0),
            ("tent_ledge_back", S.BACK_WALL_IN, (0, -1, 0),
             S.TENT_LEDGE_Z, False, 0.0),
            ("rack_ledge_left", -S.SIDE_WALL_IN, (1, 0, 0),
             S.RACK_LEDGE_Z, True, S.RACK_CENTRE_Y),
            ("rack_ledge_right", S.SIDE_WALL_IN, (-1, 0, 0),
             S.RACK_LEDGE_Z, True, S.RACK_CENTRE_Y)):
        origin, axes = ledge_socket(wall, out, seat, along=along,
                                    hanging=hanging)
        frame(d, name, origin, **axes)
    for side, sx in (("left", -1.0), ("right", 1.0)):
        # The rail's lip turns IN, under the tray, so the part faces the
        # other way on the other side.
        frame(d, f"tray_rail_{side}", (sx * S.TRAY_SLIDE_X, S.TRAY_RAIL_Y,
                                       S.FB_Z),
              z_axis=(0, 0, 1), x_axis=(-sx, 0, 0))
        # The ear is drawn in world Y and Z with its thickness along +x, so
        # placing it is a step along X: outboard of the lid's ear, which
        # takes the inboard 1.5 mm of the pair.
        frame(d, f"box_ear_{side}",
              (sx * S.HINGE_X - (S.T_BRK if sx < 0 else 0.0), 0.0, 0.0))
        # The pin goes in from OUTSIDE, so its own axis points inward.
        frame(d, f"hinge_pin_{side}",
              (sx * (S.HINGE_X + S.T_BRK), S.PIN_Y, S.PIN_Z),
              z_axis=(-sx, 0, 0), x_axis=(0, 1, 0))
    tent_ridge = S.TENT_LEDGE_Z + tent_drop + S.TENT_SEAT_CLEAR
    tilt = math.radians(180.0 + (180.0 - S.TENT_DEG) / 2.0)
    for i in range(S.BURNERS):
        x = S.burner_x(i)
        frame(d, f"burner_{i + 1}", (x, S.BURNER_MOUTH_Y, S.BURNER_Z),
              z_axis=(0, 1, 0), x_axis=(1, 0, 0))
        # The tent hangs from its own ridge, rolled about Y until both legs
        # fall away evenly onto their ledges: the tilt belongs to the socket,
        # not to a quaternion typed into the assembly.
        frame(d, f"tent_{i + 1}", (x, S.FB_Y, tent_ridge),
              z_axis=(math.sin(tilt), 0.0, math.cos(tilt)),
              x_axis=(math.cos(tilt), 0.0, -math.sin(tilt)))
        frame(d, f"grate_{i + 1}", (x, S.FB_Y, S.GRATE_LEDGE_Z))
    frame(d, "rack", (0.0, S.RACK_CENTRE_Y, S.RACK_LEDGE_Z))
    # The axis the lid turns on: the pin through both ears.
    frame(d, "lid_hinge", (0.0, S.PIN_Y, S.PIN_Z), z_axis=(1, 0, 0),
          x_axis=(0, 1, 0))
    frame(d, "panel", (0.0, 0.0, 0.0))
    frame(d, "manifold", (0.0, S.VALVE_Y, S.MANIFOLD_Z),
          z_axis=(1, 0, 0), x_axis=(0, 1, 0))
    flat_config(d, base)
    sheet_ok(d, "firebox")
    return base


# --- the lid ---------------------------------------------------------------

def hood(d):
    """The lid: a flat top with a facet folded down front and back.

    Its height is not a number here: the lid is built a little above the rim
    and then LANDED on it — the blank's plane moves by whatever the measured
    facet edge is off by, so the lid rests on the rim whatever the facet's
    angle, length or bend radius do.
    """
    part(d, "Hood", S.HOOD_STOCK)
    t = thickness(d)
    z0 = S.RIM_TOP + S.HOOD_DROP + t          # a first guess; landed below
    half = S.HOOD_TOP_Y / 2.0
    base = sheet_panel(d, "z", z0, +1,
                       (-S.HOOD_X / 2, S.FB_Y - half, z0),
                       (S.HOOD_X / 2, S.FB_Y + half, z0), name="Hood blank")
    # Folded DOWN: which of an edge's two faces is picked is what decides the
    # way a wall turns, so the facets are picked on the UNDERSIDE.
    facets = edges_at(d, lambda m: abs(m[2] - z0) < near(t)
                      and abs(abs(m[1] - S.FB_Y) - half) < near(t),
                      what="hood underside edge")
    flange(d, base, facets, S.HOOD_FACET, name="Facets",
           angle_deg=S.HOOD_FACET_DEG)

    land(d, base, S.RIM_TOP)

    state = d.call("get_state")
    z_top = state["bodies"][0]["bbox_max"][2]
    # The underside of the lid's flat: the plane the blank was drawn on,
    # after the landing moved it. The end caps are the lid's section, so
    # this is where their top edge goes.
    z_flat = next(f for f in state["features"]
                  if f.get("type") == "Sketch")["plane"]["origin"][2]
    holes(d, "z", z_top + 20.0, -1, [(0.0, S.FB_Y, 0.0)], S.THERMO_BORE,
          name="Thermometer bore")
    handle_y = S.FB_Y + S.HANDLE_Y
    holes(d, "z", z_top + 20.0, -1,
          [(sx * S.HANDLE_SPAN / 2.0, handle_y, 0.0) for sx in S.sides()],
          S.CLEAR[S.HANDLE_BOLT], name="Handle bolt holes")

    frame(d, "origin", (0.0, 0.0, 0.0))
    frame(d, "hinge", (0.0, S.PIN_Y, S.PIN_Z), z_axis=(1, 0, 0),
          x_axis=(0, 1, 0))
    frame(d, "handle", (0.0, handle_y, z_top))
    frame(d, "thermo", (0.0, S.FB_Y, z_top))
    for side, sx in (("left", -1.0), ("right", 1.0)):
        # An end cap is drawn in world Y and Z at x = 0, so its socket is
        # where its outer face goes and nothing else.
        frame(d, f"cap_{side}",
              (sx * S.HOOD_X / 2.0 - (t if sx > 0 else 0.0), 0.0, z_flat))
        frame(d, f"lid_ear_{side}",
              (sx * S.HINGE_X - (S.T_BRK if sx > 0 else 0.0), 0.0, 0.0))
    flat_config(d, base)
    sheet_ok(d, "hood")
    return {"z_flat": z_flat, "z_top": z_top, "drop": z_flat - S.RIM_TOP,
            "facet": facet_plane(d)}


def land(d, base, on_z):
    """Move the blank's plane until the part's lowest face sits on `on_z`.

    A lid that misses its rim by a bend radius looks right in every view and
    is a gap in the contact report; the correction is measured, applied to
    the sketch the blank came from, and checked again.
    """
    for _ in range(3):
        low = d.call("get_state")["bodies"][0]["bbox_min"][2]
        delta = on_z - low
        if abs(delta) < 1e-6:
            return
        state = d.call("get_state")
        sk = next(f for f in state["features"] if f.get("type") == "Sketch")
        spec = {k: v for k, v in sk.items() if k not in ("id", "index")}
        plane = dict(spec["plane"])
        plane["origin"] = [plane["origin"][0], plane["origin"][1],
                           plane["origin"][2] + delta]
        spec["plane"] = plane
        check(d.call("modify_feature", feature_id=sk["id"], feature=spec),
              "landing the lid")
    raise SystemExit("the lid would not land on the rim")


def facet_plane(d):
    """The lid's back facet, as the app reports it: (normal, a point on it).

    The outer face of the facet that leans back — the one the lid's hinge ear
    is welded to. Picked by its normal, and the largest of those, because a
    facet has one big face and several slivers around its bend.
    """
    faces = [f for f in d.call("list_elements")["bodies"][0]["faces"]
             if f["kind"] == "plane" and f["normal"][1] > 0.5
             and f["normal"][2] > 0.5]
    if not faces:
        raise SystemExit("the lid has no back facet to weld an ear to")
    best = max(faces, key=lambda f: f["area"])
    return best["normal"], best["centroid"]


def hood_cap(d, drop):
    """One end cap: the lid's own cross-section, welded in its end.

    `drop` is how far the lid's facets actually fall — measured, not the
    nominal — so the cap reaches the rim the facets landed on instead of
    stopping a bend radius above it.
    """
    part(d, "Hood end cap", S.HOOD_STOCK)
    half = S.HOOD_TOP_Y / 2.0
    pts = [(S.FB_Y - half - drop, -drop),
           (S.FB_Y - half, 0.0), (S.FB_Y + half, 0.0),
           (S.FB_Y + half + drop, -drop)]
    ents = [line(pts[i], pts[i + 1]) for i in range(len(pts) - 1)]
    ents.append(line(pts[-1], pts[0]))
    base = profile_blank(d, {"origin": [0, 0, 0], "normal": [1, 0, 0],
                             "uAxis": [0, 1, 0]}, ents, name="Cap blank")
    frame(d, "origin", (0.0, 0.0, 0.0))
    sheet_ok(d, "hood cap")
    return base


# --- welded inside: the ledges, the tray rails, the hinge ears -------------

def angle_bracket(d, name, stock, length, leg, hole=None):
    """A folded angle: a flat leg and an upstand, welded by its heel.

    Drawn with the flat leg reaching along -y from the upstand and the
    upstand turned up, so `bracket_frame` can put it on any wall of the box
    from the two directions a reader can see: where it reaches, and which
    way it stands.
    """
    part(d, name, stock)
    t = thickness(d)
    base = profile_blank(d, {"origin": [0, 0, 0], "normal": [0, 0, 1],
                             "uAxis": [1, 0, 0]},
                         [rect((-length / 2.0, -leg), (length / 2.0, 0.0))],
                         name=name + " blank")
    up = edges_at(d, lambda m: abs(m[2] - t) < near(t) and abs(m[1]) < near(t),
                  what=name + " fold edge")
    flange(d, base, up, leg, name="Upstand", position="material_inside")
    frame(d, "heel", (0.0, 0.0, t))
    flat_config(d, base)
    sheet_ok(d, name.lower())
    return base


def grate_ledge(d):
    return angle_bracket(d, "Grate ledge", S.BRACKET_STOCK, S.LEDGE_LEN,
                         S.LEDGE_LEG)


def tent_ledge(d):
    return angle_bracket(d, "Tent ledge", S.BRACKET_STOCK, S.LEDGE_LEN,
                         S.LEDGE_LEG)


def rack_ledge(d):
    return angle_bracket(d, "Rack ledge", S.BRACKET_STOCK, S.RACK_LEDGE_LEN,
                         S.LEDGE_LEG)


def tray_rail(d):
    """A Z-rail welded under the firebox: a top flange on the pan's
    underside, a web down past the tray's wall, and a lip the tray slides
    on."""
    part(d, "Tray rail", S.BRACKET_STOCK)
    t = thickness(d)
    base = profile_blank(d, {"origin": [0, 0, -0.0], "normal": [0, 0, 1],
                             "uAxis": [1, 0, 0]},
                         [rect((0.0, -S.TRAY_RAIL_LEN / 2.0),
                               (S.TRAY_RAIL_TOP, S.TRAY_RAIL_LEN / 2.0))],
                         name="Rail blank")
    web_edge = edges_at(d, lambda m: abs(m[2]) < near(t) and abs(m[0]) < near(t),
                        what="rail web edge")
    flange(d, base, web_edge, S.TRAY_RAIL_H, name="Web",
           position="material_inside")
    lip_edge = edges_at(d, lambda m: abs(m[0] - t) < near(t)
                        and m[2] < -S.TRAY_RAIL_H + 2 * t, what="rail lip edge")
    flange(d, base, lip_edge, S.TRAY_RAIL_LIP, name="Lip",
           position="material_inside")
    frame(d, "heel", (0.0, 0.0, t))
    flat_config(d, base)
    sheet_ok(d, "tray rail")
    return base


def box_ear(d):
    """The lid's hinge ear on the firebox: a rib welded up the back wall.

    Its profile is drawn in world Y and Z — it is a part of the back wall's
    elevation — so what the numbers say is where the metal is: up the wall
    to just under the rim, outboard of the rim's edge, and only then up past
    it to the pin.
    """
    part(d, "Hinge ear", S.BRACKET_STOCK)
    pts = list(S.BOX_EAR)
    ents = [line(pts[i], pts[(i + 1) % len(pts)]) for i in range(len(pts))]
    base = profile_blank(d, {"origin": [0, 0, 0], "normal": [1, 0, 0],
                             "uAxis": [0, 1, 0]}, ents, name="Ear blank")
    drill(d, "x", -5.0, +1, [(0.0, S.PIN_Y, S.PIN_Z)], S.PIN_D, 20.0,
          name="Pin bore")
    frame(d, "origin", (0.0, 0.0, 0.0))
    sheet_ok(d, "hinge ear")
    return base


def lid_ear(d, facet):
    """The other half of the lid's hinge, welded to the back facet.

    `facet` is the plane the app reports for the facet's OUTER face — its
    normal and a point on it — so the two points of the weld edge are on the
    metal they are welded to rather than near it.
    """
    part(d, "Lid ear", S.BRACKET_STOCK)
    n, c = facet
    ny, nz = n[1], n[2]
    d0 = ny * c[1] + nz * c[2]
    y_at_rim = (d0 - nz * S.RIM_TOP) / ny
    up = (-nz, ny)                       # along the facet, going up and in
    weld = [(y_at_rim + w * up[0], S.RIM_TOP + w * up[1])
            for w in S.LID_EAR_WELD]
    pts = [weld[0]] + list(S.LID_EAR) + [weld[1]]
    ents = [line(pts[i], pts[(i + 1) % len(pts)]) for i in range(len(pts))]
    base = profile_blank(d, {"origin": [0, 0, 0], "normal": [1, 0, 0],
                             "uAxis": [0, 1, 0]}, ents, name="Ear blank")
    drill(d, "x", -5.0, +1, [(0.0, S.PIN_Y, S.PIN_Z)], S.PIN_D, 20.0,
          name="Pin bore")
    frame(d, "origin", (0.0, 0.0, 0.0))
    sheet_ok(d, "lid ear")
    return base


# --- what sits in the box --------------------------------------------------

def heat_tent(d):
    """An inverted V over a burner: one fold, and it rests on two ledges.

    Drawn with its fold line ON the origin, so the assembly's socket rolls it
    about its own ridge and placing it over a burner is one number rather
    than a correction for where the ridge drifted to.
    """
    part(d, "Heat tent", S.TENT_STOCK)
    t = thickness(d)
    base = profile_blank(d, {"origin": [0, 0, 0], "normal": [0, 0, 1],
                             "uAxis": [1, 0, 0]},
                         [rect((-S.TENT_LEG, -S.TENT_LEN / 2),
                               (0.0, S.TENT_LEN / 2))], name="Tent blank")
    fold = edges_at(d, lambda m: abs(m[2] - t) < near(t)
                    and abs(m[0]) < near(t), what="tent fold edge")
    flange(d, base, fold, S.TENT_LEG, name="Second leg",
           angle_deg=180.0 - S.TENT_DEG)
    frame(d, "ridge", (0.0, 0.0, 0.0))
    flat_config(d, base)
    sheet_ok(d, "heat tent")
    # How far the legs hang below the ridge once the tent is rolled about it.
    # The tent is a prism along Y, so its cross-section's corners are the
    # midpoints of its long edges — and the lowest of them, turned by the
    # tilt, is what has to land on the ledge.
    tilt = math.radians(180.0 + (180.0 - S.TENT_DEG) / 2.0)
    drop = 0.0
    for e in d.call("list_elements")["bodies"][0]["edges"]:
        if e["kind"] != "line" or e["length"] < S.TENT_LEN - 1.0:
            continue
        m = e["midpoint"]
        drop = max(drop, -(m[0] * -math.sin(tilt) + m[2] * math.cos(tilt)))
    if drop <= 0.0:
        raise SystemExit("the heat tent has no long edges to measure")
    return {"tent_drop": drop}


def grease_tray(d):
    """A shallow pan under the drain, hemmed where a hand takes hold."""
    part(d, "Grease tray", S.TRAY_STOCK)
    t = thickness(d)
    base = blank(d, S.TRAY_X, S.TRAY_Y, name="Tray blank")
    walls = edges_at(d, lambda m: abs(m[2] - t) < near(t) and (
        abs(abs(m[0]) - S.TRAY_X / 2) < near(t)
        or abs(abs(m[1]) - S.TRAY_Y / 2) < near(t)), what="tray base edge")
    flange(d, base, walls, S.TRAY_H, name="Tray walls")
    corner_relief(d, base, name="Tray relief")
    # The outer top edge of each wall, hemmed: this is the edge a hand takes
    # hold of. Inset like the firebox's rim, or the hem meets the wall beside
    # it at the corner and the tray stops being foldable.
    top_z = t + S.TRAY_H
    inset = bend_radius(d) + t
    rim = []
    for axis, sign, extent in ((0, +1, S.TRAY_X), (0, -1, S.TRAY_X),
                               (1, +1, S.TRAY_Y), (1, -1, S.TRAY_Y)):
        rim.append(outer_edge(
            d, lambda m, a=axis, s=sign, e=extent: (
                abs(m[2] - top_z) < 3.0 and s * m[a] > e / 2 - 5.0),
            axis, sign, what="tray wall top edge"))
    hem(d, base, rim, S.TRAY_HEM, name="Safe edges",
        offset_start=inset, offset_end=inset)
    frame(d, "floor", (0.0, 0.0, 0.0))
    flat_config(d, base)
    sheet_ok(d, "grease tray")
    return base
