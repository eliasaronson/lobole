"""The hot half: firebox pan, hood, hood end caps, heat tents, grease tray."""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from grill_lib import (blank, bend_radius, corner_relief, edges_at, flange,  # noqa: E402
                       flat_config, frame, hem, normal_cut, outer_edge, part,
                       profile_blank, sheet_ok, sketch, thickness)
from _lib import circle, line, plane_at_z, rect  # noqa: E402  (rect: tent blank)

import grill_spec as S  # noqa: E402

def near(t):
    """The pick tolerance for a sheet of thickness `t`.

    A quarter of the thickness: on 0.8 mm stock a fixed 0.6 mm window also
    caught the blank's own corner edges, whose midpoints sit at t/2, and the
    flange then refused a corner as a panel edge. Derived from the sheet, like
    everything else here.
    """
    return max(1e-4, 0.25 * t)


def plane_facing_front(y):
    """A sketch plane at `y` looking toward -Y, so V runs up (+Z).

    Inside the box looking out: a normal cut travels along the sketch normal,
    so a plane OUTSIDE the box looking in would meet the front wall and then
    the back one — two panels, and no single sheet to cut.
    """
    return {"origin": [0, y, 0], "normal": [0, -1, 0], "uAxis": [1, 0, 0]}


def firebox_pan(d):
    """The pan: base, four walls, relieved corners, a rim, and its ports."""
    part(d, "Firebox", S.FIREBOX_STOCK)
    t = thickness(d)
    base = blank(d, S.BOX_X, S.BOX_Y, name="Firebox blank")

    # The four top edges of the base panel: picked on the TOP face, so the
    # walls fold up. Picking the same rim's bottom edge folds them down.
    walls = edges_at(d, lambda m: abs(m[2] - t) < near(t) and (
        abs(abs(m[0]) - S.BOX_X / 2) < near(t) or abs(abs(m[1]) - S.BOX_Y / 2) < near(t)),
        what="base panel edge")
    # material_inside: the blank IS the box's outer size, so the bend lines
    # run inside it and cross at the corners — which is what makes the
    # relief necessary, and what a brake would tear without.
    flange(d, base, walls, S.BOX_H, name="Walls", position="material_inside")

    # Four corners where two bends cross, each one relieved.
    corner_relief(d, base, name="Corner relief")

    # The rim: the outermost top edge of each wall, folded out to carry the
    # hood and stiffen the box.
    top_z = t + S.BOX_H
    rim = []
    for axis, sign, extent in ((0, +1, S.BOX_X), (0, -1, S.BOX_X),
                               (1, +1, S.BOX_Y), (1, -1, S.BOX_Y)):
        rim.append(outer_edge(
            d, lambda m, a=axis, s=sign, e=extent: (
                abs(m[2] - top_z) < 3.0 and s * m[a] > e / 2 - 5.0),
            axis, sign, what="wall top edge"))
    # Inset at both ends by a radius and a thickness: a rim that ran the
    # full length of its wall would meet the wall beside it at the corner and
    # fuse to it, and a corner of continuous metal folds from no blank.
    inset = bend_radius(d) + t
    flange(d, base, rim, S.BOX_RIM, name="Rim",
           offset_start=inset, offset_end=inset)

    # Burner ports through the FRONT wall, and the grease drain through the
    # base. Both are normal cuts: an extrude cut would come out slanted
    # anywhere it crossed a bend.
    ports = []
    for i in range(S.BURNERS):
        x = (i - (S.BURNERS - 1) / 2.0) * S.BURNER_PITCH
        ports.append(circle((x, t + S.BURNER_AXIS_Z), S.BURNER_PORT_D / 2.0))
    normal_cut(d, plane_facing_front(-S.BOX_Y / 2 + 40.0), ports,
               name="Burner ports")
    normal_cut(d, plane_at_z(200.0, flip=True),
               [circle((0.0, -110.0), S.DRAIN_D / 2.0)], name="Grease drain")

    # Where the rest of the grill anchors: the rim's back edge (the hood
    # hinges here) and the pan's underside (the cart carries it).
    # The lid hinges here, about an axis running ACROSS the grill: a frame's
    # z is the axis a revolute turns on, and a lid that turns about the
    # vertical would swing sideways like a door.
    # On TOP of the rim, not level with the wall's top edge: the lid rests on
    # the rim, and a hinge half a thickness low buries the facet in it.
    frame(d, "hinge", (0.0, S.BOX_Y / 2, top_z + t + S.LID_GAP),
          z_axis=(1, 0, 0), x_axis=(0, 1, 0))
    frame(d, "underside", (0.0, 0.0, 0.0))
    frame(d, "front_face", (0.0, -(S.BOX_Y / 2), t + S.BOX_H / 2),
          z_axis=(0, -1, 0), x_axis=(1, 0, 0))
    flat_config(d, base)
    sheet_ok(d, "firebox")
    return base


def hood(d):
    """The lid: a flat top with a facet folded down front and back."""
    part(d, "Hood", S.HOOD_STOCK)
    t = thickness(d)
    base = blank(d, S.BOX_X, S.HOOD_TOP_Y, name="Hood blank")
    facets = edges_at(d, lambda m: abs(m[2] - t) < near(t)
                      and abs(abs(m[1]) - S.HOOD_TOP_Y / 2) < near(t),
                      what="hood top edge")
    # Folded DOWN: the picked edges are on the top face, so the fold goes up
    # unless the wall is turned the other way — the lid's facets hang below,
    # so they are picked on the underside.
    facets_down = edges_at(d, lambda m: abs(m[2]) < near(t)
                           and abs(abs(m[1]) - S.HOOD_TOP_Y / 2) < near(t),
                           what="hood underside edge")
    flange(d, base, facets_down, S.HOOD_FACET, name="Facets",
           angle_deg=S.HOOD_FACET_DEG)
    normal_cut(d, plane_at_z(300.0, flip=True),
               [circle((0.0, 0.0), S.THERMO_D / 2.0)], name="Thermometer bore")
    # At the bottom of the back facet — the edge that actually lands on the
    # firebox rim — and about the same across-the-grill axis.
    import math as _m
    run = S.HOOD_FACET * _m.cos(_m.radians(S.HOOD_FACET_DEG))
    drop = S.HOOD_FACET * _m.sin(_m.radians(S.HOOD_FACET_DEG))
    frame(d, "hinge", (0.0, S.HOOD_TOP_Y / 2 + run, -drop), z_axis=(1, 0, 0),
          x_axis=(0, 1, 0))
    frame(d, "handle_left", (-S.HANDLE_SPAN / 2, -S.HOOD_TOP_Y / 2 - 40.0, -40.0))
    flat_config(d, base)
    sheet_ok(d, "hood")
    return base


def hood_cap(d):
    """One end cap: the hood's cross-section, welded on."""
    part(d, "Hood end cap", S.HOOD_STOCK)
    drop = S.HOOD_FACET * math.sin(math.radians(S.HOOD_FACET_DEG))
    run = S.HOOD_FACET * math.cos(math.radians(S.HOOD_FACET_DEG))
    half = S.HOOD_TOP_Y / 2
    # A trapezoid: the flat of the lid, and a facet falling away each side.
    pts = [(-half - run, -drop), (-half, 0.0), (half, 0.0), (half + run, -drop)]
    ents = [line(pts[i], pts[i + 1]) for i in range(len(pts) - 1)]
    ents.append(line(pts[-1], pts[0]))
    base = profile_blank(d, {"origin": [0, 0, 0], "normal": [1, 0, 0],
                             "uAxis": [0, 1, 0]}, ents, name="Cap blank")
    frame(d, "weld", (0.0, 0.0, 0.0), z_axis=(1, 0, 0), x_axis=(0, 1, 0))
    sheet_ok(d, "hood cap")
    return base


def heat_tent(d):
    """An inverted V over a burner, hung by a tab at each end."""
    part(d, "Heat tent", S.TENT_STOCK)
    t = thickness(d)
    # Drawn with the fold edge ON the origin: the part is then tilted about
    # its own ridge in the assembly, and placing it over a burner is one
    # number rather than a correction for where the ridge drifted to.
    base = profile_blank(d, plane_at_z(0.0),
                         [rect((-S.TENT_LEG, -S.TENT_LEN / 2),
                               (0.0, S.TENT_LEN / 2))], name="Tent blank")
    fold = edges_at(d, lambda m: abs(m[2] - t) < near(t)
                    and abs(m[0]) < near(t), what="tent fold edge")
    flange(d, base, fold, S.TENT_LEG, name="Second leg",
           angle_deg=180.0 - S.TENT_DEG)
    # Hung by a tab at each end, folded DOWN so it hooks over the firebox
    # wall: picked on the UNDERSIDE, since which of a rim's two edges is
    # picked is what decides the way a wall folds.
    tabs = edges_at(d, lambda m: abs(m[2]) < near(t)
                    and abs(abs(m[1]) - S.TENT_LEN / 2) < near(t)
                    and -S.TENT_LEG < m[0] < 0.0, what="tent end edge")
    flange(d, base, tabs, S.TENT_TAB, name="Hanging tabs")
    # Each tab's bend line runs into the V fold: four corners where two bends
    # cross, and the metal tears at every one of them without a cut to move
    # into. A flange's own `relief` is for a bend line stopped by an OFFSET;
    # this is the corner case, and check_sheet_metal fires until it is cut.
    # RECTANGULAR, not the default circular one: on this 60-degree ridge a
    # circular relief leaves the blank 1.67 mm thick on 1 mm sheet — see the
    # findings in docs/grill-dogfood-plan.md. A square notch is what a shop
    # would punch here anyway.
    corner_relief(d, base, name="Tab relief", style="rectangular")
    frame(d, "peak", (0.0, 0.0, t))
    flat_config(d, base)
    sheet_ok(d, "heat tent")
    return base


def grease_tray(d):
    """A shallow pan under the drain, hemmed where a hand takes hold."""
    part(d, "Grease tray", S.TRAY_STOCK)
    t = thickness(d)
    base = blank(d, S.TRAY_X, S.TRAY_Y, name="Tray blank")
    walls = edges_at(d, lambda m: abs(m[2] - t) < near(t) and (
        abs(abs(m[0]) - S.TRAY_X / 2) < near(t) or abs(abs(m[1]) - S.TRAY_Y / 2) < near(t)),
        what="tray base edge")
    flange(d, base, walls, S.TRAY_H, name="Tray walls")
    corner_relief(d, base, name="Tray relief")
    # The outer top edge of each wall, hemmed: this is the edge a hand takes
    # hold of. Inset like the firebox rim, or the hem meets the wall beside
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
    frame(d, "slide", (0.0, 0.0, 0.0))
    flat_config(d, base)
    sheet_ok(d, "grease tray")
    return base
