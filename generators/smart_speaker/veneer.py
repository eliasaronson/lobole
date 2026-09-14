"""The plywood coverings, as their own parts, referencing the walls.

They used to be an extra body inside each wall — a second solid in a part
called `front_wall`, which is not what they are. They are separate pieces of
3.6 mm ply you cut and glue on, they belong in the BOM as such, and the CAM
that cuts a wall should not have them in the way.

## How they follow the walls

Each carries a `Derived` copy of its wall as a REFERENCE (`reference: true` —
a ghost, absent from mass, BOM, export, CAM and booleans). That is the
inter-part link: the covering knows which wall it belongs to, and `get_state`
reports `stale` when that wall moves, so `update_derived` retakes it.

What the ghost does NOT do is give the profile. Projecting its outer face into
a sketch arrives as locked CONSTRUCTION geometry and an extrude refuses it —
"sketch has no closed profile". So the outline is drawn by
`walls.silhouette()`, the same function the wall itself uses. One rule, two
parts: the covering cannot end up a different shape from the panel it covers,
which is what mattered.

## The mitre

Each covering is chamfered 45 degrees along the edges that meet another
covering, with the setback equal to its own thickness — which is exactly a
mitre. Two of them meeting at a corner then close on their own faces and
nothing shows an edge.

That replaces the "owner" scheme this generator used before, where the side
coverings ran the full depth and the front and back ran between them. That
worked (the sweep was empty) but it is not what the design says: a butt joint
shows the side piece's edge at every front corner, and the point of mitring
is that it does not.

Which edges are mitred is a property of the joint, not of the part, so it is
listed here per covering: the uprights mitre at their vertical corners and
along the top, the top covering mitres all four sides. Nothing mitres at the
plinth, where there is no neighbour.
"""
from designer_sdk import SdkError

# Which way each covering faces, and which of its edges meet a neighbour.
# `outward` is the wall's own local direction to its outside face.
COVERINGS = [
    ("veneer_side", "side_wall", (-1, 0, 0), ("front", "back", "top")),
    ("veneer_front", "front_wall", (0, -1, 0), ("left", "right", "top")),
    ("veneer_back", "back_wall", (0, 1, 0), ("left", "right", "top")),
    ("veneer_top", "top_wall", (0, 0, 1), ("left", "right", "front", "back")),
]


def _outer_face(part, outward, tol=1e-3):
    """The ghost's outermost planar face pointing `outward` — the one the
    covering is glued to."""
    axis = [i for i in range(3) if outward[i]][0]
    sign = outward[axis]
    best = None
    for body in part.bodies():
        for f in body.faces(kind="plane"):
            nvec = f.normal
            if nvec is None or abs(nvec[axis] - sign) > tol:
                continue
            if sum(abs(nvec[i]) for i in range(3) if i != axis) > tol:
                continue
            if best is None or f.centroid[axis] * sign > best.centroid[axis] * sign:
                best = f
    return best


# The plane each covering is drawn on, in its wall's own local frame — which
# is also the covering's, because the ghost comes in at the identity.
def _face_plane(p, source, Plane):
    t = p["wall_thickness"]
    if source == "side_wall":
        return Plane.yz(0), True          # wall runs x 0..t, outside at 0
    if source == "front_wall":
        return Plane.xz(-t), False        # runs y 0..-t, outside at -t
    if source == "back_wall":
        return Plane.xz(0), True          # runs y 0..-t, outside at 0
    return Plane.xy(0), False             # top runs z -t..0, outside at 0


def build(app, p, name, source_part, outward, mitres):
    """One covering: a ghost of its wall for reference, the wall's own
    outline, 3.6 mm of ply, and a 45 degree mitre where it meets a
    neighbour."""
    from designer_sdk import Plane
    import walls
    part = app.part(name)
    part.activate()
    app.add_feature(feature={"type": "Derived",
                             "name": f"{source_part.name} (reference)",
                             "source_part": source_part.id,
                             "reference": True})
    plane, rev = _face_plane(p, source_part.name, Plane)
    sk = part.sketch(plane, name="outline")
    walls.silhouette(sk, p, source_part.name)
    part.extrude(sk.add(), distance=p["finish_wall_thickness"],
                 reversed=rev, name="covering")
    part.material("plywood_birch")
    _openings(part, p, source_part.name, plane, rev)
    _mitre(part, p, outward, mitres)
    return part


def _openings(part, p, source, plane, rev):
    """The baffle covering's two driver holes.

    At the FLANGE diameter, not the cut-out: the flange passes through the ply
    and lands on the rebate below, so the two together come to
    `*_flange_depth`. Left out, the covering closes over both openings and the
    drivers report 39,443 and 19,798 mm3 of interference against a sheet of
    plywood.
    """
    if source != "front_wall":
        return
    import walls
    half_h = p["carcass_half_h"]
    sk = part.sketch(plane, name="driver openings")
    for d in (walls._w170s(p, half_h), walls._alpair(p, half_h)):
        sk.circle((0, d["v"]), d=d["flange"] + p["driver_flange_clearance"])
    part.cut(sk.add(), distance=p["finish_wall_thickness"], reversed=rev,
             name="driver openings")


def _mitre(part, p, outward, which):
    """Chamfer 45 degrees, setback = thickness, on the edges that meet a
    neighbour.

    Selected by WHERE the edge is rather than by index: an edge on the
    covering's outer face whose position is extreme along an axis the joint
    runs across. A chamfer set is the one thing in this generator that has
    taken the app down, so the set is kept to the joints that need it.
    """
    thick = float(p["finish_wall_thickness"])
    axis = [i for i in range(3) if outward[i]][0]
    sign = outward[axis]
    # By NAME: the part has two bodies, the covering and the wall it is
    # referenced to, and the ghost's edges are not ours to chamfer.
    body = part.body(name="covering")
    lo, hi = part.bbox()
    # A mitre is cut from the OUTER face inward, so only the outer edge of
    # each corner is chamfered. Taking both edges asks for 2 x 3.45 mm of
    # setback out of a 3.6 mm panel and the kernel refuses the lot
    # ("radius too large for the geometry").
    outer = hi[axis] if sign > 0 else lo[axis]
    picks = []
    for edge in body.edges(kind="line"):
        m = edge.midpoint
        if m is None or edge.length < thick * 3:
            continue
        if abs(m[axis] - outer) > 0.05:
            continue
        for side in which:
            k, want = _side(side, axis)
            if k is None:
                continue
            at = hi[k] if want > 0 else lo[k]
            if abs(m[k] - at) < 0.05:
                picks.append(edge)
                break
    if picks:
        part.chamfer(picks, r=p["finish_wall_thickness"] - p["mitre_land"],
                     name="mitre")


def _side(side, face_axis):
    """(axis index, +1/-1) for a named neighbour, skipping the one the
    covering itself faces."""
    table = {"left": (0, -1), "right": (0, 1), "front": (1, -1),
             "back": (1, 1), "top": (2, 1), "bottom": (2, -1)}
    k, want = table[side]
    return (None, None) if k == face_axis else (k, want)


def build_all(app, p, walls_by_name):
    made = []
    for name, source, outward, mitres in COVERINGS:
        made.append(build(app, p, name, walls_by_name[source], outward,
                          mitres))
    return made


def placements(p):
    """Each covering sits exactly where its wall does — it was drawn on that
    wall's own face, in that wall's own frame, so the transform is the
    wall's."""
    import walls
    by_wall = {src: name for name, src, _o, _m in COVERINGS}
    out = []
    for part_name, occ_name, at, quat in walls.placements(p):
        if part_name in by_wall:
            suffix = occ_name[len(part_name):]
            out.append((by_wall[part_name],
                        by_wall[part_name] + suffix, at, quat))
    return out
