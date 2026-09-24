"""Front accessories. Photo-derived envelopes; see generators/om615/README.md.

Coordinates follow the engine (+Y rearward). Revolving hardware is built about
local Z and located by named mounting/shaft datums, not by bounding boxes.
"""

import math
from designer_sdk import Part, Plane, V, SdkError
import om615_bottom as B
import om615_top as T
from om615_spec import BELT_Y, PULLEY_D, WP_PULLEY_D, WATER_PUMP_Z
from om615_spec import (
    BLOCK_Y1, BORE, BORE_PITCH, COOLANT_HOSE_OD, COOLANT_HOSE_WALL,
    DIPSTICK_AXIS, DIPSTICK_BORE_D, DIPSTICK_BOSS, FUEL_FILTER_BOLT_D,
    FUEL_FILTER_BOLT_DZ, FUEL_FILTER_BOSS, FUEL_FILTER_BOSS_HALF, HEAD_TOP,
    HEATER_PIPE, INT_LOG, MANIFOLD_PAD_HZ, OIL_FILTER_BOLT_D,
    OIL_FILTER_BOLT_PCD, OIL_FILTER_BOSS, OIL_FILTER_BOSS_D,
    OIL_FILTER_FLANGE_T, OIL_FILTER_PORT_D, PLENUM, PORT_Z, PUMP_MOUNT,
    PUMP_MOUNT_D, TSTAT_X, WP_INLET, WP_INLET_Y)

ALT_X, ALT_Z, ALT_D = 176.0, 75.0, 76.0
ALT_LUG = 80.0
BRACKET_Z = ALT_Z - ALT_LUG
UPPER_BRACKET_Z = ALT_Z + ALT_LUG
MOUNT_BOLT_Y = -168.0
MOUNT_BOLT_OFFSETS = (-20.0, 20.0)
PITCH_OFFSET = 4.0
FAN_BACK_OFFSET = 14.0
FAN_Y, FAN_R = BELT_Y - 65.0, 175.0
PULLEYS = (
    (0.0, 0.0, PULLEY_D / 2 - PITCH_OFFSET),
    (ALT_X, ALT_Z, ALT_D / 2 - PITCH_OFFSET),
    (0.0, WATER_PUMP_Z, WP_PULLEY_D / 2 - PITCH_OFFSET),
)


def verify(part):
    part.check()
    part.activate()
    report = part.app.check_geometry()
    if not report["ok"] or report["body_count"] != 1:
        raise SdkError(f"{part.name}: invalid accessory geometry: {report}")
    return part


def belt_geometry():
    """CCW convex external tangents and wrap angles in engine XZ space."""
    spans = []
    for i, (x, z, r) in enumerate(PULLEYS):
        xx, zz, rr = PULLEYS[(i + 1) % 3]
        dx, dz = xx - x, zz - z
        d = math.hypot(dx, dz)
        a = (r - rr) / d
        b = math.sqrt(1 - a * a)
        nx, nz = a * dx / d + b * dz / d, a * dz / d - b * dx / d
        spans.append(
            dict(
                start=(x + r * nx, z + r * nz),
                end=(xx + rr * nx, zz + rr * nz),
                normal=(nx, nz),
                length=math.sqrt(d * d - (r - rr) ** 2),
            )
        )
    wraps = []
    for i, (x, z, r) in enumerate(PULLEYS):
        n0 = spans[(i - 1) % 3]["normal"]
        n1 = spans[i]["normal"]
        start = math.atan2(n0[1], n0[0])
        end = math.atan2(n1[1], n1[0])
        angle = (end - start) % (2 * math.pi)
        wraps.append(dict(center=(x, z), radius=r, start=start, angle=angle))
    length = sum(s["length"] for s in spans) + sum(
        w["radius"] * w["angle"] for w in wraps
    )
    return spans, wraps, length


def belt(app):
    p = app.part("fan belt")
    spans, wraps, length = belt_geometry()
    p.var("pitch_length", length, "Derived from the three pitch circles; mm")
    p.var("top_width", 9.5, "Workshop 13-335 lists 9.5 mm V-belts; envelope selection")
    profile = [(-3.0, -2.0), (3.0, -4.75), (3.0, 4.75), (-3.0, 2.0)]
    for i, w in enumerate(wraps):
        x, z = w["center"]
        a = w["start"]
        nx, nz = math.cos(a), math.sin(a)
        axis = p._add(
            dict(
                type="DatumAxis",
                name=f"Pulley {i + 1} axis",
                kind="two_points",
                points=[[x, BELT_Y, z], [x, BELT_Y - 1, z]],
            )
        )
        plane = Plane((x, BELT_Y, z), (-nz, 0, nx), (nx, 0, nz))
        p.revolve_profile(
            [(w["radius"] + r, y) for r, y in profile],
            plane=plane,
            axis=dict(kind="datum_axis", datum_axis_id=axis["id"]),
            angle_deg=math.degrees(w["angle"]),
            mode="new_body" if i == 0 else "add",
            name=f"Wrap {i + 1}",
        )
    for i, s in enumerate(spans):
        x, z = s["start"]
        xx, zz = s["end"]
        nx, nz = s["normal"]
        # Small tangent overlap avoids a seam-only OCC union between wraps.
        ux, uz = (xx - x) / s["length"], (zz - z) / s["length"]
        plane = Plane((x - 0.05 * ux, BELT_Y, z - 0.05 * uz), (ux, 0, uz), (nx, 0, nz))
        sk = p.sketch(plane, name=f"Span {i + 1}")
        sk.polyline(profile)
        p.extrude(sk.add(), s["length"] + 0.1, mode="add", name=f"Span {i + 1}")
    expected = 40.5 * length + 16.5 * math.tau
    if abs(p.volume() - expected) > expected * 0.001:
        raise SdkError("V-belt volume disagrees with its analytic section/path")
    p.material("rubber")
    p.frame("crank_end", origin=(0, BELT_Y, 0), z=(0, 1, 0), x=(1, 0, 0))
    verify(p)
    return p


def annulus(p, name, ro, ri, z, length, mode="new_body", center=(0, 0)):
    s = p.sketch(Plane.xy(z), name=name)
    s.circle(center, r=ro)
    if ri:
        s.circle(center, r=ri)
    return p.extrude(s.add(), length, mode=mode, name=name)


def fan(app):
    p = app.part("cooling fan")
    annulus(p, "Pressed hub", 26, 9.4, -3, 6)
    # Four pitched, tapered steel blades as in the early-engine reference.
    for k in range(4):
        a = k * math.pi / 2
        c, s = math.cos(a), math.sin(a)
        plane = Plane(
            (0, 0, 0),
            (-s * math.sin(0.22), c * math.sin(0.22), math.cos(0.22)),
            (c, s, 0),
        )
        sk = p.sketch(plane, name=f"Blade {k + 1}")
        B.rounded_polygon(
            sk,
            [(20, -9), (FAN_R - 18, -27), (FAN_R, -13), (FAN_R - 3, 28), (45, 17)],
            [2, 10, 8, 8, 5],
        )
        p.extrude(sk.add(), 1.8, mode="add", name=f"Blade {k + 1}")
    holes = p.sketch(Plane.xy(-5), name="Fan bolts")
    for k in range(4):
        a = k * math.pi / 2 + math.pi / 4
        holes.circle((18 * math.cos(a), 18 * math.sin(a)), d=6.5)
    p.extrude(holes.add(), 10, mode="cut", name="Fan bolts")
    for k in range(4):
        a = k * math.pi / 2 + math.pi / 4
        p.frame(
            f"screw_{k}",
            origin=(18 * math.cos(a), 18 * math.sin(a), 3),
            z=(0, 0, -1),
            x=(1, 0, 0),
        )
    p.material("steel")
    p.frame("hub", origin=(0, 0, 0))
    verify(p)
    return p


def fan_spacer(app):
    p = app.part("fan hub spacer")
    annulus(p, "Shaft extension", 12, 9.2, -6, 54)
    annulus(p, "Pulley flange", 25, 9.2, -6, 4, mode="add")
    annulus(p, "Fan flange", 25, 9.2, 43, 5, mode="add")
    sk = p.sketch(Plane.xy(-7), name="Flange bolt holes")
    for k in range(4):
        a = k * math.pi / 2 + math.pi / 4
        sk.circle((18 * math.cos(a), 18 * math.sin(a)), d=6.5)
        p.frame(
            f"screw_{k}",
            origin=(18 * math.cos(a), 18 * math.sin(a), -2),
            z=(0, 0, -1),
            x=(1, 0, 0),
        )
    p.extrude(sk.add(), 56, mode="cut", name="Flange bolt holes")
    p.material("steel")
    p.frame("rear", origin=(0, 0, 0))
    p.frame("fan", origin=(0, 0, 51))
    verify(p)
    return p


def alternator(app):
    """Separate rigid stator and rotor subassemblies expose the turning DOF.

    Native nested assemblies are rigid. Keeping that degree of freedom in the
    parent mechanism avoids a visually animated but physically frozen rotor.
    """
    front = app.part("alternator front casting")
    front.revolve_profile(
        [
            (10, 0),
            (36, 0),
            (55, 12),
            (60, 23),
            (60, 35),
            (52, 35),
            (49, 18),
            (32, 7),
            (10, 7),
        ],
        name="Front casting section",
    )
    for i in range(6):
        a = i * math.pi / 3
        sk = front.sketch(Plane.xy(8), name=f"Vent {i + 1}")
        sk.circle((43 * math.cos(a), 43 * math.sin(a)), d=19)
        front.extrude(sk.add(), 30, mode="cut", name=f"Vent {i + 1}")
    for i in (0, 1):
        sk = front.sketch(Plane.xy(18), name=f"Cast lug web {i + 1}")
        sign = -1 if i == 0 else 1
        sk.rect(
            (-10, min(sign * 54, sign * ALT_LUG)), (10, max(sign * 54, sign * ALT_LUG))
        )
        front.extrude(sk.add(), 17, mode="add", name=f"Cast lug web {i + 1}")
        sk = front.sketch(Plane.xy(18), name=f"Mount lug {i + 1}")
        sk.circle((0, (-1 if i == 0 else 1) * ALT_LUG), r=14)
        front.extrude(sk.add(), 17, mode="add", name=f"Mount lug {i + 1}")
        sk = front.sketch(Plane.xy(17), name=f"Pivot bore {i + 1}")
        sk.circle((0, (-1 if i == 0 else 1) * ALT_LUG), d=10.5)
        front.extrude(sk.add(), 19, mode="cut", name=f"Pivot bore {i + 1}")
    front.material("aluminum")
    front.frame("shaft", origin=(0, 0, 0))
    front.frame("mount", origin=(0, 0, 0))
    verify(front)
    stator = app.part("alternator stator")
    annulus(stator, "Laminated stator", 54, 37, 36, 51)
    slots = stator.sketch(Plane.xy(35), name="Winding slots")
    for k in range(12):
        a = k * math.tau / 12
        c, t = math.cos(a), math.sin(a)
        for lo, hi in ((-7.5, -3.5), (3.5, 7.5)):
            slots.polyline(
                [
                    (r * c - y * t, r * t + y * c)
                    for r, y in ((39.5, lo), (44.5, lo), (44.5, hi), (39.5, hi))
                ]
            )
    stator.extrude(slots.add(), 53, mode="cut", name="Winding slots")
    stator.material("steel")
    verify(stator)
    winding = app.part("alternator winding pack")
    sk = winding.sketch(Plane.yz(40), name="Copper winding bundle")
    B.rounded_polygon(sk, [(-7, 28), (7, 28), (7, 97), (-7, 97)], 7)
    B.rounded_polygon(sk, [(-4, 31), (4, 31), (4, 94), (-4, 94)], 4)
    winding.extrude(sk.add(), 4, name="Copper winding bundle")
    winding.material("copper")
    verify(winding)
    rear = app.part("alternator rear casting")
    rear.revolve_profile(
        [
            (10, 106),
            (42, 106),
            (50, 100),
            (53, 88),
            (58, 88),
            (58, 104),
            (42, 117),
            (10, 117),
        ],
        name="Rear casting section",
    )
    rear.material("aluminum")
    verify(rear)
    rotor = app.part("alternator rotor and shaft")
    annulus(rotor, "Rotor shaft", 9, 0, -21, 138)
    annulus(rotor, "Rotor poles", 34, 0, 40, 43, mode="add")
    rotor.material("steel")
    rotor.frame("shaft", origin=(0, 0, 0))
    verify(rotor)
    pulley = B.vee_pulley(app, "alternator pulley", ALT_D, 18.4)
    cooling = app.part("alternator cooling fan")
    annulus(cooling, "Pressed disc", 43, 9.3, -2.5, 1.5)
    for i in range(10):
        a = i * math.tau / 10
        c, s = math.cos(a), math.sin(a)
        sk = cooling.sketch(Plane.xy(-7), name=f"Cooling vane {i + 1}")
        sk.polyline(
            [
                (22 * c - 1 * s, 22 * s + 1 * c),
                (43 * c - 1 * s, 43 * s + 1 * c),
                (43 * c + 1 * s, 43 * s - 1 * c),
                (22 * c + 1 * s, 22 * s - 1 * c),
            ]
        )
        cooling.extrude(sk.add(), 6, mode="add", name=f"Cooling vane {i + 1}")
    cooling.material("steel")
    verify(cooling)
    fixed = app.assembly("alternator stator assembly")
    for p in (front, stator, rear):
        fixed.place(p, fixed=True, name=p.name)
    for k in range(12):
        o = fixed.place(winding, fixed=True, name=f"stator winding {k + 1}")
        a = k * math.tau / 12
        app.set_occurrence_transform(
            id=o.id,
            transform=dict(orientation=[0, 0, math.sin(a / 2), math.cos(a / 2)]),
        )
    rotating = app.assembly("alternator rotor assembly")
    rotating.place(rotor, fixed=True, name=rotor.name)
    rotating.place(pulley, at=(0, 0, -24), fixed=True, name=pulley.name)
    rotating.place(cooling, fixed=True, name=cooling.name)
    return fixed, rotating, front


def install(app, rig, block, wp_shaft, wp_mate, crank_mate):
    stator, rotor, front = alternator(app)
    # Local Z points rearwards, so the pulley is ahead of the cast front.
    mount = (ALT_X, BELT_Y + 16, ALT_Z)
    bracket = alternator_bracket(app)
    bracket_occ = rig.place(bracket, name="alternator mounting bracket")
    rig.fasten((block, "alternator_bracket"), (bracket_occ, "block"), flipped=False)
    upper = alternator_upper_bracket(app)
    upper_occ = rig.place(upper, name="alternator adjustment bracket")
    rig.fasten((block, "alternator_adjustment"), (upper_occ, "block"), flipped=False)
    for part, o in ((bracket, bracket_occ), (upper, upper_occ)):
        for i in range(2):
            mount_fastener(app, rig, o, f"foot_washer_{i}", "iso7089", "M8")
            mount_fastener(app, rig, o, f"foot_bolt_{i}", "iso4762", "M8", 25)
        for frame, std in (
            ("pivot_bolt", "iso4762"),
            ("pivot_front_washer", "iso7089"),
            ("pivot_back_washer", "iso7089"),
            ("pivot_nut", "iso4032"),
        ):
            mount_fastener(
                app, rig, o, frame, std, "M10", 40 if std == "iso4762" else 0
            )
    fixed = rig.place(stator, at=mount, name="alternator stator")
    moving = rig.place(rotor, at=mount, name="alternator rotor")
    rig.activate()
    for o in (fixed, moving):
        app.set_occurrence_transform(
            id=o.id, transform=dict(orientation=[-math.sqrt(0.5), 0, 0, math.sqrt(0.5)])
        )
    rig.fasten((bracket_occ, "alternator"), fixed, flipped=False)
    # Origin connectors on subassemblies express the actual shaft axis.
    drive = rig.revolute(fixed, moving, flipped=False)
    app.add_relation(
        type="chain",
        a=crank_mate["id"],
        b=drive["id"],
        ratio=PULLEYS[0][2] / PULLEYS[1][2],
        name="alternator V-belt drive",
    )
    f = fan(app)
    sp = fan_spacer(app)
    # The shaft's existing pulley frame is in world +Y; extension goes forward.
    so = rig.place(sp)
    fo = rig.place(f)
    rig.fasten((wp_shaft, "fan_drive"), (so, "rear"), flipped=False)
    rig.fasten((so, "fan"), (fo, "hub"), flipped=False)
    for o in (so, fo):
        for k in range(4):
            screw = app.add_fastener(standard="iso4762", size="M6", length=10)
            rig.fasten(
                (o, f"screw_{k}"),
                dict(occurrence=screw["occurrence"], origin=True),
                flipped=False,
            )
    return dict(alternator_mate=drive, fan=fo, rotor=moving)


# Diaphragm pump envelope and linkage dimensions are photo estimates, not OEM
# manufacturing dimensions. Two face-cam lobes per half-speed timer revolution.
VAC_X, VAC_Z, VAC_DEPTH = -118.0, 160.0, 70.0
ROCKER_ANGLE, LEVER_ARM = 0.12, 20.0
CAM_CENTER = (16.0, -16.0)


def cam_height(x, y):
    """Inverse roller envelope, sampled at half-degree timer increments.

    The roller axle runs radially across the offset face cam. Account for
    its cylindrical underside and finite width at every prescribed pose.
    A 0.5 mm allowance covers faceting and the sampled contact envelope.
    """
    heights = []
    for i in range(720):
        a = math.tau * i / 720
        theta = ROCKER_ANGLE * (1 - math.cos(2 * a)) / 2
        px = CAM_CENTER[0] + x * math.cos(a) - y * math.sin(a)
        py = CAM_CENTER[1] + x * math.sin(a) + y * math.cos(a)
        cy, cz = 30 - 46 * math.cos(theta), 24 - 46 * math.sin(theta)
        dy = py - cy
        if abs(px) <= 8.5 and abs(dy) <= 7:
            heights.append(cz - math.sqrt(49 - dy * dy) - 8)
    if not heights:
        raise ValueError("Cam vertex outside the roller contact annulus")
    return min(heights) - 0.5


def pump_housing(app):
    p = app.part("vacuum pump casting")
    annulus(p, "Engine mounting flange", 59, 49, 0, 5)
    sections = []
    for z, r, y in ((4, 53, 0), (18, 52, 2), (36, 47, 7), (49, 43, 10)):
        sk = p.sketch(Plane.xy(z), name="Casting station")
        sk.circle((0, y), r=r)
        sections.append(sk.add().id)
    p._add(
        dict(
            type="Loft", name="Rounded offset casting", sketch_ids=sections, mode="add"
        )
    )
    sections = []
    for z, r, y in ((-1, 49, 0), (18, 48, 2), (36, 43, 7), (50, 39, 10)):
        sk = p.sketch(Plane.xy(z), name="Core station")
        sk.circle((0, y), r=r)
        sections.append(sk.add().id)
    p._add(dict(type="Loft", name="Hollow pump core", sketch_ids=sections, mode="cut"))
    annulus(p, "Diaphragm clamping flange", 49, 39, 48, 5, mode="add", center=(0, 10))
    for z, r, n, cy in ((-1, 54, 6, 0), (46, 44, 8, 10)):
        sk = p.sketch(Plane.xy(z), name="Flange bolt bores")
        for i in range(n):
            a = math.tau * i / n
            sk.circle((r * math.cos(a), cy + r * math.sin(a)), d=5.5)
        p.extrude(sk.add(), 7, mode="cut", name="Flange bolt bores")
    for i in range(6):
        a = math.tau * i / 6
        sk = p.sketch(Plane.xy(5), name="Base screw spotface")
        sk.circle((54 * math.cos(a), 54 * math.sin(a)), r=4.6)
        p.extrude(sk.add(), 10, mode="cut", name="Base screw spotface")
        p.frame(
            f"base_screw_{i}",
            origin=(54 * math.cos(a), 54 * math.sin(a), 5),
            z=(0, 0, -1),
            x=(1, 0, 0),
        )
    # Cast bearing pedestals meet the inner wall, leaving the yoke free.
    for x in (-27, 27):
        sk = p.sketch(Plane.xy(15), name="Pivot pedestal")
        sk.circle((x, 30), r=10)
        p.extrude(sk.add(), 19, mode="add", name="Pivot pedestal")
    B.bore(p, (-40, 30, 24), (40, 30, 24), 8.4, "Rocker pivot bore")
    p.frame("mount", origin=(0, 0, 0))
    p.frame("cam", origin=(*CAM_CENTER, 8))
    p.frame("pivot", origin=(0, 30, 24), z=(1, 0, 0), x=(0, 1, 0))
    p.frame("diaphragm", origin=(0, 10, 50), z=(0, 0, -1), x=(1, 0, 0))
    p.material("aluminum")
    verify(p)
    return p


def vacuum_parts(app):
    housing = pump_housing(app)
    lid = app.part("vacuum pump valve cover")
    # Local cover coordinates remain shared with the casting for easy sectioning.
    annulus(lid, "Cover rim", 49, 38, 53.3, 3, center=(0, 10))
    sections = []
    for z, r in ((55, 44), (60, 42), (64, 34)):
        sk = lid.sketch(Plane.xy(z), name="Cover dome station")
        sk.circle((0, 10), r=r)
        sections.append(sk.add().id)
    lid._add(dict(type="Loft", name="Cover dome", sketch_ids=sections, mode="add"))
    annulus(lid, "Valve chamber", 24, 0, 64, 5, mode="add", center=(0, 10))
    sk = lid.sketch(Plane.xy(53), name="Air chamber")
    sk.circle((0, 10), r=36)
    lid.extrude(sk.add(), 7, mode="cut", name="Air chamber")
    for x in (-17, 17):
        annulus(lid, "Hose port", 6, 3.5, 65, 8, mode="add", center=(x, 10))
        sk = lid.sketch(Plane.xy(59), name="Valve passage")
        sk.circle((x, 10), r=3.5)
        lid.extrude(sk.add(), 15, mode="cut", name="Valve passage")
    sk = lid.sketch(Plane.xy(53), name="Cover screw holes")
    for i in range(8):
        a = math.tau * i / 8
        sk.circle((44 * math.cos(a), 10 + 44 * math.sin(a)), d=5.5)
    lid.extrude(sk.add(), 15, mode="cut", name="Cover screw holes")
    for i in range(8):
        a = math.tau * i / 8
        x, y = 44 * math.cos(a), 10 + 44 * math.sin(a)
        sk = lid.sketch(Plane.xy(56.3), name="Cover screw spotface")
        sk.circle((x, y), r=4.6)
        lid.extrude(sk.add(), 14, mode="cut", name="Cover screw spotface")
        lid.frame(f"screw_{i}", origin=(x, y, 56.3), z=(0, 0, -1), x=(1, 0, 0))
    lid.material("aluminum")
    verify(lid)
    diaphragm = app.part("vacuum pump diaphragm centre")
    annulus(diaphragm, "Diaphragm centre plate", 30, 0, 0, 1.5)
    annulus(diaphragm, "Actuating rod", 3, 0, 1.5, 26, mode="add")
    diaphragm.frame("slide", origin=(0, 0, 0))
    diaphragm.material("steel")
    verify(diaphragm)
    membrane = app.part("vacuum pump diaphragm rim")
    annulus(membrane, "Clamped diaphragm", 48.5, 31, 53, 0.3, center=(0, 10))
    sk = membrane.sketch(Plane.xy(52.9), name="Diaphragm bolt holes")
    for i in range(8):
        a = math.tau * i / 8
        sk.circle((44 * math.cos(a), 10 + 44 * math.sin(a)), d=5.5)
    membrane.extrude(sk.add(), 0.5, mode="cut", name="Diaphragm bolt holes")
    membrane.material("rubber")
    verify(membrane)
    rocker = app.part("vacuum pump rocker yoke")
    # Geometry in housing axes, the pivot frame is the rotation origin.
    for x in (-13, 13):
        sk = rocker.sketch(Plane.xy(21), name="Yoke arm")
        sk.polyline([(x - 3, 32), (x + 3, 32), (x + 3, -16), (x - 3, -16)])
        rocker.extrude(
            sk.add(), 6, mode="new_body" if x < 0 else "add", name="Yoke arm"
        )
    sk = rocker.sketch(Plane.xy(21), name="Actuator bridge")
    sk.rect((-16, 6), (16, 13))
    rocker.extrude(sk.add(), 6, mode="add", name="Actuator bridge")
    # Cross shaft along X and a fork-mounted roller, all driven by the pivot.
    sk = rocker.sketch(Plane.yz(-32), name="Pivot shaft")
    sk.circle((30, 24), r=4)
    rocker.extrude(sk.add(), 64, mode="add", name="Pivot shaft")
    sk = rocker.sketch(Plane.xy(20), name="Diaphragm clevis slot")
    sk.slot((0, 8), (0, 12), 7.0)
    rocker.extrude(sk.add(), 10, mode="cut", name="Diaphragm clevis slot")
    B.axis_solid(rocker, (-16, -16, 24), (16, -16, 24), 6, "Roller axle")
    rocker.frame("pivot", origin=(0, 30, 24), z=(1, 0, 0), x=(0, 1, 0))
    rocker.frame("roller", origin=(0, -16, 24), z=(1, 0, 0), x=(0, 1, 0))
    rocker.material("steel")
    verify(rocker)
    roller = B.ring(app, "vacuum pump follower roller", 14, 6.4, 17, material="steel")
    # Two-lobe axial track, derived from the moving roller's swept envelope.
    cam = app.part("vacuum pump face cam")
    annulus(cam, "Timer face plate", 24, 0, -3, 3)
    annulus(cam, "Timer shaft", 6, 0, -58, 56, mode="add")
    # Ruled triangular sectors avoid OCC's unorientable periodic pipe seam.
    # Adjacent triangles share exact vertices; finite base overlap joins them.
    n = 32

    def vertex(radius, i):
        a = math.tau * (i % n) / n
        x, y = radius * math.cos(a), radius * math.sin(a)
        return V(x, y, cam_height(x, y))

    for i in range(n):
        ia, oa, ib, ob = (
            vertex(12, i),
            vertex(20, i),
            vertex(12, i + 1),
            vertex(20, i + 1),
        )
        for j, tri in enumerate(((ia, oa, ob), (ia, ob, ib))):
            lo = cam.sketch(Plane.xy(-0.1), name=f"Track {i}-{j} base")
            lo.polyline([(v.x, v.y) for v in tri])
            plane = Plane(
                tri[0], (tri[1] - tri[0]).cross(tri[2] - tri[0]), tri[1] - tri[0]
            )
            hi = cam.sketch(plane, name=f"Track {i}-{j} face")
            hi.polyline([plane.uv(v) for v in tri])
            cam._add(
                dict(
                    type="Loft",
                    name=f"Track sector {i}-{j}",
                    sketch_ids=[lo.add().id, hi.add().id],
                    mode="add",
                )
            )
    cam.frame("shaft", origin=(0, 0, 0))
    cam.material("steel")
    verify(cam)
    return housing, lid, membrane, diaphragm, rocker, roller, cam


def install_vacuum(app, rig, cover_occ, crank_mate):
    housing, lid, membrane, diaphragm, rocker, roller, cam = vacuum_parts(app)
    ho = rig.place(housing, name="vacuum pump casting")
    rig.fasten((cover_occ, "vacuum_pump"), (ho, "mount"), flipped=False)
    for i in range(6):
        screw = app.add_fastener(standard="iso4762", size="M5", length=12)
        app.rename_occurrence(
            id=screw["occurrence"], name=f"vacuum mount screw {i + 1}"
        )
        rig.fasten(
            (ho, f"base_screw_{i}"),
            dict(occurrence=screw["occurrence"], origin=True),
            flipped=False,
        )
    # Rigid covers share the casting's local coordinates, anchored at origin.
    for part in (lid, membrane):
        part.frame("mount", origin=(0, 0, 0))
        po = rig.place(part, name=part.name)
        rig.fasten((ho, "mount"), (po, "mount"), flipped=False)
        if part is lid:
            for i in range(8):
                screw = app.add_fastener(standard="iso4762", size="M5", length=10)
                rig.fasten(
                    (po, f"screw_{i}"),
                    dict(occurrence=screw["occurrence"], origin=True),
                    flipped=False,
                )
    bearing = app.part("vacuum timer shaft bush")
    annulus(bearing, "Shaft bearing", 9, 6.2, 0, 10)
    annulus(bearing, "Bearing locating flange", 12, 6.2, -2, 2, mode="add")
    bearing.material("brass")
    bearing.frame("seat", origin=(0, 0, 0))
    verify(bearing)
    bearing_occ = rig.place(bearing, name=bearing.name)
    rig.fasten((cover_occ, "vacuum_bearing"), (bearing_occ, "seat"), flipped=False)
    co = rig.place(cam, name="vacuum pump face cam")
    cm = rig.revolute((ho, "cam"), (co, "shaft"), flipped=False)
    rig.chain(
        crank_mate, cm, teeth_a=1, teeth_b=2, name="injection timer half-speed drive"
    )
    ro = rig.place(rocker, name="vacuum pump rocker")
    rm = rig.revolute((ho, "pivot"), (ro, "pivot"), flipped=False)
    roller.frame("centre", origin=(0, 0, 0))
    rig.fasten(
        (ro, "roller"),
        (rig.place(roller, name="vacuum pump roller"), "centre"),
        flipped=False,
    )
    do = rig.place(diaphragm, name="vacuum pump diaphragm")
    dm = rig.slider((ho, "diaphragm"), (do, "slide"), flipped=False)
    lift = [
        [float(i), LEVER_ARM * ROCKER_ANGLE * (1 - math.cos(math.radians(2 * i))) / 2]
        for i in range(0, 360, 5)
    ]
    rig.cam(cm, dm, lift=lift, name="two-lobe vacuum-pump displacement")
    rig.activate()
    app.add_relation(
        type="rack",
        a=rm["id"],
        b=dm["id"],
        pitch_radius=LEVER_ARM,
        name="vacuum rocker leverage",
    )
    return dict(cam=co, rocker=ro, diaphragm=do, cam_mate=cm, slide=dm)


def mount_fastener(app, rig, occ, frame, standard, size, length=0):
    rig.activate()
    bolt = app.add_fastener(standard=standard, size=size, length=length)
    app.rename_occurrence(id=bolt["occurrence"], name=f"{occ.name} {frame}")
    rig.fasten(
        (occ, frame), dict(occurrence=bolt["occurrence"], origin=True), flipped=False
    )
    return bolt


def bracket_foot(p, z):
    sk = p.sketch(Plane.yz(100), name="Block mounting foot")
    B.rounded_polygon(
        sk, [(-182, z - 28), (-155, z - 28), (-155, z + 28), (-182, z + 28)], 4
    )
    p.extrude(sk.add(), 10, name="Block mounting foot")
    for i, dz in enumerate(MOUNT_BOLT_OFFSETS):
        B.bore(
            p,
            (99, MOUNT_BOLT_Y, z + dz),
            (111, MOUNT_BOLT_Y, z + dz),
            8.8,
            "Block attachment bore",
            over=0,
        )
        p.frame(
            f"foot_washer_{i}",
            origin=(110, MOUNT_BOLT_Y, z + dz),
            z=(-1, 0, 0),
            x=(0, 1, 0),
        )
        p.frame(
            f"foot_bolt_{i}",
            origin=(111.6, MOUNT_BOLT_Y, z + dz),
            z=(-1, 0, 0),
            x=(0, 1, 0),
        )
    p.frame("block", origin=(100, -175, z), z=(1, 0, 0), x=(0, 1, 0))


def pivot_stack_frames(p, z, head_seat, rear_seat):
    p.frame("pivot_bolt", origin=(ALT_X, head_seat - 2, z), z=(0, 1, 0), x=(1, 0, 0))
    p.frame(
        "pivot_front_washer", origin=(ALT_X, head_seat, z), z=(0, 1, 0), x=(1, 0, 0)
    )
    p.frame(
        "pivot_back_washer", origin=(ALT_X, rear_seat, z), z=(0, -1, 0), x=(1, 0, 0)
    )
    p.frame("pivot_nut", origin=(ALT_X, rear_seat + 2, z), z=(0, -1, 0), x=(1, 0, 0))


def alternator_bracket(app):
    p = app.part("alternator mounting bracket")
    z = BRACKET_Z
    bracket_foot(p, z)
    B.axis_solid(p, (108, -165, z), (ALT_X - 12, -254, z), 16, "Cast support arm")
    sk = p.sketch(Plane.xz(-250), name="Alternator pivot eye")
    sk.circle((ALT_X, z), r=14)
    p.extrude(sk.add(), 8, mode="add", name="Alternator pivot eye")
    # Trim only the lug envelope: extending this cut inboard severs the arm.
    B.slab(
        p, ALT_X - 14, -250, z - 22, ALT_X + 30, -210, z + 23,
        "Lug seating face", mode="cut"
    )
    B.bore(p, (ALT_X, -260, z), (ALT_X, -249, z), 10.5, "Pivot bolt", over=0)
    p.frame("alternator", origin=(ALT_X, BELT_Y + 16, ALT_Z), z=(0, 1, 0), x=(1, 0, 0))
    pivot_stack_frames(p, z, -258, -233)
    p.material("steel")
    p.check(solids=1)
    verify(p)
    return p


def alternator_upper_bracket(app):
    p = app.part("alternator adjustment bracket")
    z = UPPER_BRACKET_Z
    bracket_foot(p, z)
    B.axis_solid(p, (108, -165, z), (133, -229, 177), 14, "Upper support arm")
    sk = p.sketch(Plane.xz(-227), name="Adjustment strap")
    # Material must surround both ends of the adjustment slot below.
    B.rounded_polygon(
        sk, [(125, 140), (125, 177), (135, 184), (214, 180), (214, 140)], 5
    )
    p.extrude(sk.add(), 6, mode="add", name="Adjustment strap")
    sk = p.sketch(Plane.xz(-226), name="Pivot concentric adjustment slot")
    radius = 2 * ALT_LUG
    half = 5.25

    def pt(r, a):
        return (ALT_X + r * math.cos(a), BRACKET_Z + r * math.sin(a))

    a, b = math.radians(100), math.radians(80)
    sk.arc_through(
        pt(radius + half, a), pt(radius + half, math.pi / 2), pt(radius + half, b)
    )
    x, y = pt(radius, b)
    sk.arc_through(
        pt(radius + half, b),
        (x + half * math.sin(b), y - half * math.cos(b)),
        pt(radius - half, b),
    )
    sk.arc_through(
        pt(radius - half, b), pt(radius - half, math.pi / 2), pt(radius - half, a)
    )
    x, y = pt(radius, a)
    sk.arc_through(
        pt(radius - half, a),
        (x - half * math.sin(a), y + half * math.cos(a)),
        pt(radius + half, a),
    )
    p.extrude(sk.add(), 8, mode="cut", name="Adjustment slot")
    pivot_stack_frames(p, z, -250, -227)
    p.material("steel")
    p.check(solids=1)
    verify(p)
    return p


def prepare_block(block):
    # The upper block narrows: a real cast boss reaches the bracket face.
    B.axis_solid(
        block,
        (68, MOUNT_BOLT_Y, UPPER_BRACKET_Z),
        (100, MOUNT_BOLT_Y, UPPER_BRACKET_Z),
        64,
        "Alternator upper mounting boss",
    )
    for z in (BRACKET_Z, UPPER_BRACKET_Z):
        for dz in MOUNT_BOLT_OFFSETS:
            B.bore(
                block,
                (85, MOUNT_BOLT_Y, z + dz),
                (101, MOUNT_BOLT_Y, z + dz),
                8.8,
                "Alternator mounting bore",
                over=0,
            )
    block.frame(
        "alternator_adjustment",
        origin=(100, -175, UPPER_BRACKET_Z),
        z=(1, 0, 0),
        x=(0, 1, 0),
    )


def prepare_cover(cover):
    from om615_spec import COVER_Y0

    # A closed, cast side pocket bridges the whole flange into the chain case.
    # The former 7 mm annulus left the back of the pump open to daylight.
    sections = []
    for y, r in ((COVER_Y0, 59), (-243, 58), (-217, 53), (-207, 48)):
        sk = cover.sketch(Plane.xz(y), name="Vacuum pocket station")
        sk.circle((VAC_X, VAC_Z), r=r)
        sections.append(sk.add().id)
    cover._add(
        dict(type="Loft", name="Vacuum drive pocket", sketch_ids=sections, mode="add")
    )
    sections = []
    for y, r in ((COVER_Y0 - 1, 49), (-243, 48), (-223, 43), (-217, 40)):
        sk = cover.sketch(Plane.xz(y), name="Vacuum pocket core station")
        sk.circle((VAC_X, VAC_Z), r=r)
        sections.append(sk.add().id)
    cover._add(
        dict(
            type="Loft",
            name="Vacuum drive pocket core",
            sketch_ids=sections,
            mode="cut",
        )
    )
    # The side pocket and main chain case share a core. Leaving the outer
    # loft's inner wall here would put new metal across the chain guide rail.
    from om615_spec import BLOCK_Y0, COVER_GASKET_T, COVER_WALL, COVER_FRONT_T

    back = BLOCK_Y0 - COVER_GASKET_T
    sk = cover.sketch(Plane.xz(back), name="Shared timing case core")
    B._cover_outline(sk, inset=COVER_WALL, open_floor=True)
    cover.extrude(
        sk.add(),
        back - COVER_Y0 - COVER_FRONT_T,
        mode="cut",
        name="Shared timing case core",
    )
    cx, cz = VAC_X + CAM_CENTER[0], VAC_Z + CAM_CENTER[1]
    B.bore(
        cover, (cx, -218, cz), (cx, -206, cz), 18.4, "Timer bearing register", over=0
    )
    cover.frame("vacuum_bearing", origin=(cx, -207, cz), z=(0, -1, 0), x=(1, 0, 0))
    sk = cover.sketch(Plane.xz(COVER_Y0 + 8), name="Vacuum pump mounting bolts")
    for i in range(6):
        a = math.tau * i / 6
        sk.circle((VAC_X + 54 * math.cos(a), VAC_Z + 54 * math.sin(a)), d=5.5)
    cover.extrude(sk.add(), 9, mode="cut", name="Vacuum pump mounting bolts")
    cover.frame(
        "vacuum_pump", origin=(VAC_X, COVER_Y0, VAC_Z), z=(0, -1, 0), x=(1, 0, 0)
    )


# ----------------------------------------------------------- what hangs on it
# Static parts, each drawn in engine space and hung by a world-aligned frame
# that coincides with its holder's, so placing one is a translation of zero.
# Proportions are read off the photographs against the 87 mm bore and the
# 97 mm bore pitch; the interfaces they sit on are om615_spec's.
OIL_FILTER_CAN = (1.03 * BORE, 1.55 * BORE_PITCH)   # canister: 90 x 150
OIL_FILTER_COLLAR = 4.0      # the cast head and lid stand this proud of it
# The neck from the flange to the canister's head: inside the bolt circle,
# and long enough that the canister clears the bolt heads (an ISO 4762
# head is as tall as the bolt is thick) by a spanner's lip.
OIL_FILTER_NECK = (30.0, OIL_FILTER_BOLT_D + 8.0)
FUEL_FILTER_CAN = (0.92 * BORE, 1.13 * BORE_PITCH)  # canister: 80 x 110
FUEL_BRACKET_T = 8.0         # the steel bracket's plate and shelf
# The shelf the fuel filter hangs from: its canister's bottom clears the
# injection pump's mounting flange below by a hand's width of air.
FUEL_SHELF_Z = (PUMP_MOUNT[2] + PUMP_MOUNT_D / 2.0 + 10.0
                + FUEL_FILTER_CAN[1])
# Where both radiator hoses end, in free air a hose's length ahead of the
# fan's blades — about where a radiator's tanks would be.
RADIATOR_Y = FAN_Y - 1.75 * COOLANT_HOSE_OD
DIPSTICK_WALL = 1.5
DIPSTICK_TUBE_OD = DIPSTICK_BORE_D + 2.0 * DIPSTICK_WALL


def bent_tube(p, points, bend_r, od, wall, name, mode="new_body"):
    """A tube as a bender makes one: straight runs between `points` (the
    CORNERS of its centreline), each corner bent round `bend_r`, swept to
    `od` with `wall` (0 = solid)."""
    path = p._add(dict(type="Curve3D", name=f"{name} path", kind="bent_path",
                       points=[list(V(q)) for q in points], radius=bend_r))
    f = dict(type="Pipe", name=name, path_id=path["id"], outer_diameter=od,
             mode=mode)
    if wall:
        f["wall_thickness"] = wall
    return p._add(f)


def oil_filter_housing(app):
    """The oil filter housing on the block's pad: a round flange with its
    four M8 clearance holes and the port, a neck out, and the vertical
    canister standing beside the crankcase — cast head, bowl and lid with
    its centre bolt — its top near the deck, as on the engine."""
    p = app.part("oil filter housing")
    fx, fy, fz = OIL_FILTER_BOSS
    face = fx - OIL_FILTER_FLANGE_T
    can_d, can_h = OIL_FILTER_CAN
    neck_d, neck_l = OIL_FILTER_NECK
    head_d = can_d + 2.0 * OIL_FILTER_COLLAR
    xc = face - neck_l - head_d / 2.0
    B.axis_solid(p, (fx, fy, fz), (face, fy, fz), OIL_FILTER_BOSS_D,
                 "Flange", mode="new_body")
    B.axis_solid(p, (face + 1.0, fy, fz), (xc, fy, fz), neck_d, "Neck")
    base = fz - neck_d
    top = fz + neck_d + can_h
    B.disc(p, (xc, fy), head_d, base, fz + neck_d, "Filter head")
    B.disc(p, (xc, fy), can_d, fz + neck_d - 1.0, top, "Canister")
    B.disc(p, (xc, fy), head_d, top, top + 2.0 * OIL_FILTER_COLLAR, "Lid")
    B.disc(p, (xc, fy), neck_d * 2.0 / 3.0, top + 2.0 * OIL_FILTER_COLLAR - 1.0,
           top + 4.0 * OIL_FILTER_COLLAR, "Lid bolt")
    for k in range(4):
        a = math.pi / 4 + k * math.pi / 2
        by = fy + OIL_FILTER_BOLT_PCD / 2.0 * math.cos(a)
        bz = fz + OIL_FILTER_BOLT_PCD / 2.0 * math.sin(a)
        # ISO 273 medium clearance for M8.
        B.bore(p, (face - 1.0, by, bz), (fx + 1.0, by, bz),
               OIL_FILTER_BOLT_D + 1.0, "Bolt hole", over=0.0)
    B.bore(p, (fx + 1.0, fy, fz), (xc, fy, fz), OIL_FILTER_PORT_D, "Port",
           over=0.0)
    p.material("aluminum")
    p.frame("pad", origin=OIL_FILTER_BOSS, z=(0, 0, 1), x=(1, 0, 0))
    return verify(p)


def fuel_filter_bracket(app):
    """A pressed-steel bracket: a plate bolted to the boss on the head's
    pump side and, at its top, a shelf reaching out with a ring the fuel
    filter's neck drops through."""
    p = app.part("fuel filter bracket")
    bx, by, bz = FUEL_FILTER_BOSS
    hy, hz = FUEL_FILTER_BOSS_HALF
    t = FUEL_BRACKET_T
    can_d, _ = FUEL_FILTER_CAN
    cx = fuel_filter_x()
    z0, z1 = bz - hz, FUEL_SHELF_Z + t
    plate = p.sketch(Plane.yz(bx - t), name="Plate")
    B.rounded_polygon(plate, [(by - hy, z0), (by + hy, z0), (by + hy, z1),
                              (by - hy, z1)], [hy / 2.0, hy / 2.0, 0, 0])
    p.extrude(plate.add(), t, mode="new_body", name="Plate")
    B.slab(p, cx, by - hy, FUEL_SHELF_Z, bx - t + 1.0, by + hy, z1, "Arm",
           mode="add")
    B.disc(p, (cx, by), can_d + 8.0, FUEL_SHELF_Z, z1, "Ring")
    B.disc(p, (cx, by), can_d / 2.0 + 2.0, FUEL_SHELF_Z - 1.0, z1 + 1.0,
           "Ring hole", mode="cut")
    for s in (-1, 1):
        B.bore(p, (bx - t - 1.0, by, bz + s * FUEL_FILTER_BOLT_DZ),
               (bx + 1.0, by, bz + s * FUEL_FILTER_BOLT_DZ),
               FUEL_FILTER_BOLT_D + 1.0, "Bolt hole", over=0.0)
    p.material("steel")
    p.frame("head", origin=FUEL_FILTER_BOSS, z=(0, 0, 1), x=(1, 0, 0))
    p.frame("filter", origin=(cx, by, z1), z=(0, 0, 1), x=(1, 0, 0))
    return verify(p)


def fuel_filter_x():
    """The fuel filter's axis: out from the bracket's plate by the bolt
    heads' height and a spanner's lip, and its own radius."""
    return (FUEL_FILTER_BOSS[0] - FUEL_BRACKET_T - FUEL_FILTER_BOLT_D - 4.0
            - FUEL_FILTER_CAN[0] / 2.0)


def fuel_filter(app):
    """The fuel filter: a cast head sitting on the bracket's ring with its
    inlet and outlet banjo bosses and bleed screw, and the canister hanging
    below on a neck through the ring."""
    p = app.part("fuel filter")
    can_d, can_h = FUEL_FILTER_CAN
    cx, cy = fuel_filter_x(), FUEL_FILTER_BOSS[1]
    seat = FUEL_SHELF_Z + FUEL_BRACKET_T
    head_h = can_d / 4.0
    B.disc(p, (cx, cy), can_d + 4.0, seat, seat + head_h, "Head",
           mode="new_body")
    B.disc(p, (cx, cy), can_d / 2.0, FUEL_SHELF_Z - 1.0, seat + 1.0, "Neck")
    B.disc(p, (cx, cy), can_d, FUEL_SHELF_Z - can_h, FUEL_SHELF_Z,
           "Canister")
    B.disc(p, (cx, cy), can_d / 6.0, FUEL_SHELF_Z - can_h - can_d / 12.0,
           FUEL_SHELF_Z - can_h + 1.0, "Drain screw")
    for s in (-1, 1):
        B.axis_solid(p, (cx, cy, seat + head_h / 2.0),
                     (cx, cy + s * (can_d / 2.0 + 12.0), seat + head_h / 2.0),
                     can_d / 6.0, "Banjo boss")
    B.disc(p, (cx, cy), can_d / 8.0, seat + head_h - 1.0,
           seat + head_h + can_d / 10.0, "Bleed screw")
    p.material("steel")
    p.frame("seat", origin=(cx, cy, seat), z=(0, 0, 1), x=(1, 0, 0))
    return verify(p)


def upper_hose(app):
    """The upper radiator hose: pushed onto the thermostat housing's outlet
    stub (its bore IS the stub), on along the stub's axis, then forward over
    the front of the engine to end ahead of the fan."""
    p = app.part("upper coolant hose")
    root, axis, reach, _ = T.thermostat_ports()
    start = root + axis * reach
    bend = COOLANT_HOSE_OD
    turn = start + axis * bend
    end = V((TSTAT_X, RADIATOR_Y, turn[2] - COOLANT_HOSE_OD))
    bent_tube(p, [start, turn, end], bend, COOLANT_HOSE_OD,
              COOLANT_HOSE_WALL, "Upper hose")
    p.material("rubber")
    p.frame("seat", origin=tuple(start), z=tuple(axis), x=(1, 0, 0))
    return verify(p)


def lower_hose(app):
    """The lower radiator hose: up off the water pump's inlet neck, across
    the front above the belt, forward outside the fan's sweep, and down to
    where a radiator's bottom tank would be."""
    p = app.part("lower coolant hose")
    start = V((0.0, WP_INLET_Y, B.WP_INLET_Z0 + WP_INLET[0]))
    bend = COOLANT_HOSE_OD
    run_z = start[2] + bend + 5.0
    # Out far enough that the forward run clears the fan's blade tips.
    reach = FAN_R + COOLANT_HOSE_OD / 2.0 + 10.0
    run_x = math.sqrt(reach ** 2 - (run_z - WATER_PUMP_Z) ** 2)
    corners = [start, (0.0, WP_INLET_Y, run_z), (run_x, WP_INLET_Y, run_z),
               (run_x, RADIATOR_Y, run_z),
               (run_x, RADIATOR_Y, COOLANT_HOSE_OD)]
    bent_tube(p, corners, bend, COOLANT_HOSE_OD, COOLANT_HOSE_WALL,
              "Lower hose")
    p.material("rubber")
    p.frame("seat", origin=tuple(start), z=(0, 0, 1), x=(1, 0, 0))
    return verify(p)


def heater_pipe(app):
    """The heater pipe: steel, off the thermostat housing's spigot, out past
    the plenum and back along the manifold side at the height between the
    exhaust flanges and the plenum's floor, to the bulkhead end."""
    p = app.part("heater pipe")
    _, _, _, start = T.thermostat_ports()
    od, wall = HEATER_PIPE
    run_x = INT_LOG[0] + PLENUM[0] / 2.0 + od
    run_z = (PORT_Z + MANIFOLD_PAD_HZ + INT_LOG[1] - PLENUM[1] / 2.0) / 2.0
    corners = [start, (run_x, start[1], start[2]),
               (run_x, start[1] + 5.0 * od, run_z),
               (run_x, BLOCK_Y1 + 2.0 * od, run_z)]
    # A tube bender's tight bend: one and a half diameters.
    bent_tube(p, corners, 1.5 * od, od, wall, "Heater pipe")
    p.material("steel")
    p.frame("seat", origin=tuple(start), z=(0, 0, 1), x=(1, 0, 0))
    return verify(p)


def _dipstick_top():
    """Where the dipstick tube ends: out of its socket along the socket's
    lean far enough to clear the rail's ledge, then straight up beside the
    block to the head's top."""
    t, d = V(DIPSTICK_BOSS), V(DIPSTICK_AXIS)
    knee = t + d * (3.0 * DIPSTICK_TUBE_OD)
    return t, knee, V((knee[0], t[1], HEAD_TOP))


def dipstick_tube(app):
    p = app.part("dipstick tube")
    t, knee, top = _dipstick_top()
    bent_tube(p, [t, knee, top], 3.0 * DIPSTICK_TUBE_OD, DIPSTICK_TUBE_OD,
              DIPSTICK_WALL, "Dipstick tube")
    p.material("steel")
    p.frame("seat", origin=tuple(t), z=(0, 0, 1), x=(1, 0, 0))
    p.frame("handle", origin=tuple(top), z=(0, 0, 1), x=(1, 0, 0))
    return verify(p)


def dipstick(app):
    """The dipstick's handle: a cap on the tube's mouth, the blade's top
    running down inside the tube, and a flat ring to pull it by, facing the
    pump side where a hand reaches it."""
    p = app.part("dipstick")
    _, _, top = _dipstick_top()
    x, y, z = top
    cap = DIPSTICK_TUBE_OD + 4.0
    B.disc(p, (x, y), cap, z, z + cap / 3.0, "Cap", mode="new_body")
    B.disc(p, (x, y), DIPSTICK_BORE_D / 2.0, z - 5.0 * DIPSTICK_TUBE_OD,
           z + 1.0, "Blade")
    ring_od, ring_id = 2.5 * DIPSTICK_TUBE_OD, 1.5 * DIPSTICK_TUBE_OD
    ring_t = 5.0
    centre = (y, z + cap / 3.0 + ring_od / 2.0 - 2.0)
    sk = p.sketch(Plane.yz(x - ring_t / 2.0), name="Ring")
    sk.circle(centre, d=ring_od)
    sk.circle(centre, d=ring_id)
    p.extrude(sk.add(), ring_t, mode="add", name="Ring")
    p.material("plastic_white")
    p.frame("seat", origin=tuple(top), z=(0, 0, 1), x=(1, 0, 0))
    return verify(p)


# (part builder, holder key, holder frame, the part's own frame)
HANGING = (
    ("oil filter housing", oil_filter_housing, "block", "oil_filter", "pad"),
    ("dipstick tube", dipstick_tube, "block", "dipstick", "seat"),
    ("fuel filter bracket", fuel_filter_bracket, "head", "fuel_filter",
     "head"),
    ("upper coolant hose", upper_hose, "thermostat", "hose", "seat"),
    ("heater pipe", heater_pipe, "thermostat", "heater", "seat"),
    ("lower coolant hose", lower_hose, "water pump", "inlet", "seat"),
    ("fuel filter", fuel_filter, "fuel filter bracket", "filter", "seat"),
    ("dipstick", dipstick, "dipstick tube", "handle", "seat"),
)


def install_hanging(app, rig, holders):
    """Hang the static parts on their holders' frames. `holders` maps the
    keys in HANGING to occurrences; each part hung here becomes a holder
    too, so the filter goes on its bracket and the dipstick in its tube."""
    holders = dict(holders)
    for name, builder, holder, frame, own in HANGING:
        part = builder(app)
        rig.activate()
        o = rig.place(part, name=name)
        rig.fasten((holders[holder], frame), (o, own), flipped=False)
        holders[name] = o
    return {name: holders[name] for name, *_ in HANGING}


def assembly_inputs():
    return dict(
        alternator=[ALT_X, ALT_Z, ALT_D, ALT_LUG, MOUNT_BOLT_Y, *MOUNT_BOLT_OFFSETS],
        pitch_offset=PITCH_OFFSET,
        fan=[FAN_BACK_OFFSET, FAN_R],
        vacuum=[VAC_X, VAC_Z, VAC_DEPTH, *CAM_CENTER],
        rocker=[ROCKER_ANGLE, LEVER_ARM],
    )


def check_subassemblies(app):
    reports = {}
    for name in ("alternator stator assembly", "alternator rotor assembly"):
        app.assembly(name, create=False).activate()
        r = app.interference(mode="exact", max_pairs=1000, deadline_s=60)
        reports[name] = r
        if r.get("truncated") or r["clash"]:
            raise SdkError(f"{name}: incomplete or clashing internal assembly: {r}")
    return reports


def motion_dimensions(report, accessories):
    """Measure actual occurrence poses, independently of relation definitions."""
    samples = report["samples"]

    def poses(occ):
        return [
            next(p for p in s["poses"] if p["occurrence"] == occ.id) for s in samples
        ]

    def turns(occ):
        # A local X vector rotated by the quaternion, projected onto world XZ.
        angles = []
        for p in poses(occ):
            x, y, z, w = p["orientation"]
            angles.append(math.atan2(2 * (x * z - w * y), 1 - 2 * (y * y + z * z)))
        return (
            sum(math.remainder(b - a, math.tau) for a, b in zip(angles, angles[1:]))
            / math.tau
        )

    result = {}
    for name, occ, expected in (
        ("alternator", accessories["rotor"], -2 * PULLEYS[0][2] / PULLEYS[1][2]),
        ("fan", accessories["fan"], -2 * PULLEYS[0][2] / PULLEYS[2][2]),
        ("vacuum timer", accessories["vacuum"]["cam"], 1.0),
    ):
        actual = turns(occ)
        if abs(actual - expected) > 0.001:
            raise SdkError(
                f"{name}: measured {actual:.6f} turns, expected {expected:.6f}"
            )
        result[name] = dict(turns=actual, expected_turns=expected)
    ys = [p["position"][1] for p in poses(accessories["vacuum"]["diaphragm"])]
    stroke = max(ys) - min(ys)
    if abs(stroke - LEVER_ARM * ROCKER_ANGLE) > 0.01:
        raise SdkError(f"Vacuum diaphragm stroke {stroke:.4f} mm is incorrect")
    result["vacuum diaphragm"] = dict(stroke_mm=stroke)
    return result


def check_mounting_structure(app, rig):
    """Cheap preview gates: connected supports, sampled necks and bolt stacks.

    Section thresholds are model design minima, not a strength certification.
    The upper strap has two section regions around its enclosed adjustment slot.
    """
    sections, fasteners = [], []
    parts = {row["id"]: Part(app, row) for row in app.parts()}
    for name, samples, minimum in (
        ("alternator mounting bracket", (115, 139, 145, 153, 160, 164), 80),
        ("alternator adjustment bracket", (139, 145, 153, 176, 200, 211), 24),
    ):
        bracket = app.part(name, create=False)
        bracket.check(solids=1)
        for x in samples:
            section = app.section(part=bracket.id, base_plane="yz", offset=x,
                                  regions=True)
            if not section["regions"] or any(
                region["area"] < minimum for region in section["regions"]
            ):
                raise SdkError(f"{name}: insufficient support section at X={x}: {section}")
            sections.append(dict(part=name, x=x, minimum_area=minimum, **section))
        # Check the actual drilled foot axes against the mounting datums.
        holes = bracket.faces(kind="cylinder", radius=4.4, radius_tol=0.001)
        for i in range(2):
            datum = bracket.get_frame(f"foot_bolt_{i}")
            if not any(
                abs(abs(h.axis.dot(datum.z)) - 1) < 1e-5
                and (h.frame().origin - datum.origin).cross(datum.z).length() < 0.002
                for h in holes
            ):
                raise SdkError(f"{name}: foot_bolt_{i} is not aligned with a drilled hole")
        rig.activate()
        mount = rig.occurrence(name)
        for key in ("foot_bolt_0", "foot_bolt_1", "pivot_bolt"):
            bolt = rig.occurrence(f"{name} {key}")
            expected, actual = mount.frame(key), bolt.frame()
            if (expected.origin - actual.origin).length() > 0.002 or (
                expected.z.dot(actual.z) < 1 - 1e-5
            ):
                raise SdkError(f"{name}: {key} is not seated/aligned with its mounting datum")
            # Catalog bolts grow along local +Z from their under-head plane.
            _, tip = parts[bolt.part_id].bbox()
            if key.startswith("foot"):
                # Block mounting face is local X=100, bore ends at X=85.
                entry = (mount.frame("block").origin - actual.origin).dot(actual.z)
                engagement = tip.z - entry
                if not 8 <= engagement <= 15:
                    raise SdkError(f"{name}: {key} block engagement {engagement:.3f} mm")
            else:
                nut = rig.occurrence(f"{name} pivot_nut")
                nf = nut.frame()
                if (nf.origin - actual.origin).cross(actual.z).length() > 0.002 or (
                    abs(nf.z.dot(actual.z)) < 1 - 1e-5
                ):
                    raise SdkError(f"{name}: pivot nut is not coaxial with its bolt")
                lo, hi = parts[nut.part_id].bbox()
                ends = [(nf.origin + nf.z * z - actual.origin).dot(actual.z)
                        for z in (lo.z, hi.z)]
                engagement = min(tip.z, max(ends)) - max(0, min(ends))
                if engagement < hi.z - lo.z - 0.002:
                    raise SdkError(f"{name}: pivot bolt does not engage the full nut")
            fasteners.append(dict(bracket=name, bolt=key, engagement_mm=engagement))
    rig.activate()
    return dict(sections=sections, fasteners=fasteners)


def check_mountings(app, rig, hanging=True):
    """Check load-path contact and a closed pump pocket, not just non-overlap.

    `hanging=False` leaves out what hangs on the head and block, for a
    fixture built without them (tests/om615_mounting_smoke.py)."""
    structure = check_mounting_structure(app, rig)
    cover = app.part("timing cover", create=False)
    section = app.section(part=cover.id, base_plane="yz", offset=VAC_X, regions=True)
    if section["area"] < 1500:
        raise SdkError(f"Vacuum drive pocket has insufficient side section: {section}")
    rig.activate()
    pairs = [
        ("timing cover", "vacuum pump casting"),
        ("timing cover", "vacuum timer shaft bush"),
    ]
    for name in ("alternator mounting bracket", "alternator adjustment bracket"):
        pairs.extend([("block", name), (name, "alternator stator")])
        for i in range(2):
            pairs.extend(
                [
                    (f"{name} foot_bolt_{i}", f"{name} foot_washer_{i}"),
                    (f"{name} foot_washer_{i}", name),
                ]
            )
        front = name if name == "alternator mounting bracket" else "alternator stator"
        rear = "alternator stator" if name == "alternator mounting bracket" else name
        pairs.extend(
            [
                (f"{name} pivot_bolt", f"{name} pivot_front_washer"),
                (f"{name} pivot_front_washer", front),
                (f"{name} pivot_back_washer", rear),
                (f"{name} pivot_nut", f"{name} pivot_back_washer"),
            ]
        )
    pairs.extend(
        (f"vacuum mount screw {i + 1}", "vacuum pump casting") for i in range(6)
    )
    # What hangs on it sits ON its holder: a flange on its pad, a hose on the
    # end of its stub. Touching, not overlapping and not an air gap.
    holders = {"block": "block", "head": "cylinder head",
               "thermostat": "thermostat housing", "water pump": "water pump"}
    if hanging:
        pairs.extend((holders.get(holder, holder), name)
                     for name, _, holder, _, _ in HANGING)
    measured = []
    for a, b in pairs:
        r = app.measure(a=dict(occurrence=a), b=dict(occurrence=b))
        if r["distance"] > 0.002 or r.get("overlap_volume", 0) > 0.01:
            raise SdkError(f"Mounting contact failed: {a} / {b}: {r}")
        measured.append(dict(a=a, b=b, **r))
    return dict(structure=structure, pocket_side_section=section, contacts=measured)
