# The 4th axis

Two different things share the name. **Indexed** 4-axis turns the work to an
angle and then cuts ordinary 2.5D; **continuous** 4-axis turns A while the
tool cuts. Both need a machine with `hasRotaryAxis`: without it the rotary
index is refused, the rotary verbs are not on the toolbar, and no file can
carry an A word. Inside the app the rotary axis is setup-space X, the chuck
at its low end, and A turns right-handed about it; the machine's `rotary`
says what that is on the machine.

Code: `CamMount.{h,cpp}` (the mount, which setups are held in the rotary, the
chuck and tailstock envelope), `CamSetup.cpp` (index, frames), `CamSurface.{h,cpp}` (rotary patterns,
`DeriveRotaryTwist`), `CamSurfaceStrategies.cpp` (`GenerateRotarySurface`),
`CamRotary.{h,cpp}` (radius map, blank, rotary verdict), `CamRotaryFeed.{h,cpp}`
(feed on a turning move), `CamPost.cpp` (scaled F, `G92 A`), `CamGcode.cpp`
(modal A, `G92 A`), `CamSim.{h,cpp}` (`RotaryStockSim`).

## How the rotary is installed

A rotary can lie along either machine axis, with its chuck at either end, be
driven by A or B, and turn either way for a positive jog. None of that can be
guessed, so the machine profile states it (`Machine::rotary`, saved as
`rotary`):

| field | values | what it says |
|---|---|---|
| `along` | `x` \| `y` | the machine axis the rotary lies along |
| `chuck_end` | `-` \| `+` | which end of that axis the chuck is at |
| `word` | `A` \| `B` | the letter the controller drives it with |
| `top_moves` | `+x` `-x` `+y` `-y` | which way the top of the part moves on a positive jog; perpendicular to `along` |

`top_moves` is what you SEE at the machine (jog +1°, watch a pen mark on
top of the stock), so there is no handedness to reason about. The four are
stated together or not at all: half a statement is refused at load and at
`save_machine`, naming what is accepted.

`cam::MountOf` turns the statement into a quarter-turn about Z from setup to
machine, a sign on A and a letter:

| along, chuck end | machine (X, Y) from setup (x, y) |
|---|---|
| x, − | (x, y) |
| x, + | (−x, −y) |
| y, − | (−y, x) |
| y, + | (y, −x) |

The sign is + when the internal right-hand turn (the top moves toward setup
−y), carried through that quarter-turn, is the direction `top_moves` names.
It is the ONE mapping: the post writes by it (`Emitter::Work`, arc centres,
the A word of every move, `G0 A`, `G92 A`), and `ProgramInSetupSpace` reads
by its inverse, with `ParseGcode` taking the configured letter — the other
rotary letter is a parse warning, not silence. The travel checks
(`MachineWarning`, `MachineBoundsFramed`, `DeriveReachOffset`,
`MachineOverhang`) take the machine's spans through the quarter-turn
(`TravelSpanInSetup`); a quarter-turn keeps the box axis-aligned, so
`ClipToMachine` is unchanged. A setup on the bed is identity.

**Which setups are held in the rotary.** `Setup::heldIn`, saved as
`held_in` (`rotary` | `bed`), unstated is derived: rotary when the setup has
a `surface_rotary` op, an op cut at several indices, or a non-zero index.
An indexed setup at A = 0 (the
first side of a part turned for the second) must state it. `list_cam_setups`
reports it.

**Refused, not guessed.** Posting (and so verifying a posted file) a setup
held in the rotary of a machine that does not state `rotary` is refused,
naming the missing setting (`MountRefusal`): a wrong guess drives the cutter
along a part that lies the other way. Bed setups post as before.

## Chuck and tailstock

What holds the part is a keep-out round the axis, planned around AND checked
(`HoldingEnvelopeOf`, shared by both). The machine's `holding`:
`chuck_diameter_mm`, `grip_mm` (stock inside the jaws), `tailstock` (bool),
`centre_length_mm`, `centre_diameter_mm`, `tailstock_body_diameter_mm`,
`clearance_mm`. Unstated sizes take conservative defaults (Ø90 chuck, 15 mm
grip, a 25 mm Ø14 centre, a Ø60 body, 3 mm clearance), and the verdict names
each one it used (`holding_defaults`) so a default is never read as measured.
A setup overrides with `grip_mm`, `tailstock` (engaged or not) and `chuck`
(false: held some other way, no jaws at the low end); the panel shows them as
*Part held*, *In the chuck*, *mm in the jaws* and *Tailstock engaged*.

**The envelope**, in setup space from the stock's ends x0 and x1: the chuck's
radius from x0 to the jaw tips (x0 + grip); from x1 the centre, a cone from
the axis out to the centre's radius over its length, then the body's radius.

- **Planning.** Every toolpath of a rotary-held setup — continuous and
  indexed — is clipped to the X window between the jaw tips and the
  tailstock, widened by the tool's radius and the clearance, by
  `ClipToMachine`'s clip-retract-reenter (which carries A across a split).
  What it leaves out is `Toolpath::holdingClippedMm`; an operation whose
  whole cut lies in the holding fails, saying so. Both it and `CutLength`
  (`cut_length_mm`) measure the TIP's path (`SegmentLength`): a move that
  turns the part counts the arc it turns under the tool, so a spiral's
  clipped turns count as turns — in XYZ alone they came to their axial
  advance, and a ring at one X to nothing.
- **Verifier.** The tool is a cylinder of its radius from the tip up; it
  collides where its nearest approach to the axis over its X slab is inside
  the envelope's radius there (`CheckHolding`, in `Inspect` and
  `InspectRotary`, from the program's first stated XY — the parser's X0 Y0
  before it is not a move). `holding_collisions`, `worst_holding_depth_mm`,
  `_x`, `_line` and `worst_holding` (chuck or tailstock) are in `cam_verify`,
  and any collision fails `ok`.

**A rotary program's travel.** The rotary route of `VerifyGcode` checks the
program's axial and Z spans against the machine's, as the height-map route
does.

## Indexed

`Setup::rotationA` is the machine's index, separate from the fixturing frame
([setup.md](setup.md#fixturing-frame-and-rotary-index)). **Turn a face up**
(`job.index_face`, `SolveIndexA`) solves the A that points a face at the
spindle, refusing a face that points along X: the index turns the part by
R_x(A), the mount's right-hand turn, so a +Y face comes up at A = 90.
Toolpaths are generated in the setup frame (`SetupToModel` is
`frame · R_x(-A)`, setup to model), so the 2.5D pipeline, the verifier and the
simulation run unchanged; every move carries the constant A, and the post
writes one `G0 A…` at safe Z before the first operation. An indexed file
verifies and plays on the ordinary height map: one constant A is a part
standing still. Arc fitting works on indexed jobs.

### One operation at several indices

`Operation::indexA`, saved and taken over the RPC as `index_a_deg` (a list of
degrees; every op type but `surface_rotary`, capability `OpField::Indexed`),
cuts one operation at each of those indices: the rook's four crenels are one
pocket at `[0, 90, 180, 270]`, not four setups that each repeat it. Empty
cuts at the setup's `rotation_a`. In the GUI the list comes from PICKS: on a
setup held in the rotary, a floor (or an engraving's label) that does not face
the spindle solves its own index (`SolveIndexA`), and a further pick at
another index adds that index and that target to the same operation, so the
four crenel floors, clicked, are one pocket at `[0, 90, 180, 270]`
([gui.md](gui.md#indices-from-picks)). The op's settings show the list as
chips (`A0 A90 ...`, each with an x to remove it), with the typed list behind
a small "edit" for an angle nobody can pick; they are offered on a machine
with a rotary axis, and a machine without one is refused in the operation's
own error. A `surface_rotary` does not take it (the RPC refuses it by name), and
a list carried onto one from another route changes nothing (`CutAtIndices`).

`GenerateAtIndices` runs the ordinary generation once per angle, on a copy of
the setup turned to it and the bodies framed for that copy, and joins each
role's pieces (rough, and finish when there is one): a rapid at safe Z over
the next piece's start carries the new A, so the post retracts and turns
there, and every move is stamped with its A, A0 included. The targets of an
op that clears down to a floor (`OpField::NeedsFlatFloor`: pocket, face,
adaptive) are floors, and each is cut only at the index that turns it to
the spindle; a floor no index presents is named in `skipped`. An
engraving's targets are taken the same way, by their sketch plane
(`EngraveFrameOf`). Other targets (a wall, a bore) are followed at every
index. `add_cam_op` derives such an op
in the frame of the first index that presents its first target
(`IndexFrameFor`): read in the setup's own frame, a floor that faces round the
axis is a wall, and the pick was refused. A piece that fails
is named by its angle in `skipped`; the operation fails only when every angle
did. Each angle's depths are the op's own `top_z`/`bottom_z`, in that angle's
frame.

Such a file's A changes, so `VerifyGcode` reads it on the radius map, and the
backplot draws each move where it lands on the work. Tabs and reliefs are not
carried (a point in one angle's frame; the rotary check reads neither).

The caches that live across generations key on how the part is framed
(`FramingKey`: the fixture frame, the index and every placement) as well as
the caller's geometry fingerprint — one fingerprint covers every setup of a
job and every index of one operation, and keyed on it alone the outline cache
handed one framing's outlines to another.

## Continuous: `surface_rotary`

One operation type finishes (and roughs) a part turning on the axis. It needs
no pick: with no target it cuts the whole length; a target face bounds it
ALONG the axis to that face's extent (it still turns all the way round) —
**Limit to a face** / **Whole length** in the verb window. It works on the
cutter-location radius map `RotaryCL` (the model's radius field dilated by
the tool), and it reads no `top_z`/`bottom_z` (those are filled with the
blank's diameter for display only). `RotaryCL` walks each cell's kernel
outward from its centre and stops once the best contact stands within
`TipHeight` of the highest cell under the cutter (a per-block maximum): the
answer is the full kernel's, cell for cell (`cam rotary: the pruned CL equals
the full kernel's`), and a smooth turned surface visits a fraction of it.

### Patterns (`rotary_pattern`)

- **`parallel`** (default): passes along X, one A each, the part turning
  between them — a rotary raster. `stepover` is a fraction of the cutter
  converted to millimetres of arc at the part's widest radius.
- **`spiral`**: one helix end to end (one per band on a rest pass,
  [Rest bands](#rest-bands)), A turning while the tool cuts —
  continuous 4-axis. Pitch is `stepover` measured along the surface: the
  axial advance per turn shrinks by `1/sqrt(1 + s²)`, with `s` the steepest
  slope within a pitch either side of the cutter (looking both ways, or the
  pitch springs back at the foot of each wall) — but never below one cell of
  the CL map per turn, which is as close as the map can tell two turns apart.
  A wall one cell wide is a slope in the hundreds, and without the floor it
  spaced the turns microns apart across the whole window round it; a part
  that is not round has such a wall at some angle almost everywhere
  (`cam spiral: turns bunch on a wall, but never closer than a cell`).
  Angular step is derived so one
  step's arc at the widest radius is no longer than the sample step (or the
  cell). Radius by bilinear lookup wrapping in θ; cells the tool reaches
  nothing in ride at the blank radius (a spiral cannot break without a
  relink). One closed ring at constant x at each end. Points on the line
  through their neighbours within a quarter of the cusp are dropped. Good on
  surfaces of revolution with shoulders; on a twist every turn densifies,
  because a helix cannot space its turns differently at different angles.
- **`helical`**: passes that FOLLOW a twist — helices at `lead_mm` per turn
  (signed for the hand; 0 derives the part's own twist with
  `DeriveRotaryTwist`, the median over non-round column pairs of the θ shift
  that lines one up with the next), zigzagging, each pass winding A back what
  the previous wound. Spacing is `stepover` measured perpendicular to the
  passes at the widest radius; the next step shrinks by the steepest
  cross-slope anywhere in the band it can cover (floored at a sixth of the
  base step), so passes densify on flute walls and nowhere else. A part with
  no twist is cut as parallel passes and the toolpath's `strategy` says so.

### Layers, rest, and the rough verb

- A `stepdown` (mm, radial) cuts in **layers** from the blank radius inward,
  each at `max(cl, r_blank − k·stepdown)`, the last one at `cl`; on a
  spiral each layer starts on the turn the one before ended on. This is
  depth-steady, not engagement-steady, roughing.
- **A layer cuts only what the one before left.** A cell whose floor is no
  lower than the layer before's (or the blank, for the first) is empty for
  that layer, so the pass lifts over it rather than riding in air — the grip
  and every span a shallower layer already took to `cl`. The unlayered finish
  skips cells at or above the blank.
- **Passes run a tool radius past each end** of the part, so a ball centred
  beyond it rolls over the end's edge and down a domed end's steep foot. What
  lies at the chuck or the tailstock is clipped
  ([Chuck and tailstock](#chuck-and-tailstock)). Where the part ends inside a
  verifier cell, gouge is not measured in that column: the part fills it only
  partly, and the stock beside it is not the part. A ball driven into the
  end face reads in the next column in.
- **Entries** come radially down to just above what is known to be left
  ([operations.md](operations.md#surface-entries)).
- **Parallel passes link at depth.** Consecutive zigzag passes whose ends meet
  within the tool's radius, with an arc at the blank of at most two pitches
  that clears the part, are joined by a feed move that turns A, not a retract
  and a rapid.
- **Rough on the rotary** (`job.rotary_rough`, `cam::DeriveRotaryRough`) is
  the finish's derivation plus a stepdown of half the cutter and a skin of
  `DefaultFinishStock(tool)`; **Finish on the rotary** (`job.rotary_finish`)
  derives no stepdown. The pattern is in the op's settings. Over the RPC, a
  script sets `stepdown` and `stock_to_leave` on the op, which is what the
  rough verb derives.
- `rest_from_tool` works on the rotary op as on the raster
  ([operations.md](operations.md#rough-a-surface-and-rest-machining)), with
  radius for height and arc length across the axis. What the larger cutter
  left is found on ITS OWN grid, exactly: both cutters' surfaces and
  envelopes (`ToolEnvelope`) there, the leftover where the larger one's
  stands more than the tolerance above both the part and the smaller one's,
  grown by the smaller cutter's radius — round the axis in the arc of the
  leftover cell's OWN radius, not one pitch for the whole map (the middle
  cell's, which on a turned part may be a neck or off the part) — and read
  onto the rest pass's grid by the nearest cell. It is then cut
  [band by band](#rest-bands). The raster's `RestMask` bounds the envelopes over blocks
  instead (the larger tip the highest in each, the smaller the lowest), which
  overstates the leftover by a block's rise; a turned part is slopes end to
  end, and on the chess pawn blank that made the rest pass cut 135 m, twice
  its finish.

### Rest bands

A rest pass is cut per **band**: each run of columns along the axis where the
rest mask has a cell anywhere round the part, with runs closer than the
cutter's radius joined into one. Each band gets its own pass set and the tool
retracts to the safe radius between bands; where nothing is left there is no
cut at all.

- **`spiral`**: one helix per band, rings at the band's ends. Inside the band
  it follows the WHOLE contact surface round the axis — a spiral cannot break,
  so cells the finish already reached are re-cut at the surface rather than
  crossed in the air and fed back down beside the leftover. A band is walls
  either side, and the spiral's pitch shrinks on walls (the steepest slope
  within a pitch either side), so a band costs more turns per millimetre
  than a flat.
- **`parallel`** and **`helical`**: the band's passes over the masked surface,
  breaking where the mask is empty as before; grouping them by band keeps a
  pass set from rapiding between two leftovers at every angle. A zigzag link
  is checked against the whole surface, not the masked one — an emptied cell
  read as zero let a link turn A with the tool down through the lobe between
  two leftovers.

`cam rotary: a rest pass cuts only the bands its finish left` holds both
patterns to it on a shaft with two grooves.

- **Add operation** in a setup's menu adds another rotary pass on a setup that
  already holds one (`cam::AddOperationType`); a 4th-axis machine with a plate
  clamped to the bed is still a 3-axis job and gets the whole-part cut-out.

## Feed on a turning cut

A move that turns A while cutting is posted with `F = f · L_controller / L_tip`
(`cam::ControllerFeed`, `CamRotaryFeed`), because F is a fact about how the
CONTROLLER measures a move that mixes millimetres and degrees:

- `L_tip`: the path the tool tip really travels — the linear part combined
  with the arc at the move's mean radius from the axis (`TipLength`);
- `L_controller` (`FeedModelOf` the post): `mach3` and `grbl` the combined
  vector `sqrt(dx² + dy² + dz² + dA²)` with A in degrees (`Combined`);
  `linuxcnc` the XYZ length when there is one, else |dA| (`LinearFirst`).

F is then held to what the axes can do, each to its own top speed: X, Y, Z to
the machine's `rapid_feed`, A to `a_max_deg_min`. A hold is reported once per
operation (how many moves, the smallest radius, the feed they would have
needed). Where the machine states no A speed nothing holds A, and the verdict
says once what the program asks of it. `PlanFeed` (a planned move) and
`PostedMinutes` (a posted move, whose F is already scaled) are the one rule
the post, `CuttingMinutes`, the timeline and the verdict share, so the cycle
time shown is the one the controller will take. The post keeps the F in force
when the new one is within 1 % of it, and never merges a move that changes A
into the next one. Set `a_max_deg_min` from the A motor's velocity in the
controller's tuning; one timed `G1 X10 A360 F100` from rest confirms which
feed model a controller uses.

## A winding (`G92 A`)

A spiral winds A through hundreds of turns, and starts wound (its angle
counts from the axis's end). At the end of any operation whose next angle is
half a turn or more away, the post writes `G92 A<v>`, the current angle moved
by whole turns to lie nearest the next angle the file needs, so the next
operation's approach turns the part less than half a revolution. It renames the same physical angle and touches A only. The parser
reads `G92 A` as a rename, keeping its moves on the continuous angle, so the
verifier and the simulation stay in step with the machine; `G92` on a linear
axis is reported as not modelled. There is no modulo-360 output: a feed move
from 359° to 1° would turn back 358° with the tool down on any controller that
does not take the short way.

## Verifying a turning part

`VerifyGcode` routes by the FILE, not the setup: a program whose A moves is
cut from a cylinder that turns and is checked on a **radius map**
`r(x, θ)` about the rotary axis (`RadiusMap`, `InspectRotary`); one whose A
does not is checked on the height map. Asking the setup would get the case
that matters wrong — a rotary-capable machine posting a 3-axis job. The
panel's verification routes the same way.

- **The blank** is derived from the part (`MakeRotaryBlank`,
  `RotaryBlankRadius`): its extent along the axis and its furthest point from
  it, plus the setup's margins, the radial one the larger of Y and Z. The
  simulator and the verifier share this one derivation, and start from what
  the job's earlier setups left of it (`StartingRotaryStock`,
  [simulation.md](simulation.md#leftover-another-setup-removes)).
- **The grid** is derived from the arc: enough angular cells that one cell's
  arc at the blank's surface is about the resolution, so a blank of any
  diameter is measured at the same fineness. The model is sampled per
  triangle EDGE so a long thin facet is not sampled as a radius of zero.
- **Neighbour rule.** Gouge is measured against the smallest designed radius
  in a cell's neighbourhood and leftover against the largest, as on the
  height map, so a shoulder on a half-millimetre grid does not read as
  millimetres of gouge.
- **Rapids** are tested against the rotary stock by the same walk that cuts,
  never carving it.
- **Leftover measures.** `max_leftover_mm` is usually an end face, which no
  rotary pass reaches. So a rotary verdict also carries `leftover_p95_mm` (the
  typical scallop), and `leftover_surface_max_mm` / `_at` /
  `leftover_surface_p99_mm` measured OFF the end-facing cells (surface
  steeper than 70° along the axis); regions wrap round θ, each says whether it
  is `end_face`, how far round it reaches (`span_deg`, its angle the middle of
  that arc) and whether it is a `ring` — all the way round, which is what an
  inside corner no pass reaches leaves, and whose angle is 0 and means
  nothing. A patch at one angle is a pass missing or lifting there; the two
  used to look alike, because the angle was a vector average that a ring
  cancels to noise. `leftover_end_face_area_mm2` sums the end faces. Areas
  are summed cell by cell, a cell's area its axial size by its arc at its
  radius. In a rotary answer the second number of every `_at` pair is an
  angle in degrees.
- Tabs and reliefs are 2.5D statements and are not carried into a rotary
  check. Fixtures are not checked on a rotary setup, and the verdict says so
  ([simulation.md](simulation.md#fixture-collisions)); the chuck and the
  tailstock are ([Chuck and tailstock](#chuck-and-tailstock)). The cutter-reach
  finding skips `surface_rotary`: its Z span is the blank's diameter, not a
  depth.

## The rotary picture

The part turns; the tool stays where the machine keeps it. Playback carves
with the verifier's `CarveSweepRotary` on the verifier's blank
(`RotaryStockSim`), so the last frame is the verdict's stock cell for cell.
Backward seeks restore a snapshot: one is kept every `KeyframeEvery()`
segments, derived from a memory budget and the grid's size, and taken only on
the first forward visit (when it holds "every segment before this one carved,
this one untouched"). The stock is meshed as a tube `(x, r·cosθ, r·sinθ)` with
caps, in the part's frame; `R_x(A)` at the cursor turns everything attached to
the part (the ghosted bodies, their outlines, the stock) and returns to
identity when playback ends. The path is drawn in machine space around the
cursor during rotary playback; the cut passes show as grooves in the stock.
Outside playback, the backplot draws a move carrying an A where it lands on
the work (rotated back by its own angle), so passes wrap the part. The
leftover overlay is not drawn for a rotary job (it is painted from a height
map).

## Limits

- No round stock mode: the blank is derived, and `stock_centre` gives the
  rotary centreline only because the derived blank is symmetric about the
  axis.
- The chuck and tailstock are axisymmetric envelopes: a jaw's actual shape,
  a steady rest, or a part held on a mandrel are not modelled.
- No rotary constant-engagement roughing; no wrapped engraving or other
  feature-following paths beyond a twist.
