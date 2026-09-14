"""What holds the assembly together, anchored to geometry.

Everything here was placed by a computed transform first, and the results are
in the plan's defect list: T-nuts driven barrel-first into open air, a flare
mounted upside down, an amplifier inside the cabinet floor. A transform is a
number somebody worked out; a mate is a statement about two pieces of
geometry, and it survives the parameter that moves them.

So each mate below names a FRAME derived from the feature it belongs to — a
bolt circle's centre, a spigot's axis, a seating face — and never a
coordinate. `part.frame(of=[faces])` builds those from the faces the model
already has, so the bolt circle a driver is fastened to is literally the holes
the baffle was drilled with.

## What is mated and what is not

The six panels stay on derived transforms. They are the datum: a glued
carcass whose position IS the parameter table (`walls.placements`), and
mating them to each other would only restate `cabinet_internal_w` in a second
place. Everything that MOUNTS to that carcass is mated:

    the two drivers      to the baffle's bolt circles
    the port stack       flare -> bottom wall, tube -> flare, flare -> tube
    the T-nuts           into the holes they are hammered into

## Which way round, and how far along

Both questions belong to the SDK now: `asm.fasten_in_place` picks the side
and the half-turn that leave a part where its own placement put it (and
closes a purely axial residual, which is only ever the two frames' drill
depths), and `asm.seat` solves an offset by probing its sensitivity. This
module used to carry all of that.

What is still here is the one case neither covers: a part whose placement is
only approximate, where the test is not "did it move" but "is this face
looking the right way" — `_face_up`, for the port flare.
"""
from designer_sdk import Part, SdkError

import joinery


def _face_normal_at(part, normal, coord, tol=0.2):
    """The planar face with this outward normal at this position along it."""
    axis = [i for i in range(3) if normal[i]][0]
    sign = normal[axis]
    for body in part.bodies():
        for f in body.faces(kind="plane"):
            nvec = f.normal
            if nvec is None or abs(nvec[axis] - sign) > 1e-3:
                continue
            if abs(sum(abs(nvec[i]) for i in range(3) if i != axis)) > 1e-3:
                continue
            if abs(f.centroid[axis] - coord) <= tol:
                return f
    return None


def _panel_occ(part_name):
    """The occurrence a panel is placed as — the left side wall and the ring
    brace are named differently from their parts."""
    return {"side_wall": "side_wall L",
            "ring_brace": "port ring brace"}.get(part_name, part_name)


def _ring_bores(panel, p, prefix):
    """The holes of one bolt ring, in the order `hardware.placements` walks
    them, so nut N goes in hole N.

    Both come from `seating.ring`, which is the point: the fixing and the
    hole it goes in are one number in two places, not two.
    """
    import math
    import seating
    if prefix.endswith("W170S"):
        pcd, count, start = p["w170s_bolt_circle"], p["w170s_bolt_count"], 90.0
        plane = "xz"
        # The panel's OWN v, not the world z: the baffle is sketched about
        # its middle and the assembly drops it to -511 afterwards.
        centre_v = (float(p["w170s_center_from_floor"])
                    - float(p["carcass_half_h"]))
    elif prefix.endswith("brace"):
        pcd, count, start = (p["port_brace_bolt_circle"],
                             p["port_brace_bolt_count"], 0.0)
        plane, centre_v = "xy", 0.0
    else:
        pcd, count, start = (p["outer_port_bolt_circle"],
                             p["outer_port_bolt_count"], 90.0)
        plane, centre_v = "xy", 0.0
    want = []
    for u, v in seating.ring(pcd, count, start_deg=start):
        want.append((u, v) if plane == "xy" else (u, v))
    r = float(p["m4_t_nutt_dia"]) / 2
    found = [f for body in panel.bodies() for f in body.faces(kind="cylinder")
             if f.radius and abs(f.radius - r) < 0.05]
    out = []
    for u, v in want:
        best, gap = None, None
        for f in found:
            c = f.centroid
            if plane == "xy":
                d = (c.x - u) ** 2 + (c.y - v) ** 2
            else:
                d = (c.x - u) ** 2 + (c.z - (centre_v + v)) ** 2
            if gap is None or d < gap:
                best, gap = f, d
        if best is not None and gap < 4.0:
            out.append(best)
    return out


def _bores(part, rmin, rmax, want=None):
    """Cylindrical faces in a radius band — a bolt circle, or a spigot."""
    out = []
    for body in part.bodies():
        for f in body.faces(kind="cylinder"):
            if f.radius and rmin <= f.radius <= rmax:
                out.append(f)
    if want is not None and len(out) != want:
        raise SdkError(
            f"{part.name}: expected {want} bores in r {rmin}..{rmax}, "
            f"found {len(out)} — the feature that drills them changed")
    return out


def _mount_face(part, cutout_d, flange_d):
    """A driver's MOUNTING face: the annulus behind its flange that lands on
    the baffle's rebate.

    Found by area — between the cut-out and the flange diameter, less its
    bolt holes — because a driver's bounding box front is the rubber
    SURROUND, which is supposed to stand proud of the baffle. Seating a
    W170S by its bbox drove 3.94 mm of surround into the MDF and read as
    40,256 mm3 of interference, which looked like the datasheet's 7 mm
    rebate being wrong. It is not: the ply cover is cut at the FLANGE
    diameter, so 3.6 through it plus 3.4 into the MDF is the 7 the flange
    needs, and the surround stands proud on top of that.
    """
    import math
    want = math.pi / 4 * (float(flange_d) ** 2 - float(cutout_d) ** 2)
    best, err = None, None
    for body in part.bodies():
        for f in body.faces(kind="plane"):
            n = f.normal
            if n is None or n.y < 0.99:
                continue
            d = abs(f.area - want)
            if d < want * 0.15 and (err is None or d < err):
                best, err = f, d
    return best


def _lowest_down_face(part):
    """The lowest planar face that looks DOWN — what the part stands on.

    Not the biggest one. The bought flare's largest downward face is a
    7,175 mm2 annulus 10 mm up inside the bell; the thing that actually
    touches the cabinet floor is the bell lip's underside, an 805 mm2 ring
    right at the bottom. Seating on the big one hung the flare 10 mm high
    and buried the lip in the floor.
    """
    best = None
    for body in part.bodies():
        for f in body.faces(kind="plane"):
            n = f.normal
            if n is None or n.z > -0.999:
                continue
            if best is None or f.centroid.z < best.centroid.z:
                best = f
    return best


def _bolt_ring(app, asm, part, occurrence, rmin, rmax, size="M3.5"):
    """A fastener in every hole of a bolt ring, entering from the top.

    The rim is the hole's UPPER circular edge — that is the end a screw goes
    in, and `fit_fastener` measures the stack downward from it.
    """
    rims = []
    for body in part.bodies():
        for e in body.edges(kind="circle"):
            if e.radius and rmin <= e.radius <= rmax:
                rims.append(e)
    # Two edges per through hole; keep the upper one of each pair.
    best = {}
    for e in rims:
        key = (round(e.midpoint.x, 1), round(e.midpoint.y, 1))
        if key not in best or e.midpoint.z > best[key].midpoint.z:
            best[key] = e
    out, refused = [], None
    for e in best.values():
        try:
            r = asm.bolt(e, occurrence, standard="iso4762", size=size)
            out.append(f"{r.get('size')} x {r.get('length_mm')}")
        except Exception as exc:
            refused = str(exc)
    return out, refused, len(best)


def _face_up(asm, app, mate_id, occurrence, frame_name, want_above):
    """Turn a mate over until the part's bolt circle is on the right side.

    `want_above` says whether the frame should sit ABOVE the part's own
    middle. A flare bolted up under the floor has its lugs at the top; one
    bolted down onto a brace has them at the bottom. Which way a fastened
    mate takes the part is not predictable and is not the same for two mates
    written alike, so it is decided by looking.
    """
    for _ in range(2):
        asm.solve()
        lo, hi = occurrence.bbox()
        middle = (lo[2] + hi[2]) / 2.0
        above = float(occurrence.frame(frame_name).origin[2]) > middle
        if above == want_above:
            return
        app.set_mate(id=mate_id, flipped=True)
    raise SdkError(f"{occurrence.name}: neither way round puts {frame_name} "
                   f"{'above' if want_above else 'below'} its middle")


def build(app, asm, p, made):
    """Every mate, in the order that makes each one's reference already
    placed. Returns the number added."""
    n = 0
    parts = {r["name"]: r for r in app.list_parts()["parts"]
             if r.get("type") == "part"}

    def part(name):
        return Part(app, parts[name])

    occ = {o.name: o for o in asm.occurrences()}

    # --- the carcass -------------------------------------------------------
    #
    # Fastened along a TREE off the fixed left side wall, by the dowel holes
    # themselves (joinery.py). Twelve joints for six panels would constrain
    # the box five times over; a tree says it once.
    for moving, my_frame, fixed, its_frame in joinery.TREE:
        asm.fasten_in_place((occ[moving], my_frame), (occ[fixed], its_frame))
        n += 1
    print(f"  carcass: {len(joinery.TREE)} panels fastened by their dowels")

    # --- the port stack ----------------------------------------------------
    #
    # Both ends are bolted to the cabinet and the tube spans between them:
    #
    #   the lower flare hangs UNDER the bottom wall, bell opening down into
    #   the plinth, its flange bolted up against the wall's underside;
    #   the tube rises from that underside through the floor's hole and on
    #   through the ring brace;
    #   the upper flare bolts DOWN onto the ring brace, which is the
    #   structural support the port hangs from, and its bell opens into the
    #   bay.
    #
    # The lower flare is turned over. Its bell opens toward its own +Z and it
    # has to open down, which is also what the Onshape export says — that
    # occurrence carries quat [1,0,0,0].
    bottom = part("bottom_wall")
    flare = part("port_flare_bottom")
    brace = part("ring_brace")
    top = part("port_flare_top")

    bottom.frame("port bolts", of=_bores(bottom, 2.5, 3.0, 5))
    flare.frame("mount bolts", of=_bores(flare, 1.8, 2.2, 5))
    brace.frame("flare bolts", of=_bores(brace, 2.5, 3.0, 4))
    top.frame("lugs", of=_bores(top, 1.8, 2.2, 4))
    tube = part("port_tube")
    tube.frame("axis", of=_bores(tube, 52.0, 53.0, 1))

    floor_inner = -float(p["carcass_h"]) + float(p["wall_thickness"])
    m = asm.fasten((occ["port flare (floor)"], "mount bolts"),
                   (occ["bottom_wall"], "port bolts"), flipped=False)
    n += 1
    # Mouth DOWN: after the turn the bolt lugs sit BELOW the flare's middle,
    # down by the mouth, and the throat is the end that reaches the floor.
    _face_up(asm, app, m["id"], occ["port flare (floor)"], "mount bolts",
             want_above=False)
    got = asm.seat(m, lambda: occ["port flare (floor)"].bbox()[1][2],
                   floor_inner + float(p["port_throat_protrusion"]))
    print(f"  port flare hangs mouth-down under the floor "
          f"(throat top {got:.2f})")

    m2 = asm.fasten((occ["port tube"], "axis"),
                    (occ["port flare (floor)"], "mount bolts"), flipped=False)
    n += 1
    # INTO the throat, not on top of it — the tube plugs down inside the
    # flare, which is how the export has it and what makes the port one air
    # path rather than three parts in a line. Both are d105, so the fit is
    # line-to-line and `verify` expects the contact (see EXPECTED there).
    throat_top = float(occ["port flare (floor)"].bbox()[1][2])
    got = asm.seat(m2, lambda: occ["port tube"].bbox()[0][2],
                   throat_top - float(p["port_tube_insert"]))
    print(f"  port tube plugs into that throat ({got:.2f})")

    brace_top = float(occ["port ring brace"].bbox()[1][2])
    m3 = asm.fasten((occ["port flare (bay)"], "lugs"),
                    (occ["port ring brace"], "flare bolts"), flipped=False)
    n += 1
    _face_up(asm, app, m3["id"], occ["port flare (bay)"], "lugs",
             want_above=False)
    got = asm.seat(m3, lambda: occ["port flare (bay)"].bbox()[0][2],
                   brace_top)
    print(f"  port flare (bay) bolts onto the ring brace ({got:.2f})")

    # ...and the fasteners that actually do it. `fit_fastener` reads the size
    # out of the bore and the length out of the stack the bolt's axis
    # crosses, so the port is held by its own hardware rather than by a mate
    # that merely agrees with it.
    # The lugs are 4.0 mm across and NO ISO clearance hole is: M3.5 runs
    # 3.7 / 3.9 / 4.2 and M4 starts at 4.3. The bought flare is moulded for a
    # wood screw, not a machine screw, so `fit_fastener` refuses rather than
    # rounding to the nearest — and it is right to. The mate holds the flare;
    # the screws are a purchasing decision, not a modelling one.
    fitted, refused, holes = _bolt_ring(app, asm, top,
                                        occ["port flare (bay)"], 1.8, 2.2)
    if fitted:
        print(f"  {len(fitted)} fasteners into the ring brace ({fitted[0]})")
    else:
        print(f"  {holes} lugs on the ring brace, NOT fastened: "
              f"d4.0 is no ISO clearance (M3.5 is 3.7/3.9/4.2, M4 4.3) — "
              f"these take wood screws")

    # --- the divider -------------------------------------------------------
    #
    # Housed `partition_wall_cutout` into all four uprights, so the rebate IS
    # its locating feature: its end face lands on the rebate's floor. Both
    # faces are found by their normal and depth, not by a height.
    side = part("side_wall")
    div = part("divider")
    rebate = _face_normal_at(side, (1, 0, 0),
                             float(p["wall_thickness"])
                             - float(p["partition_wall_cutout"]))
    end = _face_normal_at(div, (-1, 0, 0),
                          -(float(p["cabinet_internal_w"]) / 2
                            + float(p["partition_wall_cutout"])))
    if rebate is not None and end is not None:
        div.frame("housing", of=end)
        side.frame("divider housing", of=rebate)
        asm.fasten_in_place((occ["divider"], "housing"),
                            (occ["side_wall L"], "divider housing"), tol=0.6)
        n += 1
        print("  divider seated in its housing")

    # --- the T-nuts --------------------------------------------------------
    #
    # One frame per HOLE, not one per bolt circle: a nut goes in a particular
    # hole, and a ring frame would only say which circle it is somewhere on.
    nut = part("t_nut")
    nut.frame("barrel", of=_bores(nut, float(p["m4_t_nutt_dia"]) / 2 - 0.05,
                                  float(p["m4_t_nutt_dia"]) / 2 + 0.05, 1))
    for panel_name, prefix, count in (
            ("front_wall", "T-nut W170S", int(float(p["w170s_bolt_count"]))),
            ("ring_brace", "T-nut brace",
             int(float(p["port_brace_bolt_count"]))),
            ("bottom_wall", "T-nut port",
             int(float(p["outer_port_bolt_count"])))):
        panel = part(panel_name)
        holes = _ring_bores(panel, p, prefix)
        if len(holes) != count:
            raise SdkError(f"{panel_name}: {len(holes)} holes for {prefix}, "
                           f"expected {count}")
        for i, hole in enumerate(holes, 1):
            fname = f"{prefix} {i} seat"
            panel.frame(fname, of=hole)
            occ_name = f"{prefix} {i}"
            asm.fasten_in_place((occ[occ_name], "barrel"),
                                (occ[_panel_occ(panel_name)], fname),
                                tol=0.6)
            n += 1
    print(f"  {sum((int(float(p['w170s_bolt_count'])), int(float(p['port_brace_bolt_count'])), int(float(p['outer_port_bolt_count']))))} T-nuts in their own holes")

    # --- the drivers -------------------------------------------------------
    #
    # Fastened to the baffle's own bolt rings, then seated so the flange's
    # back lands on the rebate floor. The rebate floor is `*_flange_depth`
    # behind the FINISHED surface, ply included.
    baffle = part("front_wall")
    finished = -(float(p["cabinet_internal_d"]) / 2
                 + float(p["wall_thickness"])
                 + float(p["finish_wall_thickness"]))
    for label, occ_name, hole_r, drv_r, cutout, flange, depth in (
            ("W170S", "W170S woofer", (2.6, 2.9), (2.3, 2.7),
             p["w170s_cutout_dia"], p["w170s_flange_dia"],
             p["w170s_flange_depth"]),
            ("Alpair", "Alpair 7MS", (2.0, 2.2), (1.4, 1.7),
             p["alpair_cutout_dia"], p["alpair_flange_dia"],
             p["alpair_flange_depth"])):
        drv = part(occ_name)
        baffle.frame(f"{label} bolts", of=_bores(baffle, *hole_r, want=5))
        drv.frame("bolts", of=_bores(drv, *drv_r, want=5))
        face = _mount_face(drv, cutout, flange)
        if face is None:
            raise SdkError(f"{occ_name}: no flange face to seat on")
        drv.frame("mount", of=face)
        # Which way round the mate takes the driver is not predictable and is
        # not even the same for these two: `flipped=True` seats the woofer
        # and turns the Alpair magnet-first at the room. So it is DECIDED by
        # measurement — mate it, ask which way the flange now looks, and flip
        # if it is looking out of the cabinet.
        md = asm.fasten((occ[occ_name], "bolts"),
                        (occ["front_wall"], f"{label} bolts"), flipped=False)
        n += 1
        asm.solve()
        if float(occ[occ_name].frame("mount").z[1]) < 0:
            app.set_mate(id=md["id"], flipped=True)
            asm.solve()
        if float(occ[occ_name].frame("mount").z[1]) < 0:
            raise SdkError(f"{occ_name}: neither way round faces the flange "
                           f"into the cabinet")
        floor_y = finished + float(depth)
        got = asm.seat(md,
                       lambda o=occ[occ_name]:
                           float(o.frame("mount").origin[1]), floor_y)
        front = float(occ[occ_name].bbox()[0][1])
        print(f"  {label}: flange on its rebate at {got:.2f}, surround "
              f"stands {finished - front:.2f} mm proud")
    return n
