"""Front accessories. Photo-derived envelopes; see docs/modeling/om615-accessories.md.

Coordinates follow the engine (+Y rearward). Revolving hardware is built about
local Z and located by named mounting/shaft datums, not by bounding boxes.
"""

import math
from designer_sdk import Plane, V, SdkError
import om615_bottom as B
from om615_spec import BELT_Y, PULLEY_D, WP_PULLEY_D, WATER_PUMP_Z

ALT_X, ALT_Z, ALT_D = 176.0, 75.0, 76.0
ALT_LUG = 80.0
BRACKET_Z = ALT_Z - ALT_LUG
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
            angle=math.degrees(w["angle"]),
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
    stator.material("steel")
    verify(stator)
    rear = app.part("alternator rear casting")
    rear.revolve_profile(
        [(10, 92), (37, 92), (53, 88), (58, 88), (58, 104), (42, 117), (10, 117)],
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
    annulus(cam, "Timer face plate", 24, 4.5, -3, 3)
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


def alternator_bracket(app):
    p = app.part("alternator mounting bracket")
    z = BRACKET_Z
    sk = p.sketch(Plane.yz(100), name="Block mounting foot")
    B.rounded_polygon(
        sk, [(-182, z - 16), (-155, z - 16), (-155, z + 23), (-182, z + 23)], 4
    )
    p.extrude(sk.add(), 10, name="Block mounting foot")
    B.axis_solid(p, (108, -165, z), (ALT_X - 12, -254, z), 16, "Cast support arm")
    sk = p.sketch(Plane.xz(-250), name="Alternator pivot eye")
    sk.circle((ALT_X, z), r=14)
    p.extrude(sk.add(), 8, mode="add", name="Alternator pivot eye")
    B.slab(
        p, 140, -250, z - 22, ALT_X + 30, -210, z + 23, "Lug seating face", mode="cut"
    )
    B.bore(p, (ALT_X, -260, z), (ALT_X, -249, z), 10.5, "Pivot bolt", over=0)
    p.frame("block", origin=(100, -175, z), z=(1, 0, 0), x=(0, 1, 0))
    p.frame("alternator", origin=(ALT_X, BELT_Y + 16, ALT_Z), z=(0, 1, 0), x=(1, 0, 0))
    p.material("steel")
    verify(p)
    return p


def prepare_cover(cover):
    from om615_spec import COVER_Y0

    # A real flange and open register connect the new pump to the chain case.
    sk = cover.sketch(Plane.xz(COVER_Y0 + 7), name="Vacuum pump mounting pad")
    sk.circle((VAC_X, VAC_Z), r=59)
    cover.extrude(sk.add(), 7, mode="add", name="Vacuum pump mounting pad")
    sk = cover.sketch(Plane.xz(COVER_Y0 + 8), name="Vacuum pump register")
    sk.circle((VAC_X, VAC_Z), r=48.5)
    cover.extrude(sk.add(), 10, mode="cut", name="Vacuum pump register")
    sk = cover.sketch(Plane.xz(COVER_Y0 + 8), name="Vacuum pump mounting bolts")
    for i in range(6):
        a = math.tau * i / 6
        sk.circle((VAC_X + 54 * math.cos(a), VAC_Z + 54 * math.sin(a)), d=5.5)
    cover.extrude(sk.add(), 9, mode="cut", name="Vacuum pump mounting bolts")
    cover.frame(
        "vacuum_pump", origin=(VAC_X, COVER_Y0, VAC_Z), z=(0, -1, 0), x=(1, 0, 0)
    )


def assembly_inputs():
    return dict(
        alternator=[ALT_X, ALT_Z, ALT_D, ALT_LUG],
        pitch_offset=PITCH_OFFSET,
        fan=[FAN_BACK_OFFSET, FAN_R],
        vacuum=[VAC_X, VAC_Z, VAC_DEPTH, *CAM_CENTER],
        rocker=[ROCKER_ANGLE, LEVER_ARM],
    )


def check_subassemblies(app):
    reports = {}
    for name in ("alternator stator assembly", "alternator rotor assembly"):
        app.assembly(name, create=False).activate()
        r = app.interference(mode="exact", max_pairs=100, deadline_s=60)
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
