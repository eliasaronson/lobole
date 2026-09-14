"""The six MDF panels, each drawn about its own centre.

Every wall is drawn in the frame the assembly places it in, so an occurrence
needs a position and (for the right-hand side wall, which is the left one
turned over) nothing else. The document's own frames are kept: the panel is
sketched about its middle, not about a corner, because that is what makes
`carcass_half_h` the v origin for all four uprights and lets the same dowel
row be written once.

## The plywood coverings are NOT here

They used to be an extra body inside each wall. They are separate 3.6 mm
pieces you cut and glue on, so they are their own parts now and they take
their outline FROM these walls by reference — see veneer.py.
"""
import math

from designer_sdk import Plane

import joinery

MDF = "mdf"


def _outer_d(p):
    """Half the cabinet's outside depth, veneer excluded."""
    return p["cabinet_internal_d"] / 2 + p["wall_thickness"]


def _outer_w(p):
    """Half the cabinet's outside width, veneer excluded."""
    return p["cabinet_internal_w"] / 2 + p["wall_thickness"]


def _foot_profile(p, half_span, out):
    """The plinth cut-out, as the polyline both uprights use.

    `half_span` is the panel's half width in sketch u; `out` is how far the
    foot's outer face is from the centre — the same thing for a side wall and
    less for the baffle, which sits between the side walls.

    The foot is a flat `feet_flat_width` wide at the outer edge, and the
    opening rises from the floor to the carcass on a leg leaning `feet_angle`
    off the underside. The lean is what sets the inner corner, so nothing here
    is a coordinate anybody measured:

        run = plinth_height * tan(feet_angle - 90)
    """
    half = p["carcass_half_h"]
    drop = p["plinth_height"]
    flat = p["feet_flat_width"]
    # `run` alone is a number: it needs a tangent, and the document's
    # expression language is not asked to supply one. Everything else stays a
    # Param so the outline follows the table — cast to float, as this was,
    # the whole plinth silhouette is literal and the plywood coverings (whose
    # outline is ONLY this) stop following anything at all.
    run = float(drop) * math.tan(math.radians(float(p["feet_angle"]) - 90.0))
    inner = out - flat - run
    # TWO loops, one per foot — not one polygon round both.
    #
    # The arch between the feet reaches exactly the carcass line, so the feet
    # do not touch each other and there is no single closed outline that
    # covers them. Drawn as one, the top edge is traced twice, the loop
    # self-intersects, and the union does not fail cleanly: the app reports
    # "add LOST material from body 'outline' (5731695 mm3 to 0)" — the whole
    # panel gone.
    right = [
        (half_span, -half),
        (half_span, -half - drop),
        (out - flat, -half - drop),
        (inner, -half),
    ]
    left = [(-u, v) for u, v in right]
    return right, left


def silhouette(sk, p, panel, du=0.0, dv=0.0):
    """A panel's outline, drawn the same way for the panel and for the
    plywood covering that goes on it.

    ONE function for both, because the covering IS the wall's outline: a
    projected face comes into a sketch as locked CONSTRUCTION geometry, so
    an extrude cannot use it ("sketch has no closed profile") and the shape
    has to be real. Shared here rather than described twice.

    `du`/`dv` grow it, which is what a covering needs and the wall does not.
    A covering runs to the cabinet's OUTER envelope, past the panel it is
    glued to and over the edges of the panels beside it, so that the 45
    degree mitres on two of them cancel in the corner. Drawn at the wall's
    own width instead, the baffle's covering stops 16 mm short of each side
    wall and leaves its front edge bare.
    """
    half_h = p["carcass_half_h"] + dv
    if panel == "top_wall":
        # Four lines, not `centered_rect`: a `rect` entity's corners are not
        # expression-driven, so a rect built from Params is a literal and the
        # covering stops following the cabinet's size.
        hw = p["cabinet_internal_w"] / 2 + du
        hd = p["cabinet_internal_d"] / 2 + dv
        sk.polyline([(-hw, -hd), (hw, -hd), (hw, hd), (-hw, hd)], close=True)
        return sk
    half_u = (_outer_d(p) if panel == "side_wall"
              else p["cabinet_internal_w"] / 2) + du
    right, left = _foot_profile(p, half_u, half_u)
    sk.polyline([(half_u, half_h)] + right[1:]
                + [left[3], left[2], left[1]]
                + [(-half_u, half_h)], close=True)
    return sk


def _dowel_planes(p, panel):
    """Which sketch plane each of a panel's dowel groups is drilled on, which
    way the drill goes, and how deep.

    A hole through a panel's broad FACE is half a wall deep; one into its EDGE
    or END takes the rest of the dowel. The two add up to `plug_length`.
    """
    t = p["wall_thickness"]
    w2 = p["cabinet_internal_w"] / 2
    d2 = p["cabinet_internal_d"] / 2
    shallow, deep = t / 2, p["plug_length"] - t / 2
    if panel == "side_wall":
        return {"inner": (Plane.yz(t), True, shallow)}
    if panel == "front_wall":
        return {"inner": (Plane.xz(0), False, shallow),
                "end_plus": (Plane.yz(w2), True, deep),
                "end_minus": (Plane.yz(-w2), False, deep)}
    if panel == "back_wall":
        return {"inner": (Plane.xz(-t), True, shallow),
                "end_plus": (Plane.yz(w2), True, deep),
                "end_minus": (Plane.yz(-w2), False, deep)}
    return {"edge_x_plus": (Plane.yz(w2), True, deep),
            "edge_x_minus": (Plane.yz(-w2), False, deep),
            "edge_y_minus": (Plane.xz(-d2), True, deep),
            "edge_y_plus": (Plane.xz(d2), False, deep)}


def _drill_dowels(part, p, panel):
    """Every dowel this panel takes, and a named frame on each joint's pair.

    The frame is what `mates.py` fastens by, so a panel is held to its
    neighbour by the holes the dowel actually goes in.
    """
    planes = _dowel_planes(p, panel)
    for label, key, uvs in joinery.for_panel(p, panel):
        plane, rev, depth = planes[key]
        sk = part.sketch(plane, name=label)
        for at in uvs:
            sk.circle(at, d=p["plug_dia"])
        part.cut(sk.add(), distance=depth, reversed=rev, name=label)
        want = [plane.world(u, v) for u, v in uvs]
        bores = _bores_near(part, p["plug_dia"], want)
        # A frame built from bores sits at their MIDPOINT, and the two halves
        # of a joint are drilled to different depths — 8 mm into a panel's
        # face, 22 into the other's edge. Fastened midpoint to midpoint, the
        # panel lands 15 mm inside its neighbour. So the joint frame is
        # offset back onto the FACE, which is the plane the two actually meet
        # on and the only thing both halves agree about.
        #
        # PLUS half the depth: the bore frame's Z comes back pointing OUT of
        # the panel, against the drill, so the face is forward along it. The
        # other sign put the side wall's frame at x=-113 and the baffle's at
        # -83 for a joint whose face is at -105.
        axis = part.frame(label + " bores", of=bores)
        part.frame(label, parent=axis, offset=(0, 0, float(depth) / 2))


def _bores_near(part, dia, points, tol=0.6):
    """The cylindrical faces of this diameter nearest each of `points`.

    Matched by position rather than by taking whatever the last cut returned:
    a panel carries eight of these and they must be grouped by the joint they
    belong to, not by the order the kernel hands them back.
    """
    r = float(dia) / 2.0
    found = []
    for body in part.bodies():
        for f in body.faces(kind="cylinder"):
            if f.radius and abs(f.radius - r) < 0.05:
                found.append(f)
    out = []
    for want in points:
        best, gap = None, None
        for f in found:
            c = f.centroid
            d = min(abs(c.x - want[0]), 1e9) ** 2 + (c.y - want[1]) ** 2 \
                + (c.z - want[2]) ** 2
            # Distance in the plane of the hole only: its centroid sits at
            # mid-DEPTH, which is off the sketch point along the drill axis.
            d = sum(sorted([(c.x - want[0]) ** 2, (c.y - want[1]) ** 2,
                            (c.z - want[2]) ** 2])[:2])
            if gap is None or d < gap:
                best, gap = f, d
        if best is not None:
            out.append(best)
    return out


# --------------------------------------------------------------------------
# the uprights
# --------------------------------------------------------------------------

def side_wall(app, p):
    """One side. Placed twice — the right-hand one is this turned over."""
    part = app.part("side_wall")
    half_d, half_h = _outer_d(p), p["carcass_half_h"]
    t = p["wall_thickness"]

    sk = part.sketch(Plane.yz(0), name="outline")
    sk.centered_rect((0, 0), half_d * 2, half_h * 2)
    body = part.extrude(sk.add(), distance=t, name="outline")
    part.material(MDF)

    # The divider lets `partition_wall_cutout` into the inner face.
    top = half_h - (p["alpair_chamber_h"] + t)
    sk = part.sketch(Plane.yz(t), name="divider_cutout")
    sk.rect((-(p["cabinet_internal_d"] / 2 + p["partition_wall_cutout"]), top),
            (p["cabinet_internal_d"] / 2 + p["partition_wall_cutout"], top - t))
    part.cut(sk.add(), distance=p["partition_wall_cutout"], reversed=True)

    _drill_dowels(part, p, "side_wall")

    sk = part.sketch(Plane.yz(0), name="feet")
    for loop in _foot_profile(p, half_d, half_d):
        sk.polyline(loop, close=True)
    part.extrude(sk.add(), distance=t, mode="add")

    return part


def front_wall(app, p):
    """The baffle: two driver openings, their flange recesses and bolt rings."""
    part = app.part("front_wall")
    half_w, half_h = p["cabinet_internal_w"] / 2, p["carcass_half_h"]
    t = p["wall_thickness"]

    sk = part.sketch(Plane.xz(0), name="outline")
    sk.centered_rect((0, 0), half_w * 2, half_h * 2)
    part.extrude(sk.add(), distance=t, name="outline")
    part.material(MDF)

    # Local 0 is the face the assembly turns INWARD, so the divider's rebate
    # cuts along the plane normal. Reversed, it cuts the air in front of the
    # panel and the divider then lands 4 mm inside solid MDF.
    top = half_h - (p["alpair_chamber_h"] + t)
    sk = part.sketch(Plane.xz(0), name="divider_cutout")
    sk.rect((-half_w, top), (half_w, top - t))
    part.cut(sk.add(), distance=p["partition_wall_cutout"])

    # Both drivers, from the one rule: cut the hole, rebate the flange, ring
    # of bolts. The rebate is sunk from the FINISHED face, so the veneer's
    # thickness comes out of it — that is what `- faner_width` is doing.
    for d in (_w170s(p, half_h), _alpair(p, half_h)):
        _driver_opening(part, p, **d)

    _drill_dowels(part, p, "front_wall")

    sk = part.sketch(Plane.xz(0), name="feet")
    for loop in _foot_profile(p, half_w, half_w):
        sk.polyline(loop, close=True)
    part.extrude(sk.add(), distance=t, mode="add")

    # The baffle skin runs BETWEEN the side skins, so it is the panel's own
    # width, and it is drilled with the same two openings.
    return part


def _w170s(p, half_h):
    return dict(name="w170s",
                v=p["w170s_center_from_floor"] - half_h,
                cutout=p["w170s_cutout_dia"],
                flange=p["w170s_flange_dia"],
                depth=p["w170s_flange_depth"],
                pcd=p["w170s_bolt_circle"],
                n=int(float(p["w170s_bolt_count"])),
                bolt=p["m4_t_nutt_dia"])


def _alpair(p, half_h):
    return dict(name="alpair",
                v=half_h - p["alpair_center_from_top"],
                cutout=p["alpair_cutout_dia"],
                flange=p["alpair_flange_dia"],
                depth=p["alpair_flange_depth"],
                pcd=p["alpair_bolt_circle"],
                n=int(float(p["alpair_bolt_count"])),
                bolt=p["m3_t_nutt_dia"])


def _driver_opening(part, p, name, v, cutout, flange, depth, pcd, n, bolt):
    """Opening, flange rebate and bolt ring for one driver.

    The bolt ring is a COUNT and a diameter, so `w170s_bolt_count` means
    something. The port wrote its five holes out as five coordinates, which is
    why changing the count there changed nothing.
    """
    sk = part.sketch(Plane.xz(0), name=f"{name}_cutout")
    sk.circle((0, v), d=cutout)
    part.cut(sk.add(), distance=10000.0, end_mode="symmetric")

    # In the OUTER face, not the inner one. The panel extrudes from local 0
    # (which the assembly puts at the cabinet's inside) along -Y, so the face
    # the driver lands on is at local -wall_thickness. Cut from local 0 and
    # the rebate is 3.4 mm of fresh air in front of the baffle.
    sk = part.sketch(Plane.xz(-p["wall_thickness"]),
                     name=f"{name}_flange_recess")
    sk.circle((0, v), d=flange + p["driver_flange_clearance"])
    part.cut(sk.add(), distance=depth - p["faner_width"], reversed=True)

    sk = part.sketch(Plane.xz(0), name=f"{name}_bolts")
    for i in range(n):
        a = math.pi / 2 + 2 * math.pi * i / n
        sk.circle((float(pcd) / 2 * math.cos(a),
                   float(v) + float(pcd) / 2 * math.sin(a)), d=bolt)
    part.cut(sk.add(), distance=10000.0, end_mode="symmetric")


def back_wall(app, p):
    """The back: same outline as the baffle, no openings, its own skin."""
    part = app.part("back_wall")
    half_w, half_h = p["cabinet_internal_w"] / 2, p["carcass_half_h"]
    t = p["wall_thickness"]

    sk = part.sketch(Plane.xz(0), name="outline")
    sk.centered_rect((0, 0), half_w * 2, half_h * 2)
    part.extrude(sk.add(), distance=t, name="outline")
    part.material(MDF)

    # The back is turned the other way round, so its inside face is local -t.
    top = half_h - (p["alpair_chamber_h"] + t)
    sk = part.sketch(Plane.xz(-t), name="divider_cutout")
    sk.rect((-half_w, top), (half_w, top - t))
    part.cut(sk.add(), distance=p["partition_wall_cutout"], reversed=True)

    _drill_dowels(part, p, "back_wall")

    sk = part.sketch(Plane.xz(0), name="feet")
    for loop in _foot_profile(p, half_w, half_w):
        sk.polyline(loop, close=True)
    part.extrude(sk.add(), distance=t, mode="add")

    return part


# --------------------------------------------------------------------------
# the horizontals
# --------------------------------------------------------------------------

def top_wall(app, p):
    """The top. Built the way up it is fitted — MDF below z = 0, skin above —
    so the assembly places it at the identity and nobody has to know that the
    port's copy was upside down and placed twice."""
    part = app.part("top_wall")
    t = p["wall_thickness"]

    sk = part.sketch(Plane.xy(-t), name="outline")
    sk.centered_rect((0, 0), p["cabinet_internal_w"], p["cabinet_internal_d"])
    part.extrude(sk.add(), distance=t, name="outline")
    part.material(MDF)

    _drill_dowels(part, p, "top_wall")

    return part


def bottom_wall(app, p):
    """The floor: the port's outlet, its bolt ring, and a radius on the way
    out so the air does not turn a sharp corner."""
    part = app.part("bottom_wall")
    t = p["wall_thickness"]

    sk = part.sketch(Plane.xy(0), name="outline")
    sk.centered_rect((0, 0), p["cabinet_internal_w"], p["cabinet_internal_d"])
    part.extrude(sk.add(), distance=t, name="outline")
    part.material(MDF)

    # Sized to pass the FLARE's locating rim — the widest thing that goes
    # through here. Drawn at the tube's diameter it was exactly the spigot's
    # 115 with no clearance; drawn at the spigot's it still caught the rim.
    sk = part.sketch(Plane.xy(t), name="port_hole")
    sk.circle((0, 0), d=p["port_spigot_od"] + p["port_floor_clearance"])
    part.cut(sk.add(), distance=t + p["plug_dia"], reversed=True)

    # No recess for the flare. It bolts to the UNDERSIDE and hangs into the
    # plinth; only the tube passes the hole. The recess tried here first was
    # `port_flare_bottom_dia` deep enough to go clean through 16 mm of floor,
    # which is why the flare stopped clashing with it.

    # Roll the underside of that hole. The flare's throat follows this
    # radius on its way through, and the air leaves without turning a corner.
    bore = part.body().faces(kind="cylinder", where=lambda f: (
        f.radius and abs(f.radius * 2 - (float(p["port_spigot_od"])
                                         + float(p["port_floor_clearance"])))
        < 0.05))
    if bore:
        rim = part.body().edges(kind="circle", where=lambda e: (
            e.radius and abs(e.radius * 2 - (float(p["port_spigot_od"])
                                             + float(p["port_floor_clearance"])))
            < 0.05 and e.midpoint is not None and abs(e.midpoint[2]) < 0.05))
        if rim:
            part.fillet(rim, r=p["port_hole_radius"], name="port hole radius")

    sk = part.sketch(Plane.xy(t), name="port_bolts")
    n = int(float(p["outer_port_bolt_count"]))
    for i in range(n):
        a = math.pi / 2 + 2 * math.pi * i / n
        r = float(p["outer_port_bolt_circle"]) / 2
        sk.circle((r * math.cos(a), r * math.sin(a)), d=p["m4_t_nutt_dia"])
    part.cut(sk.add(), distance=t + p["plug_dia"], reversed=True)

    _drill_dowels(part, p, "bottom_wall")

    # The electronics bolt down here, fore and aft of the port: the flare's
    # bolt circle reaches r=72 and the middle of this panel belongs to the
    # port. The amp's four are its case's OWN pattern, so a bolt passes
    # through both; the supply's two are ours, its case having no hole in the
    # face it stands on.
    sk = part.sketch(Plane.xy(t), name="case_bolts")
    for sx in (-1, 1):
        for sy in (-1, 1):
            sk.circle((p["amp_bolt_x"] / 2 * sx,
                       p["amp_y"] + p["amp_bolt_y"] / 2 * sy),
                      d=p["amp_bolt_dia"])
        sk.circle((p["psu_bolt_dist"] / 2 * sx, p["psu_y"]),
                  d=p["case_bolt_dia"])
    part.cut(sk.add(), distance=t, reversed=True, name="case_bolts")
    return part


def divider(app, p):
    """The Alpair chamber's floor. Lets `partition_wall_cutout` into all four
    uprights, which is why it is bigger than the internal section."""
    part = app.part("divider")
    over = p["partition_wall_cutout"]
    sk = part.sketch(Plane.xy(0), name="outline")
    sk.centered_rect((0, 0), p["cabinet_internal_w"] + over * 2,
                     p["cabinet_internal_d"] + over * 2)
    part.extrude(sk.add(), distance=p["wall_thickness"], name="outline")
    part.material(MDF)
    return part


# Where each wall sits, and which way up. Position only — every part above is
# drawn in the frame it is fitted in, so the one rotation left is the right
# side wall, which is the left one turned over.
def placements(p):
    half_h = float(p["carcass_half_h"])
    x = float(p["cabinet_internal_w"]) / 2 + float(p["wall_thickness"])
    y_front = -(float(p["cabinet_internal_d"]) / 2)
    y_back = float(p["cabinet_internal_d"]) / 2 + float(p["wall_thickness"])
    floor = -2 * half_h
    return [
        ("side_wall", "side_wall L", (-x, 0, -half_h), None),
        ("side_wall", "side_wall R", (x, 0, -half_h), (0, 0, 1, 0)),
        ("front_wall", "front_wall", (0, y_front, -half_h), None),
        ("back_wall", "back_wall", (0, y_back, -half_h), None),
        ("top_wall", "top_wall", (0, 0, 0), None),
        ("bottom_wall", "bottom_wall", (0, 0, floor), None),
        ("divider", "divider",
         (0, 0, -(float(p["alpair_chamber_h"])
                  + 2 * float(p["wall_thickness"]))), None),
    ]
