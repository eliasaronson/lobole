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

- A static study of the solid yoke under the camera load at 3 g (handling),
  giving the deflection at the lens and the factor of safety.
- A topology study of the yoke. The keep faces are the bearing bosses, the
  worm housing and the camera plate seat; the volume fraction is chosen for a
  stated stiffness target.
- Re-model over the shape guide, then run the static study again, and compare
  mass and lens deflection with the solid yoke.
- Record what the re-modelling cost: no B-rep comes back from a topology
  study today. That cost is this phase's finding.

### 7. The base plate on the 6090

- **The operations:** face, the pockets, the bearing bore (helical bore to a
  fit) and the tripod thread.
- **The thread mill, its cutter and its verification:** built, see
  [docs/cam/operations.md#thread-mill](../cam/operations.md#thread-mill)
  (the cutter: [#the-thread-mill-cutter](../cam/operations.md#the-thread-mill-cutter);
  the check: [simulation.md#threads](../cam/simulation.md#threads)).
  Multi-tooth cutters are not modelled.
- Workholding with the fixtures from v126.
- Tapping (G84) is not built: the 6090's spindle has no encoder, so it cannot
  rigid-tap. An operation that needs a synchronised spindle is refused on a
  machine without one.

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
