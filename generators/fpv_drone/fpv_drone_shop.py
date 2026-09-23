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
import csv
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


def _at(p):
    """A point, as one CSV cell."""
    return f"{p[0]:.1f} {p[1]:.1f} {p[2]:.1f}"


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
    """One static study: what is held, what pushes, how finely.

    A study of this name is REPLACED, not added beside: the stage is saved
    with the project now, so a second run would otherwise leave two of
    everything and no way to tell which answer belonged to which run.
    """
    part.activate()
    for old in app.call("list_studies")["studies"]:
        if old["name"] == name:
            app.call("remove_study", study=old["id"])
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
    out["study"] = s["id"]
    out["part"] = part
    return out


def plot(app, result, out):
    """A picture of the field, saved beside the numbers.

    Worth more here than in most places, because the SOLVE is not persisted:
    reopened, the study carries its load cases and no answers. A png and a
    row in fem.csv are what survive the run.
    """
    result["part"].activate()
    app.call("enter_simulation", study=result["study"])
    app.call("set_result_display", study=result["study"], field="von_mises",
             deform_scale=1.0)
    app.call("view", name="iso")
    path = os.path.join(out, "fem-" + result["name"].lower()
                        .replace(" ", "-") + ".png")
    app.call("screenshot", path=path, width=1200, height=900)
    app.call("exit_simulation")
    return path


def fem(app, out=None):
    """The load cases worth asking about, and what they answer.

    The numbers go to `fem.csv` beside the model as well as to the terminal,
    because a solved FEM result is DERIVED and the project does not keep one
    (only a topology study's density is written): reopened, these studies
    carry their load cases and report `has_results: false`. That is the right
    call — a stress field that outlives the geometry it was computed from is
    a trap — but it means the answers need somewhere to live. Re-running is
    25 s.
    """
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
    if out is not None:
        os.makedirs(out, exist_ok=True)
        for r in results:
            plot(app, r, out)
        with open(os.path.join(out, "fem.csv"), "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["study", "elements", "nodes", "max_displacement_mm",
                        "displacement_at", "max_von_mises_mpa", "stress_at",
                        "factor_of_safety"])
            for r in results:
                move, stress = r["max_displacement_mm"], r["max_von_mises_mpa"]
                w.writerow([r["name"], r["elements"], r["nodes"],
                            f"{move['value']:.3f}", _at(move["at"]),
                            f"{stress['value']:.1f}", _at(stress["at"]),
                            f"{r['factor_of_safety']:.1f}"])
            w.writerow(["CAVEAT", FEM_CAVEAT])
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
    part was modelled.

    It is two answers at once: laying THAT toward the spindle is what makes a
    sheet part cuttable (and is a no-op for one already modelled flat), and
    its openings are the ones to cut out of the part.
    """
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
        }
    return out


def footprints(app, setup, placed):
    """Each part's box IN SETUP SPACE: where the material is once the part is
    laid flat.

    One call, because `check_layout` measures exactly this to answer whether
    a part is on the sheet. The rotation that lays a part flat is the app's
    (`lay_face_up`), and working the same rotation out again here to nest
    against is how two copies of a transform come to disagree
    (docs/cam/README.md#invariants).
    """
    boxes = {}
    for s in app.check_layout(setup=setup)["setups"]:
        for pl in s["placements"]:
            boxes[pl["part"]] = (pl["box"]["min"], pl["box"]["max"])
    missing = [name for name, part in placed if part not in boxes]
    if missing:
        raise SdkError(f"no box for {missing} — has it been placed?")
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
    # Every opening in the part's own face, in one operation each: the app
    # follows all of them and names any it cannot cut, so the shapes and
    # positions of the slots stay in the model instead of being restated here
    # where a spec change could not reach them. The SMALL cutter, because one
    # operation is one tool and the narrowest cut on the aircraft is the
    # 2.2 mm slot a camera plate drops through. `drilled_bores` is where the
    # drills stop and the cutter starts: every ROUND opening up to a clearance
    # hole was drilled two operations ago, and orbiting a 2 mm cutter in a
    # hole that is already gone is minutes of cutting air. It cannot be a
    # width — the milled slots are NARROWER than the drilled holes.
    for name, job_part, _ in placed:
        app.add_cam_op(setup=setup, type="profile", tool=SMALL, side="inside",
                       drilled_bores=max(M3_CLEAR, M2_CLEAR),
                       targets=[dict(sizes[name]["flat"], part=job_part)])
    # No target at all: a profile then follows every part's own outline, which
    # is what cutting them out means. On tabs, so nothing moves when it is cut
    # free, and off a tangent arc so no dwell mark is left in the edge.
    app.add_cam_op(setup=setup, type="profile", tool=BIG, side="outside",
                   tabs=TABS, lead_arcs=True)


def cam(app, out):
    """One job per sheet: nest it, cut it, post it, and check the G-code back
    against the model."""
    print("cam:")
    # Into the machine STORE (~/.config/designer/machines), because a setup
    # names one from there and then embeds a copy — so the job file opens on
    # a machine that has never heard of this router, and reports
    # `machine_match: missing` rather than pretending. This is the one thing
    # here that writes outside the project.
    app.save_machine(machine=MACHINE)
    sizes = picks(app)
    made = []
    for sheet, thickness, wanted in SHEETS:
        copies = [name for name, qty in wanted for _ in range(qty)]
        # Same reason as the studies: this is saved with the project, so a
        # second run must replace the sheet rather than add another beside it.
        for tab in app.call("list_parts")["parts"]:
            if tab["name"] == sheet and tab["type"] == "job":
                app.call("delete_part", part=tab["id"])
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
            if s.get("refusal") or s.get("warnings"):
                raise SdkError(f"{sheet}: {s.get('refusal') or s['warnings']}")
            for pl in s["placements"]:
                if pl["fit"] != "inside" or pl.get("overlaps"):
                    raise SdkError(f"{sheet}: a part is {pl['fit']}"
                                   + (" and overlaps another"
                                      if pl.get("overlaps") else ""))

        cut_ops(app, setup, sizes, laid)
        paths = app.cam_toolpath(setup=setup, _timeout=1800)
        # A part with nothing to mill — the camera plates, whose only
        # openings are the two drilled M2 holes — is told so by the app
        # rather than known in advance here, and its openings operation goes.
        # Every OTHER error is still an error.
        empty = [op["operation"] for op in paths.get("operations", [])
                 if "nothing to cut in that face" in (op.get("error") or "")]
        for op_id in empty:
            app.remove_cam_op(id=op_id)
        if empty:
            paths = app.cam_toolpath(setup=setup, _timeout=1800)
        bad = [f"{op.get('name')}: {op['error']}"
               for op in paths.get("operations", []) if op.get("error")]
        if bad or paths.get("warnings") or paths.get("skipped"):
            raise SdkError(f"{sheet}: " + repr(bad or paths.get("warnings")
                                               or paths.get("skipped")))
        posted = app.cam_post(
            setup=setup, _timeout=1800,
            # No spaces: these go on an SD card and through a shell.
            path=os.path.join(out, sheet.replace(" mm sheet", "mm")
                              .replace(" ", "-") + ".nc"))
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
