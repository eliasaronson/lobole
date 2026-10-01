# Pan/tilt head dogfood

A geared pan/tilt head for a camera: a worm drive on each axis, printed
bodies, and an aluminium base plate cut on the 6090. It is a real object with
a plain test: it holds a camera still, turns smoothly without play, and screws
onto a tripod. Getting there needs four things the app does not have:

- **Helical geometry.** `Thread` is cosmetic only, and nothing builds a
  helical tooth, a worm or a worm wheel.
- **Bearings.** The standard parts are fasteners only.
- **Imperial threads.** The thread tables are ISO metric only. A camera
  mounts on 1/4"-20 and a tripod on 3/8"-16, both UNC.
- **CAM threads.** There is no thread-milling or tapping operation.

It also exercises topology optimisation, FEM, motion studies, fits, drawings
and CAM on one assembly, as the chess set did for turning.

The generator lives in `generators/pantilt/` and writes into
`scratch/pantilt/`, the same shape as `generators/chess/`: one stage script
per part, a `run_all.sh PORT [--from STAGE]`, and a log ending in `done` or
`failed`.

## The design

- **The load:** a mirrorless camera and lens of 1.5 kg, its centre of mass
  80 mm above the tilt axis and 40 mm in front of it. Sizes derive from this
  load and from the bearings and the worm pair chosen for it, not from
  literals.
- **The pan axis:** the base plate (aluminium, CNC) carries a bearing pair and
  the pan worm wheel. A 3/8"-16 thread in the plate mounts it on a tripod.
- **The tilt axis:** the U-shaped yoke rides on the pan wheel and carries the
  tilt worm pair and the camera plate. The camera plate takes a 1/4"-20
  screw.
- **Both worm pairs:** single start, about 40:1, and self-locking: the lead
  angle is under the friction angle for the printed pair, and the check says
  so.
- **The rest is printed in PETG**, with the clearances a printer needs.

## Phases

### 0. Survey

Done: see [pan-tilt-survey.md](pan-tilt-survey.md).

### 1. Threads: modelled, and imperial

Built: see docs/modeling/holes-and-threads.md.

### 2. The worm pair

Built: see docs/modeling/worm-gears.md and docs/assemblies/relations.md#worm.
Measured: hobbing by booleans does not scale (8 hob placements in one
multi-tool common took 105 s, about ×7 per doubling), so the wheel is the
same hob's envelope computed point by point and lofted — throated, converging
as 1/n² in the hob's steps, about 2 s a wheel; neither fallback was needed. The
`gear` relation's fixed sign is wrong for half the ways a pair goes together,
so the pair is coupled by a `worm` relation.

### 3. Bearings as standard parts

Built: see docs/parts/README.md#bearings.

### 4. Printed clearances

Built: see docs/tolerances/README.md#printed-parts.

### 5. The head, assembled

Built: see [generators/pantilt/README.md](../../generators/pantilt/README.md).
The numbers that matter:

- **Sizes from the load.** Module 1.95 by Lewis at 3 g, taken up to 2:
  d1 20, d2 80, centre distance 50, face 13.4. A PETG shaft for the 3 g
  moment needs Ø14.4, so the main bearings are 6002 (four); the worm
  journals need Ø8.5, so 609 (four). The arms put the tilt axis 224 mm up:
  the camera reaches 160 mm below it in ±90° of tilt.
- **Masses.** Head without the camera 1.51 kg (base plate 260 g of
  aluminium, yoke 589 g, tilt wheel and shaft 126 g, cradle 92 g, pan wheel
  73 g, each worm 23 g); with the 1.5 kg camera, 3.01 kg.
- **Motion.** Pan 360° (40 worm turns) and tilt ±90° (±40 turns), driven
  from the worm shafts, 48 samples each, every pair checked exactly: both
  clean, no unsolved sample. They cost 715 s and 360 s (measured before
  the wheel's teeth were cut by one replicated sector).
- **Clearances.** The camera clears the yoke by 4.8 mm at −90° (the designed
  `cam_clear`), 10.2 mm at 0 and +90°; the cradle runs 1.2 mm off the arms.
  The standing exact sweep is clean.
- **Fits.** The eight bearing seats are reported transition fits (K7 in the
  aluminium pocket; ±0.15 on the printed bores, modelled 0.3 over and read at
  what they print). `check_seat` passes every outer ring; the inner rings
  are unverified, since a one-solid bearing in a mechanism is mated to one
  seat only. The 14 other rows are frame-anchored mates, which have no
  surface to size.
- **Bought parts.** 4 × 6002, 4 × 609, 4 × M4x16 into the plate, 8 × M4x25
  with nuts and washers through the worm blocks, the 1/4-20 camera screw.

### 6. The yoke: FEM and topology

Built: see [generators/pantilt/README.md#the-yoke-under-load](../../generators/pantilt/README.md#the-yoke-under-load)
and `probe_study` ([docs/simulation/README.md#probing-a-face](../simulation/README.md#probing-a-face)).
The numbers that matter:

- **Solid yoke** (phase 5's, 591 g) at 3 g level: the lens moves 5.5 mm and
  the camera pitches 92' -- 31' under its own weight. Tilted to the longest
  lever, 10.3 mm and 210'; sideways, 13.1 mm. Factor of safety 2.7 against
  PETG's yield, 2.2 across layers (sideways, 18 MPa).
- **Topology**, the envelope at the solid's volume (fraction 0.48), the
  gear's worst case: 30 iterations, 2 h on the CPU (the GPU solver fails on
  the first solve of a part this slender). Its layout follows its one load
  case -- the -Y arm a single strut, an empty quadrant where the pan blocks
  bolt -- so the re-model reads it rather than traces it.
- **Re-modelled yoke**, 1051 g: the lens 1.8 mm / 29' level (a third of the
  solid's, the stated target), 3.2 mm tilted, 4.2 mm sideways; factor of
  safety 2.4, 1.9 across layers. It is 1.8 times the mass for 3.1 times the
  stiffness: held at its underside instead, the lens moves 0.46 mm, so three
  quarters of what is left is the Ø15 PETG pan journal and the base around
  it. The pan bearings were sized for strength; their stiffness is what a
  lighter yoke would need next.
- **Printed** as three pieces on a 256 mm bed (base, each arm), bolted with
  four ISO 4762 M4 and nuts; the assembly's sweep and both motion studies
  run clean with it.
- **A phase-5 defect:** the pan journal stopped inside the wheel's bearing
  and never reached the plate's, so the yoke hung on one bearing. Fixed.

**What the re-modelling cost.** About four hours of a session after the
study, eight build-and-study iterations at 3 to 6 minutes each, and some
200 lines of generator. Nothing comes back from the study but a picture and
an STL: the density was read off section screenshots taken by hand (the two
arms overlap in any whole view), each window was a polygon whose insets were
computed in Python, and no static study can be run on the optimised shape
itself, so whether the layout meets the target is only known after the
re-model is built. There is no snapping to the shape guide, one load case
per study (the layout is that case's), and no keep-out body: the first
envelope overlapped the tilt worm's blocks and it showed only in the result.
The other cost was getting a study to run at all: the solid yoke would not
mesh (5 µm slivers from single-precision sketch planes, which main has
since made double), the advice to Refine it renamed every face, and the
app's topology had run on the CPU for a month (fixed here).

### 7. The base plate on the 6090

Built: see [generators/pantilt/README.md](../../generators/pantilt/README.md#the-base-plate-on-the-6090).
The numbers that matter:

- **Two vise setups** from a 90 x 90 blank saw-cut from 30 mm plate. Top:
  face, clear round the disc to 2 mm over the jaws, the 6002 seat (rough,
  finish, spring pass), relief, four M4 tap holes, chamfers. Underside
  (flipped, gripped on the finished disc): face, clear the rest, the 5/16
  tap bore and the 3/8-16 UNC-2B thread (pitch diameter 8.569 in 2B's
  8.494-8.645), chamfers. Both verify ok AND complete at 0.1 mm.
- **37.8 min** of program (Top 20.1, Underside 17.7) plus six manual tool
  changes and the flip.
- **K7 is not a router's tolerance.** Ø32 K7 is 25 µm wide (+7/-18); the
  program cuts 32.000, inside it. Runout, deflection and quadrant reversal
  all push a router's bore over size, so the seat is cut, gauged in the
  setup, and re-cut with the finisher's diameter corrected; retaining
  compound on a slip fit is the fallback.
- **Not measured:** the 6090's traverse rate and travel, its spindle's
  power, speed range and runout, the vise and parallels, the ER20 nuts and
  stickouts, and the real size error of a bore.
- **App fixes it took:** clear-round (adaptive or pocket on a part's outside
  wall), a hole or thread that opens downward refused, a face's exact extent
  for derived depths, a bore's rough no longer cut under its floor, a small
  chamfer on a round rim read at its own angle, and thread marks that follow
  a face a later feature renames.

### 8. Drawings, renders, retrospective

- The base plate's drawing: the thread callout (`3/8-16 UNC-2B`), the bearing
  bore with its fit, and the datums.
- The assembly drawing with the BOM, and renders.
- A retrospective as the chess set's phase 6: where the time went, what
  blocked, and what the development loop should change.

## Format

Phases 1, 3, 4 and 7 each change what a document saves (a thread's
designation and class, a bearing part, a process allowance, a thread-mill
operation and its tool). Each one bumps `kVersion` with a golden and a row in
`docs/versioning/format.md`.

## Verification

- `run_all.sh` builds the whole head from nothing, and its log ends `done`.
- Every feature regenerates without error. Both motion studies run
  clash-free. Every fit reports its intent met.
- The yoke study reports lens deflection and safety factor before and after
  topology.
- The base plate's job verifies clean, including the thread.
- Each new capability has its own tests: a modelled M8 and a 3/8-16, a worm
  pair that meshes through a full turn with no clash beyond the backlash, a
  bearing placed and its seat checked, a thread-mill helix at the right
  pitch and depth.
- At the machine (the user): the plate cut, the head printed and assembled on
  a tripod with a camera.
