"""How a part is seated: say the face and the direction, not the quaternion.

Every orientation bug in this generator came from writing a quaternion by
hand and reasoning about which way local +Z ended up pointing. The T-nuts went
in barrel-first into open air, the brace's nuts came out lying on their sides
8 mm below the panel they fix, and the lower port flare was placed mouth-up
through the cabinet floor. None of that is caught by a clash sweep — a part
floating in a cavity touches nothing and clashes with nothing.

Worse, one of them was INTRODUCED while chasing a clash: a T-nut whose barrel
sat in its hole reported 222 mm3 of interference (the barrel was drawn wider
than the hole), and flipping the nut over made the number go away by lifting
it out of the wood. The clash count improved and the part stopped being
mounted.

So orientation is stated the way the shop would state it:

    seat(part_z_extent, on=-160.0, axis="-y", flange=1.5)

"put this part against the face at y = -160, driving INTO -y, with its first
1.5 mm proud of that face" — and the position and the quaternion come out of
that. `verify.py` then checks the result the only way that means anything:
every occurrence must touch the structure (`contact_report` components == 1).
"""
import math

# The six directions a part is ever driven in here, as (vector, quaternion).
# The quaternion is the rotation taking the part's local +Z onto the vector;
# each is written out rather than derived so it can be read and checked.
_AXES = {
    "+z": ((0.0, 0.0, 1.0), (0.0, 0.0, 0.0, 1.0)),          # identity
    "-z": ((0.0, 0.0, -1.0), (1.0, 0.0, 0.0, 0.0)),         # 180 about X
    "-y": ((0.0, -1.0, 0.0), (0.7071067811865476, 0.0, 0.0,
                              0.7071067811865476)),          # +90 about X
    "+y": ((0.0, 1.0, 0.0), (-0.7071067811865476, 0.0, 0.0,
                             0.7071067811865476)),           # -90 about X
    "+x": ((1.0, 0.0, 0.0), (0.0, 0.7071067811865476, 0.0,
                             0.7071067811865476)),           # +90 about Y
    "-x": ((-1.0, 0.0, 0.0), (0.0, -0.7071067811865476, 0.0,
                              0.7071067811865476)),          # -90 about Y
}


def quat(axis):
    """The rotation putting the part's local +Z along `axis`."""
    try:
        return list(_AXES[axis][1])
    except KeyError:
        raise ValueError(f"axis {axis!r} is not one of {sorted(_AXES)}")


def direction(axis):
    return _AXES[axis][0]


def seat(on, axis, at=(0.0, 0.0), proud=0.0):
    """Where to put a part's ORIGIN so it seats on a face.

    `on`      the face's coordinate along `axis`'s own axis (a plane, in mm)
    `axis`    which way the part drives INTO the material, e.g. "-y"
    `at`      the other two coordinates, in the order they appear in (x, y, z)
              with the driven one removed
    `proud`   how much of the part stands out of that face before it enters —
              a T-nut's flange thickness. The part is built along local +Z
              from its origin, so the origin sits `proud` back UP the axis.

    Returns (position, quaternion).
    """
    d = direction(axis)
    idx = [i for i in range(3) if d[i] != 0.0][0]
    pos = [0.0, 0.0, 0.0]
    rest = [i for i in range(3) if i != idx]
    for slot, i in enumerate(rest):
        pos[i] = float(at[slot])
    # Driving along -y means local +Z points at -y, so the origin is `proud`
    # further along +y than the face. Same for every axis: back off along -d.
    pos[idx] = float(on) - d[idx] * float(proud)
    return tuple(pos), quat(axis)


def ring(pcd, n, start_deg=90.0):
    """The (u, v) of n points on a pitch circle, first at `start_deg`.

    Measured the way a sketch measures: from +u toward +v. Both the panel's
    holes and the fixings that go in them come from here, so they cannot
    drift apart.
    """
    out = []
    for i in range(int(n)):
        a = math.radians(start_deg) + 2 * math.pi * i / int(n)
        out.append((float(pcd) / 2 * math.cos(a), float(pcd) / 2 * math.sin(a)))
    return out
