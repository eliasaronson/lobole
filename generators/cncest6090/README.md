# CNCest 6090 router (provisional)

A provisional layout of the owner's CNCest 6090 router and a Z-axis
development fixture, from photographs. **Only the 80 mm spindle diameter is
owner-supplied**; every other dimension is an estimate or a design choice,
recorded as such. It is not an OEM model and not a manufacturing release —
every run reports `status: provisional`, `manufacturing_ready: false`.

What is built: machine envelopes (base, slotted bed, gantry uprights and
beam); the stationary and moving Z plates, guide shafts, bearing and support
envelopes, an unthreaded screw envelope and a motor envelope; the Ø80 spindle
with a simplified nose in a split clamp with through-bolts and nuts; a draft
dust-shoe adapter collar with a three-view drawing. The machine layout and
the Z fixture reuse the same parts, and the Z axis is assembled directly in
each parent so it keeps its slider DOF (a rigid nested child would hide it).
There is no X/Y mechanism, no screw rotation or nut, no motor shaft or
coupling, no cables.

## Running it

Against a private instance on a scratch port:

```sh
python3 generators/cncest6090/cncest6090.py --port PORT [--out /tmp/cncest6090] [--stage preview|local|full] [--dimensions measurements.json]
```

- `preview` (default) builds and checks the custom parts' B-Reps and
  single-solid requirement; `local` adds an exact standing sweep of the Z
  fixture; `full` adds a nine-pose exact motion study over the provisional Z
  travel. The clearance verdict covers the Z fixture only, not the layout
  envelopes, and a clash, a truncated sweep or an unclean study fails the run.
- `--dimensions` takes a JSON object of names and values in mm, e.g.
  `{"spindle_l": 230, "guide_pitch": 125}`. Unknown names, non-finite and
  boolean values, and geometrically impossible ones are refused.
- Outputs: `cncest6090-provisional.dsgn`, `dimensions.json` (flat, editable,
  and accepted back by `--dimensions`), `dimension-register.json` (each value
  with its status and note), `report.json`, `drawing-check.json` and
  `accessory-collar-draft.svg`.

`tests/cncest6090_smoke.py` changes the spindle diameter 80 → 82 → 80 and
checks the actual bore, changes the guide spacing and checks shafts and
bearings, changes the bed width and checks the gantry, checks stable feature
ids and the spindle's displacement across the motion samples, and checks the
refusals.

## Dimensions

`cncest6090_spec.py` is the register: every entry has a nominal value in mm,
a status and a note saying what to measure. Statuses:

- `user_supplied` — the 80 mm spindle diameter;
- `estimate` — provisional values for the existing machine;
- `design_choice` — dimensions proposed for the model or the accessory;
- `override_unverified` — a supplied override that differs from its baseline.

Measurement uncertainty and manufacturing tolerance are different things and
neither is invented: `manufacturing_tolerance` is null in the register, and
clearances (and a 0.001 mm numerical nut-seat allowance) are model choices.

Every dimension is a native project variable. Sketch geometry, datum offsets,
extrusions and attachment frames keep their expressions, so editing a
variable in the app regenerates the model without re-running the generator —
but motion studies and the drawing are not re-run by that; regenerate them.
The bed's slot count is fixed at 16 pitches. Sketches on an XZ datum go
through an angled XY datum and an expression-driven offset datum, because the
SDK's `Plane.xz()` (normal −Y, V +Z) and the native XZ base plane (normal +Y,
V −Z) differ.

Worth measuring first when the machine is available: the spindle clamp land
and length, the spindle's offset from the moving plate, clamp hole pitch and
screw sizes, the Z plates, guide diameter and centre distance, the bearing
envelope, the real Z travel; then the bed and gantry. Read the screw and motor
markings before choosing vendor parts.
