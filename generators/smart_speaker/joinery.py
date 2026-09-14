"""Every dowelled joint in the carcass, from one rule.

The panels used to drill their own dowels independently and the holes did not
meet: the side walls put theirs at y = +/-126 and the top wall put its at
+/-110, the top wall had four holes on two of its four edges, and the baffle
had none that registered at all. Nothing lined up, so there was nothing to
mate to — which is why every part was placed by a computed transform instead.

So the joint is the unit here, not the panel. A joint knows its own length and
puts a dowel `plug_side_dist` in from each end; both panels then ask this
module where their holes go, and they cannot disagree because it is the same
number reaching both.

    joint            runs along     dowels at
    top/bottom to a side wall    Y   +/-(internal_d/2 - plug_side_dist)
    top/bottom to baffle/back    X   +/-(internal_w/2 - plug_side_dist)
    baffle/back to a side wall   Z   +/-(carcass_half_h - plug_side_dist)

Every hole is half a wall in from the face it is drilled through, which is
what puts the two halves of a joint on the same axis.

## Frames, and why they are here too

Each joint also names the FRAME each panel carries for it — built from that
joint's two bores, so `mates.py` fastens panels together by the holes the
dowels actually go in. The frame names are generated here so the two sides
cannot drift apart.
"""

# Which panels meet, and along which axis the joint runs.
#
# The pairs are written from the side wall outwards because the left side
# wall is the carcass datum; `mates.py` walks them as a TREE (see there), not
# as twelve independent constraints.
JOINTS = [
    ("side_wall", "top_wall", "depth"),
    ("side_wall", "bottom_wall", "depth"),
    ("side_wall", "front_wall", "height"),
    ("side_wall", "back_wall", "height"),
    ("front_wall", "top_wall", "width"),
    ("front_wall", "bottom_wall", "width"),
    ("back_wall", "top_wall", "width"),
    ("back_wall", "bottom_wall", "width"),
]


def spacing(p, along):
    """Half the distance between a joint's two dowels."""
    inset = float(p["plug_side_dist"])
    if along == "depth":
        return float(p["cabinet_internal_d"]) / 2 - inset
    if along == "width":
        return float(p["cabinet_internal_w"]) / 2 - inset
    if along == "height":
        return float(p["carcass_half_h"]) - inset
    raise ValueError(along)


def frame_name(a, b):
    """The frame both panels of a joint carry, named the same on each."""
    return f"joint {'-'.join(sorted((a, b)))}"


def _mid(p):
    return float(p["wall_thickness"]) / 2


# --------------------------------------------------------------------------
# where each panel's holes go, in ITS OWN sketch frame
#
# Each entry is (frame_name, plane_key, [(u, v), ...]). `plane_key` is read by
# walls.py, which owns the planes; this module owns only the positions.
# --------------------------------------------------------------------------

def side_wall(p):
    """All eight, in the inner face — the side wall receives every joint it
    is in, because the other four panels sit between the two of them."""
    half_h = float(p["carcass_half_h"])
    mid = _mid(p)
    edge = float(p["cabinet_internal_d"]) / 2 + mid
    d, h = spacing(p, "depth"), spacing(p, "height")
    return [
        (frame_name("side_wall", "top_wall"), "inner",
         [(-d, half_h - mid), (d, half_h - mid)]),
        (frame_name("side_wall", "bottom_wall"), "inner",
         [(-d, -half_h + mid), (d, -half_h + mid)]),
        (frame_name("side_wall", "front_wall"), "inner",
         [(-edge, -h), (-edge, h)]),
        (frame_name("side_wall", "back_wall"), "inner",
         [(edge, -h), (edge, h)]),
    ]


def upright(p, which):
    """The baffle or the back: four in its inner face for the horizontals,
    and four in its END faces for the side walls."""
    half_h = float(p["carcass_half_h"])
    mid = _mid(p)
    w, h = spacing(p, "width"), spacing(p, "height")
    other = "side_wall"
    return [
        (frame_name(which, "top_wall"), "inner",
         [(-w, half_h - mid), (w, half_h - mid)]),
        (frame_name(which, "bottom_wall"), "inner",
         [(-w, -half_h + mid), (w, -half_h + mid)]),
        # In the end faces, one per side wall. u is the panel's own thickness
        # direction, so the hole sits on the joint's AXIS, not on its face.
        (frame_name(which, other) + " -x", "end_minus",
         [(-mid, -h), (-mid, h)]),
        (frame_name(which, other) + " +x", "end_plus",
         [(-mid, -h), (-mid, h)]),
    ]


def horizontal(p, which):
    """The top or the bottom: two in each of its four edges."""
    mid = _mid(p)
    # v is measured from the panel's own datum face; the top wall is built
    # DOWN from z = 0 and the bottom wall UP from z = 0, so the hole is half a
    # wall either side of it.
    v = -mid if which == "top_wall" else mid
    d, w = spacing(p, "depth"), spacing(p, "width")
    return [
        (frame_name("side_wall", which) + " x+", "edge_x_plus",
         [(-d, v), (d, v)]),
        (frame_name("side_wall", which) + " x-", "edge_x_minus",
         [(-d, v), (d, v)]),
        (frame_name(which, "front_wall"), "edge_y_minus", [(-w, v), (w, v)]),
        (frame_name(which, "back_wall"), "edge_y_plus", [(-w, v), (w, v)]),
    ]


def for_panel(p, name):
    if name == "side_wall":
        return side_wall(p)
    if name in ("front_wall", "back_wall"):
        return upright(p, name)
    if name in ("top_wall", "bottom_wall"):
        return horizontal(p, name)
    return []


# The tree `mates.py` fastens along: the left side wall is fixed and every
# other panel hangs off it by one joint. A carcass has twelve dowelled joints
# and six panels, so fastening all twelve would over-constrain it five times
# over; a tree says the same thing once.
# (moving occurrence, frame on it, fixed occurrence, frame on that)
TREE = [
    ("front_wall", frame_name("front_wall", "side_wall") + " -x",
     "side_wall L", frame_name("side_wall", "front_wall")),
    ("back_wall", frame_name("back_wall", "side_wall") + " -x",
     "side_wall L", frame_name("side_wall", "back_wall")),
    ("top_wall", frame_name("side_wall", "top_wall") + " x-",
     "side_wall L", frame_name("side_wall", "top_wall")),
    ("bottom_wall", frame_name("side_wall", "bottom_wall") + " x-",
     "side_wall L", frame_name("side_wall", "bottom_wall")),
    # The right side wall closes the box off the top panel's other edge.
    ("side_wall R", frame_name("side_wall", "top_wall"),
     "top_wall", frame_name("side_wall", "top_wall") + " x+"),
]
