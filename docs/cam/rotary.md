# The 4th axis

Two different things share the name. **Indexed** 4-axis turns the work to an
angle and then cuts ordinary 2.5D; **continuous** 4-axis turns A while the
tool cuts. Both need a machine with `hasRotaryAxis`: without it the rotary
index is refused, the rotary verbs are not on the toolbar, and no file can
carry an A word. The A axis is a rotation about setup-space X.

Code: `CamSetup.cpp` (index, frames), `CamSurface.{h,cpp}` (rotary patterns,
`DeriveRotaryTwist`), `CamSurfaceStrategies.cpp` (`GenerateRotarySurface`),
`CamRotary.{h,cpp}` (radius map, blank, rotary verdict), `CamRotaryFeed.{h,cpp}`
(feed on a turning move), `CamPost.cpp` (scaled F, `G92 A`), `CamGcode.cpp`
(modal A, `G92 A`), `CamSim.{h,cpp}` (`RotaryStockSim`).

## Indexed

`Setup::rotationA` is the machine's index, separate from the fixturing frame
([setup.md](setup.md#fixturing-frame-and-rotary-index)). **Turn a face up**
(`job.index_face`, `SolveIndexA`) solves the A that points a face at the
spindle, refusing a face that points along X. Toolpaths are generated in the
setup frame `frame · R_x(A)`, so the 2.5D pipeline, the verifier and the
simulation run unchanged; every move carries the constant A, and the post
writes one `G0 A…` at safe Z before the first operation. An indexed file
verifies and plays on the ordinary height map: one constant A is a part
standing still. Arc fitting works on indexed jobs.

## Continuous: `surface_rotary`

One operation type finishes (and roughs) a part turning on the axis. It needs
no pick: with no target it cuts the whole length; a target face bounds it
ALONG the axis to that face's extent (it still turns all the way round) —
**Limit to a face** / **Whole length** in the verb window. It works on the
cutter-location radius map `RotaryCL` (the model's radius field dilated by
the tool), and it reads no `top_z`/`bottom_z` (those are filled with the
blank's diameter for display only).

### Patterns (`rotary_pattern`)

- **`parallel`** (default): passes along X, one A each, the part turning
  between them — a rotary raster. `stepover` is a fraction of the cutter
  converted to millimetres of arc at the part's widest radius.
- **`spiral`**: one helix end to end, A turning while the tool cuts —
  continuous 4-axis. Pitch is `stepover` measured along the surface: the
  axial advance per turn shrinks by `1/sqrt(1 + s²)`, with `s` the steepest
  slope within a pitch either side of the cutter (looking both ways, or the
  pitch springs back at the foot of each wall). Angular step is derived so one
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
  each at `max(cl, r_blank − k·stepdown)`, the last one the finish; on a
  spiral each layer starts on the turn the one before ended on. This is
  depth-steady, not engagement-steady, roughing.
- **Rough on the rotary** (`job.rotary_rough`, `cam::DeriveRotaryRough`) is
  the finish's derivation plus a stepdown of half the cutter and a skin of
  `DefaultFinishStock(tool)`; **Finish on the rotary** (`job.rotary_finish`)
  derives no stepdown. The pattern is in the op's settings. Over the RPC, a
  script sets `stepdown` and `stock_to_leave` on the op, which is what the
  rough verb derives.
- `rest_from_tool` works on the rotary op as on the raster
  ([operations.md](operations.md#rough-a-surface-and-rest-machining)), with
  radius for height and arc length across the axis.
- **Add operation** in the panel adds another rotary pass on a setup that
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

A spiral winds A through hundreds of turns. At the end of any operation that
leaves A a turn or more from where it started, the post writes `G92 A<v>`,
the current angle moved by whole turns to lie nearest the next angle the file
needs, so the next operation's approach turns the part less than half a
revolution. It renames the same physical angle and touches A only. The parser
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
  simulator and the verifier share this one derivation.
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
  is `end_face`, and `leftover_end_face_area_mm2` sums the end faces. Areas
  are summed cell by cell, a cell's area its axial size by its arc at its
  radius. In a rotary answer the second number of every `_at` pair is an
  angle in degrees.
- Tabs and reliefs are 2.5D statements and are not carried into a rotary
  check. Fixtures are not checked on a rotary setup, and the verdict says so
  ([simulation.md](simulation.md#fixture-collisions)). The cutter-reach
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
- Passes stop at the part's ends, so a domed end's steep foot is not reached.
- No chuck or tailstock keep-out, and nothing says how far past the ends the
  passes may go.
- No rotary constant-engagement roughing; no wrapped engraving or other
  feature-following paths beyond a twist.
