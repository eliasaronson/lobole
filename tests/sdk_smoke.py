#!/usr/bin/env python3
"""The SDK (docs/agent-scale-plan.md, G1b), headless.

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
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, HERE)
import designer_sdk as sdk  # noqa: E402
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402


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
    src = open(os.path.join(HERE, "..", "tools",
                            "designer_sdk.py")).read()
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


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_sdk_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app_proc = launch_app(binary, env=env)
    try:
        app = sdk.App(Designer.wait_for(app_proc.port))
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
        plate.box(position=(-50, -50, 0), w=100, h=100, d=8, name="Plate")
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

        # ---- Projection: a hole taken from another body's hole ----------
        # No coordinate is read off the first plate and typed into the
        # second — the sketch projects the edges and the Hole takes their
        # centres.
        cover = app.part("cover")
        cover.box(position=(-50, -50, 0), w=100, h=100, d=4, name="Cover")
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
        block.box(position=(-20, -20, 0), w=40, h=40, d=10, name="Block")
        block.frame("top", of=block.body().face(kind="plane",
                                                normal=(0, 0, 1)))
        lid = app.part("lid")
        lid.box(position=(-20, -20, 0), w=40, h=40, d=3, name="Lid")
        rig2 = app.assembly("rig2")
        bo = rig2.place(block, fixed=True)
        lo = rig2.place(lid)
        rig2.fasten((bo, "top"), lo, flipped=False)
        expect(abs(lo.frame().origin.z - 10.0) < 0.01,
               f"the lid seats on the block: {lo.frame().origin}")
        block.activate()
        block.app.modify_feature(
            index=0, feature={"type": "Box", "name": "Block",
                              "position": [-20, -20, 0], "width": 40,
                              "height": 40, "depth": 25})
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
        plate.activate()
        plate.app.modify_feature(
            index=0, feature={"type": "Box", "name": "Plate",
                              "position": [-50, -50, 0], "width": 100,
                              "height": 100, "depth": 12})
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
        # is a slot or a polygon (docs/dogfood-friction-plan.md, F2).
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

        # material(): a part made of one thing, in one call.
        f.material("aluminum")
        expect(f.volume() > 0, "still measurable after a material")
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
