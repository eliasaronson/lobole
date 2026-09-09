"""What happens to the quadcopter's parts: the loads they carry, and the
G-code that cuts them.

Two stages that both start from the same model `fpv_drone.py` builds.

`fem` puts static studies on the parts that carry a load — the arm, twice,
and the top plate — and reports what they do under it. Read the caveat in
`FEM_CAVEAT` before believing a safety factor: the solver is isotropic and
a carbon laminate is not.

`cam` makes one job per sheet thickness, nests what is cut from that sheet
into it, drills and profiles it, and posts G-code for a 600 x 400 router.
"""
import math
import os

from designer_sdk import SdkError

from fpv_drone_spec import *  # noqa: F401,F403


# --- what the parts have to carry -------------------------------------------

# One 2207 on 6S with a 5.1x4.3x3 swings about 1.5 kgf at full throttle
# (manufacturer's thrust table, the figure the 9:1 thrust-to-weight comes
# from). It is the load the arm sees in flight, and it is not the load the
# arm is designed by.
THRUST_N = 1.5 * 9.81

# What designs it is arriving. A quadcopter lands on one corner, and the
# deceleration is set by how far the structure gives — millimetres, so tens
# of g. 15 g through ONE arm is the conventional freestyle case and is what
# a 5 mm arm is chosen for.
LANDING_G = 15.0
AUW_KG = 0.654

FEM_CAVEAT = (
    "The solver is isotropic; a carbon plate is not. In-plane it is a fair "
    "proxy — the material is quasi-isotropic 3K/epoxy and these are in-plane "
    "loads — but a real arm fails by DELAMINATION at the root, and no "
    "isotropic model can see that. Read these as indicative, not qualifying: "
    "they say whether the section is in the right order of magnitude, not "
    "whether the part passes.")


def bore(part, at, radius=M3_CLEAR / 2, tol=0.6):
    """The cylindrical face of a bore at `at` (x, y), for a load to act on."""
    hits = [f for f in part.faces(kind="cylinder")
            if abs((f.radius or 0) - radius) < 0.06 and f.centroid is not None
            and abs(f.centroid[0] - at[0]) < tol
            and abs(f.centroid[1] - at[1]) < tol]
    if len(hits) != 1:
        raise SdkError(f"{part.name}: {len(hits)} bores of r{radius} at {at}")
    return hits[0].ref


def top_face(part):
    """The part's upward face, by area — what presses on it presses here."""
    faces = [f for f in part.faces(kind="plane", normal=(0, 0, 1))]
    if not faces:
        raise SdkError(f"{part.name}: no upward face")
    return max(faces, key=lambda f: f.area)


def study(app, part, name, fixed, forces, mesh_size):
    """One static study: what is held, what pushes, how finely."""
    part.activate()
    s = app.add_study(kind="static", name=name)
    app.set_study(study=s["id"], mesh_size=mesh_size)
    for face in fixed:
        app.add_load(study=s["id"], type="fixed", face=face)
    for face, newtons, direction in forces:
        app.add_load(study=s["id"], type="force", face=face,
                     magnitude=newtons, direction=list(direction))
    app.mesh_study(study=s["id"], _timeout=900)
    app.run_study(study=s["id"], _timeout=1800)
    out = app.study_results(study=s["id"])
    out["name"] = name
    return out


def fem(app):
    """The load cases worth asking about, and what they answer."""
    print("fem:")
    arm = app.part("Arm", create=False)
    top = app.part("Top plate", create=False)
    motor_bores = [bore(arm, (MOTOR_R + sx * MOTOR_BOLT_SQ / 2,
                              sy * MOTOR_BOLT_SQ / 2))
                   for sx in (1, -1) for sy in (1, -1)]
    arm_bolts = [bore(arm, (ARM_BOLT_IN, 0.0)), bore(arm, (ARM_BOLT_OUT, 0.0))]

    results = []
    # In flight: the motor pulls the arm up, the frame holds it down.
    results.append(study(
        app, arm, "Arm at full thrust", arm_bolts,
        [(f, THRUST_N / 4, (0, 0, 1)) for f in motor_bores], mesh_size=2.0))
    # Arriving: the ground stops the motor, the mass keeps going.
    landing = AUW_KG * 9.81 * LANDING_G
    results.append(study(
        app, arm, f"Arm on a {LANDING_G:g} g landing", arm_bolts,
        [(f, landing / 4, (0, 0, -1)) for f in motor_bores], mesh_size=2.0))
    # The pack presses on the deck it is strapped to; the standoffs hold it.
    deck = top_face(top)
    pack = BATT_MASS_G / 1000.0 * 9.81 * LANDING_G
    results.append(study(
        app, top, f"Top plate under the pack at {LANDING_G:g} g",
        [bore(top, diag(ARM_BOLT_OUT, k)) for k in range(4)],
        [(deck.ref, pack, (0, 0, -1))], mesh_size=2.5))

    bad = []
    for r in results:
        stress = r["max_von_mises_mpa"]["value"]
        where = r["max_von_mises_mpa"]["at"]
        move = r["max_displacement_mm"]["value"]
        # The app's own, off the material's yield — not a second opinion
        # computed here from a number typed twice.
        margin = r["factor_of_safety"]
        print(f"  {r['name']:<36} {r['elements']:>6} elem  {move:6.2f} mm  "
              f"{stress:7.1f} MPa  x{margin:5.1f}  at "
              f"({where[0]:.0f}, {where[1]:.0f}, {where[2]:.0f})")
        if margin < 2.0:
            bad.append(f"{r['name']}: {stress:.0f} MPa, a factor of "
                       f"{margin:.1f}")
    if bad:
        raise SdkError("STRUCTURE FAILED:\n  " + "\n  ".join(bad))
    print(f"  {FEM_CAVEAT}")
    return results


# --- and what cuts them -----------------------------------------------------

# A 600 x 400 hobby router, which is what a frame like this gets cut on.
# Four tools, and the reasons are the parts': carbon is DRILLED at these
# sizes, not interpolated (a 2 mm cutter would be in a 3.2 mm hole for
# minutes and blunt before it is out), and the narrowest slot on the frame is
# 2.2 mm — the one a 2 mm camera plate drops through — so nothing wider can
# cut it.
MACHINE = {
    "name": "Shop router 6040",
    "post": "grbl",
    "travel": {"min": [0.0, 0.0, -80.0], "max": [600.0, 400.0, 0.0]},
    "fit_arcs": True,
    "rapid_feed": 3000.0,
    "tools": [
        {"number": 1, "name": "3.175 mm diamond-cut", "diameter": 3.175,
         "tool_type": "flat", "flutes": 4, "feed_cut": 900.0,
         "feed_plunge": 200.0, "spindle_rpm": 18000.0,
         "chipload_min": 0.01, "chipload_max": 0.04},
        {"number": 2, "name": "3.2 mm drill", "diameter": 3.2,
         "tool_type": "drill", "flutes": 2, "feed_cut": 150.0,
         "feed_plunge": 100.0, "spindle_rpm": 9000.0},
        {"number": 3, "name": "2.2 mm drill", "diameter": 2.2,
         "tool_type": "drill", "flutes": 2, "feed_cut": 120.0,
         "feed_plunge": 80.0, "spindle_rpm": 12000.0},
        {"number": 4, "name": "2 mm diamond-cut", "diameter": 2.0,
         "tool_type": "flat", "flutes": 4, "feed_cut": 500.0,
         "feed_plunge": 150.0, "spindle_rpm": 20000.0,
         "chipload_min": 0.006, "chipload_max": 0.02},
    ],
}
BIG, SMALL, M3_DRILL, M2_DRILL = 1, 4, 2, 3

# (sheet, thickness, [(part, how many)]) — one job per thickness, because
# that is what a sheet is. WHERE each copy goes is nested from the parts'
# own extents rather than typed here: a spec change moves the geometry, and a
# typed position would not follow it.
SHEETS = [
    ("3 mm sheet", T_BOTTOM, [("Bottom plate", 1)]),
    ("2 mm sheet", T_MID, [("Mid plate", 1), ("Top plate", 1),
                           ("Camera plate", 2)]),
    ("5 mm sheet", T_ARM, [("Arm", 4)]),
]
STOCK = 320.0              # the square offcut this frame comes out of
EDGE = 12.0                # sheet edge to part: room for the clamps
GAP = 8.0                  # part to part: two cutter widths and the tabs
TABS = {"count": 4, "width": 4.0, "height": 0.6}


def slot_walls(rects, tool=SMALL):
    """A point ON each slot's wall, which is what a profile is picked by —
    the centre of an opening lies on none of them."""
    return [((cx, cy + h / 2.0), tool) for cx, cy, w, h in rects]


def openings(name):
    """Every opening cut inside this part's outline: a point on one of its
    walls, and the cutter it takes.

    Restated from the same spec constants the geometry was built from, not
    found by guessing which loops of a face are inside it. An opening a
    profile misses is a part that has to be finished by hand — and one it
    finds with too fat a cutter is a part cut somewhere it should not be, so
    the tool is named here beside the size that decides it.
    """
    y = CAM_MOUNT_GAP / 2.0 + T_SIDE / 2.0
    cx = (CAGE_TAB[0] + CAGE_TAB[1]) / 2.0
    tab = CAGE_TAB[1] - CAGE_TAB[0] + 2 * CAGE_TAB_RELIEF
    zip_tie = [(x, sy * 38.0, 2.5, 9.0) for x in (30.0, -30.0)
               for sy in (1, -1)]
    return {
        # Zip-tie slots for the motor leads, and the battery lead's strain
        # relief.
        "Bottom plate": slot_walls(zip_tie + [(-49.0, 0.0, 4.0, 14.0)]),
        # The stack stands up through the middle; the camera plates drop
        # through the two 2.2 mm slots, which is the narrowest cut on the
        # aircraft.
        "Mid plate": [((MID_OPENING / 2.0, 0.0), BIG)]
        + slot_walls([(cx, sy * y, tab, T_SIDE + 0.2) for sy in (1, -1)])
        + slot_walls([r for r in zip_tie if r[0] < 0]),
        # The camera notch is open to the front edge, so it is part of the
        # outline and not an opening at all. The strap slots are.
        "Top plate": slot_walls(
            [(sx * STRAP_AT[0], sy * STRAP_AT[1], STRAP_SLOT_L, STRAP_SLOT_W)
             for sx in (1, -1) for sy in (1, -1)]),
        # Clears the motor's shaft end and its circlip.
        "Arm": [((MOTOR_R, 0.0), BIG)],
        "Camera plate": [],
    }[name]


def wall_at(part, at, tol=1.5):
    """The wall of an opening at `at` — what a profile follows.

    Any wall of the opening will do: the operation follows the whole loop the
    pick lies on. A bore answers at its axis and a flat wall at its midpoint,
    which is what `openings` gives, so the match is tight on purpose: a loose
    one would find the part's own outline and cut a trench round it.
    """
    best, best_d = None, tol
    for f in part.faces():
        if f.centroid is None or (f.kind == "plane" and f.normal is not None
                                  and abs(f.normal[2]) > 0.1):
            continue
        d = math.hypot(f.centroid[0] - at[0], f.centroid[1] - at[1])
        if d < best_d:
            best, best_d = f, d
    if best is None:
        raise SdkError(f"{part.name}: no opening wall within {tol} mm of {at}")
    return best.ref


def bores(part, radius):
    """Every bore of that radius, left to right — one drill operation takes
    them all and cuts them in the order given."""
    found = [f for f in part.faces(kind="cylinder")
             if abs((f.radius or 0) - radius) < 0.06 and f.centroid is not None]
    found.sort(key=lambda f: (round(f.centroid[0], 2),
                              round(f.centroid[1], 2)))
    return [f.ref for f in found]


def flat_face(part):
    """The part's biggest flat face — a plate's own face, whichever way the
    part was modelled. Laying THAT toward the spindle is what makes a sheet
    part cuttable, and it is a no-op for one already modelled flat."""
    return max(part.faces(kind="plane"), key=lambda f: f.area).ref


def picks(app):
    """What each part needs cut, read off the PART tabs before any job
    exists: reading it later would switch the active tab away from the job.
    """
    out = {}
    for name in dict.fromkeys(n for _, _, w in SHEETS for n, _ in w):
        part = app.part(name, create=False)
        out[name] = {
            "flat": flat_face(part),
            "drills": [(M3_DRILL, bores(part, M3_CLEAR / 2)),
                       (M2_DRILL, bores(part, M2_CLEAR / 2))],
            "walls": [(wall_at(part, at), tool) for at, tool in openings(name)],
        }
    return out


def footprints(app, setup, placed):
    """Each part's box IN SETUP SPACE: where the material is once the part is
    laid flat.

    Read from the app one part at a time, off a stock that shrink-wraps what
    is enabled with no margin — that box IS the framed model's bounds. The
    rotation that lays a part flat is the app's (`lay_face_up`), and working
    the same rotation out again here to nest against is how two copies of a
    transform come to disagree (docs/cam/setup-frame.md).
    """
    app.set_cam_setup(id=setup, stock={"mode": "relative",
                                       "margin": [0.0, 0.0, 0.0],
                                       "bottom_margin": 0.0})
    boxes = {}
    for _, job_part in placed:
        for _, other in placed:
            app.set_job_placement(setup=setup, part=other,
                                  enabled=other == job_part)
        s = next(s for s in app.list_cam_setups()["setups"] if s["id"] == setup)
        boxes[job_part] = (s["stock"]["min"], s["stock"]["max"])
    for _, job_part in placed:
        app.set_job_placement(setup=setup, part=job_part, enabled=True)
    return boxes


def nest(placed, boxes):
    """Lay the copies out in rows on the sheet, tallest row first.

    Shelf packing, which is what a person does with a jigsaw of rectangles
    and is enough for five parts: a nesting library would fill a sheet that
    is half empty either way.
    """
    order = sorted(placed, key=lambda p: -(boxes[p[1]][1][1]
                                           - boxes[p[1]][0][1]))
    out, x, y, row = [], EDGE, EDGE, 0.0
    for name, job_part in order:
        lo, hi = boxes[job_part]
        w, h = hi[0] - lo[0], hi[1] - lo[1]
        if x + w > STOCK - EDGE:
            x, y, row = EDGE, y + row + GAP, 0.0
        if y + h > STOCK - EDGE:
            raise SdkError(f"{name} does not fit on a {STOCK:g} mm sheet")
        # `pos` translates the part's own coordinates, so it is where the
        # material starts that has to land on the sheet, not the origin.
        out.append((name, job_part, (x - lo[0], y - lo[1])))
        x, row = x + w + GAP, max(row, h)
    return out


def cut_ops(app, setup, sizes, placed):
    """Drill it, open it up, then cut it free — in that order, because a part
    cut free first moves.

    Grouped by cutter, not by part: one operation takes every target it is
    given, so the drilling of four identical arms is one drill. The order
    matters twice over — `split_output` writes one program per tool in the
    order the tools are first WANTED, so the outline must be the last thing
    the big cutter is asked for, or the parts are free before their slots are
    cut.
    """
    for tool, radius in ((M3_DRILL, M3_CLEAR / 2), (M2_DRILL, M2_CLEAR / 2)):
        targets = [dict(ref, part=job_part)
                   for name, job_part, _ in placed
                   for t, refs in sizes[name]["drills"] if t == tool
                   for ref in refs]
        if targets:
            app.add_cam_op(setup=setup, type="drill", tool=tool,
                           targets=targets)
    for want in (SMALL, BIG):
        for name, job_part, _ in placed:
            for ref, tool in sizes[name]["walls"]:
                if tool == want:
                    app.add_cam_op(setup=setup, type="profile", tool=tool,
                                   side="inside",
                                   targets=[dict(ref, part=job_part)])
    # No target at all: a profile then follows every part's own outline, which
    # is what cutting them out means. On tabs, so nothing moves when it is cut
    # free, and off a tangent arc so no dwell mark is left in the edge.
    app.add_cam_op(setup=setup, type="profile", tool=BIG, side="outside",
                   tabs=TABS, lead_arcs=True)


def cam(app, out):
    """One job per sheet: nest it, cut it, post it, and check the G-code back
    against the model."""
    print("cam:")
    app.save_machine(machine=MACHINE)
    sizes = picks(app)
    made = []
    for sheet, thickness, wanted in SHEETS:
        copies = [name for name, qty in wanted for _ in range(qty)]
        job = app.machine_part(part=copies[0], name=sheet)
        placed = [(copies[0], job["part"]["id"])]
        placed += [(name, app.add_job_part(part=name)["id"])
                   for name in copies[1:]]

        setup = app.add_cam_setup(name=sheet, machine=MACHINE["name"])["id"]
        app.set_cam_setup(id=setup, origin_at="stock_min", split_output=True)
        for name, job_part in placed:
            app.set_job_placement(setup=setup, part=job_part,
                                  lay_face_up=sizes[name]["flat"])
        laid = nest(placed, footprints(app, setup, placed))
        for _, job_part, pos in laid:
            app.set_job_placement(setup=setup, part=job_part, pos=list(pos))
        # Only now is the workpiece the sheet in the room rather than a
        # shrink-wrap of the parts on it.
        app.set_cam_setup(id=setup, stock={"mode": "fixed",
                                           "size": [STOCK, STOCK, thickness]})
        for s in app.check_layout(setup=setup)["setups"]:
            if s.get("refusal"):
                raise SdkError(f"{sheet}: {s['refusal']}")
            for pl in s["placements"]:
                if pl["fit"] != "inside" or pl.get("overlaps"):
                    raise SdkError(f"{sheet}: a part is {pl['fit']}"
                                   + (" and overlaps another"
                                      if pl.get("overlaps") else ""))

        cut_ops(app, setup, sizes, laid)
        paths = app.cam_toolpath(setup=setup, _timeout=1800)
        bad = [f"{op.get('name')}: {op['error']}"
               for op in paths.get("operations", []) if op.get("error")]
        if bad or paths.get("warnings") or paths.get("skipped"):
            raise SdkError(f"{sheet}: " + repr(bad or paths.get("warnings")
                                               or paths.get("skipped")))
        posted = app.cam_post(
            setup=setup, _timeout=1800,
            path=os.path.join(out, sheet.replace(" ", "-") + ".nc"))
        verdict = app.cam_verify(setup=setup, _timeout=3600)
        # The clock the FILE states — every cutting feed is in it, and no
        # machine states its rapid rate, so this is time cutting and not an
        # estimate of the job.
        run = app.call("cam_simulate", setup=setup, at="end", resolution=0.4,
                       _timeout=3600)
        app.call("cam_simulate", close=True, _timeout=900)
        cut = sum(op.get("cut_length_mm", 0.0)
                  for op in paths.get("operations", []))
        print(f"  {sheet:<11} {len(placed)} parts  "
              f"{len(paths.get('operations', [])):>2} ops  {cut / 1000:5.1f} m "
              f"of cutting in {run.get('cutting_minutes', 0):.0f} min  "
              f"{posted.get('lines', 0):>5} lines  gouge "
              f"{verdict.get('max_gouge_mm', 0):.3f} mm  "
              f"{verdict.get('leftover_regions', 0):>2} left standing "
              f"(tabs, slugs and offcut — see the notes)")
        if not verdict.get("ok"):
            raise SdkError(f"{sheet}: {verdict.get('says')}")
        made.append((sheet, posted, verdict, run))
    return made
