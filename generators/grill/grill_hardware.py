"""Everything on the grill that is not folded sheet: burners, gas, grates,
wheels, the tank, and the bits a hand touches.

Drawn, not stamped out of primitives. A propane bottle is a profile revolved
about its axis — a domed shoulder, a rolled foot, a collar you can get a hand
into — and a box-and-cylinder stand-in is a picture of something else. Even
the plain round bars are sketched and extruded, so every part opens the same
way and any dimension on it can be edited.

Each part carries the frames it is mated by, and the holes its JOINT needs: a
valve's screws are tapped into the valve, the burner's tail takes the screw
that comes through the firebox's back wall, and the handle's ends have plugs
to screw into. Every one of those is drawn along the part's OWN axis, so the
socket that places it is the only thing that knows which way it faces.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from grill_lib import check, drill, frame, part, sketch, tapped  # noqa: E402
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


def thread_all(d, radius, size, expect, name="Thread"):
    """Thread EVERY cylinder of `radius` — and refuse if there are not the
    `expect`ed number of them.

    A hole whose face the element map never named is invisible here, and an
    unthreaded hole with a screw in it is a clash rather than a joint, so the
    count is the check.
    """
    body = d.call("list_elements")["bodies"][0]
    faces = [f for f in body["faces"]
             if f["kind"] == "cylinder" and abs(f["radius"] - radius) < 1e-4]
    if len(faces) != expect:
        raise SystemExit(f"{name}: {expect} tappings expected, "
                         f"{len(faces)} threadable faces of radius {radius}")
    for i, f in enumerate(faces):
        r = d.call("add_feature", feature={
            "type": "Thread", "name": f"{name} {i + 1}",
            "face": {"feature_id": body["created_by"], "element": f["name"]},
            "designation": size, "pitch": S.PITCH[size],
            "allow_engagement": True})
        check(r, name)


def thread_on(d, radius, size, name="Thread"):
    """A cosmetic thread that ALLOWS engagement, on the cylinder of `radius`.

    Without it a nut on a stem is a clash, because nothing downstream can
    know the two are meant to screw together: the checker relieves the
    engaged length only between threads it was told about.
    """
    body = d.call("list_elements")["bodies"][0]
    faces = [f for f in body["faces"]
             if f["kind"] == "cylinder" and abs(f["radius"] - radius) < 1e-4]
    if not faces:
        raise SystemExit(f"{name}: no cylinder of radius {radius} to thread")
    r = d.call("add_feature", feature={
        "type": "Thread", "name": name,
        "face": {"feature_id": body["created_by"], "element": faces[0]["name"]},
        "designation": size, "pitch": S.PITCH[size], "allow_engagement": True})
    check(r, name)


# --- the gas train ---------------------------------------------------------

def burner(d):
    """A burner tube and the venturi that slides over the valve's orifice.

    Drawn along its OWN axis — local +Z, the mouth at the origin — and
    turned into the box by its socket, with a tapped hole in its tail for
    the screw that comes through the firebox's back wall.
    """
    part(d, "Burner")
    r_tube = S.BURNER_D / 2.0
    r_vent = S.VENTURI_D / 2.0
    revolve(d, "Tube", plane_at_y(0.0),
            [(0.0, 0.0), (r_vent, 0.0), (r_vent, S.VENTURI_L),
             (r_tube, S.VENTURI_L), (r_tube, S.BURNER_LEN),
             (0.0, S.BURNER_LEN)], axis="z")
    tapped(d, "z", S.BURNER_LEN + 5.0, -1, [(0.0, 0.0, 0.0)], S.BURNER_BOLT,
           5.0 + S.BURNER_TAP_DEPTH, name="Tail tapping")
    frame(d, "origin", (0.0, 0.0, 0.0))
    material(d, "stainless_steel")


def manifold(d):
    """The tube the valves screw into — drawn along its own axis, with a
    port for each valve's boss."""
    part(d, "Manifold")
    half = S.MANIFOLD_LEN / 2.0
    r_out = S.MANIFOLD_D / 2.0
    r_in = r_out - S.MANIFOLD_WALL
    revolve(d, "Manifold tube", plane_at_y(0.0),
            [(r_in, -half), (r_out, -half), (r_out, half), (r_in, half)],
            axis="z")
    drill(d, "y", -r_out - 5.0, +1,
          [(0.0, 0.0, S.burner_x(i)) for i in range(S.BURNERS)],
          S.VALVE_BOSS_D, 5.0 + S.MANIFOLD_WALL + 1.0, name="Valve ports")
    frame(d, "origin", (0.0, 0.0, 0.0))
    material(d, "brass")


def valve(d):
    """One burner valve: a body, a stem through the panel, the orifice that
    feeds the venturi, and the boss that goes up into the manifold.

    Local +Z runs into the cart and local +Y is up, so ONE socket on the
    control panel places body, stem, orifice and boss together.
    """
    part(d, "Valve")
    w, h, dp = S.VALVE_BODY
    body = extrude(d, "Body", plane_at_z(0.0),
                   [rect((-w / 2.0, -h / 2.0), (w / 2.0, h / 2.0))], dp)
    stem = extrude(d, "Stem", plane_at_z(0.0),
                   [circle((0.0, 0.0), S.VALVE_STEM_D / 2.0)],
                   -S.VALVE_STEM_L)
    body = boolean(d, "Body and stem", body, stem)
    orifice = extrude(d, "Orifice", plane_at_z(dp),
                      [circle((0.0, 0.0), S.ORIFICE_D / 2.0)], S.ORIFICE_L)
    body = boolean(d, "With orifice", body, orifice)
    # Up into the manifold, line to line with the port drilled for it.
    boss_len = S.MANIFOLD_Z - S.BURNER_Z - h / 2.0
    boss = extrude(d, "Manifold boss",
                   {"origin": [0, h / 2.0, 0], "normal": [0, 1, 0],
                    "uAxis": [1, 0, 0]},
                   [circle((0.0, -dp / 2.0), S.VALVE_BOSS_D / 2.0)], boss_len)
    boolean(d, "With boss", body, boss)
    tapped(d, "z", -5.0, +1,
           [(sx * S.VALVE_SCREW_X, 0.0, 0.0) for sx in S.sides()],
           S.VALVE_BOLT, 5.0 + 10.0, name="Screw tappings")
    frame(d, "origin", (0.0, 0.0, 0.0))
    frame(d, "knob", (0.0, 0.0, -(S.T_PANEL + S.KNOB_OFF)),
          z_axis=(0, 0, -1), x_axis=(1, 0, 0))
    material(d, "brass")


def knob(d):
    """A control knob: a skirt, a waist and a grip — revolved, because that
    is what a turned knob is — bored for the stem it grips."""
    part(d, "Knob")
    r = S.KNOB_D / 2.0
    revolve(d, "Knob body", plane_at_y(0.0),
            [(0.0, 0.0), (r - 8.0, 0.0), (r, 4.0), (r, 16.0),
             (r - 6.0, S.KNOB_H), (0.0, S.KNOB_H)], axis="z")
    # The pointer a thumb finds.
    extrude(d, "Pointer", plane_at_z(2.0),
            [rect((-3.0, r - 12.0), (3.0, r + 4.0))], S.KNOB_H - 4.0,
            mode="add")
    drill(d, "z", -5.0, +1, [(0.0, 0.0, 0.0)], S.VALVE_STEM_D,
          5.0 + S.KNOB_BORE_DEPTH, name="Stem bore")
    frame(d, "origin", (0.0, 0.0, 0.0))
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
    frame(d, "origin", (0.0, 0.0, 0.0))
    material(d, "steel")  # porcelain-coated cast iron in life


def warming_rack(d):
    """The half-shelf up under the lid, resting on two welded ledges."""
    part(d, "Warming rack")
    r = S.RACK_BAR_D / 2.0
    half = S.RACK_LEN / 2.0
    front = extrude(d, "Front rail", plane_at_x(-half),
                    [circle((-S.RACK_Y / 2.0, r), r)], S.RACK_LEN)
    back = extrude(d, "Back rail", plane_at_x(-half),
                   [circle((S.RACK_Y / 2.0, r), r)], S.RACK_LEN)
    front = boolean(d, "Rails", front, back)
    bar = extrude(d, "Bar", plane_at_y(S.RACK_Y / 2.0),
                  [circle((-half + 20.0, r), r)], S.RACK_Y)
    count = int((S.RACK_LEN - 40.0) // 40.0) + 1
    check(d.call("add_feature", feature={
        "type": "Pattern", "name": "Bars", "kind": "linear",
        "source_id": bar, "count": count, "spacing": 40.0,
        "axis": {"kind": "x"}}), "rack bars")
    frame(d, "origin", (0.0, 0.0, 0.0))
    material(d, "stainless_steel")


# --- the cart's running gear ----------------------------------------------

def wheel(d):
    """A wheel: tread, dished web and bore, revolved about the axis it turns
    on — so its own +Z IS that axis and the socket only has to aim it."""
    part(d, "Wheel")
    r_out = S.WHEEL_D / 2.0
    r_bore = S.WHEEL_BORE / 2.0
    w = S.WHEEL_W
    revolve(d, "Wheel", plane_at_y(0.0),
            [(r_bore, 0.0), (r_out - 18.0, 0.0), (r_out - 18.0, 6.0),
             (r_out, 6.0), (r_out, w - 6.0), (r_out - 18.0, w - 6.0),
             (r_out - 18.0, w), (r_bore, w)], axis="z")
    frame(d, "origin", (0.0, 0.0, 0.0))
    material(d, "plastic_black")


def axle_bolt(d):
    """The bolt a wheel turns on: a head, a plain shoulder through wheel and
    panel, and a thread for the nut inside.

    A shoulder bolt, not a screw pulled up tight through a wheel that has to
    turn: the shoulder is exactly wheel plus panel long, so the nut clamps
    its END against the panel and leaves the wheel free on it.
    """
    part(d, "Axle bolt")
    r_head = 11.0
    r_sh = S.AXLE_BOLT_D / 2.0
    r_th = 5.0
    grip = S.WHEEL_W + S.T_CART
    revolve(d, "Bolt", plane_at_y(0.0),
            [(0.0, -5.0), (r_head, -5.0), (r_head, 0.0), (r_sh, 0.0),
             (r_sh, grip), (r_th, grip), (r_th, grip + 12.0),
             (0.0, grip + 12.0)], axis="z")
    thread_on(d, r_th, S.AXLE_THREAD, name="M10 thread")
    frame(d, "origin", (0.0, 0.0, 0.0))
    frame(d, "nut", (0.0, 0.0, grip))
    material(d, "steel")


def foot(d):
    """A moulded foot with a threaded insert: a taper, revolved, tapped."""
    part(d, "Foot")
    r = S.FOOT_D / 2.0
    # About Z the profile is (radius, height): u is the distance from the
    # axis and v is how far up it. About X or Y it is the other way round,
    # which is the one thing to get right in every one of these.
    revolve(d, "Foot", plane_at_y(0.0),
            [(0.0, 0.0), (r, 0.0), (r, -(S.FOOT_H - 6.0)),
             (r - 5.0, -S.FOOT_H), (0.0, -S.FOOT_H)], axis="z")
    tapped(d, "z", 5.0, -1, [(0.0, 0.0, 0.0)], S.FOOT_BOLT, 5.0 + 15.0,
           name="Insert")
    frame(d, "origin", (0.0, 0.0, 0.0))
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
    boolean(d, "With valve", body, valve_body)
    frame(d, "origin", (0.0, 0.0, 0.0))
    frame(d, "outlet", (0.0, 0.0, top + valve_h))
    material(d, "steel")


def regulator(d):
    """The regulator, and the hose stub that runs off to the manifold."""
    part(d, "Regulator")
    body = revolve(d, "Bell", plane_at_y(0.0),
                   [(0.0, 0.0), (22.0, 0.0), (22.0, 6.0), (35.0, 10.0),
                    (35.0, 40.0), (26.0, 46.0), (0.0, 46.0)], axis="z")
    # High on the bell: the hose has to clear the bottle's collar, and the
    # stub's own radius is what kept catching its rim.
    stub = extrude(d, "Hose stub", plane_at_x(0.0), [circle((0.0, 36.0), 7.0)],
                   90.0)
    boolean(d, "Bell and stub", body, stub)
    frame(d, "origin", (0.0, 0.0, 0.0))
    material(d, "brass")


# --- the bits a hand touches ----------------------------------------------

def lid_handle(d):
    """A handle bent from one tube: two legs, two bends and a span.

    Its centreline is a BENT PATH — the corners and the former's radius —
    swept by a Pipe, which is what the part IS: a length of tube through a
    bender. Two cylinders unioned at a sharp corner is a picture of something
    nobody can make, and a spline fitted through sampled arc points is a
    tube whose bend radius nothing can read back.

    Each end takes a welded plug, tapped, so the screw that comes up through
    the lid has something to pull on. A tube with nothing in its ends is
    held on by its paint.
    """
    part(d, "Lid handle")
    half = S.HANDLE_SPAN / 2.0
    rise = S.HANDLE_RISE
    # The centreline as a BENDER is told it: the corners, and the former it
    # is bent round. Four points and a radius — where a fitted spline needed
    # twenty-nine generated points (extra ones along the straights, or the
    # tube trumpets between them, and eight per corner to approximate this
    # radius), and still had no bend radius anything could read back.
    path = d.call("add_feature", feature={
        "type": "Curve3D", "name": "Centreline", "kind": "bent_path",
        "radius": S.HANDLE_BEND_R,
        "points": [[-half, 0.0, 0.0], [-half, 0.0, rise],
                   [half, 0.0, rise], [half, 0.0, 0.0]]})
    check(path, "handle centreline")
    tube = d.call("add_feature", feature={
        "type": "Pipe", "name": "Tube", "path_id": path["id"],
        "outer_diameter": S.HANDLE_D, "wall_thickness": S.HANDLE_WALL})
    check(tube, "handle tube")
    body = owner(d, tube, "Tube")
    # A plug with a BLIND tapping, capped at the top. Bored right through it
    # is the same part to a shop and the joint reads correctly either way
    # (a thread ends a joint the way a nut does) — but on THIS body, and not
    # on a straight swept tube or an extruded one, an exact boolean between
    # the open bore and the screw in it comes back as the whole screw rather
    # than the annulus around it, 305 mm3 against 93. See the findings.
    for i, sx in enumerate(S.sides()):
        bore = S.HANDLE_PLUG - 2.0
        plug = extrude(d, f"Plug {i + 1}", plane_at_z(0.0),
                       [circle((sx * half, 0.0),
                               S.HANDLE_D / 2.0 - S.HANDLE_WALL),
                        circle((sx * half, 0.0), S.TAP[S.HANDLE_BOLT] / 2.0)],
                       bore)
        body = boolean(d, f"With plug {i + 1}", body, plug)
        cap = extrude(d, f"Plug cap {i + 1}", plane_at_z(bore),
                      [circle((sx * half, 0.0),
                              S.HANDLE_D / 2.0 - S.HANDLE_WALL)], 2.0)
        body = boolean(d, f"With plug cap {i + 1}", body, cap)
    thread_all(d, S.TAP[S.HANDLE_BOLT] / 2.0, S.HANDLE_BOLT, expect=2,
               name="Plug tapping")
    frame(d, "origin", (0.0, 0.0, 0.0))
    material(d, "stainless_steel")


def thermometer(d):
    """Bezel, glass and stem: one revolve, and the thread the nut that holds
    it in the lid runs on."""
    part(d, "Thermometer")
    r = S.THERMO_BEZEL_D / 2.0
    stem = S.THERMO_STEM_D / 2.0
    revolve(d, "Thermometer", plane_at_y(0.0),
            [(0.0, -40.0), (stem, -40.0), (stem, 0.0), (r - 6.0, 0.0),
             (r, 4.0), (r, 16.0), (r - 4.0, 18.0), (0.0, 18.0)], axis="z")
    thread_on(d, stem, "M10", name="Stem thread")
    frame(d, "origin", (0.0, 0.0, 0.0))
    material(d, "stainless_steel")


# --- the pins the grill turns on ------------------------------------------

def hinge_pin(d, name, diameter, length, head=2.5):
    """A clevis pin: a head, a plain shank line-to-line in its bores, and the
    cross hole for the split pin that keeps it there."""
    part(d, name)
    r = diameter / 2.0
    revolve(d, "Pin", plane_at_y(0.0),
            [(0.0, -head), (r + 2.5, -head), (r + 2.5, 0.0), (r, 0.0),
             (r, length), (0.0, length)], axis="z")
    drill(d, "x", -diameter, +1, [(0.0, 0.0, length - 3.0)],
          max(1.6, diameter / 3.0), 2 * diameter, name="Split-pin hole")
    frame(d, "origin", (0.0, 0.0, 0.0))
    material(d, "stainless_steel")


def lid_pin(d):
    return hinge_pin(d, "Lid hinge pin", S.PIN_D, 2 * S.T_BRK + 6.0)


def door_pin(d):
    return hinge_pin(d, "Door hinge pin", S.HINGE_PIN_D, 44.0)


def hinge_leaf(d):
    """One leaf of a corner hinge: a plate tangent to its own knuckle.

    BOTH leaves of a hinge are this part — the second is the first turned
    over — which is what makes their knuckles interleave on one pin instead
    of wanting the same 20 mm of it.
    """
    part(d, "Hinge leaf")
    rk = S.HINGE_KNUCKLE_D / 2.0
    length, tall = S.HINGE_LEAF
    body = extrude(d, "Knuckle", plane_at_z(0.0), [circle((0.0, 0.0), rk)],
                   tall)
    extrude(d, "Leaf", plane_at_z(0.0),
            [rect((0.0, rk - S.T_BRK), (length, rk))], tall, mode="add")
    drill(d, "z", -5.0, +1, [(0.0, 0.0, 0.0)], S.HINGE_PIN_D,
          5.0 + tall + 5.0, name="Pin bore")
    drill(d, "y", 0.0, +1, [(x, 0.0, tall / 2.0) for x in S.HINGE_BOLT_X],
          S.CLEAR[S.HINGE_BOLT], rk + 1.0, name="Bolt holes")
    frame(d, "origin", (0.0, 0.0, 0.0))
    material(d, "steel")
    return body
