# A part printed onto another part

## The problem

The counter (generators/counter/) came off the P1S fused solid, and the
coupon's Z-gap row with it. Every wheel and transfer started one layer
over the part below, and the slicer printed it onto that part:

- a flat face one empty layer over another part bonds across all of its
  area (the counter's ceiling skin stayed on the units wheel when the lid
  was torn off);
- a part whose first layer has more than one empty layer under it and
  nothing of its own to bridge from is printed into the air. Its first
  layer droops into loose strands and welds to whatever it lands on (the
  coupon's 0.4 and 0.6 rings).

So a flat face over another part fails at any number of layers. Working
print-in-place designs follow two rules:

1. every separate piece starts on the bed;
2. where one part prints over another, the face above is self-supporting
   (45°, or a bridge between the part's own walls) and leaves enough air
   for the strands not to reach the part below.

Nothing in Loböle states either rule:

- `check_print` passed the counter. Its overhang and bridge items look at
  each body alone, and its bed item asks only that the print's LOWEST point
  is on the bed.
- The profile's `z_gap_mm` is defined as "whole layers between a part and
  the one printed on it", which is the thing that fuses.
- The coupon's Z-gap row measures exactly that: rings that start in the
  air.

## The shape of the fix

### 1. check_print: a seventh item, `stacking`

`printcheck::Check` gains `Stacking`, run when a minimum air is known.

- **Every piece starts on the bed.** The pieces are the shells item's
  (bodies that touch are one piece). A piece whose lowest point is above
  the bed fails, naming the piece and its height. The bed item keeps its
  meaning: the print as a whole is on the bed.
- **No face lies within the minimum air over another piece.**
  - Every downward-facing triangle of the tessellation `CheckFaces` already
    makes is sampled at its centroid and its nodes. Faces on the bed are
    excluded.
  - From each sample, a ray goes down along `-up` to the nearest material,
    exact on the B-Rep (`IntCurvesFace_ShapeIntersector`, one per body,
    pruned by the body's box).
  - If the nearest material is the same piece, that is the bridges item's
    business. If it is another piece, the vertical air is measured.
  - Air under the limit (less the clearance item's tenth) fails.
  - The report is per pair of pieces: the area, the least air, and where,
    as `Where` gives it, so the GUI can show the face.
- **The limit** is the print's `printfit::MinAirMm`:
  - the profile's `z_gap_mm` when it states one;
  - else three layers of the profile's `layer_height_mm`;
  - else three layers of the FDM preset's 0.2 mm. The source says
    "process", to confirm with the coupon.
  - A process that is not FDM skips the item and says why.
  - `min_air_mm` on the RPC states the limit outright, as `min_gap_mm` does.
- `PrintCheckAsk` / `RunPrintCheck` derive the limit the same way for the
  RPC and the Print process section's *Check print*. The section gets one
  more line, with *Show* (`###print_check_stacking`) selecting the worst
  face.
- `_lib.py`'s `PRINT_ITEMS` gains `stacking`.

### 2. The profile's vertical number means air

`z_gap_mm` stays a whole number of `layer_height_mm` (the slicer ends air
at a layer). Its meaning changes to the vertical air a face needs over
another part to come away from it. The stored key does not change, and no
profile has recorded one yet, so this is not a format change.

What changes is the docs (docs/tolerances/README.md#print-profiles), the
RPC rows that describe it, the Settings window's label and the validation
message.

### 3. The coupon measures what designs use

`generators/print_coupon.py`'s Z-gap row is replaced by two rows. Every
ring sits **on the bed** around a post of the plate, Ø half the size, as
the counter's axles are.

- **Air row.** The post carries a 45° head that flares out over the ring's
  inner edge with N layers of vertical air there (1, 2, 3 and 4; either
  side of the profile's `z_gap_mm` when it has one). Sideways, the ring
  clears its post by twice the profile's gap a side, so that only the air
  varies.
  - Turn each ring: the fewest layers that turn cleanly are the air.
  - `--record --z-gap-layers N` saves it to `z_gap_mm` as today.
- **Small-gap row.** The same ring and post, with the head at the air
  row's largest air, and the bore's gap a side stepped round the profile's
  (the profile's −0.05 … +0.15).
  - It measures the sideways gap at the post's size on the first layers,
    which a Ø8 disc did not.
  - `--record --post-gap-side MM` saves it as a `gap` row for sizes up to
    the post's diameter, on top of the profile.
- `--z-gap-only` keeps its name and prints both rows on a thin plate.
- The coupon itself must pass `check_print`, stacking included.

### 4. A test piece: a print-in-place hinged box

`generators/hinge_box/build.py`, output in `scratch/hinge_box/`.

A shallow box and its lid printed flat side by side, joined along one edge
by a hinge of alternating knuckles. Every piece is on the bed. The hinge
axis is horizontal at the knuckles' radius, which is half the box's height
plus the lid's thickness, so the lid swings 180° onto the rim.

- **The knuckles** are teardrops: a circle about the axis, with 45° flanks
  down to a flat on the bed. The box is relieved round the lid's knuckles,
  and the lid round the box's, to the radius their flanks sweep.
- **Cone pins.** Between each pair of neighbouring knuckles, a 45° cone pin
  on the box's knuckle sits in a cone socket in the lid's. The clearance
  normal to the cone is what gives the vertical air under the socket's
  upper face, so the cone is sized from `MinAirMm`, and the knuckles' end
  faces from the gap a side.
- **Sizes.** Every size derives from `--size` (the box's inside width) and
  the profile, as the counter's do.
- **Checked every run:**
  - `check_geometry` is verified;
  - `check_print` passes every item, stacking included;
  - an assembly with the lid on a revolute about the hinge axis, and a
    motion study from 0° to 180° that is clean at a fifth of the gap and
    ends with the lid on the rim.
- **Negative controls that must fail stacking:**
  - the cone clearance at half → air under the limit, between the lid's and
    the box's knuckles;
  - the lid lifted one layer off the bed → a piece off the bed.
  - A run whose controls pass stops.
- A README in the counter's shape, and a row in generators/README.md.

The counter is not rebuilt in this work. Its README says what the print
showed and that `check_print`'s stacking item now fails it.

## Files

- `src/core/PrintCheck.{h,cpp}`: `Stacking`, `Report::stacking`, `Ok()`.
- `src/core/PrintAllowance.{h,cpp}`: `MinAirMm` and its source; the z gap's
  meaning and messages.
- `src/ui/PrintCheckActions.{h,cpp}`, `src/ui/` Print process section: the
  limit, the line, *Show*.
- `src/remote/RemoteInspection.cpp`: `min_air_mm`, the item in the reply,
  the MethodDoc rows (check_print, print profiles).
- `src/ui/SettingsWindow.cpp`: the z-gap label.
- `tests/test_print_check.cpp`, `tests/print_check_smoke.py`,
  `tests/print_allowance_smoke.py`.
- `generators/_lib.py`, `generators/print_coupon.py`,
  `generators/hinge_box/`, `generators/counter/README.md`,
  `generators/README.md`.
- `docs/tolerances/README.md` (checking a print, print profiles, the
  coupon).

## Verified by

- **Unit tests** (`[printcheck]`):
  - a block one layer over another, off the bed → both checks fail, with
    the area and the 0.2 air;
  - a ring on the bed under a 45° head at three layers → passes, and at one
    layer → fails with the cone's area over the ring;
  - a body's own bridge over its own floor → not counted;
  - the limit from a profile's `z_gap_mm`, from its layer height, from the
    process, and from `min_air_mm`;
  - an unprinted part and a non-FDM process → skipped.
- **The counter as built fails stacking**, at its one-layer interfaces, and
  the old coupon's Z-gap ring fails as a piece off the bed: the check sees
  the failure the printer showed.
- `print_check_smoke.py`: the item over RPC, the parameter refused out of
  range.
- The coupon and the hinged box generators run clean with their negative
  controls, and the headless and GUI suites pass.
- On the printer: the coupon's air and small-gap rows, and the box's lid
  swinging free off the bed.
