# A printed inline-four engine

## The problem

A detailed model of an inline four-stroke four, printed on a P1S and
assembled by hand: a crank, rods, pistons, a camshaft, valves. It is a
showpiece, and it asks the system for what no print dogfood has needed
together:

- a crankshaft, rods and pistons that run as one mechanism on printed
  bearings;
- valve timing: a cam drive at 2:1 and valves that must miss the pistons
  over a whole cycle;
- springs: the valves have to close again;
- many parts in few plates: a showpiece the user can print in an evening.

It is not the OM615 (generators/om615/), whose parts are real castings. This
one is designed to be printed.

## The shape of the fix

`generators/engine/`, output in `scratch/engine/`. Every size derives from
`--bore` and the print profile. The default bore is the largest at which
every print plate fits the bed in at most two plates, or three if two cannot
hold the detail; the generator searches for it and says which.

### The engine

- **Layout.** Inline four, 1-3-4-2 firing order, single overhead cam, two
  valves a cylinder, flat-plane crank (throws at 0° and 180°), so the
  crank prints flat.
- **Block.** Split at the crank's centreline into an upper block (bores
  vertical, printed deck down) and a bedplate. A cutaway window on one side
  shows the pistons and the crank turning.
- **Crankshaft.** Five main journals, counterweights, a nose for the timing
  drive and a flywheel flange. Printed flat, on the plane of its throws,
  with a flat on its underside if one is needed for the bed.
- **Rods.** Split big ends with caps on printed bolts, or pressed pins at the
  profile's `press` allowance, whichever the size allows. Small ends on
  wrist pins pressed into the pistons.
- **Head.** It carries the camshaft in its bearings and eight valves in
  guides. Each valve closes on a **TPU sleeve spring** (the jar's material),
  sized from its squeeze at full lift. The cam acts on bucket followers.
- **Timing drive.** A printed TPU toothed belt from the crank nose to the
  cam pulley, 2:1 (the `chain` relation). A printed gear train behind a
  cover is the fallback if the belt cannot be made at the size, and the
  generator says why.
- **Also:** a flywheel with timing marks (TDC for cylinder 1), a hand crank
  on the nose, and a sump.
- **Bearings.** Every journal runs in a plain printed bore at the profile's
  gap, at its size.

### In Loböle

- **The assembly:** revolutes for the crank and cam, sliders for the pistons
  and valves, rods closing slider-crank loops (as om615 does), `gear` /
  `chain` for the drive, and `cam` relations from the camshaft to every
  valve.
- **A 720° motion study.** Every sample is solved and clash-free. The
  valve-to-piston clearance is a requirement measured over the study
  (`measure: {kind: clearance, …, motion_study}`). Point traces of each
  valve are compared with the cam table.
- **Timing checked from the study:** at cylinder 1's compression TDC both its
  valves are shut, and every cylinder fires 180° after the one before it in
  1-3-4-2.

### Checked every run

- `check_geometry` verified on every part; `check_print` every item on every
  plate, with the TPU parts on their own plate;
- the 720° study, the clearance and the timing above;
- **negative controls that must fail:**
  - the cam pulley one tooth off: a valve meets a piston, or the timing
    check fails;
  - a rod 10 % long: the piston meets the head.

### What it predicts

The values for the README's test card, all checkable on the bench with
calipers and the flywheel's marks:

- swept volume and the geometric compression ratio at the scale;
- valve lift;
- the valve events in crank degrees (opening and closing of each valve);
- the crank angle between TDC marks.

## Files

- `generators/engine/` (the generator and its README), a row in
  `generators/README.md`, a `generator_engine` test.

Gaps the build finds in the app are reported, not worked around silently.

## Verified by

- the generator's checks and negative controls at the default bore and one
  other;
- `generator_engine` in the generators suite, and the headless suite;
- on the printer: the crank turns the engine through 720°, the valves open
  at the flywheel marks the card gives, and none touches a piston.
