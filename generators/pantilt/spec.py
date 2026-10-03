"""The head's numbers: the load, the worm pair, the shafts and the bearings.

Every size the parts are built from is a PROJECT variable declared here, in
order, each an expression over the ones before it, so the document carries
the derivation and a change to the load re-sizes what it should. The few
literals are the load itself, the choices that are not sizes (40:1, one
start, q = 10), and material data with its source. `declare()` writes them
(build_base.py, the first stage); `values()` reads them back as floats for
the layout arithmetic of the later stages.

Two choices are a table lookup, which an expression cannot make: the bearing
for a shaft (the first ISO 15 60-series row over the diameter the load needs,
asked of the app with `list_bearings`) and the module (rounded up to a half
module). Each is declared as a number with the minimum it was chosen over
beside it, so the document still shows why.

The sources are docs/modeling/worm-gears.md#sizing-a-pair (the worm pair's
sizing, PETG friction and strength) and docs/tolerances/README.md#printed-parts
(printed clearances) unless a comment says otherwise.

Two bearing pairs carry the head's moments: the tilt shaft's (`main_brg_*`,
sized by the load's torque about the tilt axis) and the pan journal's
(`pan_brg_*`, sized by the load at 3 g sideways about the pan collar, which
is three times the tilt torque; `PANTILT_PAN_BEARING=tilt` sizes it like
the tilt shaft instead, as the head was first built).
"""
import math
import os

# name, expression, comment -- in dependency order.
LOAD = [
    ("load_kg", "1.5", "camera and lens"),
    ("com_up", "80", "mm, the load's CoM above the tilt axis"),
    ("com_fwd", "40", "mm, the load's CoM in front of the tilt axis"),
    ("g_handling", "3", "handling load factor"),
    ("lever", "sqrt(com_up^2 + com_fwd^2)", "mm, the CoM's lever at full tilt"),
    ("tilt_torque_nm", "load_kg * 9.81 * lever / 1000",
     "N.m at 1 g, the worst tilt"),
    ("torque_3g_nmm", "tilt_torque_nm * g_handling * 1000",
     "N.mm, what the gears and shafts are sized for"),
]

MATERIAL = [
    ("nozzle", "0.4", "mm; the print allowance table assumes it"),
    ("print_wall", "8 * nozzle", "mm, eight perimeters (a 3.1 mm "
     "tooth is seven or more)"),
    ("run_gap", "3 * nozzle", "mm, between printed parts that move past "
     "each other"),
    ("petg_layer_mpa", "40", "MPa across the layers (sources read 30-50)"),
    ("petg_allow_mpa", "petg_layer_mpa / 3", "MPa, a third of it"),
    ("petg_e_mpa", "2100", "MPa, Young's modulus: the app's PETG, what "
     "its studies solve with"),
    ("mu_run", "0.25", "PETG on PETG, kinetic (sources read 0.25-0.35)"),
]

CAMERA = [
    # The load, drawn (build_tilt.py models it): a full-frame mirrorless
    # body and a zoom. Here because how far it swings under the tilt axis
    # sizes the pan journal.
    ("cam_w", "130", "mm, body width (along the tilt axis)"),
    ("cam_h", "95", "mm, body height"),
    ("cam_d", "75", "mm, body depth"),
    ("lens_d", "75", "mm, lens barrel"),
    ("cam_rho", "1.04", "g/cm3: plastic_black, what the camera is drawn in"),
    ("lens_l", "(load_kg * 1000000 / cam_rho - cam_w * cam_h * cam_d) / "
               "(pi * lens_d^2 / 4)",
     "mm, long enough that body and lens weigh load_kg"),
    ("cam_m_body", "cam_w * cam_h * cam_d * cam_rho / 1000000", "kg"),
    ("cam_m_lens", "load_kg - cam_m_body", "kg"),
    ("cam_cx", "cam_m_lens * (cam_d / 2 + lens_l / 2) / load_kg",
     "mm, the camera's CoM in front of its tripod socket"),
    ("cam_cz", "cam_h / 2", "mm, the CoM above the socket (lens on axis)"),
    ("seat_x", "com_fwd - cam_cx", "mm, the socket in front of the axis"),
    ("seat_z", "com_up - cam_cz", "mm, the cradle's top above the axis"),
    # The camera screw, sized from the socket it threads into. A camera's
    # 1/4-20 tripod socket is shallow: camera makers' manuals cap a tripod
    # screw at 5.5 mm into the body (Canon's and Nikon's, for instance).
    # ASSUMED here, not measured: measure the camera's.
    ("cam_socket", "5.5", "mm, the camera's 1/4-20 socket depth (assumed: "
     "the 5.5 mm makers' manuals allow a tripod screw)"),
    ("cam_screw_eng", "cam_socket - 0.05 in",
     "mm, the screw's engagement: a 1/4-20 pitch short of the socket's "
     "floor, so it cannot bottom out"),
    ("cam_screw_l", "0.5 in", "mm, the camera screw: ASME B18.3 1/4-20 x "
     "1/2, a stocked length"),
    ("plate_t", "max(2 * print_wall, cam_screw_l - cam_screw_eng)",
     "mm, the cradle's plate: eight perimeters, or what puts the stocked "
     "screw's engagement just inside the socket"),
]

WORM = [
    ("wheel_teeth", "40", "40:1 at one start"),
    ("worm_starts", "1", "one start: self-locking"),
    ("worm_q", "10", "diameter quotient (gamma 5.71 deg)"),
    ("lewis_y", "0.3", "Lewis form factor"),
    ("face_k", "min(2 * (0.5 + sqrt(worm_q + 1)), 0.67 * worm_q)",
     "face width in modules: KHK, held to AGMA's 0.67 d1"),
    ("worm_module_min",
     "(2 * torque_3g_nmm / (wheel_teeth * face_k * lewis_y * "
     "petg_allow_mpa))^(1/3)",
     "mm, Lewis: the 3 g tooth stress at the allowable"),
    # worm_module itself is a rounding (declared in declare()).
]

WORM_DERIVED = [
    ("worm_d1", "worm_q * worm_module", "mm, worm pitch diameter"),
    ("worm_tip_d", "worm_d1 + 2 * worm_module", "mm, the worm blank"),
    ("worm_root_d", "worm_d1 - 2.5 * worm_module", "mm"),
    ("wheel_d2", "wheel_teeth * worm_module", "mm, wheel pitch diameter"),
    ("wheel_rim_d", "worm_module * (wheel_teeth + 3)", "mm, the wheel blank"),
    ("wheel_root_d", "wheel_d2 - 2.5 * worm_module", "mm"),
    ("centre_dist", "(worm_d1 + wheel_d2) / 2", "mm, worm to wheel axis"),
    ("wheel_face", "face_k * worm_module", "mm, the wheel's rim length"),
    ("worm_len", "pi * worm_module * (4.5 + 0.02 * wheel_teeth)",
     "mm, KHK's worm length"),
    ("worm_backlash", "0.25", "mm axial, a printed pair (sources: 0.2-0.3)"),
    ("wheel_ft_3g", "2 * torque_3g_nmm / wheel_d2",
     "N, tooth force at 3 g: the worm's axial load"),
    ("eta_run", "(1 / worm_q) * (1 - mu_run / worm_q) / (1 / worm_q + mu_run)",
     "worm-driving efficiency tan(g)/tan(g + rho), tan rho = mu"),
    ("worm_torque_nm",
     "tilt_torque_nm / (wheel_teeth / worm_starts) / eta_run",
     "N.m at the knob to hold the load at 1 g"),
    ("finger_n", "8", "N, a comfortable fingertip pull on a knob"),
    ("knob_d", "2 * worm_torque_nm * 1000 / finger_n",
     "mm, so the 1 g load turns at that pull"),
]

SHAFTS = [
    ("shaft_d_min",
     "(32 * torque_3g_nmm / (pi * petg_allow_mpa))^(1/3)",
     "mm, a PETG shaft carrying the 3 g moment (pan) or torque (tilt)"),
    ("worm_bend_nmm",
     "wheel_ft_3g * (worm_d1 / 4 + worm_len * tan(20) / 4)",
     "N.mm, the axial load's couple and the separating force over the span"),
    ("worm_shaft_d_min",
     "(32 * worm_bend_nmm / (pi * petg_allow_mpa))^(1/3)",
     "mm, the worm's journals"),
]


PAN = [
    # The layout the pan journal carries (build_yoke.py builds it, and
    # checks pan_lever against where it put the tilt axis).
    ("block_h", "worm_brg_D + 2 * print_wall",
     "mm, a worm block: the bearing and a wall either side"),
    ("yoke_base_t", "4 * print_wall", "mm"),
    ("cam_clear", "4 * run_gap", "mm, the camera over the yoke at full tilt"),
    ("pan_lever",
     "block_h / 2 - wheel_face / 2 + yoke_base_t + cam_swing + cam_clear + "
     "lever",
     "mm, the load's CoM at its highest over the pan collar: the yoke's base "
     "over the pan blocks, the camera's swing and clearance under the tilt "
     "axis, and the CoM's lever over it"),
    ("pan_moment_3g_nmm", "load_kg * 9.81 * g_handling * pan_lever",
     "N.mm, the load at 3 g sideways about the pan collar"),
    ("pan_shaft_d_min",
     "(32 * pan_moment_3g_nmm / (pi * petg_allow_mpa))^(1/3)",
     "mm, the yoke's PETG pan journal, a cantilever from its collar, "
     "carrying that moment"),
]

PAN_DERIVED = [
    ("pan_collar_d", "(pan_brg_da_min + pan_brg_da_max) / 2",
     "mm, the yoke's collar: bears on the upper pan bearing's inner ring, "
     "mid-way in SKF's d_a"),
    ("pan_relief_d", "(pan_brg_da_max + pan_brg_Da_max) / 2",
     "mm, the housings' shoulders round the pan bearings' outer rings: "
     "inside D_a max, clear of the inner ring (d_a max) so it turns free"),
]


def _declare(part, rows):
    for name, expr, comment in rows:
        part.param(name, expr, comment=comment, scope="project")


def bearing_for(app, d_min, series="60"):
    """The first ISO 15 row of `series` whose bore takes `d_min`."""
    rows = app.call("list_bearings", series=series,
                    min_bore_mm=float(d_min))["bearings"]
    return rows[0]


def swing(S):
    """The deepest the camera or the cradle reaches below the tilt axis,
    anywhere in +-90 degrees: max over the tilt of x sin(t) - z cos(t), over
    the corners of their outlines in the plane the tilt turns them in (the
    lens is a cylinder along X, so its outline there is a rectangle)."""
    sx, sz, d = S["seat_x"], S["seat_z"], S["cam_d"]
    pts = []
    for x in (sx - d / 2, sx + d / 2):
        for z in (sz - S["plate_t"], sz + S["cam_h"]):
            pts.append((x, z))
    lx0 = sx + d / 2
    for x in (lx0, lx0 + S["lens_l"]):
        for z in (sz + S["cam_cz"] - S["lens_d"] / 2,
                  sz + S["cam_cz"] + S["lens_d"] / 2):
            pts.append((x, z))
    deepest = 0.0
    for i in range(-900, 901):
        t = math.radians(i / 10.0)
        for x, z in pts:
            deepest = max(deepest, x * math.sin(t) - z * math.cos(t))
    return deepest


def swing_expr():
    """`swing` as an EXPRESSION over the same variables, so the tilt axis
    follows the camera when `load_kg` (and with it the lens) changes. Over
    +-90 degrees, x sin(t) - z cos(t) peaks at the point's distance from
    the axis when it starts below it (z <= 0) and at |x| otherwise (the
    ends of the range): sqrt(x^2 + min(z, 0)^2) either way. Declared as the
    number `swing` sampled, it was a 1.5 kg camera's 160 mm under a 1.8 kg
    one's 200, and the regenerated arms 40 mm short of the lens."""
    xs = ("seat_x - cam_d / 2", "seat_x + cam_d / 2",
          "seat_x + cam_d / 2 + lens_l")
    body_z = ("seat_z - plate_t", "seat_z + cam_h")
    lens_z = ("seat_z + cam_cz - lens_d / 2", "seat_z + cam_cz + lens_d / 2")
    pts = [(x, z) for x in xs[:2] for z in body_z] + \
          [(x, z) for x in xs[1:] for z in lens_z]
    return "max(" + ", ".join(f"sqrt(({x})^2 + min({z}, 0)^2)"
                              for x, z in pts) + ")"


# Which load sizes the pan journal (`PANTILT_PAN_BEARING`):
#   moment -- the load at 3 g sideways about the pan collar (the default:
#             the journal is a cantilever under the head's overturning
#             moment, and phase 6 found it most of the lens's deflection);
#   tilt   -- the tilt shaft's size, as the head was first sized (pan
#             carries no gravity torque); kept to compare against.
PAN_SIZING = ("moment", "tilt")


def _pan_sizing():
    v = os.environ.get("PANTILT_PAN_BEARING", "moment")
    if v not in PAN_SIZING:
        raise SystemExit(f"PANTILT_PAN_BEARING={v!r}: the choices are "
                         f"{', '.join(PAN_SIZING)}")
    return v


def _bearing(app, part, key, dmin_name):
    """Declare `key`_brg_{d,D,B}: a table lookup, so numbers, with the
    reason beside them; and the row's abutment limits (`key`_brg_da_min,
    _da_max, _Da_max: SKF's, from list_bearings), which every shoulder
    and relief round the bearing is an expression over."""
    dmin = part.value_of(dmin_name)
    row = bearing_for(app, dmin)
    what = f"ISO 15 {row['designation']}, first 60-series row over {dmin_name}"
    part.param(f"{key}_brg_d", row["d_mm"], comment=what, scope="project")
    part.param(f"{key}_brg_D", row["D_mm"], comment=what, scope="project")
    part.param(f"{key}_brg_B", row["B_mm"], comment=what, scope="project")
    for name, field in (("da_min", "d_a_min_mm"), ("da_max", "d_a_max_mm"),
                        ("Da_max", "D_a_max_mm")):
        if row.get(field) is None:
            raise SystemExit(f"{row['designation']}: list_bearings gives no "
                             f"{field}, and the shoulders are sized by it")
        part.param(f"{key}_brg_{name}", row[field],
                   comment=f"mm, {row['designation']}'s abutment limit "
                           f"{field[:-3]} (list_bearings: SKF)",
                   scope="project")
    return row


def _ring(app, part, key):
    """Declare `key`_ring_{s,m,n}: the DIN 471 ring and groove for the
    `key` bearing's shaft (list_retaining_rings), and `key`_ring_land, the
    journal it needs past the bearing: the groove and its edge distance."""
    d = part.value_of(f"{key}_brg_d")
    rows = app.call("list_retaining_rings", standard="din471",
                    min_size_mm=float(d))["rings"]
    row = rows[0]
    if abs(row["size_mm"] - d) > 1e-6:
        raise SystemExit(f"no DIN 471 ring for the Ø{d:g} {key} shaft")
    what = f"{row['designation']}, the {key} shaft's (list_retaining_rings)"
    part.param(f"{key}_ring_s", row["s_mm"], comment="mm, ring thickness: " +
               what, scope="project")
    part.param(f"{key}_ring_m", row["groove"]["m_mm"],
               comment="mm, groove width: " + what, scope="project")
    part.param(f"{key}_ring_n", row["groove"]["n_mm"],
               comment="mm, edge distance past the groove: " + what,
               scope="project")
    part.param(f"{key}_ring_land", f"{key}_ring_m + {key}_ring_n",
               comment="mm, the journal past a bearing its ring locates",
               scope="project")
    return row


def declare(app, part):
    """Write every project variable; returns values()."""
    _declare(part, LOAD + MATERIAL + CAMERA + WORM)
    m_min = part.value_of("worm_module_min")
    part.param("worm_module", f"ceil(worm_module_min * 2) / 2",
               comment="mm, worm_module_min up to the next half module",
               scope="project")
    _declare(part, WORM_DERIVED + SHAFTS)
    _bearing(app, part, "main", "shaft_d_min")
    _ring(app, part, "main")
    _bearing(app, part, "worm", "worm_shaft_d_min")
    # The pan journal: how high the load stands over it is the head's
    # layout (build_yoke.py's z_mesh_pan .. tilt_z, checked there), written
    # here over what decides it, none of which is the pan bearing.
    part.param("cam_swing", swing_expr(),
               comment="mm, the deepest the camera or cradle reaches below "
                       "the tilt axis over +-90 deg (spec.swing_expr)",
               scope="project")
    sampled = swing(values(part))
    if abs(part.value_of("cam_swing") - sampled) > 1e-3:
        raise SystemExit(f"cam_swing: the expression says "
                         f"{part.value_of('cam_swing'):.4f} mm, the swing "
                         f"sampled every 0.1 deg {sampled:.4f}")
    _declare(part, PAN)
    sizing = _pan_sizing()
    over = "pan_shaft_d_min" if sizing == "moment" else "shaft_d_min"
    row = _bearing(app, part, "pan", over)
    _ring(app, part, "pan")
    _declare(part, PAN_DERIVED)
    print(f"module {m_min:.3f} -> {part.value_of('worm_module')}; pan "
          f"journal sized by the {sizing} ({over} "
          f"{part.value_of(over):.1f} mm): ISO 15 {row['designation']}; the "
          f"3 g sideways moment needs {part.value_of('pan_shaft_d_min'):.1f}")
    return values(part)


def values(part):
    """Every project variable, as {name: float}."""
    return {name: float(v["value"]) for name, v in part.vars().items()}


def designation(app, d):
    """The ISO 15 designation a bore was chosen as (for fit_bearing)."""
    return bearing_for(app, d)["designation"]
