"""The cold half: the cart that carries the firebox, and the control panel.

Every part here is one blank and a few bends — which is the point. A cart is
where sheet metal is cheapest and most obvious: flanges for stiffness, hems
where a hand goes, and nothing that a brake cannot make in one setup.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from grill_lib import (bend_radius, blank, corner_relief, edges_at, flange,  # noqa: E402
                       flat_config, frame, hem, normal_cut, outer_edge, part,
                       profile_blank, sheet_ok, thickness)
from _lib import circle, plane_at_x, plane_at_y, plane_at_z, rect  # noqa: E402

import grill_spec as S  # noqa: E402


def near(t):
    return max(1e-4, 0.25 * t)


def plane_facing_front(y):
    """At `y`, looking toward -Y; V runs up (+Z)."""
    return {"origin": [0, y, 0], "normal": [0, -1, 0], "uAxis": [1, 0, 0]}


def pan(d, x, y, wall, name, hem_rim=0.0):
    """A blank with four walls up, corners relieved — the cart's shape."""
    t = thickness(d)
    base = blank(d, x, y, name=name + " blank")
    walls = edges_at(d, lambda m: abs(m[2] - t) < near(t) and (
        abs(abs(m[0]) - x / 2) < near(t) or abs(abs(m[1]) - y / 2) < near(t)),
        what="pan base edge")
    flange(d, base, walls, wall, name=name + " walls",
           position="material_inside")
    corner_relief(d, base, name=name + " relief")
    if hem_rim > 0.0:
        inset = bend_radius(d) + t
        top_z = t + wall
        rim = []
        for axis, sign, extent in ((0, +1, x), (0, -1, x), (1, +1, y),
                                   (1, -1, y)):
            rim.append(outer_edge(
                d, lambda m, a=axis, s=sign, e=extent: (
                    abs(m[2] - top_z) < 3.0 and s * m[a] > e / 2 - 5.0),
                axis, sign, what="pan wall top edge"))
        hem(d, base, rim, hem_rim, name=name + " safe edges",
            offset_start=inset, offset_end=inset)
    return base


def control_panel(d):
    """The face the knobs come through, flanged back top and bottom."""
    part(d, "Control panel", S.PANEL_STOCK)
    t = thickness(d)
    base = profile_blank(d, plane_at_y(0.0),
                         [rect((-S.PANEL_X / 2, -S.PANEL_H / 2),
                               (S.PANEL_X / 2, S.PANEL_H / 2))],
                         name="Panel blank")
    # Turned BACK, away from the cook: picked on the rear face so the flange
    # folds away from the face rather than across it.
    rails = edges_at(d, lambda m: abs(m[1] + t) < near(t)
                     and abs(abs(m[2]) - S.PANEL_H / 2) < near(t),
                     what="panel edge")
    flange(d, base, rails, S.PANEL_TOP, name="Stiffeners",
           position="material_inside")
    # The valve stems and the igniter, square through the face.
    bores = []
    for i in range(S.BURNERS):
        x = (i - (S.BURNERS - 1) / 2.0) * S.BURNER_PITCH
        bores.append(circle((x, S.VALVE_Z), S.VALVE_BORE / 2.0))
    bores.append(circle((S.PANEL_X / 2 - 60.0, S.VALVE_Z),
                        S.IGNITER_BORE / 2.0))
    normal_cut(d, plane_facing_front(-80.0), bores, name="Valve bores")
    frame(d, "face", (0.0, 0.0, 0.0), z_axis=(0, -1, 0), x_axis=(1, 0, 0))
    flat_config(d, base)
    sheet_ok(d, "control panel")
    return base


def cart_side(d):
    """One side of the cart: a panel with a flange turned in on every edge.

    Drawn on the plane it lives on (YZ), not flat on the bench and rotated
    later: a part built in place needs no orientation in the assembly, and
    an orientation is the easiest thing in an assembly to get wrong.
    """
    part(d, "Cart side", S.CART_STOCK)
    t = thickness(d)
    base = profile_blank(d, plane_at_x(0.0),
                         [rect((-S.CART_Y / 2, 0.0), (S.CART_Y / 2, S.CART_H))],
                         name="Side blank")
    edges = edges_at(d, lambda m: abs(m[0] - t) < near(t) and (
        abs(abs(m[1]) - S.CART_Y / 2) < near(t)
        or abs(m[2]) < near(t) or abs(m[2] - S.CART_H) < near(t)),
        what="side panel edge")
    flange(d, base, edges, S.CART_FLANGE, name="Stiffening flanges",
           position="material_inside")
    corner_relief(d, base, name="Corner relief")
    # The axle passes through, so the panel has a hole for it rather than the
    # two occupying the same steel.
    # V = normal x uAxis, so on this plane v runs DOWN (-Z): the hole's
    # height is negative here, and a positive one puts it under the floor.
    normal_cut(d, {"origin": [60.0, 0, 0], "normal": [-1, 0, 0],
                   "uAxis": [0, 1, 0]},
               [circle((S.CART_Y / 2 - 70.0, -95.0), S.AXLE_D / 2 + 1.0)],
               name="Axle hole")
    frame(d, "inner", (t, 0.0, 0.0))
    flat_config(d, base)
    sheet_ok(d, "cart side")
    return base


def cart_back(d):
    """The back panel, drawn standing in the XZ plane."""
    part(d, "Cart back", S.CART_STOCK)
    t = thickness(d)
    base = profile_blank(d, plane_at_y(0.0),
                         [rect((-S.BACK_X / 2, 0.0), (S.BACK_X / 2, S.CART_H))],
                         name="Back blank")
    edges = edges_at(d, lambda m: abs(m[1] + t) < near(t)
                     and abs(abs(m[0]) - S.BACK_X / 2) < near(t),
                     what="back panel edge")
    flange(d, base, edges, S.CART_FLANGE, name="Return flanges",
           position="material_inside")
    frame(d, "inner", (0.0, 0.0, 0.0))
    flat_config(d, base)
    sheet_ok(d, "cart back")
    return base


def cart_shelf(d):
    part(d, "Cart shelf", S.CART_STOCK)
    base = pan(d, S.SHELF_X, S.SHELF_Y, S.SHELF_WALL, "Shelf")
    frame(d, "seat", (0.0, 0.0, 0.0))
    flat_config(d, base)
    sheet_ok(d, "cart shelf")
    return base


def cart_door(d):
    """A door: flat, hemmed all round so no cut edge is exposed."""
    part(d, "Cart door", S.CART_STOCK)
    t = thickness(d)
    base = profile_blank(d, plane_at_y(0.0),
                         [rect((-S.DOOR_X / 2, 0.0), (S.DOOR_X / 2, S.DOOR_Y))],
                         name="Door blank")
    inset = bend_radius(d) + t
    # Two opposite edges first, then the other two: hemming all four at once
    # would have each hem run into its neighbour at the corner. Each pick is
    # a FULL edge, so its midpoint sits on the panel's centre line — the
    # little edges the first hems leave at the corners do not.
    sides = edges_at(d, lambda m: abs(m[1] + t) < near(t)
                     and abs(abs(m[0]) - S.DOOR_X / 2) < near(t)
                     and abs(m[2] - S.DOOR_Y / 2) < near(t),
                     what="door side edge")
    hem(d, base, sides, S.DOOR_HEM, name="Side hems")
    ends = edges_at(d, lambda m: abs(m[1] + t) < near(t)
                    and (abs(m[2]) < near(t) or abs(m[2] - S.DOOR_Y) < near(t))
                    and abs(m[0]) < near(t),
                    what="door end edge")
    hem(d, base, ends, S.DOOR_HEM, name="End hems",
        offset_start=inset + S.DOOR_HEM, offset_end=inset + S.DOOR_HEM)
    frame(d, "hinge", (-S.DOOR_X / 2, 0.0, S.DOOR_Y / 2), z_axis=(0, 0, 1),
          x_axis=(1, 0, 0))
    flat_config(d, base)
    sheet_ok(d, "cart door")
    return base


def side_shelf(d):
    """A fold-down shelf: flat, with a drop edge on three sides."""
    part(d, "Side shelf", S.SHELF_STOCK)
    t = thickness(d)
    base = blank(d, S.SIDE_SHELF_X, S.SIDE_SHELF_Y, name="Shelf blank")
    drops = edges_at(d, lambda m: abs(m[2]) < near(t) and (
        abs(abs(m[0]) - S.SIDE_SHELF_X / 2) < near(t)
        or m[1] < -S.SIDE_SHELF_Y / 2 + near(t)),
        what="shelf edge")
    flange(d, base, drops, S.SIDE_SHELF_DROP, name="Drop edges",
           position="material_inside")
    corner_relief(d, base, name="Corner relief")
    frame(d, "hinge", (0.0, S.SIDE_SHELF_Y / 2, 0.0))
    flat_config(d, base)
    sheet_ok(d, "side shelf")
    return base
