# Print-in-place counter

A four-digit mechanical counter printed as ONE piece: number wheels on one
axle inside a housing with a window per digit, a fluted knob on the units
wheel, and a Geneva carry between each pair of wheels that steps the next
wheel one digit per turn and locks it in between. It comes off the bed
reading 0000.

**It does not print as designed.** The 2026-10-15 print on a P1S came off
the bed fused solid: every wheel and transfer starts one empty layer over
the part below, and the slicer printed each onto that part. `check_print`
passed it then; its `stacking` item
([checking a print](../../docs/tolerances/README.md#checking-a-print)) now
fails it: `build.py` prints the failure and the receipt keeps it, but
does not require the item, so the build still completes. The design needs
every piece on the bed and self-supporting faces with three layers of air
over the part below; until then the rest of this page describes the
counter as it was built.

    tools/headless-preview.sh --start <port>
    python3 generators/counter/build.py --port <port> --profile "K5 PLA on P1S"
    python3 generators/counter/build.py --port <port> --digits 3 --digit-height 10

Output goes to `scratch/counter/` (or `--out`): `counter.stl` (the plate, one
shell per body), `counter.dsgn` (the parts, the assembly and its studies,
the plate), `counter.png` (the plate cut through both axes) and
`counter.json`, the receipt (every size, the print pose, the measured
clearances and every study's verdict).

## The mechanism

Each wheel carries, under its band of digits, a pin and a lock disc. Beside
the stack a **transfer** turns on its own post: a three-slot Geneva at the
wheel's pin level and, under it, a 9-tooth pinion meshing the next wheel's
30-tooth gear. Once a turn the pin enters a slot and turns the Geneva 120°;
the pinion turns the next wheel 120 · 9/30 = 36°, one digit, the same way
round. For the rest of the turn the lock disc rides in the Geneva's concave
arc and the transfer, and so the next wheel, cannot turn.

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
offsets add up the train (8° at 0000). The cost is a deep slot: the pin
comes within 0.13 of the centre distance of the Geneva's axis, so the
Geneva has no bore and its transfer stands on a stub post that reaches
only into the pinion.

## Where every size comes from

The inputs are `--digit-height` (8 mm), `--digits` (4), the profile's gap
(`print_allowance` kind `gap`, half of it a side) and the layer height the
parts are sliced at (the profile's `layer_height_mm`, else `--layer`, else
0.2: Bambu Studio's "0.20 mm").
`solve()` derives the rest and refuses what will not print:

- A digit fills 70 % of its cell around the band: band radius = 10 cells / 2π
  (18.2 mm at 8 mm digits). The band's face is 0.9 of the digit height wide,
  the glyph 0.6 of it; digits are engraved two lines deep.
- The transfer's axis stands a post's radius (a quarter of the digit height)
  outside the band; the gear module follows from that distance and 30 + 9
  teeth (1.04 at 8 mm), 25° stub teeth as the planetary's. A pinion tip
  under 1.5 nozzle widths is refused: at the built-in table's 0.25 mm a
  side, 8 mm digits are refused and 10 mm print.
- The Geneva: pin circle C·sin 60°, wheel radius C·cos 60°, the pin's
  radius 3 nozzle widths or 6 % of C. The lock disc is as large as leaves each horn of the
  Geneva 4 nozzle widths wide at its tip, relieved where the Geneva swings
  through it at mid-carry.
- Vertical spaces (one part printed over another) are the profile's
  `z_gap_mm`, now the air the coupon's air row measures
  ([the coupon](../../docs/tolerances/README.md#the-coupon)); a profile
  without one gets whole layers at least the gap: one layer at 0.1 a side,
  two at 0.25. A `--layer` other than the one the profile's Z gap was
  measured at is refused. The receipt's `zgap_from` says which.

## The print

Axle vertical, the most significant wheel on the bed, the units on top
under the knob. Every surface that slides is a vertical wall spaced by the
profile's gap, which is the gap the coupon measured. Where one part sits
over another — a wheel on the wheel below, a Geneva on the band, a housing
fin on a band — the space is whole layers, on the assumption that the
slicer prints the upper part on the lower as it prints a part on a
support's interface and a firm first turn frees it. The print disproved
that: a flat face over another part fuses at any number of layers
([one part printed over another](../../docs/tolerances/README.md#one-part-printed-over-another)). Each wheel's band overhangs its lock disc, so the band's
underside is a 45° cone. The housing is a block minus every moving part's
swept envelope grown by the gap and the vertical space, so what is left is
the base, the walls, the posts, the ceiling, and the fins between the bands
that hold each wheel and transfer at its height.

Each wheel stands on the one below, so turning the units rubs the tens'
gear face while the tens is locked; that friction is the lock's to carry.

## What is checked, every run

- `check_geometry` on the plate is `verified`.
- `check_print` (docs/tolerances/README.md#checking-a-print) on the plate:
  it stands on the bed, comes off it as one piece per body (2 · digits),
  every pair of bodies is at least the gap a side apart on the exact B-rep
  (less the check's tenth: wheel and its transfer, transfer and the next
  wheel's gear, wheel on wheel, every part against the housing; on K5 PLA
  every one is 0.100–0.102), and the STL's chord deviation (the profile's
  0.01 mm) is at most a fifth of the gap. Its overhang and bridge items are
  recorded in the receipt's `print`, not required: the layer gaps print as
  ceilings over the part below. Its `stacking` item FAILS (printed and
  recorded, not required): on the 2026-10-15 plate (K5 PLA, no layer
  height in the embedded profile, so 3 × 0.2 = 0.6 mm of air required)
  seven of the eight pieces start off the bed (the thousands wheel 2.2 mm
  up, one layer over the base, the units wheel 51.6 mm), and every
  interface is 0.2 mm of air, one layer: each wheel's underside over the
  next, 609 mm² each; the housing over the units wheel, 773 mm²; the
  thousands wheel over the base, 698 mm²; each transfer over the housing,
  about 89 mm²; 20 pairs in all.
- **The counting**, in the assembly: each wheel on a revolute to the
  housing's frames; per carry an `intermittent` relation (Geneva, 3 slots)
  wheel → transfer and a `gear` relation (9:30) transfer → next wheel. Two
  motion studies drive the units 2° a sample with the mesh clash check at a
  fifth of the gap: 0000 → 0011 (ten digits and a carry) and 0998 → 1001
  (the triple carry). Both must be clean, must read what they should at both
  ends (from the wheels' own turns in the studies' poses), and at every
  sample where the units stand on a digit every wheel must stand within
  1.5° of one. Measured: the units exact, the tens 1.0°, the hundreds 1.25°,
  the thousands 1.3° at most.
- **The locks**: for each carry, with its driving wheel mid-dwell (count 5,
  50, 500), the next wheel is turned 3° each way. It must clash, and between
  the driving wheel and its transfer (the Geneva's arc into the lock disc);
  it does within half a degree. The carry relation also reports itself not
  satisfied there, since nothing turned its input: the lock, said by the
  relation (docs/assemblies/relations.md#intermittent).
- **Negative controls that must fail**: the first carry's Geneva timed 10°
  late must clash its wheel with its transfer over the carry; the first
  gear put half a tooth out must clash the transfer with the tens; and a
  transfer modelled at half the gap, beside the units wheel, must fail
  `check_print`'s clearance item (it measures 0.05 mm). A run whose controls read clean
  stops.

`generator_counter` (generators/README.md#the-big-generators-as-tests) runs
the whole of it but the still, at 10 mm digits on the built-in table, in
about 45 s.

## Printing it on a P1S

1. Build with your measured profile (`--profile "K5 PLA on P1S"`: 0.1 a
   side; "PLA on P1S": 0.15). The community's 0.25 a side needs 10 mm
   digits and prints loose.
2. Slice `counter.stl` as it comes, the base on the bed, at the layer
   height the profile was measured at (0.2 mm), **no supports** and no brim
   inside the housing. Keep elephant-foot compensation on: the bottom wheel
   stands a layer over the base.
3. Off the bed, hold the housing and turn the knob firmly through a full
   turn each way to break the layer interfaces, then count up through a
   carry. The digits are engraved: run a fine paint marker over the bands
   through the windows, or wipe filled acrylic off the face.
4. In use, lay it on its side with the knob to the right: the digits read
   upright through the windows, the units at the knob's end, and the knob
   turned away from you counts up.

Not modelled: the friction the stacked faces carry, and the force a hand
puts through a triple carry. Printed once (2026-10-15, above): it fused, as
the layer interfaces predicted by `stacking` say it must.
