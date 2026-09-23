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

    the amp and the supply   through the bay floor, the amp by the four
                             mounting holes its own case is bored with

## Which way round, and how far along

Neither question is asked here any more, and that is the point of the round
that ended it. A fastened mate between two frames has EIGHT outcomes — which
side, and which quarter turn about the joint axis — and none of them is
predictable from the call. This module used to carry four helpers that each
searched those eight with a different acceptance test written by hand: does
the part stay put, is this face looking up, is the box lying down.

The app answers it now, because it can see both frames and the caller
cannot:

    asm.join(a, b)           an exact joint, turned the way the part is
                             already placed (`add_mate derive:"twist"`)
    asm.fasten_in_place      the same, then checked that the part did not
                             move, with the eight tried and ranked if the
                             derived one did not hold
    asm.seat                 solves an offset by probing its sensitivity

What is left here is the ASSERTIONS those searches used as their acceptance
tests — `_bolts_below`, `_check_laid_down`. They are worth keeping as
assertions: each names a thing that went wrong on a real run (a flare upside
down three times, an amplifier standing on edge) and now fails loudly instead
of being searched around.
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


def _check_laid_down(occurrence, floor, p, tol=0.5):
    """That the box ended up lying on the bay floor, inside the bay.

    The turning is `asm.join`'s job now — it reads which side and how far
    round off the placement. This is the assertion that used to be the
    acceptance test of a search here: above the floor, between the walls,
    and long side across the cabinet, which is the only way round it fits
    between the baffle and the port. Kept because it is what "where there is
    room" MEANS, and because the placement it is now derived from is one this
    generator invents.
    """
    half_w = float(p["cabinet_internal_w"]) / 2
    half_d = float(p["cabinet_internal_d"]) / 2
    lo, hi = occurrence.bbox()
    span = [hi[i] - lo[i] for i in range(3)]
    if not (lo[2] > floor - tol
            and lo[0] > -half_w - tol and hi[0] < half_w + tol
            and lo[1] > -half_d - tol and hi[1] < half_d + tol
            and span[0] > max(span[1], span[2]) - tol):
        raise SdkError(
            f"{occurrence.name} did not come to rest lying on the bay floor "
            f"inside the cabinet: it spans {[round(v, 1) for v in lo]} to "
            f"{[round(v, 1) for v in hi]} with the floor at {floor:.1f}")


def _floor_bolts(panel, dia, spacing, y, tol=1.0):
    """The bores of this diameter drilled for one unit: two on the line y, or
    the four corners of a `spacing` rectangle centred on it."""
    dx, dy = float(spacing[0]), spacing[1]
    wanted = [(-dx / 2, y), (dx / 2, y)] if dy is None else [
        (sx * dx / 2, y + sy * float(dy) / 2)
        for sx in (-1, 1) for sy in (-1, 1)]
    r = float(dia) / 2.0
    out = []
    for wx, wy in wanted:
        best, gap = None, None
        for body in panel.bodies():
            for f in body.faces(kind="cylinder"):
                if not f.radius or abs(f.radius - r) > 0.05:
                    continue
                c = f.centroid
                d = (c.x - wx) ** 2 + (c.y - wy) ** 2
                if gap is None or d < gap:
                    best, gap = f, d
        if best is not None and gap < tol * tol:
            out.append(best)
    return out


def _case_bolts(unit, thin, dia, tol=0.2):
    """A bought case's own mounting holes: the bores of this size drilled
    THROUGH the face it lies on. Returns their centroids."""
    r = float(dia) / 2.0
    out = []
    for body in unit.bodies():
        for f in body.faces(kind="cylinder"):
            if not f.radius or abs(f.radius - r) > tol:
                continue
            if abs(f.axis[thin]) < 0.99:
                continue
            out.append(f)
    return out


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


def _broadest_base(part, placed):
    """The face a box lies on when you bolt it to a floor: the broad one that
    faces DOWN where the occurrence is placed.

    Two things make this less obvious than it sounds. `_lowest_down_face`
    takes whichever face the bought STEP happens to have pointing down in its
    OWN frame — for the amp a 92 x 37 edge, which stood the case up 72 mm
    tall like a book. And a brick has two equally broad faces, top and
    bottom, identical in area and equally far out, so nothing in the part
    alone chooses between them; picking by face order meant the supply could
    hang under its floor on one run and lie on it the next.

    So the part answers "which faces are broad" and the PLACEMENT answers
    "which of those looks at the floor". `placed` maps a direction in the
    part's frame to world.
    """
    lo, hi = part.bbox()
    axis = min(range(3), key=lambda i: hi[i] - lo[i])
    best, out = None, None
    for body in part.bodies():
        for f in body.faces(kind="plane"):
            n = f.normal
            if n is None or abs(n[axis]) < 0.999:
                continue
            if placed(n)[2] > -0.9:
                continue            # this one does not face the floor
            # How far out along its own normal it lies: it has to be the
            # outer skin. The amp's largest downward face is a step 10 mm
            # inside the case, and standing on that buries it in the floor.
            far = n[axis] * f.centroid[axis]
            if out is None or far > out + 0.5 or (far > out - 0.5
                                                  and f.area > best.area):
                best, out = f, max(far, out if out is not None else far)
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


def _bolts_below(occurrence, frame_name):
    """That a flare came to rest mouth-down: its bolt lugs below its middle.

    This used to be a search — turn the mate over, look, turn it back — for
    the reason docs/assemblies/README.md#deriving-a-mate-from-the-placement gives: which
    way round a fastened mate takes a part was not predictable and was not
    the same for two mates written alike. `asm.join` decides it from the
    placement now, and this is the assertion that is left. It is worth
    keeping as one: the flare's orientation was got wrong three times, and
    the bolt lugs being at the mouth end is the fact that distinguishes the
    two ways up.
    """
    lo, hi = occurrence.bbox()
    at = float(occurrence.frame(frame_name).origin[2])
    if at > (lo[2] + hi[2]) / 2.0:
        raise SdkError(f"{occurrence.name} came to rest with {frame_name} "
                       f"ABOVE its middle ({at:.1f} against "
                       f"{(lo[2] + hi[2]) / 2.0:.1f}) — it is upside down, "
                       f"so check where port.placements puts it")


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
    # `join`: the flare's PLACEMENT is the authoritative statement of which
    # way up it goes — mouth down, the orientation the source STEP has and
    # the one this got wrong three times by re-deriving it — so the app reads
    # which side and how far round off that placement rather than the caller
    # turning the mate over and looking.
    m = asm.join((occ["port flare (floor)"], "mount bolts"),
                 (occ["bottom_wall"], "port bolts"))
    n += 1
    _bolts_below(occ["port flare (floor)"], "mount bolts")
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
    m3 = asm.join((occ["port flare (bay)"], "lugs"),
                  (occ["port ring brace"], "flare bolts"))
    n += 1
    _bolts_below(occ["port flare (bay)"], "lugs")
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

    # --- the electronics ---------------------------------------------------
    #
    # Bolted through the bay floor. The two cases are not the same case:
    #
    #   the amp    has four real mounting holes through the face it lies on,
    #              so the floor is drilled to ITS pattern and the mate
    #              anchors hole-frame to hole-frame — a bolt passes through
    #              both parts, which is what a bolted joint is
    #   the supply has none: the only small holes in it are the case screws,
    #              and their axes lie IN the face it stands on. So its two
    #              floor holes are ours, and the mate anchors the box's own
    #              footprint centre over them. That is as far as the bought
    #              geometry goes, and it is still a mate — move the holes and
    #              the brick follows.
    for occ_name, holes, dia, y in (
            ("amp module", (p["amp_bolt_x"], p["amp_bolt_y"]),
             p["amp_bolt_dia"], p["amp_y"]),
            ("PSU brick", (p["psu_bolt_dist"], None),
             p["case_bolt_dia"], p["psu_y"])):
        unit = part(occ_name)
        label = f"{occ_name} bolts"
        floor_holes = _floor_bolts(bottom, dia, holes, float(y))
        want = 4 if holes[1] is not None else 2
        if len(floor_holes) != want:
            raise SdkError(f"{occ_name}: {len(floor_holes)} floor bolts of "
                           f"d{float(dia)} about y={float(y)}, wanted {want}")
        # On the FACE the unit stands on, not at the bores' own midpoint,
        # which is mid-thickness of a 16 mm panel — anchoring there sank
        # both boxes 8 mm into their own floor.
        bottom.frame(label, origin=(0.0, float(y),
                                    float(p["wall_thickness"])),
                     z=(0, 0, 1), x=(1, 0, 0))

        asm.activate()   # `occurrence` frames are an assembly-tab question
        axes = occ[occ_name].frame()

        def placed(n, axes=axes):
            """A direction in the part's frame, as the assembly has it."""
            return [axes.x[i] * n[0] + axes.y[i] * n[1] + axes.z[i] * n[2]
                    for i in range(3)]

        base = _broadest_base(unit, placed)
        if base is None:
            raise SdkError(f"{occ_name}: no broad face of it looks at the "
                           f"floor where it is placed — see bought.placements")
        lo, hi = unit.bbox()
        thin = min(range(3), key=lambda i: hi[i] - lo[i])
        if holes[1] is not None:
            # The case's own four, checked against the table that drilled the
            # floor for them. A measured number in params and the geometry it
            # was measured from can otherwise drift apart silently.
            if float(holes[0]) < float(holes[1]):
                raise SdkError("amp_bolt_x is the ACROSS spacing and has to "
                               "be the longer of the two — the mate lays the "
                               "pattern's long side across the cabinet")
            at = [f.centroid for f in _case_bolts(unit, thin, dia)]
            got = tuple(round(max(c[i] for c in at) - min(c[i] for c in at), 1)
                        for i in range(3) if i != thin)
            if sorted(got) != sorted(round(float(h), 1) for h in holes):
                raise SdkError(
                    f"{occ_name}: the case's mounting holes are {got} apart, "
                    f"but amp_bolt_x/amp_bolt_y say "
                    f"{float(holes[0])} x {float(holes[1])} — the floor is "
                    f"drilled to the table, so fix the table")
            # Where the pattern is and which way it runs — but at the height
            # of the face the case LIES on. `of=own` alone puts the frame at
            # the holes' own middle, which is up inside a flange 6 mm above
            # the base, and the case then sinks that far into the floor.
            mid = [sum(c[i] for c in at) / len(at) for i in range(3)]
            mid[thin] = base.centroid[thin]
            long_axis = max((i for i in range(3) if i != thin),
                            key=lambda i: max(c[i] for c in at)
                            - min(c[i] for c in at))
            x = [1.0 if i == long_axis else 0.0 for i in range(3)]
            unit.frame("base", origin=tuple(mid), z=base.normal, x=x)
        else:
            # Its own footprint centre, projected onto the base plane. NOT
            # the base face's centroid: a brick with a cable stub has one
            # 13 mm off the middle of the box, and the box then hangs 13 mm
            # to one side of the bolts that hold it.
            mid = [(lo[i] + hi[i]) / 2.0 for i in range(3)]
            mid[thin] = base.centroid[thin]
            unit.frame("base", origin=tuple(mid), z=base.normal)

        # NOT `fasten_in_place`: here the HOLES are the truth and the
        # placement was invented, so the mate moves the unit onto them
        # rather than being asked to preserve where it was put.
        # `join`, not `fasten`: the app reads which side and how far round
        # off the placement, where it can see both frames. `fasten` leaves
        # that to the solver, and the solver's pick between eight is
        # arbitrary — it stood this case on edge and hung it under the floor.
        asm.join((occ[occ_name], "base"), (occ["bottom_wall"], label))
        n += 1
        _check_laid_down(occ[occ_name],
                         occ["bottom_wall"].frame(label).origin[2], p)
    print("  amp and supply bolted to the bay floor")

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
