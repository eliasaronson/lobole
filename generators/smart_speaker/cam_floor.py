# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The bottom wall: the port's hole and its roundover, and every bolt hole.

One setup, UNDERSIDE UP, because that is the face the port hole's
`port_hole_radius` roundover is on (walls.bottom_wall); every other feature
here goes through, so it does not mind which way up it is cut. The floor's
dowels are all in its four edges, which is dowel-jig work, as for the top
wall (cam_back.py says why).

## The roundover

A 4 mm ball nose — the largest the owner has — walks the R11 arc in passes
(`edge_round`, which reads the radius off the model's fillet). It is its own
program, T2, run after the 6 mm has cut the hole. The ball's far side is at
most one ball diameter from the wall, inside the 6 mm kerf, so the slug still
on its tabs is never in its way.

Its speeds and feeds have NOT been test cut, unlike cam.py's two: a 2-flute
4 mm in MDF at 0.035 mm a tooth. Check them on scrap.
"""
import cam
import cam_back
from lobole.sdk import SdkError

BALL = {"number": 2, "name": "4 mm ball nose - port roundover",
        "diameter": 4.0, "tool_type": "ball", "flutes": 2,
        "feed_cut": 840.0, "feed_plunge": 300.0, "spindle_rpm": 12000.0}

UNDER = -1.0  # the floor is built z = 0..t; its underside looks -Z


def _underside(part):
    best = None
    for body in part.bodies():
        for f in body.faces(kind="plane"):
            n = f.normal
            if n is None or abs(n[2] - UNDER) > 1e-3 or abs(f.centroid[2]) > 0.15:
                continue
            if best is None or f.area > best.area:
                best = f
    return best


def _vertical_bores(part, diameter, tol=0.05):
    """Cylinders of this diameter running through the floor (axis Z) — not
    the dowels, which run into its edges."""
    out = []
    for body in part.bodies():
        for f in body.faces(kind="cylinder"):
            if not f.radius or abs(f.radius * 2 - float(diameter)) > tol:
                continue
            if abs(f.axis[2]) < 0.99:
                continue
            out.append(f)
    return sorted(out, key=lambda f: (f.centroid[1], f.centroid[0]))


def build(app, p, panel="bottom_wall"):
    """The job and its one setup. Returns (job, setup id)."""
    print("cam, bottom wall:")
    part = app.part(panel, create=False)
    part.activate()
    job = app.machine_part(part=part.id, name=f"{panel} - underside")

    under = _underside(part)
    if under is None:
        raise SdkError(f"{panel}: no underside to lay upward")
    hole_d = float(p["port_spigot_od"]) + float(p["port_floor_clearance"])
    port = _vertical_bores(part, hole_d)
    if len(port) != 1:
        raise SdkError(f"{panel}: {len(port)} port holes of d{hole_d}")

    holes = []
    for label, dia, want in (
            ("port T-nut", p["m4_t_nutt_dia"],
             int(float(p["outer_port_bolt_count"]))),
            ("amp bolt", p["amp_bolt_dia"], 4),
            ("PSU bolt", p["case_bolt_dia"], 2)):
        found = _vertical_bores(part, dia)
        if len(found) != want:
            raise SdkError(f"{panel}: {len(found)} {label} holes of "
                           f"d{float(dia)}, expected {want}")
        holes += [(label, float(dia), i, want, f)
                  for i, f in enumerate(found, 1)]

    sid = app.add_cam_setup(name=f"{panel}: underside up", post="mach3")["id"]
    app.set_cam_setup(id=sid, machine=cam.MACHINE["name"],
                      origin_at="stock_max", order="tool", split_output=True)
    app.set_cam_setup(id=sid, stock={"mode": "relative", "margin": [0, 0, 0],
                                     "bottom_margin": 0})
    for tool in cam.TOOLS + [BALL]:
        app.set_cam_setup(id=sid, tool=tool)
    app.set_cam_setup(id=sid, lay_face_up=under.ref)
    top = cam_back._stock_top(app, sid)
    bottom = top - float(p["wall_thickness"]) - cam.THROUGH_OVERRUN

    app.add_cam_op(setup=sid, type="profile", tool=1,
                   name=f"port hole d{hole_d:.0f} through",
                   targets=[port[0].ref], side="inside",
                   top_z=top, bottom_z=bottom, stepdown=2.5, tabs=cam.TAB)
    # No edge_size: the radius is the model's fillet, read off the part.
    app.add_cam_op(setup=sid, type="edge_round", tool=BALL["number"],
                   name=f"port hole R{float(p['port_hole_radius']):.0f} "
                        f"roundover", targets=[port[0].ref])
    # The bolt holes are BORED (`drill` with a cutter smaller than the
    # hole: a helix at the ramp angle, and rings from the centre out when
    # the hole is wider than two cutters). Profiled, the amp's d7 under the
    # 3 mm left a 1 mm core cut loose under the spindle, and every small
    # hole's ramp bit deeper than its passes. The bottom is STATED: derived,
    # a flat cutter stops exactly at the underside, which in MDF leaves a
    # paper skin; the other through cuts go THROUGH_OVERRUN past it.
    for label, dia, i, n, f in holes:
        app.add_cam_op(setup=sid, type="drill", tool=3,
                       name=f"{label} {i}/{n} d{dia:.1f}", targets=[f.ref],
                       top_z=top, bottom_z=bottom)

    app.set_cam_setup(id=sid, reach_offset="auto")
    print(f"  {panel}: port hole, roundover, {len(holes)} bolt holes; 3 tools")
    return job, sid
