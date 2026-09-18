"""Everything on the grill that is not folded sheet: burners, gas, grates,
wheels, the tank, and the bits a hand touches.

Drawn, not stamped out of primitives. A propane bottle is a profile revolved
about its axis — a domed shoulder, a rolled foot, a collar you can get a hand
into — and a box-and-cylinder stand-in is a picture of something else. Even
the plain round bars are sketched and extruded, so every part opens the same
way and any dimension on it can be edited.

Each part also carries the frames it is mated by: a mate connector belongs to
the part, not to a number typed into the assembly.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from grill_lib import check, frame, part, sketch  # noqa: E402
from _lib import circle, line, plane_at_x, plane_at_y, plane_at_z, rect  # noqa: E402

import grill_spec as S  # noqa: E402


# --- drawing ---------------------------------------------------------------

def loop(points):
    """A closed polyline through `points` — the profile of a revolve."""
    return [line(points[i], points[(i + 1) % len(points)])
            for i in range(len(points))]


def owner(d, reply, name):
    """The id that OWNS what a feature just made.

    A boolean consumes its operands and an add merges into its target, so the
    id that went in is not the one that comes out; the call's own digest says
    which body it left behind.
    """
    for body in reply.get("digest", {}).get("bodies", []):
        if body["name"] == name:
            return body["created_by"]
    return reply["id"]


def revolve(d, name, plane, points, axis, angle_deg=360.0, mode="new_body"):
    sk = sketch(d, plane, loop(points), name + " profile")
    r = d.call("add_feature", feature={
        "type": "Revolve", "name": name, "sketch_id": sk,
        "axis": {"kind": axis}, "angle_deg": angle_deg, "mode": mode})
    check(r, name)
    return owner(d, r, name)


def extrude(d, name, plane, entities, distance, mode="new_body", **kw):
    sk = sketch(d, plane, entities, name + " profile")
    f = {"type": "Extrude", "name": name, "sketch_id": sk,
         "distance": distance, "mode": mode}
    f.update(kw)
    r = d.call("add_feature", feature=f)
    check(r, name)
    return owner(d, r, name)


def boolean(d, name, target, tool, op="union"):
    r = d.call("add_feature", feature={
        "type": "Boolean", "name": name, "target_id": target,
        "tool_id": tool, "op": op})
    check(r, name)
    return owner(d, r, name)


def material(d, name):
    """Every body of this part is one material — the part IS the part.

    EVERY body: a patterned grate is one body per bar, and setting only the
    first left the rest with no material at all, so the grate weighed nothing
    and the whole assembly's mass came back null because of it.
    """
    for i in range(len(d.call("get_state")["bodies"])):
        d.call("set_material", material=name, body=i)


def burner_x(i):
    return (i - (S.BURNERS - 1) / 2.0) * S.BURNER_PITCH


# --- the gas train ---------------------------------------------------------

def burner(d):
    """A burner tube and the venturi that slides over the valve's orifice:
    one profile, revolved about the tube's own axis."""
    part(d, "Burner")
    r_tube = S.BURNER_D / 2.0
    r_vent = r_tube + 3.0
    back = S.BURNER_LEN / 2.0
    front = -S.BURNER_LEN / 2.0
    mouth = front - 40.0
    # (u, v) = (y along the tube, radius): the venturi, the step, the tube.
    revolve(d, "Tube", plane_at_x(0.0),
            [(mouth, 0.0), (mouth, r_vent), (front, r_vent), (front, r_tube),
             (back, r_tube), (back, 0.0)], axis="y")
    frame(d, "inlet", (0.0, mouth, 0.0), z_axis=(0, -1, 0), x_axis=(1, 0, 0))
    material(d, "stainless_steel")


def manifold(d):
    """The tube the valves hang off — a tube, with a bore."""
    part(d, "Manifold")
    half = S.MANIFOLD_LEN / 2.0
    r_out = S.MANIFOLD_D / 2.0
    r_in = r_out - 2.0
    revolve(d, "Manifold tube", plane_at_y(0.0),
            [(-half, r_in), (half, r_in), (half, r_out), (-half, r_out)],
            axis="x")
    frame(d, "centre", (0.0, 0.0, 0.0), z_axis=(1, 0, 0), x_axis=(0, 1, 0))
    # One socket per valve, on the tube's front.
    for i in range(S.BURNERS):
        frame(d, f"valve_{i + 1}", (burner_x(i), -r_out, 0.0),
              z_axis=(0, -1, 0), x_axis=(1, 0, 0))
    material(d, "brass")


def valve(d):
    """One burner valve: a drawn body, a stem through the panel, and the
    orifice that feeds the venturi behind it."""
    part(d, "Valve")
    w, h, dp = S.VALVE_BODY
    body = extrude(d, "Body", plane_at_y(dp / 2.0),
                   [rect((-w / 2.0, -h / 2.0), (w / 2.0, h / 2.0))], dp)
    stem = extrude(d, "Stem", plane_at_y(-dp / 2.0),
                   [circle((0.0, 0.0), S.VALVE_STEM_D / 2.0)], S.VALVE_STEM_L)
    body = boolean(d, "Body and stem", body, stem)
    frame(d, "stem_end", (0.0, -dp / 2.0 - S.VALVE_STEM_L, 0.0),
          z_axis=(0, -1, 0),
          x_axis=(1, 0, 0))
    frame(d, "manifold", (0.0, dp / 2.0, 0.0), z_axis=(0, 1, 0),
          x_axis=(1, 0, 0))
    material(d, "brass")


def knob(d):
    """A control knob: a skirt, a waist and a grip — revolved, because that
    is what a turned knob is."""
    part(d, "Knob")
    r = S.KNOB_D / 2.0
    # (u, v) = (y along the shaft, radius); y = 0 is the face it seats on.
    revolve(d, "Knob body", plane_at_x(0.0),
            [(0.0, 0.0), (0.0, r - 8.0), (-4.0, r), (-16.0, r),
             (-S.KNOB_H, r - 6.0), (-S.KNOB_H, 0.0)], axis="y")
    # The pointer a thumb finds.
    extrude(d, "Pointer", plane_at_z(r - 10.0),
            [rect((-3.0, -S.KNOB_H), (3.0, -2.0))], r + 4.0, mode="add")
    frame(d, "shaft", (0.0, 0.0, 0.0), z_axis=(0, -1, 0), x_axis=(1, 0, 0))
    material(d, "plastic_black")


# --- what the food sits on -------------------------------------------------

def grate(d):
    """One of three cooking-grate sections: two rails and the bars between."""
    part(d, "Cooking grate")
    rail_w = 10.0
    half_x, half_y = S.GRATE_X / 2.0, S.GRATE_Y / 2.0
    left = extrude(d, "Left rail", plane_at_z(0.0),
                   [rect((-half_x, -half_y), (-half_x + rail_w, half_y))],
                   S.GRATE_BAR_D)
    right = extrude(d, "Right rail", plane_at_z(0.0),
                    [rect((half_x - rail_w, -half_y), (half_x, half_y))],
                    S.GRATE_BAR_D)
    left = boolean(d, "Rails", left, right)
    bar = extrude(d, "Bar", plane_at_x(-half_x + rail_w),
                  [circle((-half_y + 10.0, S.GRATE_BAR_D / 2.0),
                          S.GRATE_BAR_D / 2.0)],
                  S.GRATE_X - 2 * rail_w)
    count = int((S.GRATE_Y - 20.0) // S.GRATE_PITCH) + 1
    check(d.call("add_feature", feature={
        "type": "Pattern", "name": "Bars", "kind": "linear",
        "source_id": bar, "count": count, "spacing": S.GRATE_PITCH,
        "axis": {"kind": "y"}}), "grate bars")
    frame(d, "seat", (0.0, 0.0, 0.0))
    material(d, "steel")  # porcelain-coated cast iron in life


def warming_rack(d):
    """The half-shelf up under the lid."""
    part(d, "Warming rack")
    r = S.RACK_BAR_D / 2.0
    frame_x = S.COOK_X - 20.0
    front = extrude(d, "Front rail", plane_at_x(-frame_x / 2.0),
                    [circle((-S.RACK_Y / 2.0, 0.0), r)], frame_x)
    back = extrude(d, "Back rail", plane_at_x(-frame_x / 2.0),
                   [circle((S.RACK_Y / 2.0, 0.0), r)], frame_x)
    front = boolean(d, "Rails", front, back)
    bar = extrude(d, "Bar", plane_at_y(S.RACK_Y / 2.0),
                  [circle((-frame_x / 2.0 + 20.0, 0.0), r)], S.RACK_Y)
    count = int((frame_x - 40.0) // 40.0) + 1
    check(d.call("add_feature", feature={
        "type": "Pattern", "name": "Bars", "kind": "linear",
        "source_id": bar, "count": count, "spacing": 40.0,
        "axis": {"kind": "x"}}), "rack bars")
    frame(d, "hang", (0.0, S.RACK_Y / 2.0, 0.0))
    material(d, "stainless_steel")


# --- the cart's running gear ----------------------------------------------

def wheel(d):
    """A wheel: tread, dished web and bore, revolved about the axle's own
    axis — so the hub frame IS the axis it spins on."""
    part(d, "Wheel")
    r_out = S.WHEEL_D / 2.0
    r_bore = S.AXLE_D / 2.0
    half = S.WHEEL_W / 2.0
    # (u, v) = (x along the axle, radius): bore, web, rim, tread.
    revolve(d, "Wheel", plane_at_y(0.0),
            [(-half, r_bore), (half, r_bore), (half, r_out - 18.0),
             (half - 6.0, r_out - 18.0), (half - 6.0, r_out),
             (-half + 6.0, r_out), (-half + 6.0, r_out - 18.0),
             (-half, r_out - 18.0)], axis="x")
    frame(d, "hub", (0.0, 0.0, 0.0), z_axis=(1, 0, 0), x_axis=(0, 1, 0))
    material(d, "plastic_black")


def axle(d):
    part(d, "Axle")
    half = (S.CART_X + 80.0) / 2.0
    extrude(d, "Rod", plane_at_x(-half), [circle((0.0, 0.0), S.AXLE_D / 2.0)],
            S.CART_X + 80.0)
    frame(d, "centre", (0.0, 0.0, 0.0), z_axis=(1, 0, 0), x_axis=(0, 1, 0))
    # One frame per wheel: a revolute aligns frame ORIGINS, so both wheels
    # mated to a single centre frame ended up in the same place.
    for side, x in (("left_hub", -1), ("right_hub", +1)):
        frame(d, side,
              (x * (S.CART_X / 2.0 + S.WHEEL_W / 2.0 + 28.0), 0.0, 0.0),
              z_axis=(1, 0, 0), x_axis=(0, 1, 0))
    material(d, "steel")


def foot(d):
    """A moulded foot: a taper, revolved."""
    part(d, "Foot")
    r = S.FOOT_D / 2.0
    # About Z the profile is (radius, height): u is the distance from the
    # axis and v is how far up it. About X or Y it is the other way round,
    # which is the one thing to get right in every one of these.
    revolve(d, "Foot", plane_at_y(0.0),
            [(0.0, 0.0), (r, 0.0), (r, S.FOOT_H - 6.0), (r - 5.0, S.FOOT_H),
             (0.0, S.FOOT_H)], axis="z")
    frame(d, "top", (0.0, 0.0, S.FOOT_H), z_axis=(0, 0, 1), x_axis=(1, 0, 0))
    material(d, "rubber")


# --- the gas supply --------------------------------------------------------

def tank(d):
    """A 9 kg propane bottle, drawn the way one is made: a shell revolved
    about its axis with a domed shoulder and a rolled foot, a collar you can
    get a hand into, and the valve it protects."""
    part(d, "Propane tank")
    wall = 2.5
    r = S.TANK_D / 2.0
    top = S.TANK_H
    body = revolve(d, "Bottle", plane_at_y(0.0),
                   [(0.0, 0.0), (r - 12.0, 0.0), (r - 6.0, 18.0), (r, 40.0),
                    (r, top - 70.0), (r - 30.0, top - 30.0),
                    (r - 95.0, top), (0.0, top)], axis="z")
    # The same shape a wall in: what makes it a bottle and not a billet.
    void = revolve(d, "Void", plane_at_y(0.0),
                   [(0.0, wall), (r - 12.0 - wall, wall),
                    (r - 6.0 - wall, 18.0), (r - wall, 40.0),
                    (r - wall, top - 70.0), (r - 30.0 - wall, top - 30.0),
                    (r - 95.0, top - wall), (0.0, top - wall)], axis="z")
    body = boolean(d, "Shell", body, void, op="subtract")
    collar = revolve(d, "Collar", plane_at_y(0.0),
                     [(r - 95.0, top - 20.0), (r - 83.0, top - 20.0),
                      (r - 83.0, top + S.TANK_COLLAR_H - 20.0),
                      (r - 95.0, top + S.TANK_COLLAR_H - 20.0)], axis="z")
    body = boolean(d, "With collar", body, collar)
    valve_h = S.TANK_COLLAR_H - 45.0
    valve_body = revolve(d, "Valve", plane_at_y(0.0),
                         [(0.0, top - 5.0), (20.0, top - 5.0),
                          (20.0, top + valve_h - 8.0), (14.0, top + valve_h),
                          (0.0, top + valve_h)], axis="z")
    body = boolean(d, "With valve", body, valve_body)
    frame(d, "floor", (0.0, 0.0, 0.0))
    frame(d, "outlet", (0.0, 0.0, top + valve_h))
    material(d, "steel")


def regulator(d):
    """The regulator, and the hose stub that runs off to the manifold."""
    part(d, "Regulator")
    body = revolve(d, "Bell", plane_at_y(0.0),
                   [(0.0, 0.0), (22.0, 0.0), (22.0, 6.0), (35.0, 10.0),
                    (35.0, 40.0), (26.0, 46.0), (0.0, 46.0)], axis="z")
    # High on the bell, so the hose clears the bottle's collar below it.
    # High on the bell: the hose has to clear the bottle's collar, and the
    # stub's own radius is what kept catching its rim.
    stub = extrude(d, "Hose stub", plane_at_x(0.0), [circle((0.0, 36.0), 7.0)],
                   90.0)
    body = boolean(d, "Bell and stub", body, stub)
    frame(d, "inlet", (0.0, 0.0, 0.0))
    material(d, "brass")


# --- the bits a hand touches ----------------------------------------------

def lid_handle(d):
    """A handle bent from one tube: two legs, two bends and a span.

    Swept along its own centreline — a Curve3D through the bends, then a
    Pipe — which is what the part IS: a length of tube through a bender. Two
    cylinders unioned at a sharp corner is a picture of something nobody can
    make, and it shows: the corner has a seam and no radius.
    """
    part(d, "Lid handle")
    half = S.HANDLE_SPAN / 2.0
    rise = S.HANDLE_RISE
    bend_r = 45.0  # the former the tube is bent round
    # The straights carry intermediate points of their own: a spline fitted
    # through two ends wanders between them, and the tube trumpets where it
    # does.
    pts = [[-half, 0.0, -rise - 0.0]]
    for k in range(1, 4):
        pts.append([-half, 0.0, -rise + (rise - bend_r) * k / 3.0])
    for k in range(1, 8):  # up and over the near bend
        a = math.radians(90.0 * k / 8.0)
        pts.append([-half + bend_r * (1.0 - math.cos(a)), 0.0,
                    -bend_r * (1.0 - math.sin(a))])
    span = (half - bend_r) - (-half + bend_r)
    for k in range(0, 7):
        pts.append([-half + bend_r + span * k / 6.0, 0.0, 0.0])
    for k in range(1, 8):  # and down the far one
        a = math.radians(90.0 * k / 8.0)
        pts.append([half - bend_r * (1.0 - math.sin(a)), 0.0,
                    -bend_r * (1.0 - math.cos(a))])
    for k in range(0, 4):
        pts.append([half, 0.0, -bend_r - (rise - bend_r) * k / 3.0])
    path = d.call("add_feature", feature={
        "type": "Curve3D", "name": "Centreline", "kind": "fit_spline",
        "points": pts})
    check(path, "handle centreline")
    check(d.call("add_feature", feature={
        "type": "Pipe", "name": "Tube", "path_id": path["id"],
        "outer_diameter": S.HANDLE_D, "wall_thickness": 1.5}), "handle tube")
    # Mounted by the MIDDLE of its two feet: mating it by one foot hangs the
    # whole handle off to one side of wherever it was meant to sit.
    frame(d, "mount", (0.0, 0.0, -rise))
    frame(d, "left_foot", (-half, 0.0, -rise))
    frame(d, "right_foot", (half, 0.0, -rise))
    material(d, "stainless_steel")


def thermometer(d):
    """Bezel, glass and stem: one revolve."""
    part(d, "Thermometer")
    r = S.THERMO_BEZEL_D / 2.0
    revolve(d, "Thermometer", plane_at_y(0.0),
            [(0.0, -40.0), (S.THERMO_STEM_D / 2.0, -40.0),
             (S.THERMO_STEM_D / 2.0, 0.0), (r - 6.0, 0.0), (r, 4.0),
             (r, 16.0), (r - 4.0, 18.0), (0.0, 18.0)], axis="z")
    frame(d, "seat", (0.0, 0.0, 0.0))
    material(d, "stainless_steel")
