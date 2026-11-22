# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The side wall's inner face: its divider dado and its UPPER face dowels.

One job, one setup, run on both side walls — the right-hand one is the same
part turned over (walls.side_wall), so its inner face is the same face.

## Only the top end

The panel is 1122 mm with its feet and the 6090 travels 900 in Y. With the
top end at the back of the bed and zeroed on its far-right corner (the rest
hangs off the front, as every panel here does), the dado (136..152 mm down) and the upper dowels — the top
wall's pair 8 mm down, the uprights' pair `plug_side_dist` down — are all
within 152 mm of the datum. The lower pairs, 972 and 1014 mm down, are not,
and are marked out and drilled by hand.

## The dado is STOPPED

Unlike the uprights' slot, which runs edge to edge, this one stops 12 mm in
from each edge (`cabinet_internal_d + 2 * partition_wall_cutout` long in a
`cabinet_internal_d + 2 * wall_thickness` panel). A round cutter leaves its
radius in the four corners, and the divider is square, so it would sit on
them instead of in the slot. `corner_relief: tbone` runs a bite of the
tool's radius along the slot past each corner, under the upright's end face,
where it is out of sight once the box is together.
"""
import cam
import cam_back
from lobole.sdk import SdkError

AXIS = 0     # the side wall is a slab across X, sketched on YZ
INSIDE = 1.0  # its inner face is x = +t, looking +X (walls.side_wall)


def _planar(part, at, min_area):
    best = None
    for body in part.bodies():
        for f in body.faces(kind="plane"):
            n = f.normal
            if n is None or abs(n[AXIS] - INSIDE) > 1e-3 or f.area < min_area:
                continue
            if abs(f.centroid[AXIS] - at) > 0.15:
                continue
            if best is None or f.area > best.area:
                best = f
    return best


def _upper_dowels(part, p):
    """The face dowels in the top half: bores of `plug_dia` along X."""
    r = float(p["plug_dia"]) / 2.0
    out = []
    for body in part.bodies():
        for f in body.faces(kind="cylinder"):
            if not f.radius or abs(f.radius - r) > 0.05:
                continue
            if abs(f.axis[AXIS]) < 0.99 or f.centroid[2] <= 0:
                continue
            out.append(f)
    return sorted(out, key=lambda f: (-f.centroid[2], f.centroid[1]))


def build(app, p, panel="side_wall"):
    """The job and its one setup. Returns (job, setup id)."""
    print("cam, side wall:")
    part = app.part(panel, create=False)
    part.activate()
    job = app.machine_part(part=part.id, name=f"{panel} - inner face")

    t = float(p["wall_thickness"])
    inside = _planar(part, t, min_area=1000.0)
    if inside is None:
        raise SdkError(f"{panel}: no inner face to lay upward")
    dado = _planar(part, t - float(p["partition_wall_cutout"]), min_area=500.0)
    if dado is None:
        raise SdkError(f"{panel}: no divider dado "
                       f"{float(p['partition_wall_cutout'])} mm deep")
    dowels = _upper_dowels(part, p)
    if len(dowels) != 4:
        raise SdkError(f"{panel}: {len(dowels)} face dowels in the top half, "
                       f"wanted 4 (the top wall's pair and the uprights')")

    sid = app.add_cam_setup(name=f"{panel}: inner face, top end",
                            post="mach3")["id"]
    app.set_cam_setup(id=sid, machine=cam.MACHINE["name"], order="tool",
                      split_output=True)
    app.set_cam_setup(id=sid, stock={"mode": "relative", "margin": [0, 0, 0],
                                     "bottom_margin": 0})
    for tool in cam.TOOLS:
        app.set_cam_setup(id=sid, tool=tool)
    app.set_cam_setup(id=sid, lay_face_up=inside.ref)
    # Laid on its inner face, the panel's length comes out along X — the
    # 600 mm axis, through the gantry's uprights. Spun a quarter turn in
    # plane it lies along Y, under the gantry, as the uprights' jobs do.
    job_part = cam_back.job_part_of(app, job, part, panel)
    app.set_job_placement(setup=sid, part=job_part, rot_z=90)
    top = cam_back._stock_top(app, sid)

    depth = float(p["partition_wall_cutout"])
    app.add_cam_op(setup=sid, type="pocket", tool=1,
                   name=f"divider dado {depth:.1f} deep, T-boned",
                   targets=[dado.ref], side="inside", corner_relief="tbone",
                   top_z=top, bottom_z=top - depth, stepdown=2.0,
                   stepover=0.45)
    for i, bore in enumerate(dowels, 1):
        app.add_cam_op(setup=sid, type="profile", tool=1,
                       name=f"dowel {i}/4 d{float(p['plug_dia']):.0f} "
                            f"x {t / 2:.0f} deep",
                       targets=[bore.ref], side="inside",
                       top_z=top, bottom_z=top - t / 2.0, stepdown=2.0)

    # The top end at the back of the bed, the rest off the front, zero on
    # the far-right corner: the convention every panel here is cut to.
    spin = cam_back._cut_end_at_the_back(app, sid, job_part, panel,
                                         spins=(90, 270))
    print(f"  {panel}: 5 operations, far-right zero, turned {spin}; "
          f"the lower dowels are hand work")
    return job, sid
