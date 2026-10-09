# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The BACK of an upright: its divider dado and its face dowels.

A second job per panel, and the same job for both uprights — the baffle and
the back wall carry an identical dado and an identical set of face dowels,
so this is one function called twice rather than two jobs written twice.

## Two setups, because the panel does not fit the machine

The face dowels sit at v +/-503, which is 1006 mm apart on a panel 1122
long, and the 6090 travels 900. No reach offset places one window over
both. So the panel is cut at one end, slid along the bed, and cut at the
other:

    top end     the divider dado (v 359..375) and the top dowels (v 503)
    bottom end  the bottom dowels (v -503)

Each setup's own features span 144 mm and a point respectively, so
`reach_offset="auto"` has an easy job once the ops are in — which is why it
is asked for last, as in cam.py.

## What is NOT here, and cannot be

`end_plus` and `end_minus` are the deep dowels, and they go into the panel's
16 mm END FACES — the edges it meets the side walls on. A 3-axis router with
the panel lying down cannot drill a vertical edge. Those are a dowel jig's
work, and BUILD.md says so.

That split is not an accident of this machine. A dowel joint is drilled
shallow into one part's FACE and deep into the other's EDGE (walls.py's
`_dowel_planes`), so for any pair of panels exactly one half is flat work.
The uprights get their faces drilled here; their edges are jig work; and the
side walls' side of the same joints is all face work.
"""
import cam
from designer_sdk import SdkError

# Which face of each upright is the inside, in the panel's own frame: the
# coordinate along its thickness and the way that face looks. The two are
# mirror images of each other — walls.py draws the baffle y 0..-t and the
# back wall the other way round — so this is the one thing that differs
# between the two jobs.
INSIDE = {"front_wall": (0.0, 1.0), "back_wall": (-1.0, -1.0)}

AXIS = 1  # both uprights are slabs across Y, as in cam.py


def _inside_face(part, p, panel, min_area=1000.0):
    """The panel's inner broad face — what gets laid upward here."""
    at_t, sign = INSIDE[panel]
    at = at_t * float(p["wall_thickness"])
    best = None
    for body in part.bodies():
        for f in body.faces(kind="plane"):
            n = f.normal
            if n is None or abs(n[AXIS] - sign) > 1e-3 or f.area < min_area:
                continue
            if abs(f.centroid[AXIS] - at) > 0.15:
                continue
            if best is None or f.area > best.area:
                best = f
    return best


def _dado_floor(part, p, panel):
    """The divider housing's floor: a planar face one `partition_wall_cutout`
    below the inside face, looking the same way it does."""
    at_t, sign = INSIDE[panel]
    at = (at_t * float(p["wall_thickness"])
          - sign * float(p["partition_wall_cutout"]))
    best = None
    for body in part.bodies():
        for f in body.faces(kind="plane"):
            n = f.normal
            if n is None or abs(n[AXIS] - sign) > 1e-3:
                continue
            if abs(f.centroid[AXIS] - at) > 0.15 or f.area < 500.0:
                continue
            if best is None or f.area > best.area:
                best = f
    return best


def _face_dowels(part, p, near_v, tol=2.0):
    """The face dowels near height `near_v`: bores of `plug_dia` drilled
    along the panel's THICKNESS.

    The axis is what separates them from the deep dowels in the edges, which
    are the same diameter and run across the panel instead.
    """
    r = float(p["plug_dia"]) / 2.0
    out = []
    for body in part.bodies():
        for f in body.faces(kind="cylinder"):
            if not f.radius or abs(f.radius - r) > 0.05:
                continue
            if abs(f.axis[AXIS]) < 0.99:       # an edge dowel, not a face one
                continue
            if abs(f.centroid[2] - near_v) > tol:
                continue
            out.append(f)
    return sorted(out, key=lambda f: f.centroid[0])


def _stock_top(app, setup_id):
    """The Z the stock's top face sits at in this setup, from the setup."""
    for s in app.list_cam_setups()["setups"]:
        if s["id"] == setup_id:
            # The resolved box is `stock.box`; `stock` itself is the request
            # (mode, size, margins). No box means the stock was refused.
            box = s["stock"].get("box")
            if not box:
                raise SdkError(f"setup {setup_id} has no stock: "
                               f"{s['stock'].get('refusal', 'no reason given')}")
            return float(box["max"][2])
    raise SdkError(f"setup {setup_id} has gone")


def _zero_on_the_cut_end(app, setup_id, label):
    """Put the machine's zero on the END THIS SETUP CUTS, and check it can.

    This is the whole of the reach problem, and it is a datum question
    rather than a bed-position one. The panel is 1122 mm and the 6090
    travels 900, so of the two ends exactly one is reachable from each
    origin — and which name that is depends on which way the panel was laid,
    because the two uprights are mirror images and their inside faces look
    opposite ways. So try both and keep the one that works.

    `reach_offset` is the wrong tool and was a long detour: it slides the bed
    under a FIXED origin, so with the wrong datum no value helps — measured,
    every offset either left the cutting 222 mm outside the travel or moved
    it out of the file entirely ("every cut of this operation lies beyond
    6090's travel, so none of it is in the file"). Both have to be checked,
    because the second silences the first by emitting nothing.
    """
    tried = []
    for origin in ("stock_min", "stock_max"):
        app.set_cam_setup(id=setup_id, origin_at=origin)
        app.set_cam_setup(id=setup_id, reach_offset="auto")
        says = app.cam_verify(setup=setup_id)["says"]
        paths = app.cam_toolpath(setup=setup_id)["operations"]
        empty = [o for o in paths if o.get("error") or not o.get("moves")]
        if not [m for m in says if "cannot go" in m] and not empty:
            return origin
        tried.append((origin, "outside the travel" if not empty
                      else "nothing in the file"))
    raise SdkError(f"{label}: neither end of the panel puts this setup's "
                   f"cutting inside the machine — {tried}. It wants "
                   f"splitting further, or a longer machine")


def build(app, p, panel):
    """One back-face job for one upright. Returns (job, [setup ids])."""
    part = app.part(panel, create=False)
    part.activate()
    job = app.machine_part(part=part.id, name=f"{panel} - face 2 (back)")
    # The JOB's id for this part, which is not the part document's: a job
    # holds placements and `set_job_placement` addresses one of those.
    in_job = [q for q in app.list_job_parts(job=job["id"])["parts"]
              if q.get("source") == part.id]
    if len(in_job) != 1:
        raise SdkError(f"{panel}: the job holds {len(in_job)} copies of it")
    job_part = in_job[0]["id"]

    t = float(p["wall_thickness"])
    inside = _inside_face(part, p, panel)
    if inside is None:
        raise SdkError(f"{panel}: no inner face to lay upward — has the "
                       f"panel's thickness axis changed?")
    dado = _dado_floor(part, p, panel)
    if dado is None:
        raise SdkError(f"{panel}: no divider dado at "
                       f"{float(p['partition_wall_cutout'])} mm deep; this "
                       f"job exists to cut it")

    v_top = float(p["carcass_half_h"]) - t / 2.0      # the top dowels' height
    setups = []
    # Each setup is ZEROED ON THE END IT CUTS, and that is the whole trick.
    # The two dowel groups are 1006 mm apart and the 6090 travels 900, so no
    # single origin reaches both: with `stock_max` for both, the far pair
    # lands 222 mm outside the Y travel and the program asks the controller
    # for somewhere it cannot go. Origin at the end being worked makes each
    # program local to that end — 152 mm of travel for one, 108 for the
    # other — which is also exactly what happens at the machine: clamp the
    # panel with the end you are cutting at the origin, touch off on that
    # corner, run that program, then turn the panel round and run the other.
    #
    # `reach_offset` is the wrong tool for this. It says where the bed is
    # under one fixed origin, so it can only slide a 900 window over 222 mm
    # of a 1122 panel; the reach is not the problem, the datum is.
    for label, cuts in (("Back - top end", ("dado", v_top)),
                        ("Back - bottom end", (-v_top,))):
        sid = app.add_cam_setup(name=f"{panel}: {label}", post="mach3")["id"]
        setups.append(sid)
        app.set_cam_setup(id=sid, machine=cam.MACHINE["name"], order="tool")
        # The stock IS the part — see cam.py: this panel is already cut to
        # its outline, and a margin here would read as leftover nothing
        # removes.
        app.set_cam_setup(id=sid, stock={"mode": "relative",
                                         "margin": [0, 0, 0],
                                         "bottom_margin": 0})
        for tool in cam.TOOLS:
            app.set_cam_setup(id=sid, tool=tool)
        app.set_cam_setup(id=sid, lay_face_up=inside.ref)

        # WHERE the stock's top is, asked rather than assumed. Laying the
        # INSIDE face up turns the panel the other way from cam.py's face 1,
        # and the stock comes out Z -16..0 instead of 0..16 — so a depth
        # written as `t` down to `t/2` cuts air above the part and every
        # operation comes back "produced no toolpath". Reading it back costs
        # one call and cannot be wrong; face 1's own `top = t` agrees with
        # this for its own orientation.
        top = _stock_top(app, sid)
        n = 0
        for cut in cuts:
            if cut == "dado":
                # A slot right across the panel, so it is a pocket on its own
                # floor rather than a profile: the divider slides in from
                # either end and there is no wall to follow.
                app.add_cam_op(
                    setup=sid, type="pocket", tool=1,
                    name=f"divider dado {float(p['partition_wall_cutout']):.1f}"
                         f" deep",
                    targets=[dado.ref], side="inside", use_outline=True,
                    top_z=top,
                    bottom_z=top - float(p["partition_wall_cutout"]),
                    stepdown=2.0, stepover=0.45)
                n += 1
                continue
            bores = _face_dowels(part, p, cut)
            if len(bores) != 2:
                raise SdkError(f"{panel}: {len(bores)} face dowels at "
                               f"v={cut:.0f}, wanted 2")
            for i, bore in enumerate(bores, 1):
                app.add_cam_op(
                    setup=sid, type="profile", tool=1,
                    name=f"dowel {i}/2 d{float(p['plug_dia']):.0f} "
                         f"x {t / 2:.0f} deep",
                    targets=[bore.ref], side="inside",
                    top_z=top, bottom_z=top - t / 2.0, stepdown=2.0)
                n += 1
        # WHERE THE BED IS, stated rather than inferred. `auto` resolves to
        # [0, 0] on these setups — it places the window over the cutting and
        # the cutting is already at the datum — and with the origin at an end
        # of an 1122 mm panel that leaves the far end 222 mm outside a 900 mm
        # Y travel, which the program then asks the controller for. So the
        # offset is the reach this setup actually needs: the distance from
        # its own datum to the furthest thing it cuts. Measured on the top
        # setup, anything from 152 mm up is clean and 0 is not; 152 is
        # exactly that distance.
        zeroed = _zero_on_the_cut_end(app, sid, f"{panel} / {label}")
        print(f"  {panel}: {label} — {n} operation(s), zeroed on "
              f"{zeroed.replace('stock_', '')}")
    return job, setups


def build_all(app, p, post_to=None):
    """Both uprights. The same job twice, which is the point.

    Verified and posted HERE, inside the loop, because a setup id is read
    against whichever job is active: posting them afterwards wrote the back
    wall's programs into the baffle's filenames, which the header gave away
    and nothing else would have. `build` leaves its own job active, so this
    is the one place where the pairing cannot come apart.

    `post_to(panel, which)` returns a path, or None to post nothing.
    """
    print("cam, back faces:")
    ok_all = True
    for panel in ("front_wall", "back_wall"):
        _job, sids = build(app, p, panel)
        for which, sid in zip(("top", "bottom"), sids):
            ok, _v = cam.verify(app, sid)
            ok_all = ok and ok_all
            if not ok:
                print(f"  !! {panel} back/{which} does not cut what it should")
            elif post_to is not None:
                cam.post(app, sid, post_to(panel, which))
    return ok_all
