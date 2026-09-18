"""The cold half: the cart that carries the firebox, and the control panel.

Handed, and drawn in place. The two side panels are mirror parts rather than
one part used twice, because a flange that turns IN on one side turns OUT on
the other — which is how the right-hand shelf came to pass through the
firebox in the last round. Every panel here is one blank and a few bends.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from grill_lib import (bend_radius, corner_relief, drill, edges_at, flange,  # noqa: E402
                       flat_config, frame, hem, holes, outer_edge, part,
                       sheet_ok, sheet_panel, thickness)
from _lib import circle, rect  # noqa: E402

import grill_spec as S  # noqa: E402


def near(t):
    return max(1e-4, 0.25 * t)


def hand_name(hand):
    return "left" if hand < 0 else "right"


# --- the cart's sides: everything else hangs off one of these --------------

def cart_side(d, hand):
    """One side of the cart: a panel with a flange turned IN on every edge,
    and the holes for everything bolted to it."""
    side = hand_name(hand)
    part(d, f"Cart side {side}", S.CART_STOCK)
    t = thickness(d)
    out = hand * S.SIDE_OUT
    inner = hand * S.SIDE_IN
    base = sheet_panel(d, "x", out, -hand,
                       (out, S.CART_FRONT, 0.0), (out, S.CART_BACK, S.CART_H),
                       name="Side blank")
    edges = edges_at(d, lambda m: abs(m[0] - inner) < near(t) and (
        abs(abs(m[1]) - S.CART_Y / 2) < near(t)
        or abs(m[2]) < near(t) or abs(m[2] - S.CART_H) < near(t)),
        what="side panel edge")
    flange(d, base, edges, S.CART_FLANGE, name="Stiffening flanges",
           position="material_inside")
    corner_relief(d, base, name="Corner relief")

    # Holes, each cut from a plane that sees ONE flange: a normal cut needs a
    # single sheet, and a plane that looks the length of the cart sees two.
    mine = [(x, v) for x, v in S.FIREBOX_BOLTS if x * hand > 0]
    holes(d, "z", S.CART_H - 50.0, +1, [(x, y, 0.0) for x, y in mine],
          S.CLEAR[S.FIREBOX_BOLT], name="Firebox bolt holes")
    mine = [(x, v) for x, v in S.SHELF_BOLTS if x * hand > 0]
    holes(d, "z", 20.0, -1, [(x, y, 0.0) for x, y in mine],
          S.CLEAR[S.SHELF_BOLT], name="Shelf bolt holes")
    holes(d, "z", 20.0, -1, [(hand * S.FLANGE_BOLT_X, S.FOOT_Y, 0.0)],
          S.CLEAR[S.FOOT_BOLT], name="Foot bolt hole")
    mine = [(x, v) for x, v in S.BACK_BOLTS if x * hand > 0]
    holes(d, "y", 100.0, +1, [(x, 0.0, z) for x, z in mine],
          S.CLEAR[S.BACK_BOLT], name="Back bolt holes")
    mine = [(x, v) for x, v in S.PANEL_BOLTS if x * hand > 0]
    holes(d, "y", -100.0, -1, [(x, 0.0, z) for x, z in mine],
          S.CLEAR[S.PANEL_BOLT], name="Panel bolt holes")

    # Through the panel itself: the wheel's axle bolt, the side shelf, and
    # the two hinge leaves the door hangs on.
    holes(d, "x", 0.0, hand,
          [(0.0, S.WHEEL_AXIS_Y, S.WHEEL_AXIS_Z)], S.AXLE_BOLT_D + 0.2,
          name="Axle bolt hole")
    holes(d, "x", 0.0, hand,
          [(0.0, S.SIDE_SHELF_Y_C + y, S.SIDE_SHELF_BOLT_Z)
           for y in S.SIDE_SHELF_BOLTS],
          S.CLEAR[S.SIDE_SHELF_BOLT], name="Side shelf bolt holes")
    leaf = [(0.0, S.CART_FRONT - S.T_CART - S.HINGE_TANG + x,
             S.hinge_bolt_z(hand, z))
            for z in S.DOOR_HINGE_Z for x in S.HINGE_BOLT_X]
    holes(d, "x", 0.0, hand, leaf, S.CLEAR[S.HINGE_BOLT],
          name="Hinge leaf holes")

    # --- sockets, in world coordinates, because the part is drawn in them --
    frame(d, "origin", (0.0, 0.0, 0.0))
    frame(d, "side_shelf", (hand * (S.SIDE_OUT + S.SIDE_SHELF_X / 2.0),
                            S.SIDE_SHELF_Y_C, S.SIDE_SHELF_Z),
          z_axis=(0, 0, 1), x_axis=(hand, 0, 0))
    frame(d, "foot", (hand * S.FLANGE_BOLT_X, S.FOOT_Y, 0.0))
    # The wheel stands OUTSIDE the panel, so its own axis points outward;
    # the bolt goes the other way, in from the head.
    frame(d, "wheel", (hand * S.SIDE_OUT, S.WHEEL_AXIS_Y, S.WHEEL_AXIS_Z),
          z_axis=(hand, 0, 0), x_axis=(0, 1, 0))
    frame(d, "axle_bolt", (hand * (S.SIDE_OUT + S.WHEEL_W), S.WHEEL_AXIS_Y,
                           S.WHEEL_AXIS_Z),
          z_axis=(-hand, 0, 0), x_axis=(0, 1, 0))
    knuckle = (hand * (S.SIDE_OUT + S.HINGE_TANG),
               S.CART_FRONT - S.T_CART - S.HINGE_TANG)
    # The door turns about the hinge's knuckle — the axis the leaves share.
    frame(d, "door", (knuckle[0], knuckle[1], S.DOOR_BOTTOM),
          z_axis=(0, 0, 1), x_axis=(1, 0, 0))
    for i, z in enumerate(S.DOOR_HINGE_Z):
        # The cart's leaf takes the lower half of the knuckle on the left
        # and the upper half on the right, so the door's leaf takes the
        # other one and the two interleave rather than occupying the same
        # 20 mm of pin.
        up = (0, 0, 1) if hand > 0 else (0, 0, -1)
        frame(d, f"hinge_leaf_{i + 1}", (knuckle[0], knuckle[1], z + 20.0),
              z_axis=up, x_axis=(0, 1, 0))
        frame(d, f"hinge_pin_{i + 1}", (knuckle[0], knuckle[1], z + 42.0),
              z_axis=(0, 0, -1), x_axis=(1, 0, 0))
    flat_config(d, base)
    sheet_ok(d, f"cart side {side}")
    return base


def cart_back(d):
    """The back panel: between the sides, on their back flanges."""
    part(d, "Cart back", S.CART_STOCK)
    t = thickness(d)
    face = S.CART_BACK - S.T_CART                 # the flanges' inner face
    base = sheet_panel(d, "y", face, -1,
                       (-S.BACK_X / 2, face, S.BACK_BOTTOM),
                       (S.BACK_X / 2, face, S.BACK_TOP), name="Back blank")
    rails = edges_at(d, lambda m: abs(m[1] - (face - t)) < near(t)
                     and (abs(m[2] - S.BACK_BOTTOM) < near(t)
                          or abs(m[2] - S.BACK_TOP) < near(t)),
                     what="back panel edge")
    flange(d, base, rails, S.CART_FLANGE, name="Stiffeners",
           position="material_inside")
    holes(d, "y", 100.0, +1, [(x, 0.0, z) for x, z in S.BACK_BOLTS],
          S.CLEAR[S.BACK_BOLT], name="Bolt holes")
    frame(d, "origin", (0.0, 0.0, 0.0))
    flat_config(d, base)
    sheet_ok(d, "cart back")
    return base


def cart_shelf(d):
    """The bottom shelf: a pan on the sides' bottom flanges, holding the
    bottle."""
    part(d, "Cart shelf", S.CART_STOCK)
    t = thickness(d)
    base = sheet_panel(d, "z", S.SHELF_Z, +1,
                       (-S.SHELF_X / 2, -S.SHELF_Y / 2, S.SHELF_Z),
                       (S.SHELF_X / 2, S.SHELF_Y / 2, S.SHELF_Z),
                       name="Shelf blank")
    walls = edges_at(d, lambda m: abs(m[2] - (S.SHELF_Z + t)) < near(t) and (
        abs(abs(m[0]) - S.SHELF_X / 2) < near(t)
        or abs(abs(m[1]) - S.SHELF_Y / 2) < near(t)), what="shelf base edge")
    flange(d, base, walls, S.SHELF_WALL, name="Shelf walls",
           position="material_inside")
    corner_relief(d, base, name="Corner relief")
    holes(d, "z", S.SHELF_Z + 20.0, -1, [(x, y, 0.0) for x, y in S.SHELF_BOLTS],
          S.CLEAR[S.SHELF_BOLT], name="Bolt holes")
    frame(d, "origin", (0.0, 0.0, 0.0))
    frame(d, "tank", (0.0, S.TANK_Y, S.SHELF_Z + t))
    flat_config(d, base)
    sheet_ok(d, "cart shelf")
    return base


def control_panel(d):
    """The face the knobs come through: bolted to the cart's front flanges,
    and to the firebox's front wall by a lip at the top."""
    part(d, "Control panel", S.PANEL_STOCK)
    t = thickness(d)
    base = sheet_panel(d, "y", S.PANEL_FACE, -1,
                       (-S.PANEL_X / 2, S.PANEL_FACE, S.PANEL_BOTTOM_Z),
                       (S.PANEL_X / 2, S.PANEL_FACE, S.PANEL_TOP_Z),
                       name="Panel blank")
    # Turned BACK, away from the cook: the blank's material runs to -Y, so
    # the REAR face is the one at PANEL_FACE and the front — the one a hand
    # touches — is a thickness in front of it. Picking the front face folded
    # the stiffeners forward, over the knobs.
    inset = S.PANEL_STIFF_INSET
    bottom = edges_at(d, lambda m: abs(m[1] - S.PANEL_FACE) < near(t)
                      and abs(m[2] - S.PANEL_BOTTOM_Z) < near(t),
                      what="panel bottom edge")
    flange(d, base, bottom, S.PANEL_BOTTOM_STIFF, name="Bottom stiffener",
           position="material_inside", offset_start=inset, offset_end=inset)
    top = edges_at(d, lambda m: abs(m[1] - S.PANEL_FACE) < near(t)
                   and abs(m[2] - S.PANEL_TOP_Z) < near(t),
                   what="panel top edge")
    # A flange REACHES its length plus a bend radius: the length is measured
    # from the bend's tangent line, not from the face the edge is on. The
    # lip on the end of this one has to land on the firebox's front wall, so
    # the reach is the number that matters and the length is derived from it.
    flange(d, base, top, S.PANEL_TOP_REACH - bend_radius(d),
           name="Top stiffener", position="material_inside",
           offset_start=inset, offset_end=inset)
    # And up, onto the firebox's front wall: the lip that carries the panel's
    # top. material_inside puts its outer face on the wall it bolts to.
    lip_edge = edges_at(d, lambda m: abs(m[2] - S.PANEL_TOP_Z) < near(t)
                        and abs(m[1] - S.FRONT_WALL_OUT) < near(t),
                        what="top stiffener edge")
    flange(d, base, lip_edge, S.PANEL_LIP, name="Lip",
           position="material_inside")

    # Everything through the face, from one plane in front of it: the valve
    # stems, the screws that hold each valve, and the bolts into the cart.
    bores = [(S.burner_x(i), 0.0, S.BURNER_Z) for i in range(S.BURNERS)]
    holes(d, "y", -100.0, -1, bores, S.STEM_BORE, name="Valve bores")
    screws = [(S.burner_x(i) + sx * S.VALVE_SCREW_X, 0.0, S.BURNER_Z)
              for i in range(S.BURNERS) for sx in S.sides()]
    holes(d, "y", -100.0, -1, screws, S.CLEAR[S.VALVE_BOLT],
          name="Valve screw holes")
    holes(d, "y", -100.0, -1, [(x, 0.0, z) for x, z in S.PANEL_BOLTS],
          S.CLEAR[S.PANEL_BOLT], name="Cart bolt holes")
    holes(d, "y", -100.0, -1,
          [(x, 0.0, S.PANEL_LIP_Z) for x in S.PANEL_LIP_BOLTS],
          S.CLEAR[S.PANEL_LIP_BOLT], name="Lip bolt holes")

    frame(d, "origin", (0.0, 0.0, 0.0))
    for i in range(S.BURNERS):
        frame(d, f"valve_{i + 1}", (S.burner_x(i), S.PANEL_FACE, S.BURNER_Z),
              z_axis=(0, 1, 0), x_axis=(1, 0, 0))
    flat_config(d, base)
    sheet_ok(d, "control panel")
    return base


def cart_door(d, hand):
    """A door: flat, hemmed on the three edges a hand meets, hung on two
    leaves welded to its front."""
    side = hand_name(hand)
    part(d, f"Cart door {side}", S.CART_STOCK)
    t = thickness(d)
    outer = hand * S.SIDE_OUT
    innerx = hand * (S.SIDE_OUT - S.DOOR_X)
    lo, hi = min(outer, innerx), max(outer, innerx)
    base = sheet_panel(d, "y", S.DOOR_FACE, -1,
                       (lo, S.DOOR_FACE, S.DOOR_BOTTOM),
                       (hi, S.DOOR_FACE, S.DOOR_TOP), name="Door blank")
    front = S.DOOR_FACE - t
    inset = bend_radius(d) + t
    # Hemmed toward the FRONT, so the back stays flat against the cart's
    # flange. The latch-side edge first, then top and bottom inset clear of
    # it: hemming two edges that meet would have each hem run into its
    # neighbour at the corner.
    latch = edges_at(d, lambda m: abs(m[1] - front) < near(t)
                     and abs(m[0] - innerx) < near(t),
                     what="door latch edge")
    hem(d, base, latch, S.DOOR_HEM, name="Latch hem")
    # The latch hem shortened these two edges, so their midpoints have moved
    # off the panel's centre line by half a hem: the pick is "the long edge
    # along the top and the bottom", not a point.
    ends = edges_at(d, lambda m: abs(m[1] - front) < near(t)
                    and (abs(m[2] - S.DOOR_BOTTOM) < near(t)
                         or abs(m[2] - S.DOOR_TOP) < near(t))
                    and abs(m[0] - (lo + hi) / 2.0) < S.DOOR_HEM,
                    what="door end edge")
    hem(d, base, ends, S.DOOR_HEM, name="End hems",
        offset_start=inset + S.DOOR_HEM, offset_end=inset + S.DOOR_HEM)

    knuckle = (hand * (S.SIDE_OUT + S.HINGE_TANG),
               S.DOOR_FACE - t - S.HINGE_TANG)
    frame(d, "origin", (0.0, 0.0, 0.0))
    frame(d, "hinge", (knuckle[0], knuckle[1], S.DOOR_BOTTOM),
          z_axis=(0, 0, 1), x_axis=(1, 0, 0))
    for i, z in enumerate(S.DOOR_HINGE_Z):
        up = (0, 0, -1) if hand > 0 else (0, 0, 1)
        frame(d, f"hinge_leaf_{i + 1}", (knuckle[0], knuckle[1], z + 20.0),
              z_axis=up, x_axis=(-hand, 0, 0))
    flat_config(d, base)
    sheet_ok(d, f"cart door {side}")
    return base


def side_shelf(d):
    """A side shelf: a tray with a drop edge all round, bolted to the cart
    through the drop edge nearest it.

    All four edges drop, so ONE part serves both sides: turned end for end,
    the edge that carries the bolts faces the other way.
    """
    part(d, "Side shelf", S.SHELF_STOCK)
    t = thickness(d)
    half_x, half_y = S.SIDE_SHELF_X / 2.0, S.SIDE_SHELF_Y / 2.0
    base = sheet_panel(d, "z", 0.0, -1, (-half_x, -half_y, 0.0),
                       (half_x, half_y, 0.0), name="Shelf blank")
    drops = edges_at(d, lambda m: abs(m[2] + t) < near(t) and (
        abs(abs(m[0]) - half_x) < near(t)
        or abs(abs(m[1]) - half_y) < near(t)), what="shelf edge")
    flange(d, base, drops, S.SIDE_SHELF_DROP, name="Drop edges",
           position="material_inside")
    # No corner relief here: four edges folded the same way in ONE flange
    # feature come out with their corners already open, and asking for a
    # relief is refused — "a corner relief cut nothing".
    # Only the -x drop edge is drilled: it is the one that meets the cart,
    # and a plane looking that way from the middle of the shelf sees no
    # other sheet.
    holes(d, "x", 0.0, -1,
          [(0.0, y, -(S.SIDE_SHELF_Z - S.SIDE_SHELF_BOLT_Z))
           for y in S.SIDE_SHELF_BOLTS],
          S.CLEAR[S.SIDE_SHELF_BOLT], name="Bolt holes")
    frame(d, "origin", (0.0, 0.0, 0.0))
    flat_config(d, base)
    sheet_ok(d, "side shelf")
    return base
