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

How others build worm pairs and thread-mill: `~/Documents/CAD` (FreeCAD's
gear workbench, if present, and `Mod/CAM/Path/Op/ThreadMilling.py` with its
generator and thread CSV tables; OpenSCAD and CadQuery gear libraries under
`parts/`; the slicers' hole compensation). One page of what to copy and what
to avoid, in this plan's phase notes. No code.

### 1. Threads: modelled, and imperial

Built: see docs/modeling/holes-and-threads.md.

### 2. The worm pair

Built: see docs/modeling/worm-gears.md and docs/assemblies/relations.md#worm.
Measured: hobbing by booleans does not scale (8 hob placements in one
multi-tool common took 105 s, about ×7 per doubling), so the wheel is the
same hob's envelope computed point by point and lofted — throated, converging
as 1/n² in the hob's steps, 15–23 s a wheel; neither fallback was needed. The
`gear` relation's fixed sign is wrong for half the ways a pair goes together,
so the pair is coupled by a `worm` relation.

### 3. Bearings as standard parts

Built: see docs/parts/README.md#bearings.

### 4. Printed clearances

A process allowance that fits and threads read: FDM hole shrink and XY
growth per material. It is reported by `check_fit` and `check_fits`, for
example "H7 on a printed bore: model +0.15 mm". Like a tolerance, it drives
no geometry; the generator applies it where the part is modelled. Refused
for a process or material it has no data for.

### 5. The head, assembled

- The parts, the assembly, mates, and the two worm relations.
- Motion studies: pan through 360° and tilt through ±90° with the camera
  plate on, clash-checked at every sample, driven from the worm shafts so the
  relations are what move the axes.
- Clearance assertions: the camera body clears the yoke at full tilt.
- A BOM with the bearings and the bought screws.

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
- **A ThreadMill operation** (FreeCAD's shape): a helix of G2/G3 arcs with Z
  advancing half a pitch per half turn, a lead-in arc, a lead-out, and a
  partial last turn. It is driven by the Thread feature on the hole: size,
  pitch, hand and internal/external. The helix radius is (thread diameter −
  cutter diameter)/2 for an internal thread.
- **A thread-mill cutter shape:** single-profile 60° first; multi-tooth only
  if cheap.
- **Verification judges a thread on its own terms.** The height map cannot
  see a wall that faces sideways, so the check confirms the helix's pitch,
  its depth against the class and that the cutter fits the tap-drill bore.
  The playback draws the helix.
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
