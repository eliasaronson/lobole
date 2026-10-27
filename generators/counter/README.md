# Print-in-place counter

A four-digit mechanical counter printed as ONE plate: number wheels on one
axis inside a housing with a window per digit, a fluted knob on the units
wheel, and a Geneva carry between each pair of wheels that steps the next
wheel one digit per turn and locks it in between. It comes off the bed
reading 0000, and every piece of it is printed from the bed.

    tools/headless-preview.sh --start <port>
    python3 generators/counter/build.py --port <port> --profile "K5 PLA on P1S"
    python3 generators/counter/build.py --port <port> --digits 3 --digit-height 10

Output goes to `scratch/counter/` (or `--out`): `counter.stl` (the plate, one
shell per body), `counter_job.3mf` (the
[print job](../../docs/tolerances/README.md#print-jobs), a Bambu project
when `$LOBOLE_SLICER_CONFIGS` holds `pla.json`), `counter.lob` (the parts,
the assembly and its studies, the plate and the controls), `counter.png`
(the plate) and `counter.json`, the receipt (every size and height, the
print pose, the plate's place on the bed, the measured clearances, every
`check_print` item and every study's verdict).

## What changed from the print that fused

The 2026-10-15 print on a P1S came off the bed fused solid. Its wheels sat
on one axle, each starting one empty layer over the wheel below, the
transfers on one post beside them, the housing's base under the wheels and
its fins between them, and the slicer printed every one of those pieces
onto the piece under it: a flat face one layer over another part prints ON
it ([one part printed over another](../../docs/tolerances/README.md#one-part-printed-over-another)).
`check_print`'s `stacking` item fails that layout, and the run builds it
again at two digits as a negative control (`stacked.py`, below).

This counter is built to the two rules the print taught:

1. **Every piece starts on the bed.** Each wheel stands on a tube that runs
   down inside the wheels under it to the bed (the units' innermost, a
   rod); the thousands' band stands on the bed itself. Each transfer has a
   column of its own beside the stack, at its own azimuth, from the bed up.
   The housing stands only beside and under the moving parts, never over
   them.
2. **Where a piece prints over another, it does so through 45° faces with
   the profile's air under them** (`z_gap_mm`, the coupon's air row: 0.4
   mm on K5). A wheel's pin disc is a 45° cone up from its tube, the air
   over the wheel below; a transfer's Geneva stands on a 45° cone from its
   column, the air over the pin disc; the band's underside is a 45° cone
   from the lock disc, the air over the Geneva; a pinion's teeth slope up
   from their column at 45°, the air over the band below.

The digits, the Geneva carry and its `intermittent` relation, and every
check of the counting stayed. On K5 `check_print` passes every item, the
`stacking` item with no face within the air over another piece.

## The mechanism

Each wheel carries, under its band of digits, a pin disc with a pin and a
lock disc. Beside the stack each **transfer** turns on its own column: a
three-slot Geneva at the wheel's pin level and, under it, a 9-tooth pinion
meshing the next wheel's 30-tooth gear. Once a turn the pin enters a slot
and turns the Geneva 120°; the pinion turns the next wheel 120 · 9/30 = 36°,
one digit, the same way round. For the rest of the turn the lock disc rides
in the Geneva's concave arc and the transfer, and so the next wheel, cannot
turn.

Why a Geneva and not the classic odometer carry (a two-tooth segment and a
lock ring driving a pinion with alternately full and half-width teeth):

- The classic lock is two tooth TIPS resting on a ring. A six-tooth pinion
  at the module that fits beside a digit wheel has tips under 1.5 nozzle
  widths, which the gap thins further. A Geneva locks on the whole of a
  concave arc riding the lock disc.
- The Geneva's motion has an exact closed form, which the app's
  `intermittent` relation states (`motion: "geneva"`,
  docs/assemblies/relations.md#intermittent), so the motion studies check the
  shapes themselves at a fifth of the gap. A two-tooth carry's motion is two
  teeth of a gear mesh, with no law to state, and the check would be
  checking a guess.
- The pin enters and leaves its slot radially, so the next wheel starts and
  stops at rest: no impact on printed teeth.

**Three slots, not four.** The Geneva's window is 180 − 360/N degrees of
the driving wheel. Centred between its 9 and its 0, the three-slot window
(60°) has done 97 % of the step by the time either rest position is
reached, so the next wheel stands about a degree (a 36th of a digit) off
its digit at 9 and at 0. Four slots (90°) leave it 4.5° off, and the
offsets add up the train. The cost is a deep slot: the pin comes within
0.13 of the centre distance of the Geneva's axis, which is what sizes the
counter (next).

## Where every size comes from

The inputs are `--digit-height` (8 mm), `--digits` (4), the profile's gap
(`print_allowance` kind `gap`, half of it a side), its air (`z_gap_mm`,
else three layers, the limit `check_print` holds a piece to without one)
and the layer (`layer_height_mm`, else `--layer`, else 0.2). `solve()`
derives the rest and refuses what will not print:

- **The Geneva's centre distance c** is the smallest that lets a
  transfer's column, a quarter of the digit height in radius (2 mm), rise
  from the bed past every wheel's pin disc: the disc reaches the pin
  circle and the pin, c · sin 60° + r_pin, and the column clears it by the
  gap, so c (1 − sin 60°) ≥ r_col + r_pin + g (24.64 mm on K5). At the
  slot's bottom the pin is then the gap off the column: the deep slot is
  what costs the size. The band is the pin disc's size (Ø45.1), and at
  least what the digits fill; an 8 mm digit fills 56 % of its cell round
  it.
- **The gear**: 30 + 9 teeth across c (module 1.264), 25° stub teeth as the
  planetary's. A pinion tip under 1.5 nozzle widths is refused.
- **The Geneva**: pin circle c · sin 60°, wheel radius c · cos 60°, the pin
  3 nozzle widths in radius. The lock disc is as large as leaves each horn
  of the Geneva 4 nozzle widths wide at its tip (r18.37), relieved where
  the Geneva swings through it at mid-carry.
- **The tubes**: each a wall of four lines, the gap from the next; the
  units' rod as thick as a column.
- **The transfers** stand round the +X side 38.2° apart, what keeps one's
  Geneva clear of the next one's column and the housing round it.
- **Heights**, bottom to top, each step the air: the thousands' band on the
  bed and its gear; for each wheel above, its pin disc cone, the air, the
  transfer's Geneva cone and working depth (0.4 of the digit height), the
  air less the gap, the band's cone (from two lines inside the deepest the
  horns reach into the lock disc, so the band stands on the lock disc all
  the way round, over the relief too) and face, then its gear, as tall as the
  pinion's slope, a mesh of 0.4 of the digit height, and both stacks' play
  of the air each. The units' band carries the knob. 220.3 mm tall on K5.
- **If the digits do not fit the profile's bed** (its `bed_mm`, height
  included), the run says so and builds the most that do; four fit the
  P1S's 250 mm.

## The print

Axle vertical, the thousands on the bed, the units on top under the knob.
Every surface that slides is a vertical wall spaced by the profile's gap,
which is the gap the coupon measured, and every face over another piece
is a 45° face the air over it. The housing is a block minus every moving
part's swept envelope, each run up to the top and down the air, so it holds
each transfer's column from the bed to under its pinion and the stack
round its bands, and stands over nothing; a 45° lip at −X over the units'
band keeps the stack from lifting out. The windows are teardrops on their
side (`printed: teardrop_flat`,
docs/modeling/holes-and-threads.md#printed-holes), point up.

**The digits** are seven-segment bars, gapped at the corners, engraved
into the band's face cut in and DOWN at 45°, so every wall of a pocket is
a 45° face or one that looks up: cut straight in, a glyph's upper walls
are ceilings over its pocket. A font's 0, 4, 6, 8 and 9 close round a
counter, and on a wall that counter is an island that starts in the air in
some layer (Bambu Studio's "floating regions"); seven segments close round
nothing. The lean draws a digit out along the axis by √2.

**Nothing starts in the air.** `check_print` judges faces, not where a
piece's layer first appears; the slicer does (Bambu Studio's "floating
cantilever"). A band whose 45° underside rose from the lock disc's edge
would hang over the relief, where the lock disc is cut away; it rises
from inside the relief's reach instead, on the lock disc all the way
round, and the job slices with no warning.

The plate is placed in the middle of the profile's bed, moved along x past
its keep-out zones.

## What is checked, every run

- `check_geometry` on the plate is `verified`.
- `check_print` (docs/tolerances/README.md#checking-a-print) on the plate,
  **every item**: on the bed, one piece per body (2 · digits), nothing past
  45°, no ceiling past the bridge limit (the windows' flats, 1.7 mm),
  every pair of bodies at least the gap a side apart on the exact B-rep
  (less the check's tenth: on K5 every pair 0.100–0.101), the STL's chords
  a fifth of the gap, **stacking** (every piece on the bed, and no face
  within the air over another piece: none within 0.4 mm), and `bed_fit`
  (62.9 × 58.5 × 220.3 mm in the P1S's 256 × 256 × 250, clear of its
  keep-out corner). Then the plate's print job. **Every item but
  `slender`**, which the counter fails and the run prints as OPEN rather
  than demanding: read alone, each wheel is its tube standing on the bed
  inside the wheels under it, and each transfer its column, so the units'
  is 220 mm on a 4 mm foot (55:1) and the transfers' columns 46:1 and
  29:1 ([slender](../../docs/tolerances/README.md#slender)). Nested, each
  piece is braced at the gap by the one round it, which the item does not
  model; whether the stack stands while it prints is the next print's to
  answer, and a counter that lies on its side would not ask.
- **The counting**, in the assembly: each wheel on a revolute to the
  housing's frames, each transfer on its own; per carry an `intermittent`
  relation (Geneva, 3 slots) wheel → transfer and a `gear` relation (9:30)
  transfer → next wheel. Two motion studies drive the units 2° a sample with
  the mesh clash check at a fifth of the gap: 0000 → 0011 (ten digits and a
  carry) and 9998 → 0001 (the triple carry). Both must be clean, must read
  what they should at both ends (from the wheels' own turns in the studies'
  poses), and at every sample where the units stand on a digit every wheel
  must stand within 1.5° of one. Measured: the units exact, the tens 1.02°,
  the hundreds 1.25°, the thousands 1.31° at most.
- **The locks**: for each carry, with its driving wheel mid-dwell (count 5,
  50, 500), the next wheel is turned 3° each way. It must clash, and between
  the driving wheel and its transfer (the Geneva's arc into the lock disc).
  The carry relation also reports itself not satisfied there, since nothing
  turned its input: the lock, said by the relation
  (docs/assemblies/relations.md#intermittent).
- **Negative controls that must fail**: the first carry's Geneva timed 10°
  late must clash its wheel with its transfer over the carry; the first
  gear put half a tooth out must clash the transfer with the tens; a
  transfer modelled at half the gap, beside the units wheel, must fail
  `check_print`'s clearance item (0.050 mm); and the 2026-10-15 stacked
  layout, built again at two digits (`control: stacked`, `stacked.py`),
  must fail `stacking` (three of its four pieces start off the bed, eight
  pairs within the air). A run whose controls read clean stops.

`generator_counter` (generators/README.md#the-big-generators-as-tests) runs
the whole of it but the still, on the seeded K5 profile with its air
recorded as the coupon measured it.

## Printing it on a P1S

1. Build with your measured profile (`--profile "K5 PLA on P1S"`: 0.1 a
   side and 0.4 of air). A profile without a measured air is held to three
   layers.
2. Slice `counter_job.3mf` (or `counter.stl` as it comes) at the layer
   height the profile was measured at (0.2 mm), **no supports**, no brim.
3. Off the bed, hold the housing and turn the knob gently through a full
   turn each way, then count up through a carry. The digits are engraved:
   run a fine paint marker over the bands through the windows, or wipe
   filled acrylic off the face.
4. In use, lay it on its side with the knob to the right: the digits read
   upright through the windows, the units at the knob's end, and the knob
   turned away from you counts up.

Not modelled: the friction the tubes and the stacked cones carry, the
force a hand puts through a triple carry, and the columns' bending under
it (each 4 mm across, held by the housing from the bed to under its
pinion).
