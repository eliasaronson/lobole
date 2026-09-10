# CNCest 6090 — first provisional model

This is an initial layout and Z-axis dogfood project based on the owner's
photographs. **The spindle diameter is 80 mm, supplied by the owner.** No other
machine dimension or manufacturing tolerance has been confirmed. The owner
cannot currently access the machine. Do not treat this as an OEM model or a
manufacturing release.

## What is built

- Main machine envelopes: base, visually slotted bed, gantry uprights and beam.
- Stationary and moving Z plates, guide shafts, bearing/support envelopes,
  unthreaded screw envelope and motor envelope.
- An 80 mm spindle envelope with simplified nose, a split mounting clamp,
  through-bolts and nuts, and a separate draft dust-shoe adapter collar.
- A nine-pose Z slider study over a provisional 80 mm travel.
- A provisional accessory drawing with three views and three driven dimensions.

The machine layout and Z development fixture reuse the same parts. The Z axis
is assembled directly in each parent so it retains its slider DOF; nesting the
whole axis as a rigid child would conceal that DOF. This is not yet an X/Y
mechanism. Screw rotation, screw nut, motor shaft/coupling, guide attachment
hardware, electrical connectors, cables and cable chains remain to be developed.
The motor is an envelope seated on the upper support, not a specified motor.
The accessory tab is a starting interface for a dust shoe, not a complete shoe.

## Dimensions and later measurements

`generators/cncest6090/cncest6090_spec.py` is the parameter register. Each entry has a nominal
value in mm, a status and a note describing what to measure. The statuses are:

- `user_supplied`: the 80 mm spindle diameter at the recorded baseline.
- `estimate`: provisional values for the existing machine.
- `design_choice`: dimensions proposed for the model/accessory.
- `override_unverified`: a supplied override differs from its recorded baseline.

Measurement uncertainty and manufacturing tolerance are separate concepts.
Neither is invented here. `manufacturing_tolerance` stays null in the exported
register. Clearances and the 0.001 mm numerical nut-seat allowance are explicitly
model design choices, not inferred OEM tolerances or predictions of clamp
elasticity. Each variable comment records its baseline; later edits still need
verification even if the original baseline was supplied by the owner.

Dimensions are native project variables. Sketch lines/circles, datum offsets,
extrusion distances and mounting-frame origins retain expressions. Changing a
variable in the app regenerates the existing features; a generator rerun is not
required for those dimensions. The visual bed-slot count is fixed at 16 pitches.
Variable edits do not automatically rerun motion studies or update drawings;
regenerate those checks/outputs after changing the model.

Suggested measurements when the machine is available: usable spindle clamp
land/length, spindle offset from the moving plate, clamp hole pitch and screw
sizes, Z plate dimensions, guide diameter and centre distance, bearing envelope,
actual Z travel, then bed and gantry dimensions. Inspect screw/motor markings
before selecting vendor components.

## Running and verification

Use a private instance and scratch port, never mutate the user's port 8799:

```sh
python3 generators/cncest6090/cncest6090.py --port PORT --out /tmp/cncest6090 --stage preview
python3 generators/cncest6090/cncest6090.py --port PORT --out /tmp/cncest6090 --stage full
# Overrides are a JSON object of names and numeric values in mm:
# {"spindle_l": 230, "guide_pitch": 125}
python3 generators/cncest6090/cncest6090.py --port PORT --dimensions measurements.json --stage full
```

A fresh run creates a new project in the selected instance. Preview checks the
custom part B-Reps and single-solid requirements. Local adds an exact standing
sweep of the **Z fixture**. Full also samples its Z travel. Layout envelopes are
not covered by that clearance certificate. Even a clean fixture remains
provisional and `manufacturing_ready: false`.

The regression changes spindle diameter 80 → 82 → 80, checks the actual bore,
changes guide spacing and checks both shaft and bearing placements, and changes
bed width and checks gantry placement. It verifies stable feature IDs and actual
spindle displacement across the motion samples. It also rejects unknown,
non-finite, boolean and geometrically impossible input values.

## Friction observed

- `solve_assembly` returns DOF counts only for assemblies with loops; a simple
  slider tree does not have a `dof` field. Slider studies use `dof="offset"`.
- SDK `Plane.xz()` has normal -Y and sketch V +Z; native base XZ has normal +Y
  and V -Z. The generator builds an angled XY datum to retain the SDK basis,
  followed by an expression-driven offset datum. This avoids silently moving
  hole patterns when an expression-driven plane replaces a static SDK plane.
- The feature description says datum kind `angle`, but the parser accepts
  `angled`. A parent datum also replaces, rather than accompanies, `base_plane`.
- SDK sketches made from a `Frame` currently take a snapshot of its plane;
  native datum `plane_ref` is used here to keep sketch offsets live.
- The drawing check passed while long centred notes at X=20 extended beyond
  the left border. Rendering exposed this; moving the notes to sheet centre
  corrected it. Note bounds need coverage in the drawing validator.

These are recorded findings, not claimed core fixes in this generator change.

## First measured run

The model built in 1.84 s. The 23-occurrence Z fixture passed 34 exact standing
pairs in 0.075 s and 54 motion pair checks across nine poses in 0.13 s, with no
clashes. The parameter/motion regression passed. The draft drawing passed with
three resolved dimensions, three views and no reported issues; the exported
SVG and both CAD renders were also visually inspected.

Artifacts are under `/tmp/cncest6090/`: the provisional project, layout and
Z-axis PNGs, accessory SVG, reports, `dimension-register.json` with provenance,
and a flat `dimensions.json` suitable for editing and passing to `--dimensions`.
