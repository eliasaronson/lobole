#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The SDK (docs/scripting/sdk.md), headless.

Two things are under test. First the ANTI-DUPLICATION rule that decides the
whole design: the RPC surface is generated from `describe`, so this asserts
every documented method is callable, that no hand-written SDK method shadows
one, and that the enums are read from the app rather than copied into the
module. Second the semantics `describe` cannot express — reference objects,
plane arithmetic, and the compositions — proved by building a piston and
checking it by number.

Usage: sdk_smoke.py <path-to-designer-binary>
"""
import inspect
import math
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lobole.sdk as sdk  # noqa: E402
from lobole.client import Client  # noqa: E402
from lobole.errors import UnknownArgument  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def refused(fn, *needles):
    try:
        fn()
    except (sdk.SdkError, RuntimeError) as e:
        for n in needles:
            expect(n in str(e), f"refused, but not naming '{n}': {e}")
        return str(e)
    raise AssertionError("it was accepted")


def check_generated_surface(app):
    """The surface is generated, not typed. If this ever fails, someone has
    started hand-writing wrappers and the SDK has begun to drift from the
    app."""
    documented = set(app.methods())
    expect(len(documented) > 200,
           f"describe should list the whole surface, got {len(documented)}")
    # (a) every documented method is reachable, with its doc.
    for name in documented:
        m = getattr(app, name)
        expect(callable(m), f"{name} is not callable")
        expect(m.__doc__, f"{name} carries no doc from describe")
    # (b) nothing hand-written on App shadows a documented RPC. App IS the
    #     generated surface, and a class attribute wins over __getattr__, so
    #     a method named after an RPC would silently replace it. Part and Asm
    #     are a different object with their own vocabulary: `part.fillet(...)`
    #     composing into `add_feature` is not a shadow of the `fillet` RPC,
    #     which is still `app.fillet(...)`.
    for name, _ in inspect.getmembers(sdk.App):
        if name.startswith("_"):
            continue
        expect(name not in documented,
               f"App.{name} shadows the RPC of the same name — rename it; "
               f"App is the generated surface")
    # And the semantic layer really is reachable alongside it: both meanings
    # of the names that overlap resolve, to different things.
    expect(callable(getattr(sdk.App, "part")), "app.part is the tab opener")
    expect("fillet" in documented and callable(sdk.Part.fillet),
           "app.fillet is the RPC, part.fillet is the composition")
    # (c) the enums come from the app.
    for key in ("constraint_kinds", "extrude_modes", "material_slugs",
                "feature_types", "sketch_entity_types"):
        expect(key in app.enums, f"describe's {key} is not exposed")
    expect("Frame" in str(app.enums["feature_types"]),
           "the feature-type table should carry Frame (v86)")
    # (d) a typo'd keyword is caught HERE, naming what the method takes.
    refused(lambda: app.add_part(named="x"), "named", "it takes", "name")
    # ...and for a method with no typed contract it is the refusal the app
    # would send, class and diagnostic, without the round trip.
    # Any such method will do: a refused call runs no handler.
    name = next(m for m in sorted(app.methods())
                if m not in app._schemas and not getattr(app, m).free_form
                and m.startswith(("list_", "get_")))
    seen = {}
    for side, call in (("client", lambda: getattr(app, name)(typo_key=1)),
                       ("app", lambda: app.call(name, typo_key=1))):
        try:
            call()
            raise AssertionError(f"{name}(typo_key=1) was accepted")
        except UnknownArgument as e:
            seen[side] = e.diagnostic
    expect(seen["client"] == seen["app"],
           f"the SDK's refusal differs from the app's: {seen}")
    src = open(os.path.join(HERE, "..", "python", "lobole",
                            "sdk.py")).read()
    for kind in ("coincident", "perpendicular", "distance_pl"):
        expect(f'"{kind}"' not in src,
               f"the constraint kind '{kind}' is hard-coded in the SDK; read "
               f"it from app.enums instead")


def check_verdict_contracts(app, asm):
    """A wrapper that answers a VERDICT must account for every field its RPC
    returns — a key it does not read is a caveat the caller never hears.

    `Asm.check` read `clashes` and dropped `truncated`, so a 241-occurrence
    engine whose bounded sweep reached 145 of 790 pairs came back as an
    unqualified pass, twice, in a report to a human. The table lives beside
    the wrapper in the SDK; this is what keeps it honest in both directions.
    """
    for name, contract in sdk.VERDICT_CONTRACTS.items():
        asm.activate()
        got = set(app.call(contract["rpc"]))
        known = set(contract["consumed"]) | set(contract["ignored"])
        expect(not (got - known),
               f"{name} accounts for every key {contract['rpc']} returns "
               f"(unaccounted: {sorted(got - known)})")
        gone = set(contract["consumed"]) - got - set(contract.get("optional",
                                                                   ()))
        expect(not gone,
               f"{name} consumes only keys {contract['rpc']} still returns "
               f"(gone: {sorted(gone)})")


def build_piston(part):
    """A piston, in the terms a piston is described in. Every dimension is a
    variable and every position comes from geometry or from the plane."""
    part.var("bore", 86).var("crown_h", 9).var("skirt_h", 34)
    part.var("pin_d", 22).var("ring_w", 1.5)
    # The body: a revolve of the profile, on the XZ plane about Z.
    r = "bore / 2"
    part.revolve_profile(
        [(0, 0), ("bore / 2", 0), ("bore / 2", "-(crown_h + skirt_h)"),
         (0, "-(crown_h + skirt_h)")],
        plane=sdk.Plane.xz(), axis="z", name="Piston")
    # Ring grooves: revolved cuts, the way a lathe makes them. Their radii
    # are expressions, so they follow the bore.
    for i in range(3):
        z = -(3.0 + 4.0 * i)
        g = part.sketch(sdk.Plane.xz(), name=f"Ring {i + 1}")
        g.polyline([("bore / 2 - 2.5", z), ("bore / 2 + 1", z),
                    ("bore / 2 + 1", z - 1.5), ("bore / 2 - 2.5", z - 1.5)])
        part.revolve(g.add(), axis="z", mode="cut",
                     name=f"Ring groove {i + 1}")
    # The wrist-pin bore, through, on a plane offset from the axis.
    pin = part.sketch(sdk.Plane.yz(-60), name="Pin bore")
    pin.circle((0, "-(crown_h + 14)"), d="pin_d")
    part.extrude(pin.add(), 120.0, mode="cut", name="Pin bore")
    return part.body()


def check_params_drive(app):
    """A part built ENTIRELY out of Params must follow its table.

    Note the SCOPE: `part.param` declares a part variable, so the edit below
    must name that scope too. Setting a project variable of the same name
    creates a second one and the part correctly does not move — which is how
    this test first "found" an app bug that was not there.

    `Param` is a float that carries its expression, and that is the whole
    reason a generator can be written as ordinary arithmetic. Two places did
    not know it — `circle(d=)` built its expression by string interpolation,
    and `_uv` (every line, point and polyline coordinate) had no Param branch
    at all — so both took the literal path a float subclass falls into. The
    document then builds, looks right, reports `cached, executed 0` when its
    variable changes, and rebuilds to exactly the same shape at any value.
    Nothing says so; that is why this is a test and not a review note.
    """
    part = app.part("param driven")
    w = part.param("plate_w", 60.0)
    h = part.param("plate_h", 40.0)
    d = part.param("plate_bore", 10.0)
    t = part.param("plate_t", 5.0)
    # Every coordinate is Param arithmetic and the bore is a Param: no
    # literal anywhere, and no expression STRINGS either — the point is that
    # the arithmetic form works, since that is what a generator writes.
    sk = part.sketch(sdk.Plane.xy(), name="plate")
    sk.polyline([(-w / 2, -h / 2), (w / 2, -h / 2),
                 (w / 2, h / 2), (-w / 2, h / 2)], close=True)
    part.extrude(sk.add(), distance=t, name="plate")
    bore = part.sketch(sdk.Plane.xy(), name="bore")
    bore.circle((0, 0), d=d)
    part.cut(bore.add(), distance=t, name="bore")

    lo, hi = part.bbox()
    expect(abs((hi.x - lo.x) - 60.0) < 0.01,
           f"the plate starts one plate_w across: {hi.x - lo.x:.3f}")

    app.set_variable(name="plate_w", expression="90.0", scope="part")
    lo, hi = part.bbox()
    expect(abs((hi.x - lo.x) - 90.0) < 0.01,
           f"a Param COORDINATE must drive the profile — the plate is still "
           f"{hi.x - lo.x:.3f} across after plate_w went to 90")

    # Measured AFTER the widening above, or the plate growing swamps the
    # bore growing and the comparison says nothing.
    before = part.volume()
    app.set_variable(name="plate_bore", expression="20.0",
                     scope="part")
    after = part.volume()
    expect(after < before,
           f"a Param DIAMETER must drive the bore — volume {after:.1f} did "
           f"not fall from {before:.1f} when plate_bore doubled")

    # rect() and centered_rect() too. The app's rectangle has no expression
    # fields, so a Param corner went in as its number: the pan/tilt yoke's
    # base stayed one camera's size while its holes, drawn as points,
    # followed the next camera's out through its end.
    part = app.part("param rect")
    a = part.param("rect_a", 30.0)
    b = part.param("rect_b", 20.0)
    sk = part.sketch(sdk.Plane.xy(), name="rect")
    sk.rect((0, -b / 2), (a, b / 2))
    part.extrude(sk.add(), distance=5.0, name="rect")
    sk = part.sketch(sdk.Plane.xy(), name="centred")
    sk.centered_rect((-a, 0), a / 3, b)
    part.extrude(sk.add(), distance=5.0, mode="add", name="centred")
    lo, hi = part.bbox()
    expect(abs(lo.x + 35.0) < 0.01 and abs(hi.x - 30.0) < 0.01,
           f"the rects start at x -35..30: {lo.x:.3f}..{hi.x:.3f}")
    app.set_variable(name="rect_a", expression="45.0", scope="part")
    app.set_variable(name="rect_b", expression="26.0", scope="part")
    lo, hi = part.bbox()
    expect(abs(lo.x + 52.5) < 0.01 and abs(hi.x - 45.0) < 0.01 and
           abs((hi.y - lo.y) - 26.0) < 0.01,
           f"a Param CORNER must drive a rect — x {lo.x:.3f}..{hi.x:.3f}, "
           f"{hi.y - lo.y:.3f} wide, after rect_a 45 and rect_b 26")

    # A slot along u, its centres and width Params (the cradle's camera
    # slot, which stayed under the camera's old seat).
    part = app.part("param slot")
    sx = part.param("slot_at", 10.0)
    sw = part.param("slot_w", 6.0)
    sk = part.sketch(sdk.Plane.xy(), name="slot")
    sk.slot((sx, 0), (sx + 20.0, 0), sw)
    part.extrude(sk.add(), distance=2.0, name="slot")
    app.set_variable(name="slot_at", expression="30.0", scope="part")
    app.set_variable(name="slot_w", expression="8.0", scope="part")
    lo, hi = part.bbox()
    expect(abs(lo.x - 26.0) < 0.01 and abs(hi.x - 54.0) < 0.01 and
           abs((hi.y - lo.y) - 8.0) < 0.01,
           f"a Param slot must follow its centres and width — x "
           f"{lo.x:.3f}..{hi.x:.3f}, {hi.y - lo.y:.3f} wide, not 26..54, 8")
    oblique = part.sketch(sdk.Plane.xy(), name="oblique")
    refused(lambda: oblique.slot((sx, 0), (sx + 10.0, 10.0), 4.0),
            "must run along u or v")

    # A pattern's count (the knob's flutes, one per 4 mm of a rim that
    # grows with the load).
    part = app.part("param pattern")
    n = part.param("holes_n", 4.0)
    sk = part.sketch(sdk.Plane.xy(), name="disc")
    sk.circle((0, 0), d=60.0)
    part.extrude(sk.add(), distance=2.0, name="disc")
    sk = part.sketch(sdk.Plane.xy(), name="hole")
    sk.circle((20.0, 0), d=6.0)
    hole = part.cut(sk.add(), distance=2.0, name="hole")
    part.pattern(hole, n, circular="z", name="holes")
    four = part.volume()
    app.set_variable(name="holes_n", expression="8", scope="part")
    eight = part.volume()
    one = math.pi * 3.0 ** 2 * 2.0
    expect(abs((four - eight) - 4 * one) < 0.5,
           f"a Param COUNT must drive a pattern — 4 holes to 8 took "
           f"{four - eight:.2f} mm3 away, not {4 * one:.2f}")
    refused(lambda: part.pattern(hole, n / 3, circular="z"),
            "not a whole number")


def check_bad_blend_refuses(app):
    """A blend set the kernel cannot build must FAIL, not kill the process.

    OCC's fillet builder segfaults rather than reporting a faulty contour:
    on a braced panel, nine edges blend and the tenth goes down inside
    `BRepBlend_SurfRstLineBuilder`. `EdgeBlendNamed` has always caught
    `Standard_Failure`, but a raw SIGSEGV is not one until OCC's signals are
    installed as exceptions — so the catch never saw it and a generator
    asking for one bad blend lost the whole session.
    """
    part = app.part("bad blend")
    frame = part.sketch(sdk.Plane.xy(), name="frame")
    frame.polyline([(-105, -160), (105, -160), (105, 160), (-105, 160)])
    part.extrude(frame.add(), distance=16.0, name="frame")
    win = part.sketch(sdk.Plane.xy(16), name="window")
    win.polyline([(-97, -152), (97, -152), (97, 152), (-97, 152)])
    part.cut(win.add(), distance=16.0, reversed=True, name="window")
    spokes = part.sketch(sdk.Plane.xy(), name="spokes")
    spokes.polyline([(-12, -160), (12, -160), (12, 160), (-12, 160)])
    part.extrude(spokes.add(), distance=16.0, mode="add", name="spokes")

    edges = [e for e in part.body().edges(kind="line") if e.length > 15.0]
    expect(len(edges) > 20, f"the test needs a big blend set: {len(edges)}")
    part.fillet(edges, r=8.0, name="doomed")
    # Still answering, which is the whole point of the test.
    state = part.state()
    expect(not state["regen"]["ok"],
           "a blend the kernel cannot build must be reported as failed")
    why = " ".join(f.get("error", "") for f in state["regen"]["failed"])
    expect("fillet" in why or "OCCT" in why,
           f"and say why, in its own words: {why!r}")


def check_faces_by_maker(app):
    """A feature call hands back the feature, and its faces are asked for
    by what made them (docs/modeling/toponaming.md): a cut's floor, a
    hole's bores, a Derived copy's through `via`. A cut returned None, so a
    script had nothing to ask and filtered by normal and position."""
    part = app.part("maker faces")
    sk = part.sketch(sdk.Plane.xy(), name="plate")
    sk.rect((0, 0), (60, 40))
    plate = part.extrude(sk.add(), 20, name="Plate")
    expect(plate.feature.end_cap().normal.z > 0.99,
           "a new body's feature answers for its caps")
    pk = part.sketch(sdk.Plane.xy(20), name="pocket")
    pk.rect((5, 5), (25, 25))
    pocket = part.cut(pk.add(), 8, reversed=True, name="Pocket")
    expect(isinstance(pocket, sdk.Feature) and pocket["id"] == pocket.id,
           f"a cut hands back its feature, and still reads like the dict: "
           f"{pocket!r}")
    floor = pocket.floor()
    expect(abs(floor.centroid.z - 12.0) < 1e-6 and floor.role == "end_cap",
           f"the pocket's floor, by name: {floor.name} {floor.centroid}")
    expect(len(pocket.walls()) == 4, "and its four walls")
    holes = part.sketch(sdk.Plane.xy(20), name="holes")
    holes.point((45, 10))
    holes.point((45, 30))
    h = part.hole(holes.add(), d=5, depth=10, name="Holes")
    expect(len(h.bores()) == 2 and all(b.role == "bore" for b in h.bores()),
           "a hole's bores")
    refused(lambda: h.faces(role="countersink"), "made bore")
    expect(len(part.body().faces(made_by="Pocket", role="walls")) == 4,
           "Body.faces takes made_by and role")
    expect(part.feature("Pocket").id == pocket.id, "a feature by name")
    # The edges where the plate's top meets its walls: not the pocket's
    # rims, which join the top to the POCKET's walls.
    rim = part.body().edges(between=(plate.feature.end_cap(),
                                     plate.feature.walls()))
    expect(len(rim) == 4 and all(abs(e.midpoint.z - 20.0) < 1e-6
                                 for e in rim),
           f"the top's four outer edges: {[e.name for e in rim]}")
    part.chamfer(rim, 1.0, name="Top edge")
    part.check()

    other = app.part("maker copy")
    other.activate()
    app.add_feature(feature={"type": "Derived", "name": "copy",
                             "source_part": part.id, "reference": False})
    copied = other.faces(via="copy", made_by=part.feature("Pocket"),
                         role="floor")
    expect(len(copied) == 1 and abs(copied[0].centroid.z - 12.0) < 1e-6,
           f"the copy's pocket floor, through via: {copied}")


def check_place_configured(app):
    """place(configuration=, material=): one part as two BOM lines."""
    part = app.part("slab")
    sk = part.sketch(sdk.Plane.xy(), name="slab")
    sk.rect((0, 0), (10, 10))
    t = part.param("t", 5)
    part.extrude(sk.add(), t, name="Slab")
    app.add_configuration(name="thick")
    thick = [c for c in app.list_configurations()["configurations"]
             if c["name"] == "thick"][0]["id"]
    app.set_configuration(id=thick, parameter_overrides=[
        {"name": "t", "expression": "15"}])
    asm = app.assembly("slabs")
    asm.place(part, at=(0, 0, 0), fixed=True, material="wood_maple")
    asm.place(part, at=(30, 0, 0), fixed=True, configuration="thick",
              material="wood_walnut")
    rows = app.bom()["rows"]
    got = sorted((r["configuration"], r["material"], round(r["unit_volume"]))
                 for r in rows)
    expect(got == [("", "wood_maple", 500), ("thick", "wood_walnut", 1500)],
           f"two lines, each its configuration and wood: {got}")


def check_compare_aligns(app, tmp):
    """`compare_geometry {align}` answers about the SHAPE, not the place.

    IoU is computed where the two solids sit, so "is my re-draw right?" —
    the question porting a design in asks over and over — could not be asked.
    A re-drawn port tube whose volume matched its reference to four figures
    scored 0.333, because the reference sits where the cabinet puts it and
    the part is built about its own origin 900 mm away.
    """
    ref = app.part("far tube")
    sk = ref.sketch(sdk.Plane.xy(-900.0), name="tube")
    sk.circle((0, 0), d=105.0)
    sk.circle((0, 0), d=100.0)
    ref.extrude(sk.add(), distance=250.0, name="tube")
    golden = os.path.join(tmp, "far-tube.step")
    app.export_step(path=golden)

    here = app.part("tube at origin")
    sk = here.sketch(sdk.Plane.xy(0), name="tube")
    sk.circle((0, 0), d=105.0)
    sk.circle((0, 0), d=100.0)
    here.extrude(sk.add(), distance=250.0, name="tube")

    apart = app.compare_geometry(golden_path=golden)
    expect(abs(apart["volume_ratio"] - 1.0) < 1e-3,
           f"the same shape either way: volume ratio {apart['volume_ratio']}")
    expect(apart["iou"] < 0.5,
           f"unaligned, 900 mm apart, IoU must be poor: {apart['iou']:.3f}")

    same = app.compare_geometry(golden_path=golden, align=True)
    expect(same["iou"] > 0.99,
           f"aligned, it is the same solid: IoU {same['iou']:.3f}")
    # The alignment is never silent — the offset it closed is still reported.
    expect(same.get("center_offset", 1.0) < 1e-3,
           f"aligned, the centres coincide: {same.get('center_offset')}")


# A word that names an angle. `pitch` is left out on purpose: a thread's
# pitch is a length.
ANGLE_WORDS = {"angle", "rotate", "rotation", "tilt", "twist", "sweep",
               "phase", "theta", "spin", "yaw", "roll"}


def angle_params_without_unit(fn):
    return [p for p in inspect.signature(fn).parameters
            if set(p.split("_")) & ANGLE_WORDS
            and p.split("_")[-1] not in ("deg", "rad")]


def check_angle_units():
    """Every hand-written angle keyword names its unit, as every RPC one does
    (tests/rpc_units_gate.py). A bare angle meant degrees in `revolve` and
    radians in `bolt_circle`; this catches the next helper, not only those
    (docs/scripting/sdk.md#angles-name-their-unit)."""
    def bare(angle=0.0, start_angle=0.0, rotate_deg=0.0, pitch=1.0):
        pass
    expect(angle_params_without_unit(bare) == ["angle", "start_angle"],
           "the scan must flag a bare angle, or it passes by seeing nothing")
    offenders, scanned = [], 0
    for cname, cls in inspect.getmembers(sdk, inspect.isclass):
        if cls.__module__ != sdk.__name__:
            continue
        for fname, fn in inspect.getmembers(cls, inspect.isfunction):
            if fname.startswith("_"):
                continue
            scanned += 1
            offenders += [f"{cname}.{fname}({p}=)"
                          for p in angle_params_without_unit(fn)]
    expect(scanned > 50, f"scanned only {scanned} SDK methods")
    expect(not offenders, "angle keywords that name no unit — end them "
           "_deg (or _rad): " + ", ".join(offenders))


def check_cam_op_fields(app):
    """Every field an operation REPORTS can be SET through the generated SDK.

    The generated methods refuse any keyword their `describe` row does not
    list, and the op rows are hand-kept: `tabs`, then `cut_angle_deg`,
    `sample_step_mm`, `z_step_mm` and `rotary_pattern` were each accepted by
    the server, reported in its answers and refused by `app.set_cam_op(...)`
    before the call left the client. Scripts using the raw `call` never saw
    it. The fields come from each op's own answer, so this grows with them.
    """
    if not app.call("ping").get("features", {}).get("cam", True):
        return
    app.new()
    box(app, (0, 0, 0), (40, 30, 20), "Block")
    app.call("machine_part")
    sid = app.add_cam_setup(name="fields")["id"]
    app.set_cam_setup(id=sid, has_rotary_axis=True)
    fixed = {"id", "type", "targets", "derived"}
    for t in ("profile", "pocket", "face", "drill", "adaptive", "edge_round",
              "surface_parallel", "surface_waterline", "surface_rotary"):
        op = app.add_cam_op(setup=sid, type=t, tool=1, top_z=20.0,
                            bottom_z=5.0)
        for k, v in op.items():
            if k in fixed:
                continue
            try:
                app.set_cam_op(id=op["id"], **{k: v})
            except UnknownArgument as e:
                raise AssertionError(
                    f"{t} reports `{k}` but the SDK's set_cam_op refuses it "
                    f"-- add it to the describe row: {e}") from None
    app.new()


def check_part_is_a_choice(app):
    """`app.part(name)` creates; opening a tab somebody else made is asked
    for (create=False) or declared (reuse=True). The PES pump generator,
    building into an engine project, wrote its camshaft INTO the engine's
    because the name matched and the SDK opened it without a word."""
    app.new()
    mine = app.part("engine block")
    mine.box("Top", width=10, depth=10, height=10, at=(0, 0), centered=False)
    expect(app.part("engine block").id == mine.id,
           "a client switching back to a part it created gets that part")
    rig = app.assembly("engine")
    # `Part 1` came with the project, not from this client.
    refused(lambda: app.part("Part 1"), "Part 1", "create=False",
            "reuse=True")
    # A second client — another generator on the same instance.
    other = sdk.App(app._remote)
    why = refused(lambda: other.part("engine block"),
                  "already exists", "this script did not create it",
                  "app.part('engine block', create=False)", "reuse=True")
    expect("engine block" in why, why)
    refused(lambda: other.assembly("engine"), "already exists",
            "app.assembly('engine', create=False)")
    expect(other.part("engine block", create=False).id == mine.id,
           "create=False opens the existing part")
    expect(other.part("engine block", reuse=True).id == mine.id,
           "reuse=True opens the existing part")
    expect(other.assembly("engine", create=False).id == rig.id,
           "create=False opens the existing assembly")
    fresh = other.part("pump camshaft", reuse=True)
    expect(fresh.id != mine.id, "reuse=True creates when there is none")
    expect(other.part("pump camshaft").id == fresh.id,
           "and the part it created is its own afterwards")
    refused(lambda: other.part("no such part", create=False),
            "no part named 'no such part'")
    refused(lambda: other.part("x", create=False, reuse=True),
            "create=False", "reuse=True")
    # Ids are per project: a new project forgets what the client created.
    other.new()
    refused(lambda: other.part("Part 1"), "already exists")


def extrude_distance(part, name, distance):
    """A box's height is its Extrude's distance (a Box is stored as a sketch
    and an extrude, docs/modeling/README.md#primitives): edit it there."""
    part.activate()
    feats = part.app.call("get_state")["features"]
    (i,) = [i for i, f in enumerate(feats)
            if f["name"] == name and f["type"] == "Extrude"]
    part.app.modify_feature(index=i, feature=dict(feats[i], distance=distance))


def check_primitives_take_on(app):
    """A primitive stands ON something -- a face, a plane, a frame, a sketch
    or a named world plane -- and is placed by `at=(u, v)` in its frame. The
    world-position spellings are refused BY NAME before anything reaches the
    app: they used to work, so they are what a script will send."""
    app.new()
    part = app.part("primitives")
    refused(lambda: part.box((0, 0, 0), width=10, depth=10, height=10),
            "`on` must be", "no bare world position")
    refused(lambda: part.box(position=(0, 0, 0), w=10, h=10, d=10),
            "`position=` is now `on=`")
    refused(lambda: part.box("Top", w=10, depth=10, height=10),
            "`w=` is now `width=`")
    refused(lambda: part.cylinder(center=(0, 0, 0), d=6, height=5),
            "`center=` is now `on=`")
    refused(lambda: part.cylinder("Top", d=6, h=5), "`h=` is now `height=`")
    refused(lambda: part.sphere(center=(0, 0, 0), d=6),
            "`center=` is now `on=`")
    expect(not part.bodies(reference=False),
           "nothing that was refused reached the document")
    # width along u (+X on Top), depth along v (+Y), height along the normal;
    # `at` is the centre unless centered=False.
    part.box("Top", width=40, depth=30, height=20, at=(5, 0))
    lo, hi = part.bbox()
    expect(max(abs(a - b) for a, b in zip((*lo, *hi),
                                          (-15, -15, 0, 25, 15, 20))) < 1e-3,
           f"a box on Top at (5, 0): {lo} .. {hi}")
    ball = app.part("ball")
    ball.sphere("Top", d=10, at=(100, 0))
    lo, hi = ball.bbox()
    expect(max(abs(a - b) for a, b in zip((*lo, *hi),
                                          (95, -5, -5, 105, 5, 5))) < 1e-3,
           f"a sphere's centre is ON its plane: {lo} .. {hi}")


def check_flipped_planes(app):
    """`flip=True` faces a principal plane the other way, so a feature on
    the far side extrudes a POSITIVE distance along the flipped normal. The
    axes are the ones the docstring and
    docs/scripting/sdk.md#the-principal-planes table."""
    app.new()
    axes = {"xy": (2, sdk.V(0, 0, -1), sdk.V(1, 0, 0), sdk.V(0, -1, 0)),
            "xz": (1, sdk.V(0, 1, 0), sdk.V(-1, 0, 0), sdk.V(0, 0, 1)),
            "yz": (0, sdk.V(-1, 0, 0), sdk.V(0, -1, 0), sdk.V(0, 0, 1))}
    for name, (axis, normal, u, v) in axes.items():
        plane = getattr(sdk.Plane, name)(10.0, flip=True)
        expect((plane.normal - normal).length() < 1e-9
               and (plane.u - u).length() < 1e-9
               and (plane.v - v).length() < 1e-9,
               f"{name} flipped: normal {plane.normal} U {plane.u} "
               f"V {plane.v}, documented {normal} {u} {v}")
        expect((plane.normal.cross(plane.u) - plane.v).length() < 1e-9,
               f"{name} flipped: V is normal x U")
        # A disc centred on a world point, placed through uv().
        centre = [3.0, 7.0, -4.0]
        centre[axis] = 10.0
        part = app.part(f"flipped {name}")
        part.extrude_shapes(plane, [
            lambda s: s.circle(plane.uv(centre), d=4)], 5.0, name="Boss")
        part.check(solids=1)
        lo, hi = part.bbox()
        # Along the flipped normal: xy and yz now face back toward the
        # origin, xz (whose plain normal is -Y) away from it.
        want = sorted((10.0, 10.0 + 5.0 * normal[axis]))
        expect(abs(lo[axis] - want[0]) < 1e-6
               and abs(hi[axis] - want[1]) < 1e-6,
               f"{name}(10, flip=True) extrudes 5 along {normal}: "
               f"{lo[axis]:.3f}..{hi[axis]:.3f} on axis {axis}, not {want}")
        for k in range(3):
            if k != axis:
                mid = 0.5 * (lo[k] + hi[k])
                expect(abs(mid - centre[k]) < 1e-6,
                       f"{name} flipped: uv() put the disc at {mid:.3f} on "
                       f"axis {k}, not {centre[k]}")


def check_sketch_on_frame(app):
    """`sketch(on=<Frame>)` attaches the sketch to the frame (plane_ref),
    so a frame placed by a Param carries it when the variable moves; a
    `Plane` would stay where it was put. And `on=<Face>` sits on the face."""
    app.new()
    part = app.part("on a frame")
    z = part.param("deck_z", 12.0)
    deck = part.frame("deck", origin=(0, 0, z))
    sk = part.sketch(on=deck, name="Boss")
    sk.circle((0, 0), d=8)
    boss = part.extrude(sk.add(), 5.0, name="Boss")
    sk = part.sketch(on=boss.feature.end_cap(), name="Pin")
    sk.circle((0, 0), d=3)
    part.extrude(sk.add(), 2.0, mode="add", name="Pin")
    lo, hi = part.bbox()
    expect(abs(lo.z - 12.0) < 1e-6 and abs(hi.z - 19.0) < 1e-6,
           f"on the frame at deck_z, the pin on the boss's top: "
           f"{lo.z:.3f}..{hi.z:.3f}, not 12..19")
    app.set_variable(name="deck_z", expression="20", scope="part")
    lo, hi = part.bbox()
    expect(abs(lo.z - 20.0) < 1e-6 and abs(hi.z - 27.0) < 1e-6,
           f"a sketch on a frame follows the frame's variable: "
           f"{lo.z:.3f}..{hi.z:.3f} after deck_z went to 20, not 20..27")
    refused(lambda: part.sketch(on=part.get_frame("deck")),
            "a query result, not a Frame feature")


def main():
    check_angle_units()
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_sdk_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app_proc = launch_app(binary, env=env)
    try:
        app = sdk.App(Client.wait_for(app_proc.port))
        check_generated_surface(app)
        app.new()

        # ---- Plane arithmetic: the flange bug, retired ------------------
        # UV is measured FROM the plane origin. Round-tripping a world point
        # through uv/world is the identity; adding the origin twice (what the
        # gripper script did) is not.
        p = sdk.Plane((7.57, -0.21, -23.4), (0, 0, 1), (1, 0, 0))
        w = p.world(11.5, -6.0)
        expect(abs(w.x - (7.57 + 11.5)) < 1e-9 and abs(w.y - (-0.21 - 6.0))
               < 1e-9, f"world(u, v) = origin + u*U + v*V: {w}")
        back = p.uv(w)
        expect(abs(back[0] - 11.5) < 1e-9 and abs(back[1] + 6.0) < 1e-9,
               f"uv() is world()'s inverse: {back}")
        expect(abs(p.uv(p.origin)[0]) < 1e-12,
               "the plane origin is UV (0, 0) — not its own world position")
        # V = normal x U, spelled out rather than guessed.
        xz = sdk.Plane.xz()
        expect(abs(xz.v.z - 1.0) < 1e-9,
               f"the XZ helper's V is world +Z, the V-axis trap: {xz.v}")

        # ---- The piston -------------------------------------------------
        part = app.part("piston")
        body = build_piston(part)
        part.check()
        vol = part.volume()
        # A solid cylinder minus three grooves and the pin bore, to a few %.
        solid = math.pi * 43.0 ** 2 * 43.0
        expect(0.80 * solid < vol < solid,
               f"the piston is a cylinder less its cuts: {vol:.0f} of "
               f"{solid:.0f}")
        lo, hi = part.bbox()
        expect(abs((hi.x - lo.x) - 86.0) < 0.01,
               f"it is one bore across: {hi.x - lo.x:.3f}")
        expect(abs((hi.z - lo.z) - 43.0) < 0.01,
               f"and crown + skirt tall: {hi.z - lo.z:.3f}")
        # It reads short, and the source says so — physical lines of code,
        # comments and docstring excluded, wrapping counted against it.
        lines = [l for l in inspect.getsource(build_piston).splitlines()
                 if l.strip() and not l.strip().startswith("#")
                 and '"""' not in l]
        expect(len(lines) <= 20,
               f"a piston in twenty lines of SDK: {len(lines)}")

        # ---- A Param drives what it is passed to ------------------------
        check_params_drive(app)

        # ---- A bad blend refuses, and the session survives it ------------
        check_bad_blend_refuses(app)

        # ---- Faces by what made them --------------------------------------
        check_faces_by_maker(app)

        # ---- One part placed in two configurations and woods -------------
        check_place_configured(app)

        # ---- compare_geometry can be asked about shape alone -------------
        check_compare_aligns(app, tmp)

        # ---- References, not floats -------------------------------------
        bore = body.face(kind="cylinder",
                         where=lambda f: abs(f.radius - 11.0) < 0.01)
        expect(bore.diameter and abs(bore.diameter - 22.0) < 0.01,
               f"the pin bore knows its own diameter: {bore.diameter}")
        fr = bore.frame()
        expect(abs(abs(fr.z.dot(sdk.V(1, 0, 0))) - 1.0) < 1e-3,
               f"and its axis, from the app: {fr.z}")
        # A face that matches several is a refusal, never the first of them.
        refused(lambda: body.face(kind="plane"), "matched", "narrow it")
        refused(lambda: body.face(kind="torus"), "no face matched")

        # ---- A parametric edit moves everything that came from geometry --
        part.var("bore", 100)
        part.check()
        lo, hi = part.bbox()
        expect(abs((hi.x - lo.x) - 100.0) < 0.01,
               f"the bore variable drives the body: {hi.x - lo.x:.3f}")
        part.var("bore", 86)

        # ---- A bolt circle, and the frame that names it -----------------
        plate = app.part("plate")
        plate.var("pcd", 60).var("bolt_d", 6.6)
        plate.box("Top", width=100, depth=100, height=8, name="Plate")
        top = plate.body().face(kind="plane", normal=(0, 0, 1))
        hole, frame = plate.bolt_circle(top.plane(), n=6, pcd=60, d=6.6)
        plate.check()
        expect(frame is not None, "the bolt circle names its own centre")
        # On the pattern's axis. Z is the bores' mid-depth, because a
        # cylindrical face's frame origin is its centroid projected onto its
        # own axis — the app's answer, not a number this test assumed.
        expect(abs(frame.origin.x) < 0.01 and abs(frame.origin.y) < 0.01,
               f"centred on the pattern: {frame.origin}")
        bores = plate.body().faces(kind="cylinder")
        expect(len(bores) == 6, f"six holes were drilled: {len(bores)}")

        # ---- Angles name their unit (docs/scripting/sdk.md#angles-name-their-unit) -------------
        # An old spelling is refused naming the new one, before anything
        # reaches the app: `start_angle` was RADIANS, and read as degrees it
        # would be a plausible wrong part.
        refused(lambda: plate.bolt_circle(top.plane(), n=6, pcd=60, d=6.6,
                                          start_angle=0.5),
                "start_deg", "radians")
        refused(lambda: plate.pattern(hole, 4, circular="z", angle=90),
                "step_deg")
        refused(lambda: plate.revolve(None, angle=90), "angle_deg")
        refused(lambda: plate.revolve_profile([], angle=90), "angle_deg")
        refused(lambda: plate.frame("f", rotate=(0, 0, 30)), "rotate_deg")
        refused(lambda: plate.sketch(top.plane(), name="p").polygon(
            (0, 0), 6, r=5, rotate=0.5), "rotate_deg", "radians")

        flange = app.part("flange")
        flange.box("Top", width=100, depth=100, height=8, name="Flange")
        ftop = flange.body().face(kind="plane", normal=(0, 0, 1))
        flange.bolt_circle(ftop.plane(), n=4, pcd=60, d=6.6, start_deg=45)
        flange.check()
        want = (30 * math.cos(math.radians(45)), 30 * math.sin(math.radians(45)))
        centres = [e.center for e in flange.body().edges(kind="circle")]
        expect(any(abs(c.x - want[0]) < 0.01 and abs(c.y - want[1]) < 0.01
                   for c in centres),
               f"start_deg=45 puts a hole at 45 degrees: {centres}")

        # No step: the copies go evenly round a full turn, not all back onto
        # the original (the old 360 default).
        spokes = app.part("spokes")
        lug = spokes.cylinder("Top", d=6, height=5, at=(30, 0))
        spokes.pattern(lug, 4, circular="z")
        spokes.check()
        lo, hi = spokes.bbox()
        expect(abs(lo.x + 33) < 0.01 and abs(hi.y - 33) < 0.01,
               f"four lugs spread round the axis: {lo} .. {hi}")

        # A Param in `constrain` keeps its formula.
        link = app.part("link")
        link_len = link.param("link_len", 20)
        ls = link.sketch(sdk.Plane.xy(), name="Link").line((0, 0),
                                                           (15, 0)).add()
        (eid,) = [e["id"] for e in ls.entities() if e.get("type") == "line"]
        ls.constrain("distance", [[eid, 0], [eid, 1]], value=link_len)
        link.var("link_len", 32)
        line = next(e for e in ls.entities() if e["id"] == eid)
        length = math.dist(line["a"], line["b"])
        expect(abs(length - 32) < 1e-6,
               f"the constraint follows its variable: {length}")
        refused(lambda: ls.constrain("angle", [[eid, 0]], value=link_len),
                "value_deg")

        # ---- Projection: a hole taken from another body's hole ----------
        # No coordinate is read off the first plate and typed into the
        # second — the sketch projects the edges and the Hole takes their
        # centres.
        cover = app.part("cover")
        cover.box("Top", width=100, depth=100, height=4, name="Cover")
        d1 = cover.frame("origin", origin=(0, 0, 0))
        cover.app.add_feature(feature={
            "type": "Derived", "name": "Plate ghost",
            "source_part": plate.id, "reference": True})
        ghost_edges = [e for b in cover.bodies() if "ghost" in b.name.lower()
                       or b.created_by != cover.body(name="Cover").created_by
                       for e in b.edges(kind="circle")
                       if abs((e.radius or 0) - 3.3) < 0.01
                       and abs(e.midpoint.z - 8.0) < 0.01]
        expect(len(ghost_edges) == 6,
               f"six ghost hole rims to project: {len(ghost_edges)}")
        sk = cover.sketch(sdk.Plane.xy(4.0), name="From the plate")
        sk.project(ghost_edges)
        s = sk.add()
        centres = s.projected_centers()
        expect(len(centres) == 6,
               f"the projection gave six centres: {len(centres)}")
        cover.hole(s, d=6.6, through=True, name="Cover holes")
        cover.check()
        cover_bores = [f for f in cover.body(name="Cover").faces(kind="cylinder")]
        expect(len(cover_bores) == 6,
               f"and six holes were drilled through the cover: "
               f"{len(cover_bores)}")
        # They line up with the plate's, because neither number was typed.
        for c in centres:
            expect(abs(math.hypot(*c) - 30.0) < 0.01,
                   f"on the same 60 mm pitch circle: {c}")

        # ---- A batch is one transaction --------------------------------
        strip = app.part("strip")
        with strip.batch() as b:
            for i in range(8):
                sk = b.add_feature(feature={
                    "type": "Sketch", "name": f"S{i}",
                    "plane": sdk.Plane.xy().json(),
                    "entities": [{"type": "circle",
                                  "center": [10.0 * i, 0], "radius": 3.0}]})
                b.add_feature(feature={
                    "type": "Extrude", "name": f"Post {i}",
                    "sketch_id": sk, "distance": 5.0, "mode": "new_body"})
        expect(b.response["regenerations"] == 1,
               f"one regeneration for sixteen features: {b.response}")
        expect(len(strip.bodies()) == 8, "eight posts")
        strip.app.undo()
        expect(len(strip.bodies()) == 0, "and one undo takes them all back")

        # ---- The assembly check catches a floating part ----------------
        asm = app.assembly("rig")
        a = asm.place(plate, fixed=True)
        c = asm.place(cover, at=(0, 0, 40))
        refused(asm.check, "floating")
        # Seat the cover ON the plate: a frame taken from the plate's top
        # face, mated to the cover's own origin. Nothing here is a
        # coordinate — the seat is wherever the top face is.
        plate.activate()
        plate.frame("seat", of=plate.body().face(kind="plane",
                                                 normal=(0, 0, 1)))
        asm.activate()
        asm.fasten((a, "seat"), c, flipped=False)
        asm.check(clearances=False)
        check_verdict_contracts(app, asm)
        expect(abs(c.frame().origin.z - 8.0) < 0.01,
               f"the cover sits on the plate's 8 mm top face: "
               f"{c.frame().origin}")
        # And it FOLLOWS. A block and a lid, so the edit below moves the
        # seat without changing what the seat is.
        block = app.part("block")
        block.var("t", 10)
        block.box("Top", width=40, depth=40, height=10, name="Block")
        block.frame("top", of=block.body().face(kind="plane",
                                                normal=(0, 0, 1)))
        lid = app.part("lid")
        lid.box("Top", width=40, depth=40, height=3, name="Lid")
        rig2 = app.assembly("rig2")
        bo = rig2.place(block, fixed=True)
        lo = rig2.place(lid)
        # Overlapping solids and budget-limited sweeps must both fail.
        refused(rig2.check_clashes, "clash")
        refused(lambda: rig2.check_clashes(max_pairs=0), "TRUNCATED")
        refused(lambda: rig2.check_clashes(mode="mesh"), "exact")
        rig2.fasten((bo, "top"), lo, flipped=False)
        expect(not rig2.check_clashes()["clash"],
               "a lid seated on its block touches without interfering")
        expect(abs(lo.frame().origin.z - 10.0) < 0.01,
               f"the lid seats on the block: {lo.frame().origin}")
        extrude_distance(block, "Block", 25)
        rig2.activate()
        rig2.solve()
        expect(abs(lo.frame().origin.z - 25.0) < 0.01,
               f"and follows the face the frame was built from, with nothing "
               f"re-entered: {lo.frame().origin}")

        # The other half of that promise: when an edit means the named face
        # genuinely no longer exists, the frame REFUSES and the mate refuses
        # with it — it does not quietly leave the part at its last good pose.
        # Thickening the plate stops its bolt holes breaking the top surface,
        # so that face stops being the one the seat was built from.
        extrude_distance(plate, "Plate", 12)
        asm.activate()
        r = asm.solve()
        expect(r["errored_mates"] == 1,
               f"the mate refuses rather than reusing its last good pose: "
               f"{r}")
        err = next(m.get("error", "") for m in r["mates"])
        expect("seat" in err and "did not resolve" in err,
               f"and says which frame and why: {err}")
        # The assembly's own check surfaces the consequence too, rather than
        # reporting a tidy structure built on a mate that did not solve: the
        # cover is left where it was and the thicker plate now runs through
        # it.
        refused(asm.check, "clash")

        # ---- What the friction round added -------------------------------
        # arc(): the entity type always existed (slot emits two) but there was
        # no way to draw one, so every rounded corner in the air-cooled engine
        # is a slot or a polygon (generators/README.md#single-cylinder-and-air-cooled-engines).
        d = app.part("arcs")
        sk = d.sketch(sdk.Plane.xy(0), name="D")
        sk.arc((0, 0), 20.0, 0, 180)
        sk.line((-20, 0), (20, 0))
        d.extrude(sk.add(), 5.0, name="D")
        expect(abs(d.body().volume - math.pi * 400 / 2 * 5) < 1e-3,
               f"a half-disc prism is pi r^2 / 2 * h: {d.body().volume}")

        # extent(): how far a profile actually REACHES, before extruding it.
        # A slot reaches half its width past each centre — the thing that put
        # a crank web through a barrel.
        probe = d.sketch(sdk.Plane.xz(0), name="probe")
        probe.slot((0, 0), (0, 43), width=90)
        lo, hi = probe.extent()
        expect(abs(lo.y + 45.0) < 1e-6 and abs(hi.y - 88.0) < 1e-6,
               f"the slot spans -45..88, not 0..43: {lo.y}..{hi.y}")

        # extrude_shapes(): overlapping loops are refused in ONE sketch, so
        # this is one extrude each, added together.
        e = app.part("overlap")
        e.extrude_shapes(sdk.Plane.xy(0), [
            lambda s: s.circle((0, 0), d=40),
            lambda s: s.circle((30, 0), d=40),
        ], 10.0, name="Two bosses")
        expect(len(e.bodies()) == 1,
               f"the shapes fused into one body: {len(e.bodies())}")

        # islands(): the check that would have caught four bolt bosses hanging
        # off a deck flange with every other check green (F1).
        expect(len(e.islands(z=5)) == 1,
               f"overlapping bosses are ONE island: {e.islands(z=5)}")
        f = app.part("apart")
        f.extrude_shapes(sdk.Plane.xy(0), [
            lambda s: s.circle((0, 0), d=40),
            lambda s: s.circle((100, 0), d=40),
        ], 10.0, name="Two bosses")
        expect(len(f.islands(z=5)) == 2,
               f"and separated ones are TWO: {f.islands(z=5)}")
        e.check(solids=1)
        f.check(solids=2)
        refused(lambda: f.check(solids=1), "apart", "expected 1", "got 2")
        for invalid in (-1, True, 1.5, "1"):
            refused(lambda: f.check(solids=invalid), "non-negative integer")

        # material(): a part made of one thing, in one call.
        f.material("aluminum")
        expect(f.volume() > 0, "still measurable after a material")
        # ---- part(name) creates; opening another's is asked for ---------
        check_part_is_a_choice(app)

        # ---- A primitive stands on something, never at a world point ---
        check_primitives_take_on(app)

        # ---- A flipped plane extrudes the other way ---------------------
        check_flipped_planes(app)

        # ---- A sketch on a frame follows it ------------------------------
        check_sketch_on_frame(app)

        # ---- Every CAM op field is reachable through the generated SDK ---
        # Last: it starts a new project, and everything above works in one.
        check_cam_op_fields(app)
        print("sdk_smoke: ok")
    finally:
        app_proc.terminate()
        try:
            app_proc.wait(timeout=10)
        except Exception:
            app_proc.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
