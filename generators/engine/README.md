# A printed inline four

A four-stroke inline four in PLA, printed on a P1S and assembled by hand:
a flat-plane crankshaft in a split block, four rods with lapped caps on
pressed pins, pistons on pressed wrist pins, a single overhead camshaft of
pressed lobes on bucket followers, eight valves closed by TPU bellows
springs, a printed TPU toothed belt at 2:1, a flywheel with TDC marks and a
hand crank. Nothing is bought: every pin and dowel is printed and pressed.

It is not the [OM615](../om615/README.md), whose parts are castings. It is
designed to be printed, and it asks the app what no print dogfood had asked
together: a mechanism of four slider-crank loops on printed bearings, valve
timing, springs, and many parts in few plates.

    tools/headless-preview.sh --start <port>
    python3 generators/engine/build.py --port <port> --profile "K5 PLA on P1S"
    python3 generators/engine/build.py --port <port> --profile "K5 PLA on P1S" --bore 32

The profile must state its bed (`bed_mm`): the plates are packed into it.
`print_coupon.py --seed` gives the P1S profiles theirs.

Output, untracked, in `scratch/engine/` (or `$LOBOLE_SCRATCH/engine`, or
`--out`): `engine.dsgn`, the receipt `engine.json`, the test card
`engine_card.md`, `plate1.stl` and `plate2.stl` (PLA), `plate_tpu.stl`, the
[print jobs](../../docs/tolerances/README.md#print-jobs) `engine_job.3mf`
(the PLA plates, a Bambu project when `$LOBOLE_SLICER_CONFIGS` holds
`pla.json`) and `engine_tpu_job.3mf` (the TPU plate, a spool of its own),
and two stills (`engine.png`, `engine_section.png`). The plates pack round
the profile's keep-out zone, and every piece is its own object in the job.

| file | what |
|---|---|
| `engine_spec.py` | every size from the bore and the profile; the valve timing, the cam lobe and its lift table, the belt's path and teeth, the footprints, the packer and the bore search, and a 2-D sweep of the rods through the crankcase |
| `engine_parts.py` | one function per part, each where it sits in the engine (or in its own frame), with its print orientation |
| `build.py` | the parts, the assembly, the studies, every check and control, the predictions, the plates, the card |

## The engine

- **Layout.** Inline four, firing 1-3-4-2, flat-plane crank (throws at 0°,
  180°, 180°, 0°), bore = stroke, single overhead cam, two valves a
  cylinder in one row under it. The engine frame: X the crank axis, the
  timing belt and hand crank at the front (-X), the flywheel at the rear;
  seen from the front the crank turns clockwise.
- **Block**, split at the crank's centre line. Four bores in a narrow
  barrel block over a crankcase that flares out at 45°, so the block
  prints deck down with no overhang; five bulkheads carry the mains' upper
  halves. A pointed cutaway window on the +Y side opens into each bay and
  bore. A notch at each bore's mouth, across the rod's width only, clears
  the rod's corners where the bore's chord is narrower than the rod swings
  (a real block's rod clearance notch); the siamesed walls between bores
  keep their thickness. The TDC pointer is engraved in the rear face over
  the flywheel's rim.
- **Bedplate** with the mains' lower halves, walls round the crank's
  sweep, open to the **sump** below; the engine stands on the sump.
- **Crankshaft**: five mains and four pins of one diameter, eight webs,
  a nose and a tail. It prints on the plane of its throws, so everything
  is cut flat `r/√2` under the axis: the deepest flat that leaves no round
  face looking down past 45°. The nose and tail are smaller rounds with a
  90° keel down to that flat, which both prints and keys what is pressed
  on (the pulley, the hand crank's arm and the flywheel go on one way
  only, so the flywheel's marks and the pulley's teeth are where the
  timing says).
- **Rods** with split big ends. Each side of the bore the rod and its cap
  overlap in a half lap, rod in one half of the thickness, cap in the
  other, and two pins pressed through both laps hold the cap (vertical as
  each prints, since the laps print on the bed). Wrist pins are pressed
  into the pistons through teardrop holes (pointing at the crown, which
  prints down), and turn in the rods' small ends.
- **Head.** Per valve a counterbore and a 45° seat, a throat, a port at
  45° out to the side (intake +Y, exhaust -Y), a guide and a bucket bore;
  over them the cam's trough and its lower saddles. It prints deck down.
  The **cam cover** holds the upper saddles in ribs between the troughs,
  printed top down.
- **Valve train.** The camshaft is a D-flatted bar (printed on its flat)
  with eight lobes, the cam pulley and a rear collar pressed on it: each
  lobe's D-bore sets its angle, and its number is engraved on its nose.
  Each lobe is the hull of a base circle and a nose circle acting on a flat
  bucket, so its lift is exactly the support function the cam relation's
  table carries. The bucket is pressed on the valve's stem tip and is the
  spring's retainer; the spring is a **TPU bellows** sleeve in the bucket
  bore, its walls no flatter than 45° so it prints standing.
- **Timing belt**: a printed TPU T-profile belt, pitch 4 mm (ten lines),
  on an 18-tooth crank pulley and a 36-tooth cam pulley (16/32 at bore
  32: the smallest even count whose root clears the keel). The cam's height
  is raised the fraction of a pitch that makes the belt a whole number of
  teeth; the valve guides take it. The pulleys' grooves are the envelope of
  a belt tooth rolling in and out along the straight spans, not only its
  seated shape: seated grooves alone let the teeth just off the arc touch
  the pulley's. Each pulley has one flange, on opposite sides, so the belt
  is held between them. The belt is the fallback's opposite: it was made,
  so the gear train was not.
- **Flywheel** with a tick every 10° and longer every 30° on its rear face,
  `1 4` at cylinders 1 and 4's TDC and `2 3` at 2 and 3's; a **hand
  crank** on the nose, its handle pressed in.
- **Dowels** pressed into both halves of each joint: block and bedplate,
  bedplate and sump, block and head, head and cover.

## Where every size comes from

Two inputs: `--bore` and the print profile (`print_allowance` for each
running gap and press). Fractions of the bore are design choices said on
their lines in `engine_spec.py`; a size with a printing floor (a wall of
five lines, a stem of eight) records when it sits on it, and the run says
which. The rest:

- **Running gaps.** Every journal, pin and bucket in a plain printed bore
  at the profile's gap at its size; axial play the gap a side.
- **Presses** at the profile's `press` allowance (0, the built-in default:
  K5 PLA has no press row yet), every pressed face marked a **printed
  press** (`set_tolerance {printed: "press"}`), so the clash check relieves
  and lists them.
- **The tolerance stack**, `c_stack`: five printed sizes in a chain
  (rod, piston, deck, seat, valve), each about the gap a side. It is the
  least the valves may come to the pistons, and the rods to the crankcase.
- **Rod length** is the longer of 1.8 strokes and what keeps the pistons'
  skirts in their bores over everything turning with the crank. **The
  squish** is the least that keeps the valves `c_stack` off the pistons
  over the cycle (the model's lift against the piston's drop, found on a
  0.1° grid); a rod 10 % long must then reach the head.
- **Valves** 0.36 bore across, lift a quarter of that (L/D 0.25, curtain
  area = throat area), lash the gap a side at the bucket. The cam's lobe
  is sized for that lift and lash and the timing below, its base circle
  the larger of the bar's and what keeps a nose; the lobe's width is what
  keeps the follower's contact on the bucket's face.
- **Springs** from their squeeze: 10 % with the valve shut, 30 % at full
  lift, so the free length is lift / 0.2.
- **The default bore** is searched: the largest whole millimetre whose
  PLA pieces, by their footprints as they print, pack into **two** of the
  profile's beds, else three, kept clear of its keep-out zones. At K5 PLA
  on P1S that is **37 mm** in two plates (38 before the P1S's front-left
  corner was kept clear): 38 needs a third, and from 49 the crankshaft is
  longer than the bed. The plates are packed again from the built parts' measured boxes,
  and each one checked.

### Valve timing

A mild road cam, crank degrees after the cylinder's firing TDC: exhaust
opens 140 (40 before BDC), closes 370 (10 after TDC); intake opens 350,
closes 580 (40 after BDC). These are the PRINT's events, where the lift
passes the lash. The model's bucket rides a constant `lash + seat travel`
off its lobe at every angle, so a lobe never touches its bucket in the
clash check; its valve opens a lash further than the print's while it is
open, which errs safe for the valve-to-piston check.

## What is checked, every run

- `check_geometry` verified on every part; the rods' and webs' sweep
  through the crankcase in 2-D before anything is built.
- **The 720° study**: the crank driven from 0 to 720° at a fifth of a belt
  tooth (4° at 37 mm), every sample solved and clash free at a mesh
  deflection of a fifth of the smallest gap, the belt hidden (it cannot
  move with its pulleys, below). Every pressed pair is relieved as a
  printed press and listed (74).
- **Timing from the study's traces**: each valve's lift against its
  lobe's table (5e-7 mm), its events against the timing above (within a
  sample), each piston's TDCs, cylinder 1 firing at crank 0 with both its
  valves shut, and the four firing 180° apart in 1-3-4-2.
- **The springs' squeeze** from the buckets' traced undersides: 10 % and
  30 %.
- **Requirements** in the project: each valve's least clearance to its
  piston over the study (`clearance` with `motion_study`, exact), at least
  `c_stack`; and the study clean (`motion_clean`).
- **The belt**: the same 720° with the belt shown, sampled at every crank
  pulley tooth, where both pulleys stand where the belt's teeth were
  modelled: clean.
- **Negative controls**, which must fail and do:
  - the cam pulley a tooth off (20 crank degrees at 37 mm): every intake
    valve meets its piston at the overlap, and every valve's lift and
    events leave its lobe's table;
  - rods 10 % long: every piston meets the head.
- **Every print plate's `check_print`**, every item, `bed_fit` in the
  profile's bed and clear of its keep-out zone included, every piece on
  the bed; the TPU plate printed by the same profile's printer, its pieces
  TPU, with the limits the PLA rows would set stated (no TPU table).

`generator_engine` (generators/README.md#the-big-generators-as-tests) runs
all of it but the stills, on the seeded P1S profile.

## The default engine, and its test card

`--profile "K5 PLA on P1S"`: bore and stroke 37 (38 before the plates
packed round the P1S's keep-out corner), rod 66.6, cylinders 41.1 apart,
the deck 101.2 over the crank and the cam 151.6; valves 13.3 on 4.0 stems,
buckets 14.8, lobes 6.2 wide, springs 16.7 free; belt 103 teeth on 18/36.

| what | predicted |
|---|---|
| swept volume, four cylinders | 159.1 cm³ |
| geometric compression ratio | 29.9 : 1 (a 1.3 mm squish over a flat crown; the ports are open, so it compresses nothing) |
| valve lift | 3.33 mm, lash 0.10 |
| valve events | as above, every valve within a sample |
| firing | 1-3-4-2, TDC marks 180° apart |
| springs | 10 % / 30 % of 16.7 mm |
| valve to piston, least | 0.51 mm |

| plate | pieces |
|---|---|
| `plate1.stl` | block, bedplate, sump, head, cam pulley, two rods, the caps, a bucket and two valves, the handle, wrist pins, dowels and cap pins (43 pieces, 101 mm tall) |
| `plate2.stl` | crankshaft, flywheel, cam cover, two rods, cam bar and lobes, pistons, the crank arm and pulley, the other buckets and valves, collar (34) |
| `plate_tpu.stl` | the belt and eight springs |

Orientations, each piece's place on its plate and the assembly order are
on the card. No supports, no brim. Modelled solid the two PLA plates are
1569 g of PLA (the block, head, bedplate and sump are most of it); printed
at Bambu Studio's 0.20 mm Standard (2 walls, 5/3 shells, 15 % grid, the
settings every PLA part states) `mass_properties` weighs them 581 g, 37 %
of solid, and Bambu's own slice of `engine_job.3mf` 611 g (443 g and 168 g
a plate). Not an evening's print, the plan's hope, but two long ones.

## Not modelled, and what the app could not say

- **A spring has one length.** Nothing in the app is compliant, so the
  assembly shows each spring as it is at full lift (a separate part) and
  the printed one at its free length; the squeeze is computed from the
  buckets' traces, not seen.
- **A belt cannot move.** It is a part fastened to the block; its teeth
  meet the pulleys' only at the samples where both pulleys have turned a
  whole tooth, so the 720° study hides it and a second study checks it at
  those samples. The teeth on the straight spans are where the belt was
  modelled, not where a running belt's would be.
- **Contact, friction and the springs' force** are the bench's: whether
  a hand turns it easily, and whether TPU bellows close the valves against
  the buckets' friction, is what the print answers.
- **A TPU part has no allowance table** (the tables carry PLA and PETG):
  its plate is printed by the PLA profile's printer for its bed and layer,
  its pieces TPU by their material, and states every limit the PLA rows
  would set. The springs state only their walls, so the TPU plate has no
  printed mass.
- **No draft on an extrude**: a lobe printed standing would need one (or
  an intersection with a cone) under every lobe; the lobes are separate
  parts pressed on a bar instead.
