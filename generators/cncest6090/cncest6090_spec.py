# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Provisional CNCest 6090 dimensions, millimetres; no inferred tolerances."""

from dataclasses import dataclass, asdict
import math


@dataclass(frozen=True)
class Dimension:
    value: float
    status: str
    note: str


# 'User supplied' is not a calibrated measurement or a manufacturing tolerance.
D = {
    "spindle_d": Dimension(
        80, "user_supplied", "User confirmed spindle diameter; tolerance unknown."
    ),
}
for name, value, note in [
    ("bed_w", 800, "Overall bed width; measure extrusion stack."),
    ("bed_l", 1100, "Overall bed length, not travel."),
    ("bed_top", 100, "Bed surface above base datum."),
    ("bed_t", 20, "Table extrusion thickness."),
    ("gantry_y", 650, "Gantry location at the illustrated reference pose."),
    ("gantry_h", 500, "Gantry top above base datum."),
    ("gantry_t", 20, "Upright plate thickness."),
    ("beam_depth", 80, "Gantry beam depth."),
    ("z_w", 180, "Stationary Z plate width."),
    ("z_h", 320, "Stationary Z plate height."),
    ("back_t", 18, "Stationary Z plate thickness."),
    ("guide_d", 16, "Z guide shaft diameter; inspect markings."),
    ("guide_pitch", 120, "Centre distance between Z guide shafts."),
    ("guide_y", -18, "Guide axes forward of stationary plate."),
    ("end_t", 15, "Guide end support thickness."),
    ("bearing_h", 35, "Linear bearing housing length."),
    ("bearing_w", 34, "Linear bearing housing width."),
    ("bearing_front", -45, "Bearing housing front face."),
    ("bearing_back", -6, "Bearing housing rear face."),
    ("carriage_z", 80, "Moving plate lower edge at low travel limit."),
    ("carriage_h", 120, "Moving plate height."),
    ("carriage_t", 15, "Moving plate thickness."),
    ("spindle_y", -110, "Spindle axis forward of stationary plate."),
    ("spindle_z", 30, "Spindle body bottom relative to Z assembly datum."),
    ("spindle_l", 220, "Cylindrical spindle body length; excludes nose."),
    ("clamp_z", 100, "Main spindle clamp bottom relative to Z datum."),
    ("clamp_h", 45, "Main clamp height."),
    ("clamp_r", 54, "Main clamp outer radius; approximate construction."),
    ("clamp_bolt_pitch", 120, "Clamp-to-carriage mounting hole pitch."),
    ("screw_d", 12, "Z screw envelope diameter; exact screw type unconfirmed."),
    ("motor_w", 57, "Motor envelope only; exact model/mounting unconfirmed."),
    ("motor_l", 56, "Motor body length."),
    ("z_travel", 80, "Provisional checked travel, not a machine specification."),
]:
    D[name] = Dimension(value, "estimate", note)
for name, value, note in [
    (
        "seat_allowance",
        0.001,
        "Numerical nut seating allowance for transformed B-Reps; not a physical fit specification.",
    ),
    ("tip_clearance", 5, "Low-pose tool-tip clearance above the model bed."),
    ("bore_clearance", 0.2, "Diametral modelling clearance; not an OEM fit tolerance."),
    ("clamp_gap", 2, "Unloaded split; elastic clamping is not simulated."),
    ("accessory_z", 55, "Draft collar below main clamp; check usable spindle land."),
    ("accessory_h", 15, "Draft accessory collar thickness."),
    ("accessory_r", 50, "Draft accessory collar outer radius."),
]:
    D[name] = Dimension(value, "design_choice", note)


def dimensions(overrides=None):
    values = {key: dim.value for key, dim in D.items()}
    for key, value in (overrides or {}).items():
        if key not in D:
            raise ValueError(f"Unknown dimension: {key}")
        if (
            isinstance(value, bool)
            or not isinstance(value, (float, int))
            or not math.isfinite(value)
        ):
            raise ValueError(f"{key}: expected a finite number in mm")
        values[key] = value
    for key, value in values.items():
        if (
            key not in ("guide_y", "bearing_front", "bearing_back", "spindle_y")
            and value <= 0
        ):
            raise ValueError(f"{key}: must be positive")
    if values["spindle_d"] / 2 + values["bore_clearance"] >= min(
        values["clamp_r"], values["accessory_r"]
    ):
        raise ValueError("Spindle bore leaves no collar wall")
    if (
        values["carriage_z"] + values["carriage_h"] + values["z_travel"]
        > values["z_h"] - values["end_t"]
    ):
        raise ValueError("Z travel exceeds the guide support envelope")
    return values


def manifest(values):
    return {
        key: dict(
            asdict(dim),
            value=values[key],
            status=dim.status if values[key] == dim.value else "override_unverified",
            manufacturing_tolerance=None,
        )
        for key, dim in D.items()
    }
