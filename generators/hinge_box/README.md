# Print-in-place hinged box

A shallow box and its lid printed flat side by side, joined along one edge
by a hinge of five alternating knuckles on 45° cone pins. It comes off the
bed as two pieces that are already hinged, and the lid swings 180° onto
the rim.

    tools/headless-preview.sh --start <port>
    python3 generators/hinge_box/build.py --port <port> --profile "K5 PLA on P1S"
    python3 generators/hinge_box/build.py --port <port> --size 80

Output goes to `scratch/hinge_box/` (or `--out`): `hinge_box.stl` (the
plate, one shell per piece), `hinge_box.dsgn` (the parts, the plate, the
assembly and its study, the negative controls) and `hinge_box.json`, the
receipt (every size, `check_print`'s items, the swing and the controls).

## Why it exists

The [counter](../counter/README.md) came off a P1S fused solid: every wheel
started one empty layer over the part below and was printed onto it, and
the coupon's old Z-gap rings, printed a few layers over the plate with
nothing under them, drooped into strands and welded to their posts. A part
printed in place follows two rules instead:

1. every piece starts on the bed;
2. where one part prints over another, the face above is self-supporting
   (45°, or a bridge between the part's own walls) and leaves at least the
   print's minimum air over the part below.

`check_print`'s `stacking` item checks both
([checking a print](../../docs/tolerances/README.md#checking-a-print)).
This box is the smallest useful thing built to them: both pieces lie on
the bed, and the only faces over the other piece are the cone sockets over
the pins.

## The hinge

- **The axis** runs along the box's back edge, horizontal, at half the
  box's height plus the lid's thickness. Turned 180° about it, the lid's
  inner face lands at the box's height: on the rim.
- **The knuckles** are teardrops: a circle about the axis as large as the
  axis is high, with 45° flanks down to a flat on the bed. The box carries
  the two end knuckles and the middle one, the lid the two between, each
  the gap a side from its neighbours along the axis. A web joins each
  knuckle to its part.
- **The relief.** A teardrop's flat reaches past the circle, so a knuckle
  sweeps a circle through its flat's corners as it turns. Each part stands
  off the axis by that radius plus the gap: the box's back wall and the
  lid's edge sit outside the circle the other part's knuckles sweep, so a
  knuckle never meets the other part anywhere in the swing.
- **The cone pins.** Between each pair of neighbouring knuckles, a 45° cone
  on the box's knuckle stands in a cone socket in the lid's. The socket is
  the pin moved along the axis by the minimum air: a normal clearance of
  air/√2 on the cone, and exactly the air vertically, over the pin's top
  and under its bottom. Those are the only faces over the other piece, and
  both are 45° overhangs, so the slicer prints them without support.

## Where every size comes from

The inputs are `--size` (60 mm: the box's inside width along the hinge),
the profile's gap (`print_allowance` kind `gap` at the knuckle's diameter,
half of it a side) and the minimum air, check_print's stacking limit: the
profile's `z_gap_mm`, else three layers of its `layer_height_mm`, else of
`--layer`, else of the FDM preset's 0.2 mm. `solve()` derives the rest and
refuses what will not print:

- walls, floor and lid a 25th of the size (2.4 mm at 60: six lines of a
  0.4 mm nozzle; under four lines is refused); the inside 0.6 of the size
  deep, the box 0.3 of it high outside;
- the axis height and knuckle radius from the box (above), the flat
  (√2 − 1) of it a side, the swept radius through the flat's corners, the
  standoff the swept radius plus the gap;
- five knuckles along the outside width, the gap a side between them;
- the pin 0.45 of the knuckle's radius at its root and 0.3 of that at its
  flat tip; the socket's air the minimum air, raised to whole layers of at
  least the gap a side normal to the cone. A lid knuckle too short for two
  sockets, or a socket that leaves the knuckle under half a wall, is
  refused.

At 60 mm with K5 PLA: a box 64.8 × 40.8 × 18 mm outside, a 2.4 mm lid,
Ø20.4 knuckles on an axis 10.2 mm up sweeping r11.04, 0.6 mm of air on
the cones (0.424 normal).

## What is checked, every run

- `check_geometry` on the plate is `verified`.
- `check_print` on the plate passes **every** item: on the bed, two pieces
  for two bodies, no overhang past 45° (the teardrops' flanks and the
  cones are 45° exactly), no ceiling, every pair of faces at least the gap
  a side apart on the exact B-Rep (the knuckles' end faces are the
  closest), the STL's chords a fifth of the gap, and stacking: both pieces
  on the bed and no face within the minimum air over the other.
- **The swing**, in the assembly `hinge box`: the box fixed, the lid on a
  revolute to the box's `hinge` frame. A motion study turns it from 0° to
  180° in 5° samples with the mesh clash check at a fifth of the gap. Every
  sample before 180° must be clean; half way the lid must be over the axis
  (a lid swung the other way would pass through the bed, which no clash
  check sees); at 180° its inner face must stand at the box's height over
  the box's footprint, and the only clash allowed there is the lid on the
  rim (a mesh check calls touching a clash).
- **Negative controls that must fail stacking:** the same box with a lid
  whose sockets give the cones half the air (between the lid's and the
  box's knuckles), and the plate with the lid lifted one layer off the bed
  (a piece off the bed; the bed item still passes, since the print as a
  whole is on it). A run whose controls pass stops.

`generator_hinge_box` (generators/README.md#the-big-generators-as-tests)
runs the whole of it on the built-in table, in a few seconds.

## Printing it

1. Build with your measured profile (`--profile "K5 PLA on P1S"`).
2. Slice `hinge_box.stl` as it comes, at the layer height the minimum air
   was taken from, **no supports**.
3. Off the bed, hold the box and lift the lid's free edge: the hinge
   should turn at once. Swing it over onto the rim.

Untested on the printer.
