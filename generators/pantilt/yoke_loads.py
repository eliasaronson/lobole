"""What the yoke carries: the camera's load at 3 g, followed down its path.

camera -> cradle -> tilt shaft -> the two arm bearings, and the worm pair:
the tilt wheel's tooth force at its mesh with the worm, which the worm's
blocks hand to the ledge. The shaft, wheel, cradle and camera are one rigid
body on two bearings (radial at both, axial at the +Y one, where the wheel
and its collar are); its equilibrium gives what each seat and the ledge
push on the yoke. The yoke sits on the pan bearings, which is where a study
fixes it.

`case()` returns the forces ON THE YOKE (newtons, world axes) and the
geometry the lens is carried by; `lens()` turns what the yoke's seats did
(probe_study) into how far the lens moved. Everything is from the project
variables (spec.py, build_tilt.py, build_yoke.py).
"""
import math


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def _add(*vs):
    return tuple(sum(c) for c in zip(*vs))


def _scale(k, v):
    return tuple(k * c for c in v)


def _rot_y(p, t):
    """`p` (x, z) about the tilt axis by `t` radians, nose down positive."""
    x, z = p
    return (x * math.cos(t) + z * math.sin(t), -x * math.sin(t) + z * math.cos(t))


def geometry(S, y_pos, y_neg):
    """Where things are on the tilt axis: the bearings' centres (from the
    seats' own faces), the mesh point, the worm's axis."""
    zt = S["tilt_z"]
    r2 = S["wheel_d2"] / 2
    return {
        "axis": (0.0, 0.0, zt),
        "y_pos": y_pos, "y_neg": y_neg,
        "r2": r2,
        "mesh": (0.0, S["tilt_mesh_y"], zt - r2),
        "worm": (0.0, S["tilt_mesh_y"], zt - S["centre_dist"]),
        # the CoM and the lens's front, in front of and above the axis at
        # zero tilt
        "com": (S["com_fwd"], S["com_up"]),
        "lens": (S["seat_x"] + S["cam_d"] / 2 + S["lens_l"],
                 S["seat_z"] + S["cam_cz"]),
    }


def worst_tilt_deg(S):
    """The tilt at which the CoM is furthest out in front: its lever."""
    return math.degrees(math.atan2(S["com_up"], S["com_fwd"]))


def case(S, G, name, tilt_deg, accel):
    """The forces on the yoke for the camera at `tilt_deg`, accelerated by
    `accel` g in world axes (a 3-vector of g, e.g. (0, 0, -3))."""
    t = math.radians(tilt_deg)
    zt = S["tilt_z"]
    m = S["load_kg"]
    W = _scale(m * 9.81, accel)  # N, on the camera's CoM
    cx, cz = _rot_y(G["com"], t)
    com = (cx, 0.0, zt + cz)
    O = G["axis"]
    # The gear holds the tilt: its tooth force cancels the load's moment
    # about the axis (y). At the mesh, under the axis, a force along x is
    # the tangential one; the separating force pushes the wheel away from
    # the worm below (+z), tan(20 deg) of it.
    m_y = _cross(_add(com, _scale(-1, O)), W)[1]
    ft = -m_y / (-G["r2"])  # (r x F).y = -r2 * Fx at the mesh
    fp = (ft, 0.0, abs(ft) * math.tan(math.radians(20.0)))
    ext = [(com, W), (G["mesh"], fp)]
    fsum = _add(*[f for _, f in ext])
    msum = _add(*[_cross(_add(p, _scale(-1, O)), f) for p, f in ext])
    ya, yb = G["y_pos"], G["y_neg"]
    # The bearings' reactions on the shaft: radial at both, axial at +Y.
    # x: ra + rb = -Fx, ya ra + yb rb = Mz;  z: ra + rb = -Fz,
    # ya ra + yb rb = -Mx.
    def pair(total, moment):
        rb = (moment - ya * total) / (yb - ya)
        return total - rb, rb
    rax, rbx = pair(-fsum[0], msum[2])
    raz, rbz = pair(-fsum[2], -msum[0])
    ray = -fsum[1]
    on_yoke = {
        "bore_pos": (-rax, 0.0, -raz),
        "bore_neg": (-rbx, 0.0, -rbz),
        "shoulder_pos": (0.0, -ray, 0.0),
        "ledge": _scale(-1, fp),
    }
    total = _add(*on_yoke.values())
    assert all(abs(a - b) < 1e-6 for a, b in zip(total, W)), (total, W)
    return {"name": name, "tilt_deg": tilt_deg, "accel_g": accel,
            "loads": on_yoke, "tooth_force_n": fp,
            "lens": lens_point(S, G, t)}


def lens_point(S, G, t):
    lx, lz = _rot_y(G["lens"], t)
    return (lx, 0.0, S["tilt_z"] + lz)


def lens(G, c, pos, neg, worm_x, axial_y):
    """How far the lens moved, from what the seats did: `pos`, `neg` the
    bearing bores' translations (probe_study, mm), `worm_x` the ledge's
    motion along x carried to the worm's axis, `axial_y` the +Y seat's
    shoulder along y. The camera is one body with the shaft; it turns about
    its own axis by what the worm let the wheel's teeth move."""
    ya, yb = G["y_pos"], G["y_neg"]
    span = ya - yb

    def shaft(y):
        k = (y - yb) / span
        return tuple(b + (a - b) * k for a, b in zip(pos, neg))
    rx = (pos[2] - neg[2]) / span
    rz = -(pos[0] - neg[0]) / span
    ry = (shaft(G["mesh"][1])[0] - worm_x) / G["r2"]
    theta = (rx, ry, rz)
    u0 = shaft(0.0)
    u0 = (u0[0], axial_y, u0[2])
    arm = _add(c["lens"], _scale(-1, G["axis"]))
    d = _add(u0, _cross(theta, arm))
    return {"lens_mm": d, "lens_magnitude_mm": math.sqrt(sum(x * x for x in d)),
            "camera_turn_rad": theta,
            "camera_pitch_arcmin": math.degrees(ry) * 60.0}
