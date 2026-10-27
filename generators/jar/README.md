# Screw-top jar with a TPU gasket

A PLA jar and lid with modelled threads, and a TPU 95A gasket ring pressed
into the lid. The lid screws down until its skirt lands on the jar's
shoulder, and at that stop the jar's rim sinks a quarter of the gasket's
thickness into it. Filled with water and turned upside down, the gasket is
what keeps it in. The first thread printed from Loböle, and the first
part made in two materials.

    tools/headless-preview.sh --start <port>
    python3 generators/jar/build.py --port <port> --profile "K5 PLA on P1S"
    python3 generators/jar/build.py --port <port> --inner-diameter 80 --volume 400

Output goes to `scratch/jar/` (or `--out`), each STL in the orientation it
prints in:

| file | material | prints |
|---|---|---|
| `jar.stl` | PLA | upright |
| `lid.stl` | PLA | top down |
| `gasket.stl` | TPU 95A | flat |

There is also `jar.lob` (the parts, the assembly `screw-top jar` and the
negative controls), `jar.png` and `jar-section.png` (a cut through the
axis), and `jar.json`, the receipt: every size, the allowances it used,
the clearances, the measured squeeze and every check's verdict.

With a profile that states a bed, the run also writes two
[print jobs](../../docs/tolerances/README.md#print-jobs), one a spool:
`jar_job.3mf` (the jar and lid side by side on one PLA plate) and
`jar_tpu_job.3mf` (the gasket on a plate of its own, carrying its 1 wall,
2/2 shells and 15 % gyroid per object, below, printed with the TPU profile
on the same printer: `--tpu-profile`, else the one TPU profile on the PLA
profile's bed, which `print_coupon.py --seed` installs as `TPU 95A on
P1S`). Each is a Bambu project when
`$LOBOLE_SLICER_CONFIGS` holds `pla.json` and `tpu.json`
(`tools/bambu-config.py`), else a one-plate 3MF Bambu opens with your own
presets. Sliced by Bambu's CLI, the gasket prints one wall on each of its
two outlines and two solid layers top and bottom; Bambu weighs it 0.89 g of
TPU in 2.5 minutes, and `mass_properties` estimates 0.80 g printed against
1.15 g solid. The jar and lid's plate: 67.8 g in 1.5 h.

## Where every size comes from

The inputs are `--inner-diameter` (the opening, 60 mm) and `--volume`
(200 ml), and the print process: `--profile NAME`, one of your print
profiles (docs/tolerances/README.md#print-profiles), or else `--process
fdm --material pla`, the built-in table. The rest derives from them. At
the defaults:

| | rule | at Ø60, 200 ml |
|---|---|---|
| wall, floor | a thirtieth of the opening, whole lines, four at least | 2.0 (5 lines) |
| inner height | the volume over the bore's area | 70.7 |
| pitch | a fifteenth of the opening, to the half millimetre | 4 |
| flank angle | 45° (below) | 45° |
| thread depth | 5H/8, H = P / (2 tan 45°) | 1.25 |
| basic major | the bore plus the wall plus the depth, each side | Ø66.5 |
| threaded length | two turns | 8 |
| outside | the lid's major plus a wall | Ø70.9 |
| gasket | a thirtieth of the opening in whole layers, six at least | 2.0 (10 layers) |
| squeeze | 25 % of the gasket | 0.5 |
| groove depth | half the gasket, in layers | 1.0 |

The jar is 72.7 mm tall to the rim and the lid 13.5. Both fit a P1S's
256 mm plate side by side.

## The thread

A single-start, right-hand thread with **45° flanks**: the Thread feature's
`flank_angle_deg` (docs/modeling/holes-and-threads.md#flank-angle). An ISO
60° flank lies 30° off the horizontal, so the flank that faces down
overhangs 60° from vertical. That is the load flank on both parts: on the
jar, printed upright, and on the lid, printed top down. At 45° every flank
is at the usual printable limit and needs no support. The flats are ISO's
(P/8 at the crest, P/4 at the root), only shallower.

The **print allowance** is the table's thread rows, applied the way
`PrintedThreadFor` says to model a printed thread, as a custom thread on
the face. The neck is a boss at the basic major plus the
`external_thread` allowance (Ø66.4 for −0.1). The lid is a bore at the
basic minor plus the `internal_thread` allowance (Ø64.4 for +0.4). The
generator reads each thread's `print` report back and requires that both
print at the basic Ø66.5 major.

What that leaves is a running fit: 0.25 mm radial at crest and root, and
0.5 mm of axial play. Centred, that is 0.18 mm along each flank's normal.

## The seal

Two rings keep the water in, and the project states both as requirements
(`seal`, [seals](../../docs/inspect/README.md#seals)), checked every run:

- **the gasket on the rim**, at least the rim's flat wide (0.8 mm): the
  rim's flat and chamfer pressed into it read 1.39 mm wide at the defaults,
  and 0.38 mm deep on average there (the overlap's area over its width);
- **the skirt on the shoulder**, at least two lines wide (0.8 mm, the
  narrowest ring that prints solid): the lid's skirt lands flat on the
  jar's shoulder, a ring 2.00 mm wide. The first print had no gasket, and
  this ring alone held water upside down.

The squeeze is set by a **hard stop**, not by torque. The lid's skirt
lands on the jar's shoulder, and at that height the rim is a fixed depth
into the gasket. "Hand-tight" then becomes a dimension instead of a
feeling. The rim is the top of a lip a line inside the thread's root, so
the groove runs out through it and the lid's crests clear it. It is
chamfered on the outside to a flat two lines (0.8 mm) wide. That flat,
and the chamfer below it, is what presses into the gasket: a narrow
contact needs less force for the same squeeze. The rim clears the ceiling
by 0.5 mm with the gasket squeezed.

The gasket sits in a 1 mm groove in the lid's ceiling. Its outside is
pressed in by 0.1 mm a side, and its bore clears the groove's boss by
0.1. The groove's walls take the PLA `hole` and `shaft` allowances, the
gasket the TPU table's (docs/tolerances/README.md#tpu): it is printed in
fdm tpu, its bore modelled +0.2 and its outside −0.1 on the diameter, both
rows to confirm with a coupon.

**The force.** Bambu Lab gives TPU 95A a Young's modulus of 7.8 MPa. A
solid gasket squeezed 25 % under the rim's 1.3 mm-wide contact takes
roughly E · 0.25 · 250 mm² ≈ 500 N. That is an upper bound, since the
rubber also spreads sideways. Through a 45° thread at a friction of 0.3,
plus the gasket rubbing on the rim, it takes on the order of 10 N·m:
more than a hand turns a 71 mm lid. A solid gasket therefore stops short
of the stop. It still squeezes, but by whatever the hand manages. Print it
soft instead (below). The squeeze the geometry sets is the same either
way; the infill sets the torque needed to reach it. That torque is an
estimate the generator does not check: no study here models a 25 %
elastomer strain, and a linear one would not be honest at that strain.

## What is checked, every run

- **Each part is verified** (`check_geometry`), and each thread's `print`
  report prints the basic major.
- **The lid is upside down and on its stop.** It is placed by mates on
  frames taken from circular edges (the shoulder's, the mouth's, the
  groove floor's, the gasket's). The lid's mouth must land on the
  shoulder and its groove floor at the height above that only an
  upside-down lid has.
- **The angle at which the threads meet is FOUND.** It is not computed
  from where the kernel starts each helix. The lid is turned through a
  revolution at its stop, 15° at a time, and the exact interference of lid
  and jar is read at each step. The clean window is refined at both edges
  to a tenth of a degree, and the lid is set in its middle. The window
  must match the axial play over the lead (45° at the defaults; 45.2° is
  measured).
- **The squeeze is measured on the solved assembly**, in two independent
  ways. The first is the rim flat's height against the world height of the
  gasket's underside, as the mates put it: 0.500 mm, 25.0 %. The second is
  the exact overlap of gasket and jar against the rim's section above that
  depth, revolved: 100.7 mm³ against 100.7, within 2 %. The fraction must
  lie in 20-30 %.
- **Both seals close all the way round** (`seal` requirements, above), each
  at least its width.
- **Nothing else clashes, nothing floats** (`asm.check`): the gasket's
  overlap with the jar is the one contact allowed, by its measured volume.
  Near misses are not raised, since the thread and the gasket's bore are
  running fits.
- **It screws on** (an `assembly_sequence` requirement,
  [sequences](../../docs/assemblies/README.md#assembly-sequences)): the
  gasket pressed into the lid's groove, and the two screwed onto the neck
  as one, turning a turn a pitch along the modelled threads, from clear to
  the stop: pressed only the squeeze (0.5 mm), forced nothing past it.
  (Read with the turn spread over the longer of the two ways' travels,
  before the check followed the helix itself, it was forced 7.3 mm.)
- **It unscrews.** A screw relation backed by the lid's Thread feature
  (its custom pitch and hand,
  docs/assemblies/relations.md) couples the lid's turn to its rise. A
  motion study turns it two turns out from where the gasket lets go, with
  the mesh clash check at a fifth of the flank clearance. It must be
  clean.
- **Printability**: `check_print`
  (docs/tolerances/README.md#checking-a-print) on each part in its print
  orientation: on the bed, one watertight piece, and nothing facing down
  more than 45° from vertical off the bed (each part's process's limit,
  the gasket's TPU's too). The angles are the B-rep's
  surface normals, so the 45° thread flanks and edge chamfers read 45.0
  with no area past it and need no slack. Each part's whole report is in
  the receipt's `stl.<part>.print`.

Four **negative controls**, each of which must fail its check or the run
stops:

- the lid turned half a pitch out of phase at its stop interferes with the
  jar (500 mm³);
- a lid whose skirt is longer by the squeeze and a layer stops short:
  −0.2 mm, −10 %;
- a lid with a slot a wall wide through its skirt's foot: `check_seal`
  finds the ring on the shoulder open at the slot (178.5-181.5°);
- the same jar with an ISO 30° flank has about 1050 mm² past the overhang
  limit (worst 60°).

The generator test `generator_jar`
(generators/README.md#the-big-generators-as-tests) runs the default build
without the stills.

## Printing it on a P1S

**The PLA parts.** Use 0.2 mm layers, no supports and no brim. Turn
elephant-foot compensation on: the first layer is the jar's bottom edge
and the lid's top edge, both chamfered 0.4 mm for it.

- **Walls: five** (the wall is 2.0 mm, five 0.4 mm lines). That makes it
  solid. With fewer walls and sparse infill between them, water finds the
  gaps between lines and weeps through. Use ten bottom layers on the jar.
- Print the jar upright and the lid top down, as the STLs come. The lid's
  top on the textured plate gives it some grip.
- The thread's fit comes from the profile's `internal_thread` and
  `external_thread` rows, not its gap. On the built-in table those are
  community numbers. If the lid binds or rattles, print `print_coupon.py`'s
  thread pair, record whether it went (`--record ... --thread-went`), and
  rebuild with that profile.

**The TPU 95A gasket.**

- **Dry it** at 70 °C for 8 hours, Bambu's figure for TPU 95A. TPU takes
  up moisture within hours, and wet TPU strings and foams.
- **Not through the AMS.** TPU 95A is too soft for the AMS. Feed it from
  the external spool holder behind the printer.
- Use Bambu Studio's "Bambu TPU 95A" (or "Bambu TPU 95A HF") filament
  preset. It sets the nozzle at 220-240 °C, the plate at about 35 °C, and
  caps the volumetric speed: about 3.6 mm³/s for TPU 95A and 12 for the
  HF grade. Keep the part cooling fan on.
- Print it flat at 0.2 mm, so its 2.0 mm is exactly ten layers. Use no
  brim and no supports.
- **Print it soft: 1 wall, 2 top and 2 bottom layers, 15 % gyroid** (the
  gasket part's slicer settings, which `jar_tpu_job.3mf` carries). A
  solid 95A ring takes more torque to squeeze than a hand gives (see
  above). With one wall, the walls lie outside the rim's contact (the
  gasket spans r 29.3-31.9 and the rim touches r 30.0-31.3), so the rim
  presses on skin over infill.
- Press it into the lid's groove with a thumb, working round. If it falls
  out when the lid is upside down, a drop of CA glue holds it.

**Testing it.** Screw the lid down until the skirt meets the shoulder.
Fill the jar halfway with water, close it, and hold it upside down over a
paper towel for a few minutes, then leave it there for an hour.
