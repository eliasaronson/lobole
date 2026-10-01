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
"""

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
    ("mu_run", "0.25", "PETG on PETG, kinetic (sources read 0.25-0.35)"),
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


def _declare(part, rows):
    for name, expr, comment in rows:
        part.param(name, expr, comment=comment, scope="project")


def bearing_for(app, d_min, series="60"):
    """The first ISO 15 row of `series` whose bore takes `d_min`."""
    rows = app.call("list_bearings", series=series,
                    min_bore_mm=float(d_min))["bearings"]
    return rows[0]


def declare(app, part):
    """Write every project variable; returns values()."""
    _declare(part, LOAD + MATERIAL + WORM)
    m_min = part.value_of("worm_module_min")
    part.param("worm_module", f"ceil(worm_module_min * 2) / 2",
               comment="mm, worm_module_min up to the next half module",
               scope="project")
    _declare(part, WORM_DERIVED + SHAFTS)
    # The bearings: a table lookup, so a number, with its reason beside it.
    for key, dmin_name in (("main", "shaft_d_min"),
                           ("worm", "worm_shaft_d_min")):
        dmin = part.value_of(dmin_name)
        row = bearing_for(app, dmin)
        what = f"ISO 15 {row['designation']}, first 60-series row over {dmin_name}"
        part.param(f"{key}_brg_d", row["d_mm"], comment=what, scope="project")
        part.param(f"{key}_brg_D", row["D_mm"], comment=what, scope="project")
        part.param(f"{key}_brg_B", row["B_mm"], comment=what, scope="project")
    print(f"module {m_min:.3f} -> {part.value_of('worm_module')}")
    return values(part)


def values(part):
    """Every project variable, as {name: float}."""
    return {name: float(v["value"]) for name, v in part.vars().items()}


def designation(app, d):
    """The ISO 15 designation a bore was chosen as (for fit_bearing)."""
    return bearing_for(app, d)["designation"]
