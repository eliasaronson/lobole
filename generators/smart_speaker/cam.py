"""The baffle's job on the 6090: what it takes to cut the front panel.

One setup, baffle up, and fourteen operations — two flange recesses, two
through openings and ten screw holes. Ten of the fourteen are one hole each,
so they are written from `w170s_bolt_count` and `alpair_bolt_count` in a loop
rather than authored one at a time; changing a bolt count changes the program.

## Targets are picked by GEOMETRY

Every operation names a face found by what it IS — the planar annulus at the
rebate depth, the cylinder of the opening, the cylinder of a bolt hole — and
never by an element string like `10/cap1;M10`. Those strings are valid only
for the feature order that produced them, and this generator's feature order
is not the ported document's. A job that survives a rebuild has to ask the
model, not remember an answer.

Vertical cylinders are CAM walls, so a round opening is a `profile` on its own
bore and needs no onion-skin twin of the part.

## No CNC configuration

The ported job carried one, suppressing the veneer, the side plug holes and a
chamfer so the machining shape was the panel alone. The coverings are their
own parts here (veneer.py) and there is no chamfer, so the panel already IS
the machining shape. That configuration was a workaround for a part that held
two things at once.
"""
from designer_sdk import SdkError

# The machine, as the ported job embedded it. Saved to the store so a fresh
# checkout can cut: a setup NAMES a machine and embeds a copy, and naming one
# that is not there is a refusal at the worst moment.
MACHINE = {
    "name": "6090",
    "post": "mach3",
    "rapid_feed": 1500.0,
    "has_rotary_axis": True,
    "travel": {"min": [0.0, 0.0, 0.0], "max": [600.0, 900.0, 150.0]},
}

# Two cutters. The ER11 collet tops out at 7 mm, so the openings and recesses
# go with a 6 mm flat and the screw holes with a 3 mm — one tool change in the
# program.
# Speeds and feeds AS TEST CUT, read back out of the ported job
# (~/Documents/smart_speaker.dsgn/jobs/). These were 12000 rpm on both tools
# here, which is half the chip load the shop settled on:
#
#     tool          as generated          as cut       mm/tooth
#     6 mm 4F   1200 @ 12000 rpm   1200 @  6000        0.025 -> 0.050
#     3 mm 2F    700 @ 12000 rpm   1100 @  9000        0.029 -> 0.061
#
# Both were under the band the app wants for sheet goods and the 3 mm now
# sits inside it. A cutter run at half the chip load it wants does not fail
# loudly — it rubs, heats and burns — so the only authority worth having
# here is a cut that happened.
TOOLS = [
    {"number": 1, "name": "6 mm flat - recesses and openings",
     "diameter": 6.0, "tool_type": "flat", "flutes": 4,
     "feed_cut": 1200.0, "feed_plunge": 400.0, "spindle_rpm": 6000.0},
    {"number": 3, "name": "3 mm flat - screw holes",
     "diameter": 3.0, "tool_type": "flat", "flutes": 2,
     "feed_cut": 1100.0, "feed_plunge": 300.0, "spindle_rpm": 9000.0},
]

THROUGH_OVERRUN = 0.5  # mm past the far face, so a through cut is through
TAB = {"count": 4, "height": 3.0, "width": 8.0}


# Picks are made in the PART's own frame, where the baffle faces -Y — it is
# sketched upright and the assembly stands it up. The setup lays it face-up
# for cutting, but a target is a {feature_id, element} reference and CAM
# resolves it through that placement, so nothing here needs the machine's Z.
AXIS, OUT = 1, -1.0


def _planar(part, at, tol=0.15, min_area=100.0):
    """The planar face pointing out of the baffle, `at` along its axis."""
    best = None
    for body in part.bodies():
        for f in body.faces(kind="plane"):
            n = f.normal
            if n is None or abs(n[AXIS] - OUT) > 1e-3 or f.area < min_area:
                continue
            if abs(f.centroid[AXIS] - at) > tol:
                continue
            if best is None or f.area > best.area:
                best = f
    return best


def _bores(part, diameter, tol=0.05):
    """Every cylinder of this diameter, in a stable order (by angle about the
    baffle's axis) so operation N is the same hole on every run."""
    import math
    u, v = [i for i in range(3) if i != AXIS]
    out = []
    for body in part.bodies():
        for f in body.faces(kind="cylinder"):
            if f.radius and abs(f.radius * 2 - float(diameter)) < tol:
                out.append(f)
    return sorted(out, key=lambda f: math.atan2(f.centroid[v], f.centroid[u]))


def build(app, p, part_name="front_wall"):
    """The job, its setup and its operations. Returns the setup id."""
    print("cam:")
    app.save_machine(machine=MACHINE)

    baffle = app.part(part_name)
    baffle.activate()
    job = app.machine_part(part=baffle.id, name=f"{part_name} - face 1")

    setup = app.add_cam_setup(name="Front - baffle up", post="mach3")
    sid = setup["id"]
    # Ordered by TOOL, not as authored. The operations are written driver by
    # driver, which reads well and alternates the 6 mm and the 3 mm four
    # times; at the machine each change is a stop, a collet and a re-zero.
    app.set_cam_setup(id=sid, machine=MACHINE["name"], origin_at="stock_max",
                      order="tool")
    # The stock IS the part: this panel arrives already cut to its outline
    # from the sheet, and this job only puts the driver features in it. The
    # default relative stock carries a 2 mm margin and a millimetre in Z,
    # and since nothing here cuts an outline that rim is material the
    # toolpath never touches and the part does not want — 3,878 mm2 of
    # "leftover" against the 150 mm2 the ported job reports, all of it an
    # artefact of stock nobody has. Zero margin makes the check mean
    # something: it then matches the job that was test cut, 150.3 against
    # 150.4 mm2, to within a sampling cell.
    app.set_cam_setup(id=sid, stock={"mode": "relative", "margin": [0, 0, 0],
                                     "bottom_margin": 0})
    for tool in TOOLS:
        app.set_cam_setup(id=sid, tool=tool)

    # Lay the panel on its back, baffle up. The part is sketched upright and
    # the assembly stands it up; a setup that does not turn it over is asked
    # to cut a face pointing at the wall, and answers "this setup cuts nothing
    # yet". This is the out-of-plane half of fixturing — the in-plane half
    # (where on the bed, and spun how far) is set_job_placement.
    t = float(p["wall_thickness"])
    face_up = _planar(baffle, -t, min_area=1000.0)
    if face_up is None:
        raise SdkError("no outer face on the baffle to lay upward")
    app.set_cam_setup(id=sid, lay_face_up=face_up.ref)

    # Laid that way, the stock top is one wall thickness above the job's
    # floor, which is where every operation starts from.
    top = t
    n = 0

    for label, flange, depth, cutout, bolt, count in (
            ("W170S", p["w170s_flange_dia"], p["w170s_flange_depth"],
             p["w170s_cutout_dia"], p["m4_t_nutt_dia"],
             int(float(p["w170s_bolt_count"]))),
            ("Alpair", p["alpair_flange_dia"], p["alpair_flange_depth"],
             p["alpair_cutout_dia"], p["m3_t_nutt_dia"],
             int(float(p["alpair_bolt_count"])))):
        sunk = float(depth) - float(p["faner_width"])
        recess = top - sunk

        # The flange recess: a pocket on its own floor, which is the planar
        # face left at that depth — at -t + sunk in the part's own frame.
        floor = _planar(baffle, -t + sunk)
        if floor is None:
            raise SdkError(f"{label}: no rebate floor at z={recess:.2f} — the "
                           f"baffle's flange recess is not where CAM expects")
        app.add_cam_op(setup=sid, type="pocket", tool=1,
                       name=f"{label} flange recess d{float(flange):.1f} "
                            f"x {top - recess:.1f}",
                       targets=[floor.ref], side="inside", use_outline=True,
                       top_z=top, bottom_z=recess, stepdown=3.0, stepover=0.45)
        n += 1

        # The opening, all the way through, on tabs so the slug does not lift.
        wall = _bores(baffle, cutout)
        if not wall:
            raise SdkError(f"{label}: no d{float(cutout):.1f} opening to cut")
        app.add_cam_op(setup=sid, type="profile", tool=1,
                       name=f"{label} opening d{float(cutout):.0f} through",
                       targets=[wall[0].ref], side="inside", lead_arcs=True,
                       top_z=top, bottom_z=-THROUGH_OVERRUN, stepdown=2.5,
                       tabs=TAB)
        n += 1

        # The screw holes, one operation each, from the RECESS floor down —
        # not from the stock top, or the cutter cuts air through the rebate
        # it has already made.
        holes = _bores(baffle, bolt)
        if len(holes) != count:
            raise SdkError(f"{label}: {len(holes)} holes of d{float(bolt)}, "
                           f"expected {count}")
        for i, hole in enumerate(holes, 1):
            app.add_cam_op(setup=sid, type="profile", tool=3,
                           name=f"{label} screw hole {i}/{count} "
                                f"d{float(bolt):.1f}",
                           targets=[hole.ref], side="inside",
                           top_z=recess, bottom_z=-THROUGH_OVERRUN,
                           stepdown=1.5)
            n += 1

    # AFTER the operations: the reach window is placed over what the setup
    # cuts, and asked for first it refuses — "this setup cuts nothing yet, so
    # there is nowhere to put the window". The panel is longer than the
    # machine's 900 mm of travel, so the plinth end hangs off the bed and
    # this is what decides where.
    app.set_cam_setup(id=sid, reach_offset="auto")

    print(f"  {n} operations on the {MACHINE['name']}, "
          f"{len(TOOLS)} tools, targets picked by geometry")
    return job, sid


def verify(app, sid):
    """Simulate the POSTED program against the model and report.

    `complete: false` is CORRECT here — the openings are cut on tabs and the
    slugs are still attached, which is the point of them.
    """
    paths = app.cam_toolpath(setup=sid)
    empty = [o for o in paths.get("operations", [])
             if not o.get("moves") or o.get("error")]
    for o in empty:
        print(f"    !! {o.get('name')}: {o.get('error') or 'no moves'}")
    v = app.cam_verify(setup=sid)
    print(f"  verify: gouge {v.get('max_gouge_mm', 0):.3f} mm, "
          f"leftover {v.get('max_leftover_mm', 0):.3f} mm, "
          f"rapids through stock: {v.get('rapids_through_stock')}")
    # A GOUGE is the failure — material taken that the model keeps. Leftover
    # is not: the openings are cut on tabs, so their slugs are still attached
    # and stand the full thickness of the panel. That is the tabs working.
    ok = (not empty and v.get("max_gouge_mm", 1.0) < 0.01
          and not v.get("rapids_through_stock"))
    return ok, v


def post(app, sid, path):
    """Write the G-code. This is the artefact the whole job exists for."""
    r = app.cam_post(setup=sid, path=str(path))
    print(f"  posted {path} — {r.get('lines')} lines"
          + (f", SKIPPED {r.get('skipped')}" if r.get("skipped") else ""))
    return r
